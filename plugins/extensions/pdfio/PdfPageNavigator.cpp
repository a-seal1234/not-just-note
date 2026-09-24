/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

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

#include <KoDocumentInfo.h>

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
/// down, how often that is asked, and the smallest gap between two such writes.
///
/// Long enough that a paragraph is not copied to disk between every two strokes, short enough
/// that putting the pen down and the app dying within the same breath still loses nothing but
/// the last stroke -- and a page turn never has to be what makes an unsaved page safe.
constexpr qint64 InkSettleMs = 600;
constexpr qint64 AutoSaveTickMs = 250;
constexpr qint64 AutoSaveMinGapMs = 800;

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

    /// The vertical scrollbar instead, which says the same thing as integers and never moves on its
    /// own. KisCanvasController::resetScrollBars() sets its value to the top-left corner of the
    /// painted region, in image pixels, and its range to leave the visible height out of the image
    /// height -- so the middle of the viewport is value + visible / 2, with no converter, no zoom
    /// and no layout state involved. Only the vertical axis matters: a strip is vertical.
    auto *scrollArea = dynamic_cast<QAbstractScrollArea *>(m_view->canvasController());
    QScrollBar *bar = scrollArea ? scrollArea->verticalScrollBar() : nullptr;
    if (!bar) {
        return;
    }
    const int visible = qMax(1, int(imageSize.height()) - (bar->maximum() - bar->minimum()));

    /// Where the document origin sits inside the canvas widget, in widget pixels. Turning the
    /// widget's middle back into a document point this way does not go through the rectangle
    /// mapping that reported nonsense.
    /// The canvas controller already answers "which document point is in the middle of the
    /// viewport" -- it is the value the controller pans by, computed from
    /// imageRectInWidgetPixels() rather than from the converter's document offset, which is in a
    /// different unit and put the centre of the viewport outside the image (centre (2744,-3700) of
    /// a 1102x5058 image).
    const QPointF centre(0.0, bar->value() + visible / 2.0);

    /// Which page the middle of the view is over, in document coordinates.
    ///
    /// This replaced a rule that watched how far the page had been panned past its own edge, and
    /// that rule was wrong. A page that merely sits low in the viewport -- which is what the
    /// headless canvas does, and what centring can do anywhere -- is indistinguishable, to such a
    /// rule, from a page pulled down past its top. It then turned pages nobody had scrolled, every
    /// time the timer fired, until flooding the log was the only thing the application was doing.

    /// A reading that cannot be true is not evidence about where the view is, so it is dropped
    /// rather than acted on: an out-of-range zoom, or a centre outside the image, is the converter
    /// answering from a canvas that has not been laid out yet.
    const QRectF imageBounds(QPointF(0, 0), imageSize);
    if (!imageBounds.adjusted(-4, -4, 4, 4).contains(centre)) {
        if (now - m_lastRejectLog > 2000) {
            m_lastRejectLog = now;
            say(QStringLiteral("scroll: dropped a reading: zoom %1, centre (%2,%3) of a %4x%5 image")
                    .arg(zoom, 0, 'f', 3).arg(qRound(centre.x())).arg(qRound(centre.y()))
                    .arg(qRound(imageSize.width())).arg(qRound(imageSize.height())));
        }
        return;
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
    m_lastInkChange = 0;
    m_lastAutoSave = 0;

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

bool PdfPageNavigator::rollToPage(int index, QString *why, int centreOn)
{
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
    KisPaintLayer *ink = nullptr;
    for (quint32 i = 0; i < m_document->image()->root()->childCount(); ++i) {
        KisNodeSP child = m_document->image()->root()->at(i);
        if (child->name() == QStringLiteral("Ink")) {
            ink = qobject_cast<KisPaintLayer *>(child.data());
            break;
        }
    }

    const QList<PdfStripLayout::Slot> slots = target.slots();
    int changed = 0;

    /// Phase one: write every page whose band is about to disappear, and touch not a single pixel
    /// until every one of those writes has landed.
    ///
    /// The write used to be started from inside the repaint below, and its result was dropped on
    /// the floor -- the band was wiped straight afterwards whatever the write did. A write that
    /// failed to start, or that never reported back (which is what Krita does to a second
    /// background save started while the first is running), therefore took the only copy of that
    /// page's ink with the wipe, and a scroll was enough to reach it: ink gone, sometimes, and
    /// only when the timing lined up. Failing before any wipe leaves the strip exactly as it was:
    /// a roll that cannot be completed safely is a roll that does not happen -- the rule the
    /// bounded page window already follows for evicting a page.
    ///
    /// m_savingPages is held for the whole roll: the follow timer and the queued re-centre both
    /// watch it, and neither may turn a page on top of the writes this is waiting for.
    const bool wasSaving = m_savingPages;
    m_savingPages = true;
    for (int i = 0; i < slots.size(); ++i) {
        const int oldPage = i < m_stripPages.size() ? m_stripPages.at(i) : -1;
        const int newPage = slots.at(i).page;
        if (oldPage == newPage || oldPage < 0) {
            continue;
        }
        QString saveWhy;
        if (!saveThroughQueue(oldPage, &saveWhy)) {
            m_savingPages = wasSaving;
            fail(why, QStringLiteral("the window cannot move past page %1: %2. The strip is unchanged.")
                          .arg(oldPage + 1)
                          .arg(saveWhy));
            say(QStringLiteral("strip: roll refused before touching anything: %1").arg(*why));
            return false;
        }
    }

    /// And nothing is still landing when the incoming pages are read back out of their artifacts
    /// below: an idle write queued behind the ones just made would have those reads waiting on a
    /// file it has not finished yet.
    drainWrites(nullptr);

    for (int i = 0; i < slots.size(); ++i) {
        const int oldPage = i < m_stripPages.size() ? m_stripPages.at(i) : -1;
        const int newPage = slots.at(i).page;
        if (oldPage == newPage) {
            continue;
        }
        ++changed;

        /// The whole band, because the page arriving may be smaller than the one that was there.
        /// The leaving page's ink is already on disk -- phase one wrote every leaving page before
        /// this loop was allowed to run -- so the wipe below cannot take anything with it.
        if (ink) {
            ink->paintDevice()->fill(slots.at(i).cell,
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
        if (ink) {
            ink->setDirty(slots.at(i).cell);
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

        const QImage savedInk = PdfInkLoader::loadInk(
            QDir(m_projectDir).filePath(m_manifest.pages.at(newPage).kraFile), nullptr);
        if (ink && !savedInk.isNull()) {
            ink->paintDevice()->convertFromQImage(savedInk, nullptr,
                                                  slots.at(i).rect.x(), slots.at(i).rect.y());
            ink->setDirty(slots.at(i).cell);
        }
        if (paper) {
            paper->setDirty(slots.at(i).cell);
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

    m_savingPages = wasSaving;
    say(QStringLiteral("strip: rolled the window, repainting %1 slot(s)").arg(changed));
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
    if (!m_turnFromScroll && m_view && m_view->canvasController() && slot < m_stripRects.size()) {
        m_view->canvasController()->setPreferredCenter(QPointF(m_stripRects.at(slot).center()));
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
            QTimer::singleShot(0, this, [this, active]() {
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
                const bool rolled = rollToPage(active, &rollWhy, centreOn);
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
                    m_view->canvasController()->setPreferredCenter(
                        QPointF(m_stripRects.at(slot).center()));
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
        if (rollToPage(index, &rollError)) {
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
    const QImage savedInk = PdfInkLoader::loadInk(kraPath, nullptr);
    if (!savedInk.isNull()) {
        if (KisPaintLayer *stroke = qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data())) {
            stroke->paintDevice()->convertFromQImage(savedInk, 0, 0, 0);
            say(QStringLiteral("restored %1x%2 of ink from %3")
                    .arg(savedInk.width()).arg(savedInk.height()).arg(kraPath));
        }
    }

    return showImage(image, PdfProjectBuilder::inkStrokeLayer(image), index, PdfStripLayout(), why);
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
            viewGuard->canvasController()->setPreferredCenter(QPointF(pageRect.center()));

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
    if (now - m_lastInkChange < InkSettleMs) {
        /// The pen may still be down. A write takes a copy of the whole ink of its page, and
        /// copying a paragraph one stroke at a time is work the settle gap exists to avoid.
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

    /// The rectangle that page occupies in the strip, and the one ink layer everything is in. A
    /// document holding one page has neither, and needs no cropping.
    QRect pageArea;
    QList<KisNodeSP> inkLayers;
    const int slot = m_stripPages.indexOf(page);
    if (slot >= 0 && slot < m_stripRects.size()) {
        pageArea = m_stripRects.at(slot);

        for (quint32 i = 0; i < m_document->image()->root()->childCount(); ++i) {
            KisNodeSP child = m_document->image()->root()->at(i);
            if (child->name() == QStringLiteral("Ink")) {
                inkLayers.append(child);
                break;
            }
        }
    }

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
        const QImage thumb =
            projection->createThumbnailUncached(ThumbnailPixels, ThumbnailPixels, thumbArea);
        if (!thumb.isNull()) {
            QDir().mkpath(QFileInfo(thumbPath).absolutePath());
            thumb.save(thumbPath, "PNG");
        }
    }

    /// The copy is made while the page is still alive, and it owns its own pixels, so the editing
    /// document can be closed immediately afterwards.
    KisDocument *inkOnly = nullptr;
    if (pageArea.isValid() && !inkLayers.isEmpty()) {
        inkOnly = PdfPageSaver::createInkOnlyDocument(m_document->image(), pageArea, inkLayers, why);
    } else {
        inkOnly = PdfPageSaver::createInkOnlyDocument(m_document->image(), why);
    }
    if (!inkOnly) {
        return false;
    }

    const QString path = QDir(m_projectDir).filePath(
        PdfSession::pageFileName(m_manifest.pages.at(page).index));
    QDir().mkpath(QFileInfo(path).absolutePath());

    /// Deleted when the save reports back rather than by waiting: a nested event loop around
    /// sigSavingFinished wedged on the second save.
    QObject::connect(inkOnly, &KisDocument::sigSavingFinished, inkOnly,
                     [this, inkOnly, path, page, then](const QString &) {
        say(QStringLiteral("saved %1 (%2 bytes)").arg(path).arg(QFileInfo(path).size()));
        KisPart::instance()->removeDocument(inkOnly, true);

        /// Whoever queued this page hears back only once it is actually on disk, which is how the
        /// pages of a strip are written one after another rather than all at once.
        if (then) {
            then();
        }
    });

    if (!PdfPageSaver::saveInkOnly(inkOnly, path, why)) {
        KisPart::instance()->removeDocument(inkOnly, true);
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
