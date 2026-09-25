/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QPainter>
#include "PdfPageNavigator.h"

#include <cstdio>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QEventLoop>
#include <QFileInfo>
#include <QtMath>

#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QWidget>
#include <QTimer>

#include "backend/PdfRenderBackend.h"
#include "PdfPageStripDecoration.h"
#include "session/PdfStripBuilder.h"
#include "session/PdfInkLoader.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <KoColorSpaceConstants.h>
#include <KoDocumentInfo.h>

#include <kis_group_layer.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>

#include <KoZoomMode.h>
#include <kis_canvas_controller.h>
#include <kis_node_manager.h>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <KisViewManager.h>
#include <KisPart.h>
#include <KisView.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// The view that is actually showing \a document.
///
/// Not the value addViewAndNotifyLoadingCompleted() returns: that came back as the view of a
/// different document, so activating an ink layer, adding the strip decoration, setting the zoom
/// and scrolling to a page were all being done to the wrong one -- which is why strokes kept
/// landing on the page they started on. KisPart knows the views; this asks it.
KisView *viewForDocument(KisDocument *document)
{
    if (!document) {
        return nullptr;
    }

    /// KisView::document(), not viewManager()->document(). The view manager belongs to the main
    /// window and its document() returns the document of whichever view is *active*, so asking it
    /// reports the wrong document for every other view -- which is how this ended up activating an
    /// ink layer in one document while pointing at another.
    const QList<QPointer<KisView>> views = KisPart::instance()->views();
    for (const QPointer<KisView> &view : views) {
        if (view && view->document() == document) {
            return view;
        }
    }
    return nullptr;
}

void say(const QString &message)
{
    /// Both sinks: see the note in PdfIoProbe. On Android stderr goes nowhere and it is qWarning,
    /// through Krita's Android log handler, that reaches logcat.
    fprintf(stderr, "[pdfio] %s\n", qPrintable(message));
    fflush(stderr);
    qWarning("[pdfio] %s", qPrintable(message));

    /// Also to a file. Krita is single-instance: a second launch -- a probe, or the harness opening
    /// the application for the user -- hands its work to the instance that is already running and
    /// exits, so that launch's stderr stays empty and there is nothing to read afterwards. This
    /// file is the record of what the instance the user is actually looking at did.
    {
        static const QString logPath = [] {
            const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir().mkpath(dir);
            return QDir(dir).filePath(QStringLiteral("pdfio.log"));
        }();
        QFile log(logPath);
        if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            log.write(QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8());
            log.write(" pid=");
            log.write(QString::number(QCoreApplication::applicationPid()).toUtf8());
            log.write(" ");
            log.write(message.toUtf8());
            log.write("\n");
        }
    }
}

/// The root node the notebook calls "Ink": the group its content belongs in.
///
/// Both modes build one -- PdfProjectBuilder for a single page, PdfStripBuilder for the strip --
/// so this is the group itself. A node of that name which is not a group is reported as "no group"
/// by the caller rather than used as one.
KisNodeSP inkNodeOf(const KisImageSP &image)
{
    if (!image || !image->root()) {
        return KisNodeSP();
    }
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        const KisNodeSP child = image->root()->at(i);
        if (child->name() == QStringLiteral("Ink")) {
            return child;
        }
    }
    return KisNodeSP();
}

/**
 * Moves every content layer that ended up outside the Ink group into it, keeping its name and its
 * pixels. Nothing is deleted and nothing is flattened: a page's render (the desk and the
 * "PDF page N" layers) stays where it is, and every other root layer -- one the user made, or one
 * the roll restored from an artifact under a name the strip did not have -- goes inside the group.
 *
 * The group is the builders' to make, and both make it -- PdfProjectBuilder for a single page,
 * PdfStripBuilder for the strip -- so this does not invent one: a notebook with no Ink group has
 * nowhere to put content, and saying so is better than growing a second shape to keep in step.
 * \a image by value: KisSharedPtr hands back a const KisImage through a const smart pointer, and
 * addNode() and moveNode() are not const methods.
 *
 * How many content layers are still outside the group is reported, which is the measurable form of
 * the rule: zero once it has run.
 */
int adoptContentIntoInk(KisImageSP image)
{
    if (!image || !image->root()) {
        return 0;
    }

    KisNodeSP ink = inkNodeOf(image);
    const bool hasGroup = qobject_cast<KisGroupLayer *>(ink.data()) != nullptr;

    QList<KisNodeSP> strays;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        const KisNodeSP child = image->root()->at(i);
        if (child == ink || PdfPageSaver::isPageBackground(child)) {
            continue;
        }
        strays.append(child);
    }

    if (!strays.isEmpty() && hasGroup) {
        KisNodeSP above = ink->childCount() > 0 ? ink->lastChild() : KisNodeSP();
        for (KisNodeSP child : strays) {
            if (image->moveNode(child, ink, above)) {
                say(QStringLiteral("moved layer \"%1\" into Ink").arg(child->name()));
                above = child;
            }
        }
    } else if (!strays.isEmpty()) {
        say(QStringLiteral("no Ink group to move %1 content layer(s) into").arg(strays.size()));
    }

    int outside = 0;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        const KisNodeSP child = image->root()->at(i);
        if (child != ink && !PdfPageSaver::isPageBackground(child)) {
            ++outside;
        }
    }
    say(QStringLiteral("layers outside Ink: %1").arg(outside));
    return outside;
}

/// One gesture, one page.
constexpr qint64 TurnCooldownMs = 700;

/// How long the view has to sit still before the page under it is opened. Long enough that a
/// gesture in progress never triggers it, short enough not to feel deliberate.
constexpr qint64 SettleMs = 450;

/// How far from a page's own rectangle the question "which page is this point on" still answers a
/// page, in strip-image pixels. It has to exceed half the gap between two slots (SlotGap in
/// PdfStripLayout), or a view resting in the middle of that gap answers -1 -- no page at all --
/// and the active page never changes however long the user stops there.
constexpr qreal StripSlotGap = 200.0;

/// A centre resting exactly on the divide between two pages stays on the page it is already on,
/// so the answer does not flip on every 150 ms tick.
constexpr qreal StripCentreHysteresis = 48.0;

/// How long one page write may take to reach the disk before the save queue declares it failed.
///
/// A write that never reports is the wedge: the ink it carries is on disk nowhere, and everything
/// queued behind it would wait forever. The bound turns that into one failed page instead of the
/// permanent state of the notebook -- and it is the same number everything waits on, because a
/// wait that never finishes is worse than one that says it could not. On a close that cannot be
/// completed, the fallback is still Krita's own prompt: the user is told rather than the ink
/// silently dropped.
constexpr int SaveLandingTimeoutMs = 15000;

/// The idle half of the saving pipeline: how long the ink has to be quiet before it is written
/// down, how often that is asked, the smallest gap between two such writes, and the longest the
/// ink may sit unsaved.
///
/// Six hundred milliseconds was the first answer and the running notebook showed why it was
/// wrong: a stroke marks every page of the strip -- which page it fell on is only decided at save
/// time -- so every pause in writing, the moment between two sentences, rewrote all five pages,
/// about ten megabytes, over and over. Measured in pdfio.log: seven full five-page cycles inside
/// thirty seconds of taking notes.
///
/// Ten seconds of quiet is a pause someone actually took. The five minute backstop is what keeps
/// ink safe from someone who never pauses. Neither is what protects a page turn: the turn, the
/// roll and the close each write their pages and wait for the landing themselves, which is where
/// the ink would otherwise be lost. The idle write only decides how much is at risk if the
/// application dies -- at most the last ten seconds, or five minutes of continuous writing.
constexpr qint64 InkSettleMs = 10000;
constexpr qint64 AutoSaveTickMs = 250;
constexpr qint64 AutoSaveMinGapMs = 1000;
constexpr qint64 AutoSaveMaxIntervalMs = 5 * 60 * 1000;

/// The gap the strip decoration leaves between pages, in widget pixels.
constexpr qreal GapWidgetPixels = 16;

/// How wide a generated thumbnail is. The docker shows it smaller still; the extra is there so it
/// stays sharp when the interface is scaled up.
constexpr int ThumbnailPixels = 256;

/// The coarsest a page is rendered at on its way to a thumbnail. Below this, text stops being
/// recognisable and the thumbnail stops being useful for choosing a page.
constexpr qreal ThumbnailRenderDpi = 96;

} // namespace

PdfPageNavigator::PdfPageNavigator()
    : m_saves(SaveLandingTimeoutMs)
{
    /// The window's save is the plugin's own page save: the ink-only document, the crop when the
    /// page lives in a strip, and the asynchronous write saveCurrentPage already owns. Wired once
    /// here rather than at each call site, so no page switch can run without it -- and through
    /// the queue, because the window decides an eviction from this answer, and its answer has to
    /// be "the ink is on disk", not "the write was started".
    m_window.setCapacity(m_scope);
    m_window.setSaver([this](int index, QString *why) { return saveThroughQueue(index, why); });

    /// The one place a write is started, and never more than one at a time.
    ///
    /// The stamp is the other half. saveCurrentPage() takes its copy of the ink at the moment it
    /// starts, so the page may only be called clean when the write lands if the ink has not moved
    /// since that copy was taken -- a stroke that lands while the write is in the air keeps the
    /// page marked, and the next idle write picks it up. Without the stamp, "the write landed"
    /// would quietly mean "the write landed, except for the stroke made during it", and the
    /// window would evict the page on exactly that lie.
    m_saves.setStarter([this](int page, PdfSaveQueue::DoneFn done) {
        m_lastWriteError.clear();
        m_saveStamps[page] = m_lastInkChange;

        QString startWhy;
        if (!saveCurrentPage(&startWhy, page, [this, page, done]() {
                if (m_lastInkChange == m_saveStamps.value(page, -1)) {
                    m_window.setDirty(page, false);
                }
                done(true);
            })) {
            m_lastWriteError = startWhy;
            m_saveStamps.remove(page);
            return false;
        }
        return true;
    });
}

PdfPageNavigator *PdfPageNavigator::instance()
{
    static PdfPageNavigator navigator;
    return &navigator;
}

QString PdfPageNavigator::projectRoot()
{
    /// The policy -- Documents on the desktop, app-private on Android, and the fallback when
    /// Documents is not there -- belongs to PdfSession, which is where it can be tested without a
    /// navigator, a renderer or a window behind it.
    return PdfSession::projectRoot();
}

bool PdfPageNavigator::hasNotebook() const
{
    return !m_projectDir.isEmpty() && m_manifest.isValid();
}

bool PdfPageNavigator::scrollFollowEnabled() const
{
    return m_scrollFollow;
}

void PdfPageNavigator::setScrollFollowEnabled(bool enabled)
{
    if (m_scrollFollow == enabled) {
        return;
    }
    m_scrollFollow = enabled;
    say(QStringLiteral("turning pages by panning is now %1").arg(enabled ? "on" : "off"));
    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
}

int PdfPageNavigator::windowSlotFor(const QPointF &point) const
{
    if (m_stripPages.isEmpty() || m_stripCells.size() != m_stripPages.size()) {
        return -1;
    }

    int best = -1;
    qreal bestDistance = 1e18;
    for (int i = 0; i < m_stripCells.size(); ++i) {
        const QRectF cell(m_stripCells.at(i));
        const qreal dx = qMax(qMax(cell.left() - point.x(), qreal(0)), point.x() - cell.right());
        const qreal dy = qMax(qMax(cell.top() - point.y(), qreal(0)), point.y() - cell.bottom());
        const qreal distance = qSqrt(dx * dx + dy * dy);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }

    /// The slot the view already holds wins while the reading is close to it, so a centre resting
    /// between two slots does not flicker between them.
    if (m_windowSlot >= 0 && m_windowSlot < m_stripCells.size()) {
        const QRectF held(m_stripCells.at(m_windowSlot));
        const qreal dx = qMax(qMax(held.left() - point.x(), qreal(0)), point.x() - held.right());
        const qreal dy = qMax(qMax(held.top() - point.y(), qreal(0)), point.y() - held.bottom());
        if (qSqrt(dx * dx + dy * dy) <= bestDistance + StripCentreHysteresis) {
            return m_windowSlot;
        }
    }

    return best;
}

void PdfPageNavigator::checkScrollFollow()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    /// Said even when the switch is off: an empty log must never be read as "the follow is running
    /// and simply never decides anything".
    if (!m_scrollFollow) {
        if (now - m_lastFollowLog > 5000) {
            m_lastFollowLog = now;
            say(QStringLiteral("scroll: follow is OFF (PDF Notebook > Turn pages by panning)"));
        }
        return;
    }

    if (m_savingPages) {
        return;
    }

    if (!m_view || !m_view->canvasBase() || m_index < 0) {
        return;
    }

    KisCanvas2 *canvas = m_view->canvasBase();
    const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
    QWidget *widget = canvas->canvasWidget();
    KisDocument *document = m_document;
    if (!converter || !widget || !document || !document->image()) {
        return;
    }

    /// Nothing is decided while the canvas is still being placed. The view is zoomed and centred by
    /// this code when a page opens, and a tick landing inside that reported a zoom of 47 while the
    /// canvas was at 0.14 and a "centre" at the document origin -- not the page the user is looking
    /// at. Acting on it turned nothing.
    if (now < m_viewSettleUntil) {
        return;
    }

    const QSizeF imageSize(document->image()->width(), document->image()->height());

    /// The zoom is read for the log only. Nothing is decided on it: it was measured jumping
    /// 0.500 -> 0.667 and then 0.500 -> 0.250 in consecutive ticks 150 ms apart, because the
    /// converter it comes from is re-laid out while the strip repaints.
    const qreal zoom = converter->effectiveZoom();

    /// The vertical scrollbar, for the log. Its integers are the record of what the canvas
    /// controller thought the view was doing, and telling a wrong reading from a stale one needs
    /// both numbers.
    ///
    /// It is NOT what the centre is computed from any more. resetScrollBars() sets its range from
    /// KisCoordinatesConverter::minimumOffset()/maximumOffset() and its value from
    /// documentOffset(), and all of those are in WIDGET pixels -- the bounds are built as
    /// dPointMax - canvasWidgetSize + vast scrolling, against the canvas widget. The arithmetic
    /// that used to be here read them as image pixels and mixed the two, which made the centre
    /// drift with the zoom by everything the scrollbar was ahead of the image: measured on the
    /// tablet, the same place in the strip read 6670 at zoom 0.641 and 7888 at zoom 1.027, the
    /// follow turned pages nobody had scrolled to, and at zoom 0.939 the "visible" term went
    /// negative so the reading came out as centre (0,-244) of a 1653x12240 image -- outside the
    /// image, dropped as impossible, with the follow blind while the user was pinch-zooming.
    auto *scrollArea = dynamic_cast<QAbstractScrollArea *>(m_view->canvasController());
    QScrollBar *bar = scrollArea ? scrollArea->verticalScrollBar() : nullptr;

    /// The middle of the viewport, asked of the one thing that knows where the image sits: the
    /// converter's own widget-to-image transform, at the same widget centre that transform was
    /// computed against. There are no units to get wrong and no zoom term of its own -- zooming
    /// about the middle of the viewport leaves the middle of the viewport where it was, which is
    /// what stops a pinch from looking like a scroll, and it is right at the end of the notebook
    /// as well: at the bottom the reading is the bottom of the image minus half a viewport, which
    /// is inside the last page's own cell rather than two pages above it.
    QPointF centre = converter->widgetToImage(converter->widgetCenterPoint());

    /// Which page the middle of the view is over, in document coordinates.
    ///
    /// This replaced a rule that watched how far the page had been panned past its own edge, and
    /// that rule was wrong. A page that merely sits low in the viewport -- which is what the
    /// headless canvas does, and what centring can do anywhere -- is indistinguishable, to such a
    /// rule, from a page pulled down past its top. It then turned pages nobody had scrolled, every
    /// time the timer fired, until flooding the log was the only thing the application was doing.

    /// A middle outside the image is not automatically a bad reading.
    ///
    /// The canvas can be scrolled past an edge -- Krita keeps half a viewport of room around the
    /// document -- and at the end of a notebook that is exactly where a reader stops, so a middle
    /// just past the bottom edge IS the last page being looked at. Clamped to the edge it answers
    /// that, and the follow keeps working where it used to go blind: measured on the tablet, the
    /// window sat at the bottom of the strip for fifty seconds with every reading dropped as
    /// impossible, which is what "it just stays on the last page" was.
    ///
    /// A reading a whole viewport or more outside is still the converter answering from a canvas
    /// that has not been laid out yet, and is dropped as before.
    const QRectF imageBounds(QPointF(0, 0), imageSize);
    if (!imageBounds.adjusted(-4, -4, 4, 4).contains(centre)) {
        const qreal reach = 0.5 * widget->height() / qMax(qreal(0.0001), zoom);
        if (!imageBounds.adjusted(-reach, -reach, reach, reach).contains(centre)) {
            if (now - m_lastRejectLog > 2000) {
                m_lastRejectLog = now;
                say(QStringLiteral("scroll: dropped a reading: zoom %1, centre (%2,%3) of a %4x%5 image, "
                                   "bar %6 of %7..%8")
                        .arg(zoom, 0, 'f', 3).arg(qRound(centre.x())).arg(qRound(centre.y()))
                        .arg(qRound(imageSize.width())).arg(qRound(imageSize.height()))
                        .arg(bar ? bar->value() : -1)
                        .arg(bar ? bar->minimum() : 0)
                        .arg(bar ? bar->maximum() : 0));
            }
            return;
        }

        const QPointF raw = centre;
        centre.setX(qBound(imageBounds.left(), centre.x(), imageBounds.right()));
        centre.setY(qBound(imageBounds.top(), centre.y(), imageBounds.bottom()));
        if (now - m_lastRejectLog > 5000) {
            m_lastRejectLog = now;
            say(QStringLiteral("scroll: centre (%1,%2) is past the edge of a %3x%4 image; read as (%5,%6)")
                    .arg(qRound(raw.x())).arg(qRound(raw.y()))
                    .arg(qRound(imageSize.width())).arg(qRound(imageSize.height()))
                    .arg(qRound(centre.x())).arg(qRound(centre.y())));
        }
    }

    /// The one conversion between the two page systems. Inside a window the slot is decided from
    /// the window's own geometry and mapped to a page here; outside one, a document holds a single
    /// page and the point is mapped the old way.
    const int pageUnderCentre = m_stripPages.isEmpty()
        ? pageAtDocumentPoint(centre, zoom)
        : [this, &centre]() {
              const int slot = windowSlotFor(centre);
              return slot >= 0 && slot < m_stripPages.size() ? m_stripPages.at(slot) : -1;
          }();

    /// Act only once the view has stopped on that page. Turning one costs about 650 ms, measured,
    /// so it must not happen in the middle of a gesture.
    /// Said out loud because "the active page does not change" cannot be told apart from "the
    /// centre never moved" on a machine nobody can look at.
    if (pageUnderCentre != m_candidatePage) {
        say(QStringLiteral("scroll: centre (%1,%2) of a %3x%4 viewport at zoom %5 -> page %6 "
                           "(candidate was %7, current %8)")
                .arg(qRound(centre.x())).arg(qRound(centre.y()))
                .arg(widget->width()).arg(widget->height())
                .arg(zoom, 0, 'f', 3)
                .arg(pageUnderCentre + 1).arg(m_candidatePage + 1).arg(m_index + 1));
        m_candidatePage = pageUnderCentre;
        m_candidateSince = now;
        return;
    }

    /// Turning from the centre is OFF, and the reading is kept for the log only.
    ///
    /// The value this decides on comes from Krita's canvas controller, and it is not stable enough
    /// to decide on: measured, consecutive ticks 150 ms apart reported zoom 0.500 -> 0.667 and
    /// centre (414,4395) -> (526,5881), then 0.500 -> 0.250 and (177,4081) -> (30,2027). The zoom
    /// and the widget geometry it is derived from are re-laid out while the strip repaints, so a
    /// page computed from it changes on its own and the follow turns both ways. Until a stable
    /// source is wired in -- the canvas scrollbars carry the same information as integers -- the
    /// page changes from the commands this plugin owns: Next, Previous and the docker, which are
    /// deterministic. The reading below is logged so the instability stays visible.
    constexpr bool TurnFromTheViewCentre = true;

    if (!TurnFromTheViewCentre || pageUnderCentre == m_index) {
        /// A heartbeat, so "the follow is running" is visible even when nothing needs turning.
        if (now - m_lastFollowLog > 3000) {
            m_lastFollowLog = now;
            say(QStringLiteral("scroll: centre (%1,%2) at zoom %3 -> page %4 "
                               "(current %5, candidate age %6 ms)")
                    .arg(qRound(centre.x())).arg(qRound(centre.y())).arg(zoom, 0, 'f', 3)
                    .arg(pageUnderCentre + 1).arg(m_index + 1).arg(now - m_candidateSince));
        }
        return;
    }

    if (pageUnderCentre < 0) {
        return;
    }

    if (now - m_candidateSince < SettleMs || now - m_lastTurn < TurnCooldownMs) {
        return;
    }

    m_lastTurn = now;
    m_candidatePage = -1;

    say(QStringLiteral("scroll: settled on page %1 after %2 ms (current %3) -> turning")
            .arg(pageUnderCentre + 1).arg(now - m_candidateSince).arg(m_index + 1));

    QString why;
    m_turnFromScroll = true;
    const bool turned = showPage(pageUnderCentre, &why);
    m_turnFromScroll = false;
    if (!turned) {
        say(QStringLiteral("scroll: could not open page %1 (%2)").arg(pageUnderCentre + 1).arg(why));
    }
}

QPointF PdfPageNavigator::preferredCenterFor(KisView *view, const QPointF &imagePoint)
{
    if (!view || !view->canvasBase() || !view->canvasBase()->coordinatesConverter()) {
        /// No canvas to ask. The image point is the best guess there is for a view that does not
        /// exist, and no caller reaches this point with one.
        return imagePoint;
    }

    const KisCoordinatesConverter *converter = view->canvasBase()->coordinatesConverter();
    return converter->imageToWidget(imagePoint)
        - converter->imageRectInWidgetPixels().topLeft();
}

void PdfPageNavigator::ensureThumbnail(int index)
{
    if (!hasNotebook() || index < 0 || index >= m_manifest.pages.size()) {
        return;
    }

    const QString path = QDir(m_projectDir).filePath(m_manifest.pages.at(index).thumbFile);
    if (QFileInfo::exists(path)) {
        Q_EMIT thumbnailReady(index);
        return;
    }

    if (!m_thumbnailQueue.contains(index)) {
        m_thumbnailQueue.append(index);
    }

    if (!m_thumbnailTimer) {
        m_thumbnailTimer = new QTimer(this);
        connect(m_thumbnailTimer, &QTimer::timeout, this, &PdfPageNavigator::makeOneThumbnail);
    }
    if (!m_thumbnailTimer->isActive()) {
        /// One at a time, slow enough that the window keeps redrawing while a long notebook fills
        /// in.
        m_thumbnailTimer->start(40);
    }
}

void PdfPageNavigator::makeOneThumbnail()
{
    if (m_thumbnailQueue.isEmpty()) {
        m_thumbnailTimer->stop();
        return;
    }

    const int index = m_thumbnailQueue.takeFirst();
    if (index < 0 || index >= m_manifest.pages.size()) {
        return;
    }

    if (!m_thumbnailBackend) {
        m_thumbnailBackend.reset(PdfRenderBackend::create());
    }
    if (!m_thumbnailBackend || !m_thumbnailBackend->isOpen()) {
        if (!m_thumbnailBackend || !m_thumbnailBackend->open(sourcePath())) {
            return;
        }
    }

    /// Rendered coarser than the page but never as coarse as the thumbnail's own pixel count
    /// suggests. Asking for exactly 256 pixels of an A4 page means about 31 dpi, and text at 31 dpi
    /// is a grey smear: the thumbnail was unreadable. Rendering at 96 and shrinking costs a
    /// megapixel and looks like a page.
    const PdfPageInfo info = m_thumbnailBackend->pageInfo(index);
    const qreal widthPt = qMax(qreal(1), info.sizePt.width());
    const qreal dpi = qBound(ThumbnailRenderDpi, ThumbnailPixels * 72.0 / widthPt, qreal(200));

    const QImage page = m_thumbnailBackend->renderPage(index, dpi);
    if (page.isNull()) {
        return;
    }

    const QImage thumbnail = page.scaled(ThumbnailPixels, ThumbnailPixels,
                                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString path = QDir(m_projectDir).filePath(m_manifest.pages.at(index).thumbFile);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!thumbnail.save(path, "PNG")) {
        return;
    }

    say(QStringLiteral("thumbnail for page %1 written (%2x%3)")
            .arg(index + 1).arg(thumbnail.width()).arg(thumbnail.height()));
    Q_EMIT thumbnailReady(index);
}

KisView *PdfPageNavigator::currentView() const
{
    return m_view;
}

int PdfPageNavigator::pageAtDocumentPoint(const QPointF &point, qreal zoom) const
{
    if (m_index < 0 || m_index >= m_manifest.pages.size() || !m_document || !m_document->image()) {
        return -1;
    }

    /// In a strip the question is asked about the SLOT's band, not the page's rectangle: the centre
    /// of the viewport spends real time between two pages, and answering -1 for all of it armed the
    /// settle timer on nothing however long the view sat still -- which is why a strip never
    /// changed the active page once it stopped moving. The cells tile the strip, so every point
    /// belongs to a page, and the preferred page with a hysteresis keeps a centre resting on the
    /// divide from flipping every tick. The approximation below is for a document that holds one
    /// page and has no neighbours to name.
    if (!m_stripPages.isEmpty()) {
        return PdfStripLayout::nearestPage(m_stripPages, m_stripCells, point,
                                            StripSlotGap, m_index, StripCentreHysteresis);
    }

    const QSizeF page(m_document->image()->width(), m_document->image()->height());
    const QRectF pageRect(0, 0, page.width(), page.height());
    if (pageRect.contains(point)) {
        return m_index;
    }

    /// The open page's own scale: the manifest speaks in points and the image in pixels.
    const qreal pageWidthPt = m_manifest.pages.at(m_index).sizePt.width();
    const qreal pixelsPerPoint = pageWidthPt > 0 ? page.width() / pageWidthPt : 1.0;

    /// The same arrangement the strip decoration draws: the neighbours directly above and below,
    /// each at its own size, separated by the gap.
    const qreal gap = GapWidgetPixels / qMax(qreal(0.0001), zoom);

    for (int direction : { -1, 1 }) {
        const int other = m_index + direction;
        if (other < 0 || other >= m_manifest.pages.size()) {
            continue;
        }

        const QSizeF neighbour = m_manifest.pages.at(other).sizePt * pixelsPerPoint;
        const qreal top = direction > 0 ? pageRect.bottom() + gap
                                        : pageRect.top() - gap - neighbour.height();

        if (QRectF(pageRect.left(), top, neighbour.width(), neighbour.height()).contains(point)) {
            return other;
        }
    }

    return -1;
}

KisDocument *PdfPageNavigator::currentDocument() const
{
    return m_document;
}

const PdfSessionManifest &PdfPageNavigator::manifest() const
{
    return m_manifest;
}

const PdfPageWindow &PdfPageNavigator::pageWindow() const
{
    return m_window;
}

QString PdfPageNavigator::sourcePath() const
{
    return PdfSession::sourcePath(m_projectDir, m_manifest.sourceFile);
}

int PdfPageNavigator::pageCount() const
{
    return m_manifest.pages.size();
}

int PdfPageNavigator::currentIndex() const
{
    return m_index;
}

QString PdfPageNavigator::projectDir() const
{
    return m_projectDir;
}

bool PdfPageNavigator::openNotebook(const QString &pdfPath, QString *why)
{
    if (!QFileInfo::exists(pdfPath)) {
        fail(why, QStringLiteral("no such file: %1").arg(pdfPath));
        return false;
    }

    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend) {
        fail(why, QStringLiteral("no PDF render backend on this platform"));
        return false;
    }

    if (!backend->open(pdfPath)) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(pdfPath));
        return false;
    }

    /// Best effort: a notebook that is not there yet is made under the current root. Not being
    /// able to make that directory is not fatal -- migrateFromLegacy falls back to the legacy root,
    /// which is where an older notebook already is.
    QDir().mkpath(projectRoot());

    /// One directory per source PDF, keyed by its content, so reopening returns to the same
    /// notebook instead of starting a second one.
    const QString base = QFileInfo(pdfPath).completeBaseName();
    const QString key = QString::fromLatin1(PdfSessionManifest::sha256OfFile(pdfPath).left(8));
    const QString name = base + QLatin1Char('-') + key;

    /// A notebook that predates the folder moving to Documents is moved here on this first open.
    /// A move that cannot be made leaves this pointing at the legacy copy, so the notebook opens
    /// where it is rather than failing -- and the old copy is still there, untouched.
    QString moveError;
    const QString projectDir = PdfSession::migrateFromLegacy(name, &moveError);
    if (!moveError.isEmpty()) {
        say(QStringLiteral("notebook stays at %1: %2").arg(projectDir, moveError));
    }

    if (!QDir().mkpath(projectDir)) {
        fail(why, QStringLiteral("cannot create %1").arg(projectDir));
        return false;
    }

    const PdfSessionManifest manifest =
        QFileInfo::exists(PdfSession::manifestPath(projectDir))
            ? PdfSession::openProject(projectDir, why)
            : PdfSession::createProject(projectDir, pdfPath, *backend, why);

    if (!manifest.isValid(why)) {
        return false;
    }

    say(QStringLiteral("project ready: %1 pages at %2").arg(manifest.pages.size()).arg(projectDir));

    /// A page of a notebook that is being replaced is written before the new manifest takes over:
    /// after the assignment below, its index and its file name would be read out of the new file.
    /// Through the queue and waited for -- an unwaited write here would be carrying ink across
    /// the very manifest swap that decides which file it belongs to.
    if (m_document && m_document->image() && m_index >= 0) {
        QString saveError;
        if (!saveThroughQueue(m_index, &saveError)) {
            say(QStringLiteral("could not save the page of the notebook being replaced: %1").arg(saveError));
        }
    }

    /// Quitting does not have to close a view first -- qApp->quit() is enough -- so the same save
    /// is hung on the application's quit as well as on the view. Whichever happens first does the
    /// work; the other finds the document clean and does nothing.
    hookApplicationQuitOnce();

    /// The new notebook starts with an empty window, and its counters describe one notebook rather
    /// than the whole process.
    m_window.clear();

    /// And with no ink-change history: the clock the idle write waits on starts at the first
    /// stroke actually made in this notebook, not wherever the last one left it.
    ///
    /// The overdue clock starts NOW, for the same reason in the other direction: left at zero it
    /// would mean "overdue since the epoch", and the five minute backstop would fire on the very
    /// first tick after the first stroke.
    m_lastInkChange = 0;
    m_lastAutoSave = QDateTime::currentMSecsSinceEpoch();

    m_projectDir = projectDir;
    m_manifest = manifest;
    const bool shown = showPage(0, why);
    Q_EMIT pageChanged(m_index, pageCount(), base);

    /// Watched on a timer rather than from the canvas: panning arrives as wheel or touch events
    /// depending on the device, and where the page ended up afterwards is the same question either
    /// way.
    if (!m_scrollWatch) {
        m_scrollWatch = new QTimer(this);
        connect(m_scrollWatch, &QTimer::timeout, this, &PdfPageNavigator::checkScrollFollow);
    }
    m_scrollWatch->start(150);

    /// And the idle write, on a timer of its own: the follow asks where the view has settled,
    /// this asks whether the ink has been quiet long enough to be written down. Separate
    /// questions that used to be the same one -- there was no answer to the second at all until
    /// a page turn made it the only chance to save anything.
    if (!m_autoSaveTimer) {
        m_autoSaveTimer = new QTimer(this);
        connect(m_autoSaveTimer, &QTimer::timeout, this, &PdfPageNavigator::pumpAutoSave);
    }
    m_autoSaveTimer->start(AutoSaveTickMs);

    return shown;
}

void PdfPageNavigator::closeCurrentPage()
{
    /// The view takes the document with it, and removing the document as well leaves the view
    /// alive with nothing behind it. See the note in showPage.
    if (m_view) {
        m_view->closeView();
        m_view = nullptr;
        m_document = nullptr;
    } else if (m_document) {
        KisPart::instance()->removeDocument(m_document, true);
        m_document = nullptr;
    }
}

bool PdfPageNavigator::buildForStrip(int index, QString *why)
{
    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend || !backend->open(sourcePath())) {
        fail(why, QStringLiteral("the renderer cannot open the source"));
        return false;
    }

    say(QStringLiteral("building a strip of %1 around page %2").arg(m_scope).arg(index + 1));

    /// PdfStripBuilder::build() reads every slot's saved ink back out of its artifact, so the
    /// queue is drained first: any one of those pages may be the one a write is still carrying,
    /// and a read before it lands shows the page as it was before the ink that is on its way.
    drainWrites(nullptr);

    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(m_manifest, index, m_scope, m_dpi,
                                                                *backend, m_projectDir, why);
    if (!strip.image) {
        return false;
    }

    QList<int> pages;
    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        pages.append(slot.page);
    }
    say(QStringLiteral("strip holds pages %1").arg(
        [&pages]() {
            QStringList names;
            for (int page : pages) {
                names.append(page < 0 ? QStringLiteral("-") : QString::number(page + 1));
            }
            return names.join(QLatin1Char(','));
        }()));

    Q_UNUSED(pages);
    return showImage(strip.image, strip.activeInkLayer, index, strip.layout, why);
}

bool PdfPageNavigator::rollToPage(int index, QString *why, int centreOn, bool keepTheReadingPage)
{
    /// A roll that does not get far must not leave the canvas anchored to a point from a window
    /// it never moved to.
    m_rollAnchored = false;

    if (!m_document || !m_document->image()) {
        fail(why, QStringLiteral("no strip is open"));
        return false;
    }

    if (centreOn < 0 || centreOn >= m_manifest.pages.size()) {
        centreOn = index;
    }
    const PdfStripLayout target = PdfStripLayout::forWindow(m_manifest, centreOn, m_scope, m_dpi);
    if (!target.isValid()) {
        fail(why, QStringLiteral("the new window has no valid layout"));
        return false;
    }

    /// Only if the image would come out the same size, which it does whenever every slot is the
    /// same cell. Otherwise the document really does have to be built again.
    if (target.imageSize() != QSize(m_document->image()->width(), m_document->image()->height())) {
        fail(why, QStringLiteral("the new window is a different size"));
        return false;
    }
    if (m_stripPaper.size() != target.slots().size()) {
        fail(why, QStringLiteral("the strip does not have the slots it should"));
        return false;
    }

    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend || !backend->open(sourcePath())) {
        fail(why, QStringLiteral("the renderer cannot open the source"));
        return false;
    }

    const KoColorSpace *colorSpace = m_document->image()->colorSpace();

    const QList<PdfStripLayout::Slot> slots = target.slots();

    /// Phase one: write EVERY page the window holds -- all of them, not only the ones whose band
    /// is about to leave -- and touch not a single pixel until every one of those writes has
    /// landed.
    ///
    /// Saving only the leaving pages was the incremental design: keep whatever is already
    /// correct and patch the difference. It holds only while "already correct" is true, and
    /// nothing proved that -- a write that failed to start, or that never reported (Krita wedges
    /// a second background save started while the first is running), left the screen showing ink
    /// that was on disk nowhere, and a scroll was enough to reach it. Writing everything first
    /// gives the redraw below one thing to be built from: the artifacts. After a window move the
    /// strip and the disk are checked against each other on every roll instead of assumed to
    /// agree, which is the flow: save the window, then redraw it from what was saved.
    ///
    /// A clean pass, because each write waits inside an event loop and the pen can land a stroke
    /// while it does: the mark set is re-read after every pass, and a page still marked means the
    /// ink moved after its own write took its copy, so the pass runs again to catch it. Three
    /// passes that never come out clean mean the user is drawing THROUGH the roll, and the roll
    /// then refuses -- the stroke stays on screen and in memory, and the idle write takes it when
    /// the pen lifts. Redrawing from a disk that does not have the stroke would be losing it the
    /// slow way.
    ///
    /// m_savingPages is held for the whole roll: the follow timer and the queued re-centre both
    /// watch it, and neither may turn a page on top of the writes this is waiting for.
    const bool wasSaving = m_savingPages;
    m_savingPages = true;

    const auto stripIsClean = [this]() {
        for (int page : m_window.dirtyPages()) {
            if (m_stripPages.contains(page)) {
                return false;
            }
        }
        return true;
    };

    int saved = 0;
    constexpr int MaxSavePasses = 3;
    for (int pass = 1;; ++pass) {
        for (int page : m_stripPages) {
            if (page < 0) {
                continue;
            }
            QString saveWhy;
            if (!saveThroughQueue(page, &saveWhy)) {
                m_savingPages = wasSaving;
                fail(why, QStringLiteral("the window cannot move past page %1: %2. The strip is unchanged.")
                              .arg(page + 1)
                              .arg(saveWhy));
                say(QStringLiteral("strip: roll refused before touching anything: %1").arg(*why));
                return false;
            }
            ++saved;
        }

        if (stripIsClean()) {
            break;
        }
        if (pass >= MaxSavePasses) {
            m_savingPages = wasSaving;
            fail(why, QStringLiteral("the ink kept arriving while the window was being written; "
                                     "the strip is unchanged"));
            say(QStringLiteral("strip: roll refused after %1 write passes: %2").arg(pass).arg(*why));
            return false;
        }
        say(QStringLiteral("strip: the ink moved during write pass %1; writing the window again")
                .arg(pass));
    }

    /// And nothing is still landing when the redraw reads the artifacts back below: an idle write
    /// queued behind the ones just made would have those reads waiting on a file it has not
    /// finished yet.
    drainWrites(nullptr);

    /// Phase two: redraw the WHOLE window from what was just written -- every slot, paper and
    /// ink alike, whether its page changed bands or not. Nothing on screen comes from before the
    /// roll: the paper is rendered again from the source PDF and the ink is read back out of the
    /// artifact it was just written to, so after a move what you see IS the disk, and a slot
    /// holding stale pixels -- the class of bug this flow replaces -- cannot survive a window
    /// move.
    ///
    /// Synchronous: no event loop runs inside this loop, so no stroke can land between a wipe and
    /// the repaint that follows it.
    ///
    /// The strip's content layers, by name: the managed "Ink" and whatever else the pages carry,
    /// one layer per kind spanning the whole strip. A page is the part of every one of those
    /// layers inside that page's rectangle, which is how the artifact is written.
    QList<KisPaintLayer *> content;
    KisPaintLayer *ink = nullptr;
    /// Recursive, because the rule below can have moved the strip's content inside an Ink group: a
    /// reader that walked the root children only would stop finding the very layer it repaints.
    std::function<void(KisNodeSP)> collectContent = [&](KisNodeSP parent) {
        for (quint32 i = 0; i < parent->childCount(); ++i) {
            KisNodeSP child = parent->at(i);
            if (PdfPageSaver::isPageBackground(child)) {
                continue;
            }
            if (KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(child.data())) {
                content.append(layer);
                if (!ink && layer->name() == QStringLiteral("Ink")) {
                    ink = layer;
                }
            } else if (qobject_cast<KisGroupLayer *>(child.data())) {
                collectContent(child);
            }
        }
    };
    collectContent(m_document->image()->root());

    /// Read every page's layers BEFORE the repainting starts.
    ///
    /// Opening an artifact means opening a document, and doing that inside the loop that is
    /// repainting the live strip is where this crashed on the tablet: SIGSEGV on the main loop
    /// thread, faulting frames inside this library, about three hundred milliseconds after a
    /// window's writes had landed. Read first, repaint second -- and the log says which page was
    /// being read, which is the line the next crash would be read from.
    QHash<int, QList<QPair<QString, QImage>>> pageLayers;
    for (const PdfStripLayout::Slot &slot : slots) {
        if (slot.page < 0 || pageLayers.contains(slot.page)) {
            continue;
        }

        const QString kraPath =
            QDir(m_projectDir).filePath(m_manifest.pages.at(slot.page).kraFile);
        say(QStringLiteral("strip: reading the layers of page %1").arg(slot.page + 1));
        pageLayers.insert(slot.page, PdfInkLoader::loadInkLayersFromSidecar(kraPath, nullptr));
        say(QStringLiteral("strip: page %1 came back with %2 layer(s)")
                .arg(slot.page + 1)
                .arg(pageLayers.value(slot.page).size()));

        /// A layer the page has and the strip does not yet gets its place here, before anything
        /// is repainted, rather than in the middle of the repaint.
        for (const QPair<QString, QImage> &saved : pageLayers.value(slot.page)) {
            bool known = false;
            for (KisPaintLayer *layer : content) {
                if (layer->name() == saved.first) {
                    known = true;
                    break;
                }
            }
            if (known) {
                continue;
            }

            KisPaintLayerSP added = new KisPaintLayer(m_document->image(), saved.first,
                                                      OPACITY_OPAQUE_U8);
            m_document->image()->addNode(added, m_document->image()->root());
            content.append(added.data());
            say(QStringLiteral("strip: layer \"%1\" joins the strip for page %2")
                    .arg(saved.first)
                    .arg(slot.page + 1));
        }
    }

    for (int i = 0; i < slots.size(); ++i) {
        const int newPage = slots.at(i).page;

        /// The whole band, because the page arriving may be smaller than the one that was there.
        /// Every page's content is on disk -- phase one wrote the whole window before this loop
        /// was allowed to run -- so the wipe below cannot take anything with it. Every content
        /// layer, because every one of them belongs to the page that is arriving.
        for (KisPaintLayer *layer : content) {
            layer->paintDevice()->fill(slots.at(i).cell,
                                       KoColor(Qt::transparent, m_document->image()->colorSpace()));
        }

        /// const_cast because KisSharedPtr::data() hands back a const node, and the paper layer is
        /// this code's to repaint.
        KisPaintLayer *paper = qobject_cast<KisPaintLayer *>(
            const_cast<KisNode *>(m_stripPaper.at(i).data()));
        if (paper) {
            /// The desk colour for the room around the page, then the page itself.
            paper->paintDevice()->fill(slots.at(i).cell,
                                       KoColor(QColor(96, 96, 96), m_document->image()->colorSpace()));
        }

        /// Krita is told the slot changed. Writing into a paint device directly does not do that,
        /// and without it the canvas goes on showing what was there before: the window rolled, the
        /// slots held the right pages, and the screen did not.
        if (paper) {
            paper->setDirty(slots.at(i).cell);
        }
        for (KisPaintLayer *layer : content) {
            layer->setDirty(slots.at(i).cell);
        }

        if (newPage < 0) {
            continue;
        }

        const QImage rendered = backend->renderPage(newPage, m_dpi);
        if (paper && !rendered.isNull()) {
            paper->paintDevice()->convertFromQImage(rendered, nullptr,
                                                    slots.at(i).rect.x(), slots.at(i).rect.y());
            paper->setName(PdfStripBuilder::backgroundLayerName(newPage));
        }

        /// Every layer the page has, into the layer of the same name at this page's own
        /// rectangle. Nothing is read here: the reads happened above, before the repaint started.
        /// A layer the strip does not have falls back to Ink -- the content still lands, and it
        /// never builds a layer in the middle of a repaint.
        const QList<QPair<QString, QImage>> saved = pageLayers.value(newPage);
        for (const QPair<QString, QImage> &entry : saved) {
            KisPaintLayer *target = nullptr;
            for (KisPaintLayer *layer : content) {
                if (layer->name() == entry.first) {
                    target = layer;
                    break;
                }
            }
            if (!target) {
                target = ink;
                if (target) {
                    say(QStringLiteral("strip: page %1 carries layer \"%2\" and the strip has "
                                       "none; its pixels go into Ink")
                            .arg(newPage + 1)
                            .arg(entry.first));
                }
            }
            if (!target) {
                continue;
            }

            target->paintDevice()->convertFromQImage(entry.second, nullptr,
                                                    slots.at(i).rect.x(), slots.at(i).rect.y());
            target->setDirty(slots.at(i).cell);
        }
        if (paper) {
            paper->setDirty(slots.at(i).cell);
        }
    }

    /// And the placement rule the notebook asked for: a content layer that ended up outside the
    /// Ink group -- one the user made, or one restored from an artifact under a name the strip did
    /// not have, which the block above adds at the root -- is moved INTO it, keeping its name and
    /// its pixels. Never deleted, never flattened, and the number still outside is logged so the
    /// rule is measurable.
    adoptContentIntoInk(m_document->image());

    /// What the roll is not allowed to change: the page being read.
    ///
    /// The window moves under a canvas that does not move with it, so the same pixels end up
    /// holding a different page. Measured on the tablet: a roll taken while page 5 filled the
    /// view left page 8 under the same middle, and the follow then turned to 8 -- the reader's
    /// position had been moved by bookkeeping. The page under the middle of the viewport is
    /// recorded here, together with the point of it that is in the middle, and the canvas is put
    /// back on that point of that page once the new mapping is in place. A roll then looks like
    /// nothing happened, except that there are more pages to scroll to -- which is all it is.
    ///
    /// Read after the repaint and before the mapping changes: the pixels are the new window's
    /// already, but what the reader is looking at is still the old window's page. Anything
    /// scrolled while the writes were landing is included, because it has moved the canvas by
    /// now -- and the anchor is what keeps a two second write from teleporting the reader.
    int anchorPage = -1;
    QPointF anchorInPage;
    if (keepTheReadingPage && m_view && m_view->canvasBase()
        && m_view->canvasBase()->canvasWidget()
        && !m_view->canvasBase()->canvasWidget()->size().isEmpty()
        && m_view->canvasBase()->coordinatesConverter()) {
        const KisCoordinatesConverter *converter = m_view->canvasBase()->coordinatesConverter();
        const QPointF centre = converter->widgetToImage(converter->widgetCenterPoint());
        const int anchorSlot = windowSlotFor(centre);
        if (anchorSlot >= 0 && anchorSlot < m_stripPages.size()
            && anchorSlot < m_stripRects.size()) {
            anchorPage = m_stripPages.at(anchorSlot);
            anchorInPage = centre - QPointF(m_stripRects.at(anchorSlot).topLeft());
        }
    }

    Q_UNUSED(colorSpace);

    m_stripPages.clear();
    m_stripRects.clear();
    m_stripCells.clear();
    for (const PdfStripLayout::Slot &slot : slots) {
        m_stripPages.append(slot.page);
        m_stripRects.append(slot.rect);
        m_stripCells.append(slot.cell);
    }

    /// And the canvas goes back on the same point of the same page. The two places that would
    /// otherwise centre the view on the ACTIVE page stand down while m_rollAnchored is set
    /// (activateWithinStrip() right below, and the 350 ms follow-up the queued roll schedules):
    /// the active page is not necessarily the page being read, because the pen can scroll on
    /// while the writes are landing.
    if (anchorPage >= 0 && m_view && m_view->canvasController()) {
        const int anchorNow = m_stripPages.indexOf(anchorPage);
        if (anchorNow >= 0 && anchorNow < m_stripRects.size()) {
            m_rollAnchor = QPointF(m_stripRects.at(anchorNow).topLeft()) + anchorInPage;
            m_rollAnchored = true;
            m_view->canvasController()->setPreferredCenter(preferredCenterFor(m_view, m_rollAnchor));
            m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 800;
            say(QStringLiteral("strip: the window moved under page %1; the canvas stays on it")
                    .arg(anchorPage + 1));
        }
    }

    m_savingPages = wasSaving;
    say(QStringLiteral("strip: window moved to page %1: %2 write(s) landed, %3 slot(s) redrawn from disk")
            .arg(index + 1)
            .arg(saved)
            .arg(slots.size()));
    return activateWithinStrip(index, why);
}

bool PdfPageNavigator::activateWithinStrip(int index, QString *why)
{
    if (m_stripPages.isEmpty() || !m_document || !m_document->image()) {
        fail(why, QStringLiteral("no strip is open"));
        return false;
    }

    const int slot = m_stripPages.indexOf(index);
    if (slot < 0) {
        fail(why, QStringLiteral("page %1 is not in the strip").arg(index + 1));
        return false;
    }

    /// No node is activated here, and that is the whole change.
    ///
    /// There used to be one ink group per page, and turning a page meant telling Krita which of
    /// them was active. Nothing made that work in the running application -- strokes went on
    /// landing on the first page whatever was called -- and the strip has one ink layer now, so
    /// there is nothing to activate. Which page a stroke belongs to is decided when the page is
    /// saved, by cropping the rectangle the page occupies.

    /// And put it in the middle of the viewport.
    ///
    /// A strip holds several pages in one image, so making another page active does not move the
    /// view at all: the page that just became active is the one the user cannot see. Centring it
    /// leaves the page above and the page below partly on screen, so which page is being written
    /// on is visible rather than something to look up in a layer panel -- and turning a page moves
    /// the canvas, which is what a page turn should feel like.
    ///
    /// ensureVisibleDoc was not enough: it only scrolls far enough to bring a rectangle into view,
    /// and a page taller than the viewport can be "in view" while sitting anywhere.
    /// Not when the scroll itself decided, for the reason in m_turnFromScroll: the canvas is
    /// already where the user put it. An explicit turn still moves the canvas.
    /// And not when the roll has just put the canvas back on the page being read: that is a
    /// different point from the active page's centre whenever the pen scrolled on while the
    /// writes were landing, and moving to the active page's centre there would be the very jump
    /// the anchor exists to remove.
    if (!m_turnFromScroll && !m_rollAnchored && m_view && m_view->canvasController()
        && slot < m_stripRects.size()) {
        m_view->canvasController()->setPreferredCenter(
            preferredCenterFor(m_view, QPointF(m_stripRects.at(slot).center())));
        m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 800;
    }

    m_stripActiveSlot = slot;
    m_index = index;

    /// The window follows the active page, but only once the active page has reached its edge.
    ///
    /// A strip holds five pages and the canvas can only be scrolled inside those five, so waiting
    /// for a page outside the window to be asked for -- which is what the dispatch did -- means the
    /// window never moves: the notebook goes on and the strip stays on pages 1..5. Rolling here,
    /// when the active page sits on the first or last slot and the centred window would differ,
    /// moves the window one set at a time. Rolling at every step instead would repaint on every
    /// turn, which is the cost the strip exists to avoid.
    /// The roll is QUEUED rather than called.
    ///
    /// rollToPage() ends by calling this function again, so calling it from here directly -- which
    /// is what the first version did -- recursed through the same frames until the stack ran out
    /// (SIGSEGV, core 243431). Queued, this call has returned before the roll starts and there is
    /// no recursion left to grow. A page at the very end of the notebook cannot be centred any
    /// further and the clamp makes first equal to what the window holds, so nothing is scheduled.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int slots = m_stripPages.size();
    if (slots > 1 && (slot == 0 || slot == slots - 1) && pageCount() > slots
        && !m_rollingWindow && !m_savingPages && now - m_lastWindowRoll > 700) {
        const int first = qBound(0, index - slots / 2, qMax(0, pageCount() - slots));
        if (!m_stripPages.isEmpty() && m_stripPages.first() != first) {
            say(QStringLiteral("strip: moving the window from page %1 to page %2 (active %3)")
                    .arg(m_stripPages.first() + 1).arg(first + 1).arg(index + 1));
            m_lastWindowRoll = now;
            const int active = index;
            /// Whether the view decided this turn, read now because by the time the roll runs the
            /// flag belongs to whatever the user did in between. A roll the view decided must
            /// leave the reader's page where it is; an explicit turn is meant to move the canvas.
            const bool fromScroll = m_turnFromScroll;
            QTimer::singleShot(0, this, [this, active, fromScroll]() {
                if (m_rollingWindow || m_savingPages || m_stripPages.isEmpty()) {
                    return;
                }
                if (m_stripPages.indexOf(active) < 0) {
                    return;
                }
                m_rollingWindow = true;
                /// One slot in from the side the movement came from, not the middle: the page just
                /// left stays on screen above (or below) the active one, and the follow has three
                /// pages ahead instead of two -- which is what it turned back across.
                const bool movingDown = (m_stripPages.indexOf(active) == m_stripPages.size() - 1);
                const int centreOn = movingDown ? active + 1 : active - 1;
                QString rollWhy;
                const bool rolled = rollToPage(active, &rollWhy, centreOn, fromScroll);
                m_rollingWindow = false;
                if (!rolled) {
                    say(QStringLiteral("strip: could not move the window to page %1 (%2)")
                            .arg(active + 1).arg(rollWhy));
                    return;
                }

                /// And centre the view again once the repainted slots have been laid out. The first
                /// setPreferredCenter is applied before the canvas has taken the new geometry, so
                /// the view stayed where it was and the middle of the viewport landed on the page
                /// above the active one -- which the follow then read as "the user moved" and
                /// turned back to. Measured: after a roll the centre came out at the top of the new
                /// window (219,912) while the active page sat one cell below it.
                QTimer::singleShot(350, this, [this, active]() {
                    if (m_stripPages.isEmpty() || !m_view || !m_view->canvasController()) {
                        return;
                    }
                    const int slot = m_stripPages.indexOf(active);
                    if (slot < 0 || slot >= m_stripRects.size()) {
                        return;
                    }
                    if (m_rollAnchored) {
                        /// The roll placed the canvas on the page being read rather than on the
                        /// active page. Nothing moves it again, and the follow restarts from
                        /// nothing known: the next reading says where the middle is, and no
                        /// decision is taken from a value derived under the old mapping.
                        m_rollAnchored = false;
                        /// The same point again, for the same reason the other path repeats
                        /// here: the first call can land before the canvas has taken the
                        /// repainted slots.
                        m_view->canvasController()->setPreferredCenter(
                            preferredCenterFor(m_view, m_rollAnchor));
                        m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 1500;
                        m_candidatePage = -1;
                        m_candidateSince = QDateTime::currentMSecsSinceEpoch();
                        m_lastTurn = QDateTime::currentMSecsSinceEpoch();
                        m_windowSlot = -1;
                        return;
                    }

                    m_view->canvasController()->setPreferredCenter(
                        preferredCenterFor(m_view, QPointF(m_stripRects.at(slot).center())));
                    m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 1500;

                    /// And the follow's counter is set from the truth, not left holding what it
                    /// read under the old mapping.
                    ///
                    /// There are two page systems here and they were being read as one: the slot
                    /// inside the window (0..4, with the strip image's own y) and the notebook page
                    /// (1..36). A roll changes which notebook page sits in a slot, so the value the
                    /// follow derives from the centre changes without the user moving anything --
                    /// and it turned back to the page it thought had been left. Restarting the
                    /// counter from the active page keeps the two apart.
                    m_candidatePage = m_index;
                    m_candidateSince = QDateTime::currentMSecsSinceEpoch();
                    m_lastTurn = QDateTime::currentMSecsSinceEpoch();
                    m_windowSlot = slot;
                });
            });
        }
    }

    say(QStringLiteral("page %1 is now the active slot of the strip").arg(index + 1));
    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
    ensureThumbnail(index - 1);
    ensureThumbnail(index + 1);
    return true;
}

int PdfPageNavigator::scope() const
{
    return m_scope;
}

void PdfPageNavigator::setScope(int scope)
{
    m_scope = qMax(1, scope);
    /// The window bound follows the scope: design A is one page, design B would be three. Pages
    /// already open are not thrown out here; the extra slots are given back on the next page turn.
    m_window.setCapacity(m_scope);
}

bool PdfPageNavigator::showPage(int index, QString *why)
{
    if (m_inPageTurn) {
        /// A second turn arriving from inside an event loop this one is waiting in -- a click
        /// while a write is landing -- is refused, not interleaved. Two turns deciding at once
        /// where the ink belongs is how it ends up neither where it was drawn nor on disk.
        fail(why, QStringLiteral("a page turn is already in progress"));
        return false;
    }
    m_inPageTurn = true;
    /// A new turn is a new intent: an anchor an earlier roll left for the canvas is stale the
    /// moment the user asks for a page.
    m_rollAnchored = false;
    /// Cleared on every way out of this function, the refusals below included.
    struct TurnGuard {
        bool &held;
        ~TurnGuard() { held = false; }
    } guard{ m_inPageTurn };

    if (!hasNotebook()) {
        fail(why, QStringLiteral("no notebook is open"));
        return false;
    }
    if (index < 0 || index >= m_manifest.pages.size()) {
        fail(why, QStringLiteral("page %1 is outside the notebook").arg(index + 1));
        return false;
    }

    /// Whether the page that is open has to be written before the next one can take its place is
    /// the bounded window's decision, not a save here. It evicts the least recently used clean page
    /// for free, saves a dirty page before letting it go, and -- the case that used to lose ink --
    /// refuses the whole page turn when that save cannot be made. Nothing is dropped to make room.
    ///
    /// The dirty flag is the document's own modified flag. Krita sets it when a stroke lands
    /// (sigImageModified through KisDocument::setImageModified) and clears it when the page is
    /// written, so the window is told what the document actually holds rather than trusting a
    /// signal the switch might have missed. Scope one is design A: one document, one page, so the
    /// document's flag is the page's flag. The strip (design B) keeps several pages in one document
    /// and has its own save ordering -- it is off at scope one, and it is left alone here.
    if (m_stripPages.isEmpty() && m_document && m_document->image() && m_index != index) {
        /// Only ever promoted to dirty here, never cleared. The modified flag is Krita's, and a
        /// page's mark is cleared by the write that put its ink on disk (see the queue's starter
        /// in the constructor) -- reading a flag this code does not own used to be able to call
        /// a page with ink nowhere "clean", and a clean page is the one the window evicts
        /// without saving.
        if (m_document->isModified()) {
            m_window.setDirty(m_index, true);
        }
    }

    /// And the window is told about the page being opened whenever it does not already hold it,
    /// not only when the index changes.
    ///
    /// The index test alone leaves a hole, and it is one the notebook path walks straight into:
    /// openNotebook() clears the window and then shows page 0 of the new notebook, so when the page
    /// that was already open was page 0 as well, m_index == index and the window was never told
    /// about the new page. It stayed empty, and the next turn then had nothing to evict -- no
    /// eviction, no save before it -- while the document holding that page's ink was replaced by
    /// the new page's, which is the ink loss this window exists to prevent.
    ///
    /// Found by tests/PdfNavigatorIntegrationTest.cpp: it turns from page 1 of a notebook that was
    /// opened while page 1 was already up, and the window's own counters said no eviction had
    /// happened.
    if (m_stripPages.isEmpty() && !m_window.isOpen(index)) {
        QString windowError;
        if (!m_window.open(index, &windowError)) {
            fail(why, windowError);
            say(QStringLiteral("page turn refused: %1").arg(windowError));
            return false;
        }
    }

    /// A page that is already inside the open strip costs nothing to reach: no renderer, no
    /// document, no view. Unlocking its slot and asking for it is the whole point of design B --
    /// the 650 ms a page turn costs is almost all document and view, measured on the tablet.
    if (m_document && m_stripPages.contains(index)) {
        return activateWithinStrip(index, why);
    }

    /// A page one step outside it needs the window moved, not rebuilt.
    if (m_document && !m_stripPages.isEmpty()) {
        QString rollError;
        if (rollToPage(index, &rollError, -1, m_turnFromScroll)) {
            return true;
        }
        say(QStringLiteral("strip: rolling was not possible (%1); building instead").arg(rollError));
    }

    const bool opened = m_scope > 1 ? buildForStrip(index, why) : buildForSinglePage(index, why);
    if (!opened && m_stripPages.isEmpty() && m_document && m_document->image() && m_index >= 0) {
        /// The page that is open never moved -- showImage did not run -- so put it back in the
        /// window. The eviction above was made on the promise that the new page would take the
        /// slot, and that promise is off when the build fails.
        m_window.open(m_index, nullptr);
    }

    return opened;
}

bool PdfPageNavigator::buildForSinglePage(int index, QString *why)
{
    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend) {
        fail(why, QStringLiteral("no PDF render backend on this platform"));
        return false;
    }

    const QString source = PdfSession::sourcePath(m_projectDir, m_manifest.sourceFile);
    if (!backend->open(source)) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(source));
        return false;
    }

    /// Step by step on purpose. Opening a document on Android crashed inside Qt without saying
    /// where, and these lines are what turned "somewhere after the copy" into a stage.
    say(QStringLiteral("rendering page %1").arg(index + 1));

    KisImageSP image = PdfProjectBuilder::buildPageImage(m_manifest.pages.at(index), *backend, 200.0, why);
    if (!image) {
        return false;
    }
    say(QStringLiteral("rendered %1x%2 at %3 dpi").arg(image->width()).arg(image->height()).arg(image->xRes()));

    /// Whatever was already drawn on this page is put back before it is shown. Saving alone is not
    /// enough: a page rebuilt from the source comes back with an untouched Ink layer, so without
    /// this step ink that was written is invisible the moment the page is left and returned to.
    ///
    /// But only once the queue says nothing is still writing it. This page can be rebuilt while
    /// its own write is still in the air -- the turn that left it started that write only moments
    /// ago -- and reading the artifact before the write lands rebuilds the page WITHOUT the very
    /// ink it was evicted for. That is what "the ink comes back sometimes" was: not missing, just
    /// not landed yet, read too early, and then overwritten by the next save of a page that never
    /// saw it.
    {
        QString drainWhy;
        if (!drainWrites(&drainWhy)) {
            /// Not fatal: the read below may be stale, but refusing to open the page would trade
            /// a visible miss for an unusable notebook. The write's own outcome has already been
            /// reported through the queue, and the ink lands eventually if it can land at all.
            say(QStringLiteral("page %1: a write was still in flight while rebuilding (%2)")
                    .arg(index + 1)
                    .arg(drainWhy));
        }
    }

    const QString kraPath = QDir(m_projectDir).filePath(m_manifest.pages.at(index).kraFile);

    /// Whatever was drawn on this page comes back as the layers it was made of, not as one picture.
    ///
    /// The artifact is a .kra and holds the page's own layers (see PdfPageSaver), so a layer the
    /// user made is a layer again -- its own name, inside the page's Ink group, and separate from
    /// the rest. The merged image is what this used to load, and stays the fallback: an artifact
    /// that cannot be read as a document still shows its picture.
    KisNodeSP inkGroup;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        if (child->name() == PdfProjectBuilder::inkLayerName()) {
            inkGroup = child;
            break;
        }
    }

    QString restoreWhy;
    if (inkGroup && PdfInkLoader::loadInkLayersInto(kraPath, image, inkGroup, &restoreWhy)) {
        say(QStringLiteral("restored the page's own layers from %1").arg(kraPath));
    } else {
        if (!restoreWhy.isEmpty()) {
            say(QStringLiteral("page %1: %2; falling back to the flattened artifact")
                    .arg(index + 1)
                    .arg(restoreWhy));
        }

        const QImage savedInk = PdfInkLoader::loadInk(kraPath, nullptr);
        if (!savedInk.isNull()) {
            if (KisPaintLayer *stroke = qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data())) {
                stroke->paintDevice()->convertFromQImage(savedInk, 0, 0, 0);
                say(QStringLiteral("restored %1x%2 of ink from %3")
                        .arg(savedInk.width()).arg(savedInk.height()).arg(kraPath));
            }
        }
    }

    /// The same placement rule as the strip's roll: a content layer that is outside the page's
    /// Ink group is moved into it, keeping its name and its pixels.
    adoptContentIntoInk(image);

    /// The node to draw on: the group's own stroke layer when the restored layers left one, and
    /// the first layer of the group when they took its place.
    KisNodeSP activeNode = PdfProjectBuilder::inkStrokeLayer(image);
    if (!activeNode && inkGroup && inkGroup->childCount() > 0) {
        activeNode = inkGroup->at(0);
    }

    return showImage(image, activeNode, index, PdfStripLayout(), why);
}

bool PdfPageNavigator::showImage(KisImageSP image, KisNodeSP activeNode, int index,
                                 const PdfStripLayout &layout, QString *why)
{
    Q_UNUSED(why);

    KisDocument *document = KisPart::instance()->createDocument();

    /// A page is not a file, and Krita must not treat it as one.
    ///
    /// The document has no URL, so as soon as it is modified Krita's autosave timer starts and
    /// generateAutoSaveFileName() falls back to Krita's default autosave location -- where the
    /// next start offers it in the recovery dialog, next to a normal user's own files. Worse, what
    /// would be written is the whole editing document with the rendered page in it, not our
    /// ink-only page artifact. Switched off here, this document never schedules that timer and
    /// never writes that file. See docs/verify/AUTOSAVE-SEPARATION.md.
    document->setAutoSaveActive(false);

    /// And the tab says which page of which notebook it is rather than "Not Saved": caption() has
    /// nothing but the URL to name a document by, and there is no URL.
    const QString notebook = QFileInfo(m_manifest.sourceFile).completeBaseName();
    document->setUntitledCaption(QStringLiteral("%1 - page %2/%3")
                                     .arg(notebook)
                                     .arg(index + 1)
                                     .arg(m_manifest.pages.size()));

    /// Kept for the Document Information dialog; the tab no longer reads it (see caption()).
    document->documentInfo()->setAboutInfo(QStringLiteral("title"), notebook);
    document->setCurrentImage(image, true, activeNode);
    document->setProperty("pdfioProjectDir", m_projectDir);
    document->setProperty("pdfioPageIndex", m_manifest.pages.at(index).index);
    say(QStringLiteral("document created, image attached"));

    KisPart::instance()->addDocument(document);
    say(QStringLiteral("document registered"));

    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    say(QStringLiteral("main window %1").arg(window ? "found" : "MISSING"));
    KisView *view = window ? window->addViewAndNotifyLoadingCompleted(document) : nullptr;
    say(QStringLiteral("view %1").arg(view ? "created" : "NOT created"));

    /// And the one that is really showing this document, rather than whatever that call returned.
    if (KisView *mine = viewForDocument(document)) {
        view = mine;
    } else {
        say(QStringLiteral("view: no view claims this document; acting on the wrong one is likely"));
    }

    /// And make it the one Krita is working in, unconditionally.
    ///
    /// activeView() can already report this view while the view manager's current view is still
    /// another one, and it is the view manager that the node manager follows -- so the guarded
    /// version of this call did nothing, and activating an ink layer went on being a no-op against
    /// a different document. Every stroke stayed on the page it started on.
    if (view && window) {
        window->setActiveView(view);
        say(QStringLiteral("view: set as the active view (activeView was %1)")
                .arg(window->activeView() == view ? QStringLiteral("already this one")
                                                  : QStringLiteral("another one")));
    }

    /// Closing our tab makes the page safe before Krita can ask about it.
    ///
    /// Without this, a modified page raises "do you want to save it?", where Yes writes the whole
    /// editing document -- rendered page included -- as a .kra, and No drops the ink drawn since
    /// the last page turn. The handler writes the ink through the notebook's own page save and
    /// clears the modified flag, so queryClose() finds nothing to ask about. If it cannot write,
    /// it says so and returns false, and the normal prompt runs: the user is told rather than the
    /// ink silently dropped.
    if (view) {
        /// Only the page that is actually open is the navigator's to write here. The other views
        /// that pass through a close are the ones a page turn has just replaced; their ink is the
        /// page turn's business, and the turn either wrote it before it evicted the page or
        /// refused to move at all. Answering for them keeps the prompt away without writing the
        /// page that is open a second time.
        const QPointer<KisView> viewGuard = view;
        view->setPreCloseHandler([this, viewGuard]() {
            if (viewGuard && viewGuard == m_view) {
                return prepareForClose();
            }
            return true;
        });
    }

    /// The neighbouring pages are shown by the view that has just been created, not by whichever
    /// one this code happens to be running in.
    if (view && view->canvasBase()
        && !view->canvasBase()->decoration(QStringLiteral("pdfioPageStrip"))) {
        view->canvasBase()->addDecoration(
            KisCanvasDecorationSP(new PdfPageStripDecoration(QStringLiteral("pdfioPageStrip"), view)));
    }

    /// Zoomed to the active page, not to the document. A strip is several pages tall, so a view
    /// fitted to the whole document shows all of them at once at eight percent, and nothing is
    /// legible -- which is what "it is not full screen" looked like.
    ///
    /// The zoom is worked out here rather than asked for by mode: ZOOM_WIDTH was tried and left the
    /// view at eight percent, so the arithmetic is done against the canvas the view actually got.
    /// Deferred, because the view has no size at all the moment it is created.
    if (layout.isValid() && view) {
        const QPointer<KisView> viewGuard = view;
        const QRect pageRect = layout.slots().at(layout.activeSlot()).rect;

        const QPointer<KisDocument> documentGuard = document;
        QTimer::singleShot(400, this, [this, viewGuard, documentGuard, pageRect]() {
            if (!viewGuard || !viewGuard->canvasController() || !viewGuard->canvasBase()) {
                return;
            }

            /// Once per document. A turn inside a strip keeps the same document, and re-fitting it
            /// moves the centre with the zoom: the page the follow reads then changes on its own.
            if (m_zoomPlacedFor == documentGuard) {
                return;
            }
            m_zoomPlacedFor = documentGuard;

            QWidget *widget = viewGuard->canvasBase()->canvasWidget();
            const QSize viewport = widget ? widget->size() : QSize();
            if (viewport.isEmpty() || pageRect.isEmpty()) {
                say(QStringLiteral("zoom: no viewport (%1x%2) to fit a %3x%4 page into")
                        .arg(viewport.width()).arg(viewport.height())
                        .arg(pageRect.width()).arg(pageRect.height()));
                return;
            }

            /// Sized so the active page takes about three fifths of the viewport.
            ///
            /// Fitting the page exactly filled the viewport, and then the page that comes next is
            /// simply not on screen however the view is centred -- "even at page three you cannot
            /// see four". Leaving two fifths of the height free puts the top of the next page and
            /// the bottom of the previous one on screen, with the active page between them.
            constexpr qreal ActivePageShare = 0.6;

            const qreal zoom = qBound(qreal(0.02),
                                      qMin(qreal(viewport.width()) / pageRect.width(),
                                           (qreal(viewport.height()) * ActivePageShare)
                                               / pageRect.height()),
                                      qreal(8.0));

            say(QStringLiteral("zoom: fitting a %1x%2 page into a %3x%4 viewport gives %5")
                    .arg(pageRect.width()).arg(pageRect.height())
                    .arg(viewport.width()).arg(viewport.height()).arg(zoom));

            viewGuard->canvasController()->setZoom(KoZoomMode::ZOOM_CONSTANT, zoom);
            viewGuard->canvasController()->setPreferredCenter(
                preferredCenterFor(viewGuard.data(), QPointF(pageRect.center())));

            /// The view has just been placed by this code. Let the follow logic start from a clean
            /// slate instead of from whatever the previous document left behind, or the page that
            /// was just opened can be turned away again before it has settled -- which is what "it
            /// does not stay where it was opened" looked like.
            m_candidatePage = -1;
            m_candidateSince = 0;
            m_lastTurn = QDateTime::currentMSecsSinceEpoch();
            m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 800;
        });
    }

    /// Only now, with the new page up, is the old one given back. Closing first would take the
    /// view that is running this very code with it.
    const QPointer<KisDocument> previousDocument = m_document;
    const QPointer<KisView> previousView = m_view;

    m_document = document;
    m_view = view;
    m_index = index;
    m_stripPages.clear();
    m_stripRects.clear();
    m_stripCells.clear();
    for (const PdfStripLayout::Slot &slot : layout.slots()) {
        m_stripPages.append(slot.page);
        m_stripRects.append(slot.rect);
        m_stripCells.append(slot.cell);
    }
    m_stripActiveSlot = layout.isValid() ? layout.activeSlot() : -1;

    /// The pipeline hears about the ink itself, from the image that carries it.
    ///
    /// sigImageModified is what every stroke goes through on its way into the undo stack, and it
    /// fires however the image was changed -- unlike the document's modified flag, which the
    /// strip's direct writes into a paint device never set. Every mark it raises makes a page
    /// the idle write will pick up and the page window will refuse to evict unsaved.
    ///
    /// Connected here, to this image, and the old connection dropped first: the document is
    /// replaced whenever a turn rebuilds, and the previous image's signal must not go on marking
    /// pages of the new one -- a mark nothing could ever clear, because no write of this
    /// notebook could reach it.
    if (m_inkChangeConnection) {
        disconnect(m_inkChangeConnection);
    }
    m_inkChangeConnection = QObject::connect(image.data(), &KisImage::sigImageModified, this,
                                             [this]() {
        m_lastInkChange = QDateTime::currentMSecsSinceEpoch();
        markOpenPagesDirty();
    });

    /// The paper of each slot, in slot order, so the rolling window can repaint one of them.
    m_stripPaper.clear();
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        const QString name = image->root()->at(i)->name();
        if (name != QStringLiteral("Desk") && name != QStringLiteral("Ink")) {
            m_stripPaper.append(image->root()->at(i));
        }
    }

    /// Closed on the next turn of the event loop rather than right here. Closing a view and
    /// removing its document re-enters the window layout, and doing that inside the call that is
    /// opening the next page hung: the probe stopped after the first turn and had to be killed.
    if (previousView || previousDocument) {
        const QPointer<KisView> doomedView = previousView;
        const QPointer<KisDocument> doomedDocument = previousDocument;

        QTimer::singleShot(0, this, [doomedView, doomedDocument]() {
            if (doomedView) {
                /// The view takes the document with it: Krita closes a document when its last view
                /// goes. Removing the document as well was a double teardown, and it crashed --
                /// the document died while the view lived on, and a queued signal compressor then
                /// called slotUpdateDocumentTitle on that view, which reached KisDocument::path
                /// through a null document.
                doomedView->closeView();
            } else if (doomedDocument) {
                /// No view was ever made for it, so nobody else will close it.
                KisPart::instance()->removeDocument(doomedDocument, true);
            }
        });
    }

    /// The pages either side are drawn by the strip decoration, and it can only draw what has been
    /// rendered. Asking here means the neighbours fill in as the page is opened, rather than only
    /// once the page selector has been scrolled.
    ensureThumbnail(index - 1);
    ensureThumbnail(index + 1);

    say(QStringLiteral("page %1 of %2 open").arg(index + 1).arg(m_manifest.pages.size()));
    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
    return true;
}

bool PdfPageNavigator::saveStripPages()
{
    if (m_stripPages.isEmpty()) {
        /// A document holding one page has nothing to crop; the usual save is the whole of it --
        /// and through the queue, so this save cannot start while an idle write is still in the
        /// air (two in the air is the wedge, and this action is exactly the one a user reaches
        /// for while the idle write may be running).
        QString why;
        if (!saveThroughQueue(m_index, &why)) {
            say(QStringLiteral("could not save the page: %1").arg(why));
            return false;
        }
        return true;
    }

    /// One after another, each started when the last reports finished. Krita saves in the
    /// background, and starting a second save while the first is running wedges it -- which is how
    /// the earlier attempt at saving several pages ended.
    QList<int> pages;
    for (int page : m_stripPages) {
        if (page >= 0) {
            pages.append(page);
        }
    }

    say(QStringLiteral("saving %1 pages of the strip").arg(pages.size()));

    /// One after another, each waited for before the next starts.
    ///
    /// It used to chain them through sigSavingFinished -- a std::function that called itself from
    /// inside the signal -- and that is where the application died with SIGSEGV: the call stack
    /// grew through callbacks instead of through this loop (core 243431, while saving a notebook).
    /// savePageAndWait() waits on a bounded event loop instead, the same one the close path uses,
    /// and the follow timer is suspended for the duration so a page turn cannot run inside the save
    /// that is cropping the layer it would move.
    m_savingPages = true;
    bool ok = true;
    for (int page : pages) {
        QString why;
        if (!savePageAndWait(page, &why)) {
            say(QStringLiteral("could not save page %1 (%2)").arg(page + 1).arg(why));
            ok = false;
            break;
        }
    }
    m_savingPages = false;

    if (ok) {
        say(QStringLiteral("saved %1 pages").arg(pages.size()));
    }
    return ok;
}

void PdfPageNavigator::hookApplicationQuitOnce()
{
    if (m_quitHookInstalled || !QCoreApplication::instance()) {
        return;
    }
    m_quitHookInstalled = true;

    /// Quitting does not have to close the view first, so the same save is hung on the
    /// application's own quit. It is a no-op when the view's close has already done it.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
        prepareForClose();
    });
}

bool PdfPageNavigator::prepareForClose()
{
    if (!m_document || !m_document->image() || m_index < 0) {
        /// No page open: there is nothing here to lose.
        return true;
    }

    /// Whatever an idle write or an earlier turn already started lands first, whatever the
    /// modified flag below says: that flag describes the editing document, not the queue, and a
    /// write still in the air is ink that has reached disk nowhere yet.
    {
        QString drainWhy;
        drainWrites(&drainWhy);
    }

    if (!m_document->isModified()) {
        /// The ink is already on disk, or there was never any. Either way Krita has nothing to ask
        /// about, so there is no prompt to pre-empt.
        return true;
    }

    QList<int> pages;
    if (m_stripPages.isEmpty()) {
        pages.append(m_index);
    } else {
        for (int page : m_stripPages) {
            if (page >= 0) {
                pages.append(page);
            }
        }
    }

    QString why;
    for (int page : pages) {
        if (!savePageAndWait(page, &why)) {
            say(QStringLiteral("closing: page %1 could not be written (%2); the close goes back to Krita's own prompt")
                    .arg(page + 1).arg(why));
            return false;
        }
    }

    /// Only now. The document stayed dirty until its ink was written, which is what kept the page
    /// window's own rule -- a dirty page is never dropped -- honest while the write was in flight.
    m_document->setModified(false);
    say(QStringLiteral("closing: page %1 of %2 written through the notebook, nothing for Krita to ask about")
            .arg(m_index + 1).arg(m_manifest.pages.size()));
    return true;
}

bool PdfPageNavigator::savePageAndWait(int index, QString *why)
{
    /// The queue's wait, not a private one. This page queues behind whatever is already in the
    /// air and waits for its own landing, and the bound -- and the recovery when a write never
    /// reports at all -- belong to the queue. A private wait here would start a second write
    /// while one was running, which is exactly the wedge this pipeline exists to remove.
    return saveThroughQueue(index, why);
}

bool PdfPageNavigator::saveThroughQueue(int index, QString *why)
{
    /// Writes block, and the rest of the notebook is told so for as long as they do.
    ///
    /// checkScrollFollow() refuses to turn a page with m_savingPages held, the queued roll
    /// refuses to repaint, and pumpAutoSave() stands down -- all three would otherwise run inside
    /// the very event loop this wait is spinning, turning a page on top of the write that is
    /// cropping the layer it would move.
    const bool wasSaving = m_savingPages;
    m_savingPages = true;
    const bool ok = m_saves.saveNow(index, why);
    m_savingPages = wasSaving;

    if (ok) {
        return true;
    }

    /// The queue knows the page failed; the navigator is what knows what the disk said.
    if (why && !m_lastWriteError.isEmpty()) {
        fail(why, QStringLiteral("page %1 was not written: %2")
                      .arg(index + 1)
                      .arg(m_lastWriteError));
    }
    say(QStringLiteral("queue: page %1 did not land (%2)")
            .arg(index + 1)
            .arg(why && !why->isEmpty() ? *why : QStringLiteral("no reason recorded")));
    return false;
}

bool PdfPageNavigator::drainWrites(QString *why)
{
    const bool wasSaving = m_savingPages;
    m_savingPages = true;
    const bool ok = m_saves.waitIdle(why);
    m_savingPages = wasSaving;

    if (!ok) {
        say(QStringLiteral("queue: writes were still in flight (%1)")
                .arg(why && !why->isEmpty() ? *why : QStringLiteral("no reason recorded")));
    }
    return ok;
}

void PdfPageNavigator::markOpenPagesDirty()
{
    if (m_index < 0) {
        return;
    }

    if (m_stripPages.isEmpty()) {
        m_window.setDirty(m_index, true);
        return;
    }

    /// Every page the strip holds, not only the active one. Which page a stroke belongs to is
    /// decided when the page is saved, by the rectangle it sits in -- at stroke time the
    /// navigator does not know, and marking only the active page would let a stroke drawn on a
    /// neighbour leave the strip with nothing but its own memory to live in.
    for (int page : m_stripPages) {
        if (page >= 0) {
            m_window.setDirty(page, true);
        }
    }
}

void PdfPageNavigator::pumpAutoSave()
{
    if (!hasNotebook() || !m_document || !m_document->image() || m_index < 0) {
        return;
    }
    if (m_savingPages || m_rollingWindow || m_inPageTurn || m_saves.pendingCount() > 0) {
        /// A turn or a write owns the notebook right now. The queue decides when writes run;
        /// this tick only asks for one when nothing else is happening.
        return;
    }
    if (m_lastInkChange <= 0) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool quiet = now - m_lastInkChange >= InkSettleMs;
    const bool overdue = now - m_lastAutoSave >= AutoSaveMaxIntervalMs;
    if (!quiet && !overdue) {
        /// The pen may still be down and nothing is overdue: writing now would copy a paragraph
        /// one stroke at a time, which is the work the settle gap exists to avoid -- and in a
        /// strip it copies every page's ink, not only the one being written on.
        return;
    }
    if (now - m_lastAutoSave < AutoSaveMinGapMs) {
        return;
    }

    /// Everything the window holds that the ink itself has marked unsaved -- and only pages this
    /// document can actually crop. A mark left by a page that is no longer in the strip has
    /// nowhere to be written from, and saving it would put this document's ink into that page's
    /// file, which is the mistake the rectangle-cropping design exists to avoid.
    QList<int> mine;
    for (int page : m_window.dirtyPages()) {
        if (m_stripPages.isEmpty() ? page == m_index : m_stripPages.contains(page)) {
            mine.append(page);
        }
    }
    if (mine.isEmpty()) {
        return;
    }

    m_lastAutoSave = now;
    for (int page : mine) {
        QString why;
        if (!saveThroughQueue(page, &why)) {
            /// The page stays marked: the next quiet tick tries again rather than pretending it
            /// reached the disk. A mark cleared by a failed write is the same lie as evicting on
            /// one.
            say(QStringLiteral("autosave: page %1 is still unsaved (%2)").arg(page + 1).arg(why));
        }
    }
}

bool PdfPageNavigator::saveCurrentPage(QString *why, int index, std::function<void()> then)
{
    /// Which page to write. Not necessarily the one that is open: the whole strip can be written
    /// in one go, because which page a stroke belongs to is decided by the rectangle it sits in.
    const int page = index >= 0 ? index : m_index;
    if (!m_document || !m_document->image() || page < 0 || page >= m_manifest.pages.size()) {
        /// Nothing open is not a failure; it only means there is nothing to write. The caller is
        /// told the same moment -- the queue is owed exactly one completion for every write it
        /// starts, and it would go on waiting for a landing this function is never going to
        /// report if the promise went unkept here.
        if (then) {
            then();
        }
        return true;
    }

    /// The rectangle that page occupies in the strip. A document holding one page has none, and
    /// needs no cropping.
    QRect pageArea;
    const int slot = m_stripPages.indexOf(page);
    if (slot >= 0 && slot < m_stripRects.size()) {
        pageArea = m_stripRects.at(slot);
    }

    /// Which layers are written is PdfPageSaver's decision now: the page's own layers, every one of
    /// them, and never the render of the source page. It used to be decided here, by finding the
    /// single layer called "Ink" -- so a layer the user made outside it was dropped in silence, and
    /// a group inside it lost its contents to a flat copy.

    const QRect thumbArea = pageArea.isValid()
        ? pageArea
        : QRect(0, 0, m_document->image()->width(), m_document->image()->height());

    /// A thumbnail of the page as it looks, ink included, so the docker can show what each page
    /// holds without opening it. A thumbnail is a scaled copy, not a document, which is the whole
    /// reason this is affordable and rendering neighbouring pages is not.
    const QDir project(m_projectDir);
    const QString thumbPath = project.filePath(m_manifest.pages.at(page).thumbFile);
    if (KisPaintDeviceSP projection = m_document->image()->projection()) {
        /// Of the page's own rectangle, or a strip's thumbnail would be a picture of the strip.
        ///
        /// The page keeps its shape inside the square. The projection helper scales to exactly the
        /// width and height it is given, so asking it for a square squashes an A4 page into one --
        /// measured as a 256x256 thumbnail that was 99.6% white pixels, which is what a preview of a
        /// mostly blank page looks like once it has been stretched. Transparent margins and a
        /// hairline around the page make its shape visible in the list.
        const QSize scaled = thumbArea.size().scaled(QSize(ThumbnailPixels, ThumbnailPixels),
                                                     Qt::KeepAspectRatio);

        /// The picture is the page as it looks -- paper, ink and all -- in the page's own shape.
        /// No square canvas and no margins: a thumbnail is a small page, and the page it stands for
        /// is a portrait sheet of paper, not a square. The other renderer in this file has always
        /// produced exactly this shape (measured on the tablet as 197x256 for A4).
        const QImage thumb =
            projection->createThumbnailUncached(scaled.width(), scaled.height(), thumbArea);
        if (!thumb.isNull()) {
            QDir().mkpath(QFileInfo(thumbPath).absolutePath());
            thumb.save(thumbPath, "PNG");
        }
    }

    /// The copy is made while the page is still alive, and it owns its own pixels, so the editing
    /// document can be closed immediately afterwards.
    KisDocument *pageDocument = pageArea.isValid()
        ? PdfPageSaver::createPageLayersDocument(m_document->image(), pageArea, why)
        : PdfPageSaver::createPageLayersDocument(m_document->image(), why);
    if (!pageDocument) {
        return false;
    }

    const QString path = QDir(m_projectDir).filePath(
        PdfSession::pageFileName(m_manifest.pages.at(page).index));
    QDir().mkpath(QFileInfo(path).absolutePath());

    /// Deleted when the save reports back rather than by waiting: a nested event loop around
    /// sigSavingFinished wedged on the second save.
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, pageDocument,
                     [this, pageDocument, path, page, then](const QString &) {
        say(QStringLiteral("saved %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
        KisPart::instance()->removeDocument(pageDocument, true);

        /// Whoever queued this page hears back only once it is actually on disk, which is how the
        /// pages of a strip are written one after another rather than all at once.
        if (then) {
            then();
        }
    });

    if (!PdfPageSaver::saveDocument(pageDocument, path, why)) {
        KisPart::instance()->removeDocument(pageDocument, true);
        return false;
    }

    /// The thumbnail of this page has just been rewritten from the ink that was saved, so anything
    /// showing it -- the page selector -- is told, rather than waiting for the page to be opened
    /// again before it notices.
    Q_EMIT thumbnailReady(page);

    return true;
}

bool PdfPageNavigator::next(QString *why)
{
    if (m_index + 1 >= pageCount()) {
        fail(why, QStringLiteral("this is the last page"));
        return false;
    }
    return showPage(m_index + 1, why);
}

bool PdfPageNavigator::previous(QString *why)
{
    if (m_index <= 0) {
        fail(why, QStringLiteral("this is the first page"));
        return false;
    }
    return showPage(m_index - 1, why);
}
