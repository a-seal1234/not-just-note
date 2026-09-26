/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QPainter>
#include "PdfPageNavigator.h"

#include <cstdio>

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QEventLoop>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <kis_image_config.h>
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

/// Where the recent-notebook list is kept, and how one entry is made. Here rather than only with the
/// menus that show the list, because removing a notebook forgets its entry at the same moment, and a
/// format written down in two places is a format that drifts.
const char *const RecentNotebooksKey = "pdfio/recentNotebooks";

const QChar RecentNotebookSeparator = QLatin1Char(10);
const int MaxRecentNotebooks = 10;

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

/// How long a notebook change waits before the page is rebuilt. The old view has to be gone before
/// the new document is built -- Krita closes it on the event loop -- and this is the same wait
/// every notebook open uses.
constexpr int ReloadSettleMs = 700;

/// The gap the strip decoration leaves between pages, in widget pixels.
constexpr qreal GapWidgetPixels = 16;

/// The zoom that fits \a pageRect into \a viewport, with the active page taking \a share of its
/// height, or 0 when there is nothing to fit.
///
/// Three fifths rather than all of it, because fitting the page exactly filled the viewport and the
/// page that comes next was then simply not on screen however the view was centred -- "even at page
/// three you cannot see four". Two fifths of the height left free puts the top of the next page and
/// the bottom of the previous one on screen, with the active page between them.
///
/// \a share is the reading share in force (PdfPageNavigator::readingSharePercent()), passed in
/// rather than read from a constant here because it is the user's to set now. Which page a scroll
/// leaves under the centre depends on this zoom: the same finger movement covers zoom-times fewer
/// document pixels as the share grows, so a bigger page changes the reading page after more
/// scrolling and a smaller one after less.
///
/// One place, because a window move that RESIZES the document has to fit again: the fit is placed
/// once per document (see showImage), which was right while a roll could not change the size.
qreal fitZoomFor(const QSize &viewport, const QRect &pageRect, qreal share)
{
    if (viewport.isEmpty() || pageRect.isEmpty() || share <= 0.0) {
        return 0.0;
    }

    return qBound(qreal(0.02),
                  qMin(qreal(viewport.width()) / pageRect.width(),
                       (qreal(viewport.height()) * share) / pageRect.height()),
                  qreal(8.0));
}

/// The box a saved preview is fitted into, in pixels -- an A4 page comes out 814x1152.
///
/// It was 256, which is 180x256 for A4, and the tablet draws a page card up to 320x452 LOGICAL px
/// on a 3K panel, a device ratio of 2-2.5: up to 800x1130 device pixels, so the 180x256 file was
/// stretched 4.4x and the user reported "thumbnail too pixellated". 1152 covers that need with a
/// little margin, and it is a multiple of 64 for the texture caches.
///
/// The cost is small, and measured rather than guessed: a page of text saved at 814x1152 is 24.6 KB
/// against 11.1 KB at 181x256 (Qt PNG, a rendered page with headings and 45 lines), so an 18 page
/// notebook pays about 230 KB more in total. The render before it is NOT the bottleneck -- it is at
/// 96 dpi or more -- so this only stops throwing the pixels away at the last step.
constexpr int ThumbnailPixels = 1152;

/// The coarsest a page is ever rendered at, on its way to a preview or to the strip. Below this,
/// text stops being recognisable: a page is not worth drawing coarser than this. Shared, so the two
/// paths that render a page cannot drift into two floors.
constexpr qreal CoarsestRenderDpi = 96;

/// The dpi a page is rendered at when no budget is set, and the finest any budget can make it -- the
/// fixed 200 dpi the strip has always rendered at.
constexpr qreal DefaultRenderDpi = 200;

/// Where the rendered window's memory budget is kept, in megabytes. 0 means no budget, which is what
/// a notebook that has never been given one gets.
///
/// The key an earlier build used for its pixel target -- pdfio/maxPagePixels -- is deliberately NOT
/// read: that target is superseded (it bounded the longest side whatever the page's shape, so the
/// memory it bought changed with the shape), and a value left in a settings file must not resurrect
/// the behaviour the user replaced. It is left in the file rather than deleted: nothing writes a key
/// nobody reads, and a downgrade should find its own setting where it left it.
const char *const MemoryBudgetMbKey = "pdfio/memoryBudgetMb";

/// Where the strip's page count, and whether the strip is on, are kept.
///
/// TWO settings rather than one, and that is the whole point: turning the strip off with the menu's
/// switch must not forget how many pages it had. An "off" therefore writes only StripOnKey and
/// leaves StripPagesKey exactly where it was, and turning the strip back on reads it again.
const char *const StripPagesKey = "pdfio/stripPages";
const char *const StripOnKey = "pdfio/stripOn";

/// Where the page-loading trigger is kept: how long the page under the centre must stay there before
/// the strip turns to it, and how much of the viewport the active page takes at fit.
const char *const ScrollSettleMsKey = "pdfio/scrollSettleMs";
const char *const ReadingSharePercentKey = "pdfio/readingSharePercent";

/// What a reading share is held between, and the three fifths every build before the setting used.
///
/// The floor is the fraction's own domain rather than a quality guess: a share of zero is a page with
/// no height at all, and the fit's own 2% floor (see fitZoomFor) would silently take over. The
/// ceiling is where the rule stops doing what it is for: above 100% the page is taller than the
/// viewport, so the top of the next page and the bottom of the previous one -- the two fifths the
/// three fifths deliberately leave free, because "even at page three you cannot see four" -- are
/// gone again.
constexpr int MinReadingSharePercent = 1;
constexpr int MaxReadingSharePercent = 100;
constexpr int DefaultReadingSharePercent = 60;

/// What a TYPED budget is held between.
///
/// The floor is a quality choice, not a crash guard: 20 MB is about where the longest page of the
/// windows this ships with stops being a page. On the reported notebook (five pages, six full-size
/// layers) 20 MB comes out around 41 dpi -- a 340x450 px A4 sheet, small but still readable as a
/// layout -- and half of it is a grey smear. The derivation's own mechanical floor (one pixel for the
/// longest page) is orders of magnitude below anything a dialog should offer.
constexpr int MinMemoryBudgetMb = 20;

/// The share of the device's physical memory the strip may be given.
///
/// NOT a cap on what may be typed: the guard is computed from the machine's own RAM at run time, so a
/// powerful desktop can be given several gigabytes while a small tablet is stopped at the number it
/// cannot hold -- naming the RAM and the number when it is. A half is the fraction, and the
/// justification is measured rather than guessed: the app already holds about 890 MB of PSS with no
/// document open on the 8 GB tablet, so half leaves the OS, Krita and the panel their share while
/// still being far above anything this ships with (the measured five-slide window is 1461 MiB at
/// 200 dpi, and the user's own 212 page notebook is modest beside that).
constexpr qreal MemoryBudgetRamFraction = 0.5;



/// How many paint layers \a node and everything under it hold. Each one is a full-size allocation in
/// the strip image, so this is the layer count a budget is divided by.
void countPaintLayers(KisNodeSP node, int *count)
{
    /// Not const pointers: a const KisNodeSP hands back a const KisNode* through data(), and
    /// qobject_cast refuses to cast the constness away. The walking pointer is ours and there is
    /// nothing const about it.
    for (quint32 i = 0; i < node->childCount(); ++i) {
        KisNodeSP child = node->at(i);
        if (qobject_cast<KisPaintLayer *>(child.data())) {
            ++*count;
        }
        countPaintLayers(child, count);
    }
}

/// Whether a roll should behave as if its document went away after the write phase. Set by a test
/// only; see PdfPageNavigator::setDocumentGoneAfterWritesForTests().
bool &documentGoneAfterWritesForTests()
{
    static bool gone = false;
    return gone;
}

} // namespace

PdfPageNavigator::PdfPageNavigator()
    : m_saves(SaveLandingTimeoutMs)
    , m_sourceRenderers([]() { return PdfRenderBackend::create(); })
{
    /// The memory budget the user chose, if one ever was. Read before anything can render, so the
    /// first page of the first notebook this process opens is already inside it -- and a fresh
    /// install, or a notebook nobody has touched this for, gets 0: the fixed 200 dpi the strip has
    /// always rendered at, exactly.
    m_memoryBudgetMb = qMax(0, QSettings().value(QLatin1String(MemoryBudgetMbKey), 0).toInt());

    /// The strip's page count and whether it is on, read BEFORE the window's bound is set: a strip
    /// that was turned off opens as one page at a time with its count remembered, and the switch
    /// turns it back on at that count.
    ///
    /// Five and on is the notebook's own default, so a settings file that has never been touched
    /// opens exactly as it always did. A stored count below the smallest real strip -- three, because
    /// the active page has one either side of it -- is read as the default rather than as a window
    /// this build cannot lay out, and an even one is rounded up because the strip is symmetric.
    {
        const int stored = QSettings().value(QLatin1String(StripPagesKey), 5).toInt();
        m_stripPageCount = stored >= 3 ? stored : 5;
        if (m_stripPageCount % 2 == 0) {
            ++m_stripPageCount;
        }
        m_scope = QSettings().value(QLatin1String(StripOnKey), true).toBool() ? m_stripPageCount : 1;
    }

    /// The page-loading trigger, read here so the first tick of the first notebook already uses the
    /// value the user chose. Both are clamped exactly as their setters clamp them, so a value written
    /// by another build -- or edited by hand -- cannot put the follow in a state the menu could not.
    ///
    /// Neither has a ceiling for the settle delay: a value so large that the trigger never fires is
    /// what the "Turn pages by panning" switch already says, so there is nothing to protect the code
    /// from. The reading share is held between the fraction's own ends; see MinReadingSharePercent.
    {
        const QSettings settings;
        m_scrollSettleMs = qMax(0, settings.value(QLatin1String(ScrollSettleMsKey), int(SettleMs)).toInt());
        m_readingSharePercent = qBound(MinReadingSharePercent,
                                       settings.value(QLatin1String(ReadingSharePercentKey),
                                                      DefaultReadingSharePercent).toInt(),
                                       MaxReadingSharePercent);
    }

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

int PdfPageNavigator::scrollSettleMs() const
{
    return m_scrollSettleMs;
}

void PdfPageNavigator::setScrollSettleMs(int milliseconds)
{
    /// Only the floor is a clamp: a negative settle has no meaning, and there is no ceiling because
    /// nothing above one breaks. See the declaration for why.
    const int settle = qMax(0, milliseconds);

    /// Written whether or not the value changed, like the budget: the menu offers values, and the one
    /// in memory may not be the one another build wrote to the file.
    QSettings settings;
    settings.setValue(QLatin1String(ScrollSettleMsKey), settle);

    if (settle == m_scrollSettleMs) {
        return;
    }
    m_scrollSettleMs = settle;

    /// The value in force, on the [pdfio] line the user already reads. The next tick uses it; there
    /// is nothing else to apply, because the follow reads it as it decides.
    say(settle == 0
            ? QStringLiteral("the strip turns on the next tick after the view settles on a page")
            : QStringLiteral("the strip turns %1 ms after the view settles on a page").arg(settle));
}

int PdfPageNavigator::readingSharePercent() const
{
    return m_readingSharePercent;
}

int PdfPageNavigator::minReadingSharePercent()
{
    return MinReadingSharePercent;
}

int PdfPageNavigator::maxReadingSharePercent()
{
    return MaxReadingSharePercent;
}

qreal PdfPageNavigator::fitZoomForViewport(const QSize &viewport, const QRect &pageRect) const
{
    return fitZoomFor(viewport, pageRect, readingShare());
}

void PdfPageNavigator::setReadingSharePercent(int percent)
{
    const int share = qBound(MinReadingSharePercent, percent, MaxReadingSharePercent);

    QSettings settings;
    settings.setValue(QLatin1String(ReadingSharePercentKey), share);

    if (share == m_readingSharePercent) {
        return;
    }
    m_readingSharePercent = share;

    say(QStringLiteral("the active page now takes %1% of the viewport when the strip is fitted")
            .arg(share));

    /// Applied to what is on screen, because a share the user has just set has to be visible: the fit
    /// is the only thing it changes, so the zoom is re-set for the page that is open and the CENTRE
    /// is deliberately left where the reader put it (the fit itself centres on the active page, but
    /// this is not that gesture -- it is the same choice the roll's resize makes). The view-settle
    /// guard is pushed out so the follow does not read the transient zoom.
    if (!m_view || !m_view->canvasBase() || !m_view->canvasController() || !m_document
        || !m_document->image()) {
        return;
    }

    QWidget *widget = m_view->canvasBase()->canvasWidget();
    if (!widget || widget->size().isEmpty()) {
        return;
    }

    const QRect pageRect = (m_stripActiveSlot >= 0 && m_stripActiveSlot < m_stripRects.size())
        ? m_stripRects.at(m_stripActiveSlot)
        : m_document->image()->bounds();
    const qreal zoom = fitZoomForViewport(widget->size(), pageRect);
    if (zoom <= 0.0) {
        return;
    }

    say(QStringLiteral("zoom: the reading share is %1%, so a %2x%3 page fits the %4x%5 viewport at %6")
            .arg(share)
            .arg(pageRect.width()).arg(pageRect.height())
            .arg(widget->width()).arg(widget->height())
            .arg(zoom));
    m_view->canvasController()->setZoom(KoZoomMode::ZOOM_CONSTANT, zoom);
    m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 800;
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

    /// The settle delay is the user's (pdfio/scrollSettleMs, 450 ms by default): a longer one makes
    /// a burst of scrolling rest before anything turns, a shorter one follows the finger, and 0
    /// turns on the next tick. The cooldown is not a setting: it is the same pause the turn itself
    /// needs, so that one gesture cannot turn two pages.
    if (now - m_candidateSince < m_scrollSettleMs || now - m_lastTurn < TurnCooldownMs) {
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

QSize PdfPageNavigator::previewBoxFor(const PdfPageRecord &record, const QSize &room)
{
    if (room.isEmpty()) {
        return QSize();
    }

    /// The reader's size and nothing else: a turn swaps the sides, a scale changes both and a box
    /// changes the ratio, and displaySizePt() is the one place all three are already applied.
    const QSizeF display = record.displaySizePt();
    if (!display.isValid() || display.isEmpty()) {
        return QSize();
    }

    /// Rounded to whole points first, then fitted with Qt's own arithmetic, so every surface that
    /// prepares a preview for the same room agrees about the box to the pixel.
    const QSize page(qMax(1, qRound(display.width())), qMax(1, qRound(display.height())));
    return page.scaled(room, Qt::KeepAspectRatio);
}

QString PdfPageNavigator::thumbnailPathFor(const QString &projectDir, const PdfPageRecord &record)
{
    /// An empty name is "no preview yet", not the project directory: joining it would hand every
    /// caller the directory, and QFileInfo::exists() would then say the page has a picture.
    if (projectDir.isEmpty() || record.thumbFile.isEmpty()) {
        return QString();
    }
    return QDir(projectDir).filePath(record.thumbFile);
}

void PdfPageNavigator::ensureThumbnail(int index)
{
    if (!hasNotebook() || index < 0 || index >= m_manifest.pages.size()) {
        return;
    }

    /// An empty name is "no preview yet" -- and it is NOT the project directory, which is what
    /// QDir::filePath(QString()) would answer and what QFileInfo::exists() would then confirm. A
    /// page whose name an edit stripped was believed to have a preview forever; nameMissingPreviews()
    /// settles a name for such a page when the notebook is adopted, and until it has one there is
    /// nowhere a generated file could be written that any reader would look at.
    const QString name = m_manifest.pages.at(index).thumbFile;
    if (!name.isEmpty() && QFileInfo::exists(QDir(m_projectDir).filePath(name))) {
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
        /// One at a time, slow enough that the window keeps redrawing while a long notebook fills in.
        ///
        /// Deliberately NOT one of the settings beside the page-loading trigger, and the reason is
        /// that it is a YIELD rather than a threshold: the wall time of one preview is the manifest
        /// read, the page render, the ink load and the PNG write, all of them tens of milliseconds,
        /// and this interval only decides how much of the main loop is left for the canvas BETWEEN
        /// them. Halving it would not make a long notebook fill in noticeably sooner, and it would
        /// take exactly that time away from redrawing the page the reader is on. What the user asked
        /// to move -- when the strip loads and turns -- is the settle delay and the reading share.
        m_thumbnailTimer->start(40);
    }
}

void PdfPageNavigator::nameMissingPreviews()
{
    if (m_projectDir.isEmpty() || m_manifest.pages.isEmpty()) {
        return;
    }

    bool missing = false;
    for (const PdfPageRecord &page : m_manifest.pages) {
        if (page.thumbFile.isEmpty()) {
            missing = true;
            break;
        }
    }
    /// Every page names its preview already, which is every notebook this build writes: nothing is
    /// written merely for opening one.
    if (!missing) {
        return;
    }

    /// The allocator the notebook itself uses: a number past every artifact name the list already
    /// holds, so a name handed out here is one no other page's preview is using. A number is skipped
    /// when its preview name is somehow taken, which keeps the naming rule in one place.
    int number = m_manifest.effectiveNextPageNumber();
    QSet<QString> taken;
    for (const PdfPageRecord &page : m_manifest.pages) {
        if (!page.thumbFile.isEmpty()) {
            taken.insert(page.thumbFile);
        }
    }

    int named = 0;
    for (PdfPageRecord &page : m_manifest.pages) {
        if (!page.thumbFile.isEmpty()) {
            continue;
        }
        QString name;
        do {
            name = PdfSession::thumbFileNameForNumber(number++);
        } while (taken.contains(name));
        page.thumbFile = name;
        taken.insert(name);
        ++named;
    }
    if (named == 0) {
        return;
    }
    m_manifest.nextPageNumber = number;

    /// The manifest is where every reader gets the name from -- the docker, the strip decoration,
    /// a generated preview's own writer and the ops screen all read it -- so the list is settled on
    /// disk as well as in memory. A write that fails leaves the in-memory names in force: the
    /// previews are generated and written at exactly these names, and the next open derives the
    /// same ones again.
    QString why;
    if (!m_manifest.writeTo(PdfSession::manifestPath(m_projectDir), &why)) {
        say(QStringLiteral("%1 page(s) were given a preview name in memory, but the notebook could not "
                           "be written: %2")
                .arg(named)
                .arg(why));
        return;
    }
    say(QStringLiteral("%1 page(s) that recorded no preview were given one").arg(named));
}

void PdfPageNavigator::makeOneThumbnail()
{
    if (m_thumbnailQueue.isEmpty()) {
        m_thumbnailTimer->stop();
        return;
    }

    const int index = m_thumbnailQueue.takeFirst();
    if (index < 0) {
        return;
    }

    /// The manifest the notebook is on DISK, not the one held in memory.
    ///
    /// A turn is written to the manifest and to the artifact on disk before the navigator is
    /// reloaded, and ensureThumbnail() goes on answering from the in-memory record until it is. A
    /// preview generated in that window would show the page the wrong way up -- and, because
    /// ensureThumbnail() returns early once the file exists, it would never be redone. Reading it
    /// here is one small JSON file per generated preview; a manifest that cannot be read falls back
    /// to the one we have rather than leaving the page with no preview at all.
    PdfSessionManifest manifest =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(m_projectDir), nullptr);
    if (!manifest.isValid()) {
        manifest = m_manifest;
    }
    if (index >= manifest.pages.size()) {
        return;
    }

    /// The name the picture is written at: the record's own, the same one every reader joins onto
    /// the project directory. A record that names none has nowhere a reader would look, and joining
    /// an empty name would write at the project directory itself -- so nothing is generated for
    /// such a page. nameMissingPreviews() is what gives it a name, on adoption.
    const QString thumbName = manifest.pages.at(index).thumbFile;
    if (thumbName.isEmpty()) {
        return;
    }

    /// Through the source renderers: the page's own source and its own page inside it. A page
    /// whose source went away gets no thumbnail, which is the same answer the rest of the notebook
    /// gives, rather than a picture of whatever file happened to be first.

    /// Rendered coarser than the page but never as coarse as the preview's own pixel count
    /// suggests. Asking for exactly the preview's pixels of an A4 page means about 31 dpi, and text
    /// at 31 dpi is a grey smear: the thumbnail was unreadable. So the render is aimed at the
    /// preview box between a floor and a ceiling: 96 dpi is the coarsest a page is ever rendered at
    /// on its way to a preview and 200 the finest, and a BIGGER preview raises the dpi between them
    /// rather than lowering it.
    const PdfPageInfo info = m_sourceRenderers.pageInfo(manifest, m_projectDir, index, nullptr);
    const qreal widthPt = qMax(qreal(1), info.sizePt.width());
    const qreal dpi = qBound(CoarsestRenderDpi, ThumbnailPixels * 72.0 / widthPt, DefaultRenderDpi);

    const QImage page = m_sourceRenderers.renderPage(manifest, m_projectDir, index, dpi, nullptr);
    if (page.isNull()) {
        return;
    }

    /// The page's ink, which the render alone does not have.
    ///
    /// The preview used to be the source render and nothing else, so the moment one was GENERATED
    /// rather than written by a save -- which is every page a turn has just dropped -- a page that
    /// had been drawn on came back as blank paper. The artifact holds the ink page-local at
    /// whatever dpi it was written, while the render is at this preview's dpi, so each layer is
    /// scaled onto the render. Read from the PNG sidecar, which opens no document at all; a page
    /// with no artifact has no ink, and nothing is composited.
    QImage composed = page;
    const QString kraPath = QDir(m_projectDir).filePath(manifest.pages.at(index).kraFile);
    const QList<QPair<QString, QImage>> ink =
        PdfInkLoader::loadInkLayersFromSidecar(kraPath, nullptr);
    if (!ink.isEmpty()) {
        QPainter painter(&composed);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const QPair<QString, QImage> &layer : ink) {
            if (layer.second.isNull()) {
                continue;
            }
            painter.drawImage(QRect(QPoint(0, 0), composed.size()), layer.second);
        }
    }

    /// Fitted into the preview box, and never blown up: a page whose render is smaller than the box
    /// -- a narrow sheet the 200 dpi ceiling caps below it -- is saved at the pixels it really has
    /// rather than at a size invented for it.
    const QSize box = composed.size().scaled(QSize(ThumbnailPixels, ThumbnailPixels),
                                             Qt::KeepAspectRatio);
    const QImage thumbnail = (composed.width() > box.width() || composed.height() > box.height())
        ? composed.scaled(box, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        : composed;

    const QString path = QDir(m_projectDir).filePath(thumbName);
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (!thumbnail.save(path, "PNG")) {
        return;
    }

    say(QStringLiteral("thumbnail for page %1 written (%2x%3, rendered at %4 dpi%5)")
            .arg(index + 1).arg(thumbnail.width()).arg(thumbnail.height()).arg(dpi)
            .arg(ink.isEmpty() ? QString() : QStringLiteral(", ink included")));
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

    /// The open page's own scale: the manifest speaks in points and the image in pixels. The box
    /// the image measures is displaySizePt() -- the page as the reader shows it, which for a page
    /// set down at an angle is its bounding box, bigger than the source's sizePt -- or the scale
    /// would be wrong for exactly the pages this fallback exists for.
    const qreal pageWidthPt = m_manifest.pages.at(m_index).displaySizePt().width();
    const qreal pixelsPerPoint = pageWidthPt > 0 ? page.width() / pageWidthPt : 1.0;

    /// The same arrangement the strip decoration draws: the neighbours directly above and below,
    /// each at its own size, separated by the gap.
    const qreal gap = GapWidgetPixels / qMax(qreal(0.0001), zoom);

    for (int direction : { -1, 1 }) {
        const int other = m_index + direction;
        if (other < 0 || other >= m_manifest.pages.size()) {
            continue;
        }

        const QSizeF neighbour = m_manifest.pages.at(other).displaySizePt() * pixelsPerPoint;
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


QStringList PdfPageNavigator::recentNotebooks()
{
    QSettings settings;
    return settings.value(QLatin1String(RecentNotebooksKey)).toStringList();
}

void PdfPageNavigator::setRecentNotebooks(const QStringList &entries)
{
    /// Kept as a QStringList and put in through QVariant::fromValue: the Qt5 Android build has no
    /// QVariant conversion from the QList<QString> mid() hands back, and this code is built there.
    const QStringList kept = entries.mid(0, MaxRecentNotebooks);
    QSettings settings;
    settings.setValue(QLatin1String(RecentNotebooksKey), QVariant::fromValue(kept));
}

QString PdfPageNavigator::recentNotebookDir(const QString &entry)
{
    return entry.section(RecentNotebookSeparator, 0, 0);
}

QChar PdfPageNavigator::recentNotebookSeparator()
{
    return RecentNotebookSeparator;
}

void PdfPageNavigator::forgetRecentNotebook(const QString &projectDir)
{
    const QString gone = QFileInfo(projectDir).absoluteFilePath();

    QStringList kept;
    for (const QString &entry : recentNotebooks()) {
        const QString existing = recentNotebookDir(entry);
        /// An entry that names no directory is dropped as well: the menus prune those out for the
        /// same reason -- there is nothing behind them to open.
        if (existing.isEmpty() || QFileInfo(existing).absoluteFilePath() == gone) {
            continue;
        }
        kept.append(entry);
    }

    setRecentNotebooks(kept);
}

bool PdfPageNavigator::isInternalNotebookName(const QString &name)
{
    /// A LIST of the names this application really writes, not a prefix rule and not a comparison
    /// against this run's cache name: the names are ours and finite, while a name that merely starts
    /// the same way is one only a person would type.
    static const QStringList internalNames = {
        QStringLiteral("pdfio-picked"),
        QStringLiteral("pdfio-picked-pages"),
        QStringLiteral("pdfio-picked-notebook"),
        QStringLiteral("pdfio-picked-image"),
        QStringLiteral("pdfio-picked-notes"),
    };

    QString candidate = name.trimmed();

    /// The manifest holds a base name, but a build that wrote the file's own name into it would have
    /// kept the extension: either way it is the same internal name.
    const QStringList extensions = { QStringLiteral(".pdf"), QStringLiteral(".pnb"),
                                     QStringLiteral(".png") };
    for (const QString &extension : extensions) {
        if (candidate.endsWith(extension, Qt::CaseInsensitive)) {
            candidate.chop(extension.size());
            break;
        }
    }

    /// And the "(2)" a second copy gets, from our own cache or from the file manager that made it:
    /// "pdfio-picked-notes (2)" is the internal name "pdfio-picked-notes".
    static const QRegularExpression copySuffix(QStringLiteral("\\s*\\((\\d+)\\)$"));
    candidate.remove(copySuffix);

    return internalNames.contains(candidate.trimmed(), Qt::CaseInsensitive);
}

QString PdfPageNavigator::defaultNotebookName()
{
    /// Not translated, and not the desktop's locale: this is written into the manifest as the
    /// notebook's name and read back on every later run, so a name in the language of whoever
    /// imported it would be a different notebook after a locale change. The date is what tells two
    /// imports apart in the recent list, which is what a default name is for.
    return QStringLiteral("Notebook %1").arg(QDate::currentDate().toString(Qt::ISODate));
}

QString PdfPageNavigator::usableNotebookName(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty() || isInternalNotebookName(clean)) {
        return defaultNotebookName();
    }
    return clean;
}

bool PdfPageNavigator::ensureNotebookName(bool nameWhenEmpty, QString *why)
{
    if (!hasNotebook()) {
        fail(why, QStringLiteral("no notebook is open"));
        return false;
    }

    const QString path = PdfSession::manifestPath(m_projectDir);
    QString readWhy;
    PdfSessionManifest manifest = PdfSessionManifest::readFrom(path, &readWhy);
    if (!manifest.isValid()) {
        fail(why, QStringLiteral("cannot read the notebook's manifest: %1").arg(readWhy));
        return false;
    }

    const bool internal = isInternalNotebookName(manifest.name);
    if (!internal && !manifest.name.isEmpty()) {
        /// A name a person chose, or one a provider gave: never touched.
        return true;
    }
    if (!internal && !nameWhenEmpty) {
        /// Nothing to write yet: the provider's own name is being asked for, and a default written
        /// first would look like a chosen name and the provider's answer would then be refused.
        return true;
    }

    const QString before = manifest.name;
    manifest.name = defaultNotebookName();
    if (!manifest.writeTo(path, &readWhy)) {
        fail(why, QStringLiteral("cannot write the notebook's name: %1").arg(readWhy));
        return false;
    }

    /// The reader's copy as well: manifest() is what the docker and the tab read, and leaving it on
    /// the internal name would show one name in the manifest and another on the screen.
    m_manifest.name = manifest.name;

    say(internal ? QStringLiteral("the notebook was named after the picker's own copy (\"%1\"); it is "
                                  "now \"%2\"")
                       .arg(before, manifest.name)
                 : QStringLiteral("the notebook is named \"%1\"").arg(manifest.name));
    return true;
}

bool PdfPageNavigator::isInsideNotebookStore(const QString &projectDir)
{
    if (projectDir.isEmpty()) {
        return false;
    }

    /// Both sides resolved where they can be, so "…/store/../store/notes", a trailing slash and a
    /// symbolic link are all judged by where they really point: removeRecursively() would otherwise
    /// be aimed, through a link inside the store, at anything on the disk.
    const QFileInfo rootInfo(projectRoot());
    const QString root = rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath()
                                                                : rootInfo.canonicalFilePath();
    if (root.isEmpty()) {
        /// No store to be inside of: nothing may be removed.
        return false;
    }

    const QFileInfo info(projectDir);
    QString dir = info.canonicalFilePath();
    if (dir.isEmpty()) {
        /// A directory that is already gone has no canonical path; it is judged by where it was
        /// asked about, which is what forgetting its entry needs.
        dir = info.absoluteFilePath();
    }

    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return dir != root && dir.startsWith(prefix);
}

namespace {

/// Whether every DIRECTORY in the tree below \a dir can be written to, which is what removing what
/// is inside it needs.
///
/// The point is the promise the caller makes: a removal that cannot be completed must leave the
/// notebook as it was rather than half-deleted. removeRecursively() keeps going after a failure, so
/// the tree is checked first and nothing is unlinked when the answer is no. A symbolic link is
/// skipped -- it is removed as a link, never followed.
bool treeIsRemovable(const QString &dir)
{
    if (!QFileInfo(dir).isWritable()) {
        return false;
    }

    QDirIterator it(dir, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isSymLink()) {
            continue;
        }
        if (!info.isWritable()) {
            return false;
        }
    }
    return true;
}

} // namespace

bool PdfPageNavigator::removeNotebookStore(const QString &projectDir, QString *why)
{
    if (projectDir.isEmpty()) {
        fail(why, QStringLiteral("there is no notebook directory to remove"));
        return false;
    }

    /// Only inside the store. Every path the plugin removes is one the store made, and this is the
    /// rule that keeps a manifest, a picker or a hand-edited setting from aiming this at the user's
    /// own documents. Checked before anything is touched.
    if (!isInsideNotebookStore(projectDir)) {
        fail(why, QStringLiteral("%1 is not inside the notebook store (%2), so it is not ours to "
                                 "remove")
                      .arg(QFileInfo(projectDir).absoluteFilePath(),
                           QFileInfo(projectRoot()).absoluteFilePath()));
        return false;
    }

    const QString dir = QFileInfo(projectDir).absoluteFilePath();

    const QFileInfo info(dir);
    if (!info.exists()) {
        /// Already gone: the entry outlived the directory, and forgetting the entry is the work.
        forgetRecentNotebook(dir);
        return true;
    }
    if (!info.isDir() || info.isSymLink()) {
        fail(why, QStringLiteral("%1 is not a notebook directory").arg(dir));
        return false;
    }

    /// Checked before a single thing is unlinked: a tree that cannot be emptied is refused whole, so
    /// the notebook the caller has open is left exactly as it was.
    if (!treeIsRemovable(dir)) {
        fail(why, QStringLiteral("%1 cannot be emptied: a directory in it is not writable, so "
                                 "nothing was removed")
                      .arg(dir));
        return false;
    }

    if (!QDir(dir).removeRecursively()) {
        fail(why, QStringLiteral("%1 could not be removed; some of it may already be gone").arg(dir));
        return false;
    }

    forgetRecentNotebook(dir);
    return true;
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

    return adoptNotebook(projectDir, manifest, 0, base, why);
}

bool PdfPageNavigator::openNotebookDir(const QString &projectDir, QString *why)
{
    if (projectDir.isEmpty() || !QFileInfo::exists(PdfSession::manifestPath(projectDir))) {
        fail(why, QStringLiteral("%1 does not hold a notebook").arg(projectDir));
        return false;
    }

    /// Read the way every notebook is read, so a directory that cannot be opened says why rather
    /// than showing an empty notebook.
    const PdfSessionManifest manifest = PdfSession::openProject(projectDir, why);
    if (!manifest.isValid(why)) {
        return false;
    }

    say(QStringLiteral("project ready: %1 pages at %2").arg(manifest.pages.size()).arg(projectDir));
    /// The notebook's own name, the same one the docker, the tab and Recent show.
    return adoptNotebook(projectDir, manifest, 0, manifest.displayName(), why);
}

bool PdfPageNavigator::adoptNotebook(const QString &projectDir, const PdfSessionManifest &manifest,
                                     int anchorPage, const QString &label, QString *why)
{
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

    /// AND NOTHING MAY STILL BE LANDING when the manifest below changes.
    ///
    /// A write computes the file it writes from m_projectDir + m_manifest at the moment its own
    /// turn comes, not at the moment it was asked for. One requested for the OLD notebook and
    /// started after the swap below would write the OLD document's pixels into the NEW project --
    /// and its own page number, index and rectangle are the new notebook's, so the artifact that
    /// comes back is the previous notebook's ink on the new notebook's page. Waiting for the queue
    /// to empty before the swap is what makes every write belong to exactly one notebook.
    {
        QString drainWhy;
        if (!drainWrites(&drainWhy)) {
            /// Nothing to refuse: the queue is wedged and the swap cannot make it worse. The write
            /// that never lands is named, and the checks below still hold.
            say(QStringLiteral("adopting %1 with a write still in the air: %2").arg(projectDir, drainWhy));
        }
    }

    /// Everything that describes the notebook being replaced, kept so a new notebook that cannot be
    /// BUILT can be put back. The invariant is the one the report broke: the navigator may never
    /// describe one notebook while showing another -- the tab, the docker and the ops screen all
    /// read the manifest, and the ink restore and every save resolve their files against the project
    /// directory, so a swap that is not followed by a document is a notebook's pixels served under
    /// another notebook's geometry.
    const QString previousDir = m_projectDir;
    const PdfSessionManifest previousManifest = m_manifest;
    const PdfPageWindow previousWindow = m_window;
    const QList<int> previousStripPages = m_stripPages;
    const QList<QRect> previousStripRects = m_stripRects;
    const QList<QRect> previousStripCells = m_stripCells;
    const QList<KisNodeSP> previousStripPaper = m_stripPaper;
    const int previousActiveSlot = m_stripActiveSlot;
    const int previousWindowSlot = m_windowSlot;
    const QHash<int, qint64> previousSaveStamps = m_saveStamps;
    const QList<int> previousThumbnailQueue = m_thumbnailQueue;
    const qint64 previousInkChange = m_lastInkChange;
    const qint64 previousAutoSave = m_lastAutoSave;
    const bool hadOpenDocument = m_document && m_document->image();

    /// The new notebook starts with an empty window, and its counters describe one notebook rather
    /// than the whole process.
    m_window.clear();

    /// And the strip of the notebook being replaced goes with it -- its slots, cells and paper
    /// layers, and the slot bookkeeping. Without this, showPage() below finds the page it is asked
    /// for already inside the OLD strip and only unlocks that slot: the tab changes to the new
    /// notebook while the previous notebook's pages stay on the canvas, which is what switching
    /// with Recent notebooks looked like. The plugin closes the old view first, but a path that
    /// forgets to cannot be allowed to leave the old strip behind.
    m_stripPages.clear();
    m_stripRects.clear();
    m_stripCells.clear();
    m_stripPaper.clear();
    m_stripActiveSlot = -1;
    m_windowSlot = -1;

    /// AND THE RENDERERS ARE THE NOTEBOOK'S OWN, so they must not outlive it.
    ///
    /// PdfSourceRenderers caches one open backend per RELATIVE source file name the manifest
    /// records, and two notebooks can hold different PDFs with the same name: every Android import
    /// is a copy called "pdfio-picked.pdf" in its own project directory. Left open, the second
    /// notebook's pages were rendered through the FIRST notebook's still-open file -- the new
    /// manifest's page count, sizes and rotation, which is the new "proportion", with the old
    /// notebook's pixels drawn into them. That is the report this path answers. closeNotebook() has
    /// always cleared them; a notebook that REPLACES another has to as well, so the shared relative
    /// name becomes a cache miss and the right file is opened from the new project directory.
    m_sourceRenderers.clear();

    /// And the queued thumbnails and write stamps, which name pages of the notebook being left: a
    /// preview of a page that now means something else, and stamps that belong to the old ink.
    /// finishReload() clears the same two for the same reason.
    m_thumbnailQueue.clear();
    m_saveStamps.clear();

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

    /// A notebook that carries the picker's own cache name is repaired here, on every open, before
    /// anything reads it: the manifest is where the tab, the Start screen, the recent list and the
    /// export suggestion all get their name from. A notebook with NO name is left alone -- on Android
    /// the provider's own name is asked for after this -- so this only ever replaces a name the
    /// application itself made up for a cache copy.
    ensureNotebookName(/*nameWhenEmpty=*/false, nullptr);

    /// And the same for a page that records no preview: the name is the durable half, and without
    /// one a page can never be given a picture again -- see nameMissingPreviews(). Before showPage()
    /// below, because that is where the first previews are asked for.
    nameMissingPreviews();

    const int anchor = qBound(0, anchorPage, manifest.pages.size() - 1);
    const bool shown = showPage(anchor, why);

    /// THE SWAP IS NOT COMPLETE UNTIL THE DOCUMENT IS. showPage() below can refuse -- a page turn
    /// already in progress, a source that cannot be rendered, a build that comes back empty -- and
    /// when it does, the manifest and the project directory have already been replaced. That is the
    /// mixed state: the imported notebook's name, page count and geometry in every reader (the tab,
    /// the docker, the ops screen) and every path resolution, with the previous notebook's document
    /// still on the canvas. Its ink is then saved through the NEW manifest's file names, and the
    /// next build of the new notebook restores that ink onto the new pages -- the reported "old
    /// notebook's ink and its Inserted image drawn over the imported pages".
    ///
    /// So a new notebook that cannot be built is put back. The document was never touched --
    /// showPage refuses before showImage, or not at all -- and the notebook that was in force is
    /// described again. The refusal is loud and the caller's open fails, which is the honest answer.
    ///
    /// The window and the strip bookkeeping go back only when their DOCUMENT is still there: a
    /// caller that closed the old view first (the plugin's Import, "Open a notebook") leaves nothing
    /// for them to describe, and restoring page rectangles for a document that is gone would be the
    /// same class of lie this is fixing, the other way round.
    if (!shown) {
        m_projectDir = previousDir;
        m_manifest = previousManifest;
        if (hadOpenDocument) {
            m_window = previousWindow;
            m_stripPages = previousStripPages;
            m_stripRects = previousStripRects;
            m_stripCells = previousStripCells;
            m_stripPaper = previousStripPaper;
            m_stripActiveSlot = previousActiveSlot;
            m_windowSlot = previousWindowSlot;
            m_saveStamps = previousSaveStamps;
            m_thumbnailQueue = previousThumbnailQueue;
            m_lastInkChange = previousInkChange;
            m_lastAutoSave = previousAutoSave;

            /// pageChanged tells every reader which notebook is in force again -- the tab, the docker
            /// and the recently-added list all get it from the navigator.
            Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
        }

        /// The renderers were cleared for the notebook that did NOT open. The one that is in force
        /// gets its own back on demand -- same project directory, same relative names -- so nothing
        /// needs restoring here; the cache is a cache.
        say(QStringLiteral("the notebook at %1 was not adopted (%2); %3 is the notebook in force")
                .arg(projectDir,
                     why && !why->isEmpty() ? *why : QStringLiteral("no reason recorded"),
                     m_manifest.displayName()));
        return false;
    }

    Q_EMIT pageChanged(m_index, pageCount(), label);

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
    /// The active page's own source, opened once. This used to open "the source" and render pages
    /// by their notebook position; both are wrong the moment a notebook holds pages from more than
    /// one PDF, or in an order other than its source's.
    if (!m_sourceRenderers.forPage(m_manifest, m_projectDir, index, why)) {
        return false;
    }

    say(QStringLiteral("building a strip of %1 around page %2").arg(m_scope).arg(index + 1));

    /// PdfStripBuilder::build() reads every slot's saved ink back out of its artifact, so the
    /// queue is drained first: any one of those pages may be the one a write is still carrying,
    /// and a read before it lands shows the page as it was before the ink that is on its way.
    drainWrites(nullptr);

    /// The dpi the page-size bound implies for THIS window, derived once and handed to the builder:
    /// the layout the strip is built at and the layout a later roll of it builds have to be the same
    /// resolution, or a roll would render the pages at a scale the layout does not describe.
    /// The dpi the budget implies for THIS window, derived once and handed to the builder: the
    /// layout the strip is built at and the layout a later roll of it builds have to be the same
    /// resolution, or a roll would render the pages at a scale the layout does not describe. The
    /// count is the shape this builder is about to make -- one band per slot plus Ink -- because the
    /// content layers of the pages' artifacts have not been read back yet; a roll then counts the
    /// layers that really exist.
    const qreal dpi = dpiForBudget(m_memoryBudgetMb, index, 0);
    m_renderedDpi = dpi;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(m_manifest, index, m_scope, dpi,
                                                                m_sourceRenderers, m_projectDir, why);
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

void PdfPageNavigator::setDocumentGoneAfterWritesForTests(bool gone)
{
    documentGoneAfterWritesForTests() = gone;
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
    /// One derivation for the whole roll: the window it lays out and the pages it renders below are
    /// the same resolution, so a window cannot be built at one and repainted at another.
    ///
    /// The layer count is the window ARRIVING's, which is not always the one leaving's: a scope change
    /// makes the new window more pages (or fewer), and a budget divided across the OLD count would
    /// let a bigger window cost more than the budget while making a smaller one needlessly coarse.
    /// The content layers the strip already carries are the part that does not scale with the slot
    /// count, and the bands the arriving window needs is the shape the layout is about to make. With
    /// the scope unchanged this is exactly stripLayerCount(), which is what the roll used before.
    const int contentLayers = qMax(0, int(stripLayerCount() - m_stripPaper.size()));
    const int plannedSlots =
        int(PdfStripLayout::forWindow(m_manifest, centreOn, m_scope, DefaultRenderDpi).slots().size());
    const qreal dpi =
        dpiForBudget(m_memoryBudgetMb, centreOn, qMax(1, plannedSlots + contentLayers));
    m_renderedDpi = dpi;
    const PdfStripLayout target = PdfStripLayout::forWindow(m_manifest, centreOn, m_scope, dpi);
    if (!target.isValid()) {
        fail(why, QStringLiteral("the new window has no valid layout"));
        return false;
    }

    /// The size the window arriving needs, and the size the document has now. They differ whenever
    /// the pages on screen change size -- a notebook of mixed sizes, or a page the notebook has
    /// turned by a free angle -- and the document is RESIZED to the target below rather than the
    /// roll being refused. The refusal used to hand the move to showPage(), which then built the
    /// whole document and view again: that is the cost a window move must not pay, and it is what
    /// the layout's constant size existed to avoid. The layout is now the size of the window that
    /// is up (see PdfStripLayout::forWindow), so the two are allowed to differ here.
    ///
    /// A notebook whose pages are all one size never differs, which is the common case and the one
    /// that must stay exactly as fast as it is: nothing below runs for it.
    const QSize targetSize = target.imageSize();
    const QSize sizeNow(m_document->image()->width(), m_document->image()->height());

    /// A window with a different number of slots -- a scope change -- is deliberately NOT refused
    /// here any more. The bands are re-cut inside this roll, after the writes and before the resize:
    /// see the band surgery below. It used to refuse, and the caller then built the whole document
    /// and view again, which is not what a different page count should cost.


    /// Every source the new window draws from, opened before anything is done: a roll that cannot
    /// render its pages has to refuse before it writes and wipes the strip, not after.
    for (const PdfStripLayout::Slot &slot : target.slots()) {
        if (slot.page < 0) {
            continue;
        }
        if (!m_sourceRenderers.forPage(m_manifest, m_projectDir, slot.page, why)) {
            return false;
        }
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

    /// The document this roll is for, and the window it is writing, both taken BEFORE the nested
    /// loops below.
    ///
    /// A reload queued behind a notebook change runs inside them, and it replaces the document and
    /// clears the window: iterating m_stripPages while something inside a save clears it would be
    /// a range-for over a list that changed under it, and the roll would go on to repaint the new
    /// document with the old window's rectangles. The copy makes the loop write the window this
    /// roll was started for, and the guard after drainWrites() refuses if the document is not that
    /// one any more.
    KisDocument *const documentThisRollIsFor = m_document;
    const QList<int> pagesToWrite = m_stripPages;

    const auto stripIsClean = [this, &pagesToWrite]() {
        for (int page : m_window.dirtyPages()) {
            if (pagesToWrite.contains(page)) {
                return false;
            }
        }
        return true;
    };

    int saved = 0;
    constexpr int MaxSavePasses = 3;
    for (int pass = 1;; ++pass) {
        for (int page : pagesToWrite) {
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

    /// The document is asked about again, because everything from here down reads it.
    ///
    /// The write phase above runs nested event loops -- one inside every page write, and one inside
    /// drainWrites() -- and anything the application queued runs inside them. A reload queued
    /// behind a notebook change is exactly that, and it takes this document away: m_document is a
    /// QPointer and goes NULL the moment its document is destroyed, and a reload REPLACES it with
    /// another, which is just as bad, because the rest of this function would repaint the new
    /// document with the old window's rectangles and then write this notebook's slot bookkeeping
    /// over it. Either way the roll is not what should happen now: m_savingPages is given back,
    /// the reason is said, and the window is left where it is. A refusal is an answer the callers
    /// already know: the queued roll logs it and the follow carries on, and showPage() falls
    /// through to building the strip.
    ///
    /// The test-only flag at the head reaches the same branch without the timing: it is the seam
    /// that proves this refusal, and nothing in the plugin sets it.
    if (documentGoneAfterWritesForTests() || m_document.data() != documentThisRollIsFor
        || !m_document || !m_document->image()) {
        m_savingPages = wasSaving;
        fail(why, QStringLiteral("the document went away while the window was being written; "
                                 "the strip is unchanged"));
        say(QStringLiteral("strip: roll refused: the document is not the one this roll started for"));
        return false;
    }

    /// The band surgery: the window's own shape, cut to the slots this roll is laying out.
    ///
    /// A different page count is a different WINDOW, not a different notebook, so the paper layers
    /// are re-cut here rather than the document being rebuilt. The surplus bands go, and the missing
    /// ones are inserted directly ABOVE the topmost paper band -- which is where the Ink group
    /// begins. A band above the ink hides every stroke on the page it covers, which is why the
    /// insertion point is the one place it can be.
    ///
    /// AFTER the write phase and BEFORE the resize and the repaint, in that order and for two
    /// reasons: a roll that has to refuse -- because a page could not be written, or the document
    /// went away -- must leave the strip exactly as it was, and the writes are the only thing here
    /// that can refuse; and the new bands are allocated at the size the document still has, then
    /// grown or cropped by the resize below along with every other layer, so they arrive at the
    /// size the repaint works in.
    ///
    /// The repaint below gives each band its page and its name, so an inserted band is a page
    /// background from the moment it exists: named like the other bands, so a slot the repaint
    /// finds no page for is still recognized as a background rather than as content.
    const int arrivingSlots = slots.size();
    if (m_stripPaper.size() != arrivingSlots) {
        KisImageSP image = m_document->image();

        int removed = 0;
        int added = 0;
        if (m_stripPaper.size() > arrivingSlots) {
            /// The bands that go are the topmost ones. Which band holds which slot does not matter
            /// -- every band is wiped and repainted from its slot's artifact below -- but taking
            /// them off the top keeps the list and the stack in the same order.
            while (m_stripPaper.size() > arrivingSlots) {
                KisNodeSP surplus = m_stripPaper.takeLast();
                if (surplus && image->removeNode(surplus)) {
                    ++removed;
                }
            }
        } else {
            QList<KisNodeSP> inserted;
            while (m_stripPaper.size() < arrivingSlots) {
                const int slot = int(m_stripPaper.size());
                const int page = slot < slots.size() ? slots.at(slot).page : -1;
                KisPaintLayerSP band = new KisPaintLayer(image,
                                                         PdfStripBuilder::backgroundLayerName(page),
                                                         OPACITY_OPAQUE_U8);
                band->setUserLocked(true);
                /// Directly above the topmost band that is already there: the Ink group is above
                /// every one of them, so the new band lands below it.
                const KisNodeSP above = m_stripPaper.isEmpty() ? KisNodeSP() : m_stripPaper.last();
                if (!image->addNode(band, image->root(), above)) {
                    /// Nothing is left half re-cut: the bands this loop did insert go again, and
                    /// the roll refuses with the window it started with.
                    for (const KisNodeSP &rollback : inserted) {
                        image->removeNode(rollback);
                    }
                    m_savingPages = wasSaving;
                    fail(why, QStringLiteral("the strip could not take the band for slot %1")
                                  .arg(slot + 1));
                    say(QStringLiteral("strip: roll refused: a band could not be added to the strip"));
                    return false;
                }
                inserted.append(band);
                m_stripPaper.append(band);
                ++added;
            }
        }

        say(QStringLiteral("strip: the window was re-cut to %1 slot(s): %2 band(s) removed, %3 added")
                .arg(arrivingSlots)
                .arg(removed)
                .arg(added));

        /// Adding or removing a node marks the image changed, and the strip is a view of the
        /// notebook rather than a document to write. The resize below clears the mark again where
        /// Krita's own operation makes it, and the end of the roll clears whatever the repaint and
        /// the layer adoption leave behind -- this one is here so the surgery on its own can never
        /// be what leaves the tab saying the strip is modified.
        clearModifiedFlag();
    }

    /// The document is made the size of the window that is arriving, when it is not already.
    ///
    /// Shrinking is cropImage() and not resizeImage() on purpose: cropImage is the one that DROPS
    /// the pixels outside the new rectangle, and those pixels are the memory the user watched
    /// climb. resizeImage() keeps every pixel it had, which is right for a pure growth and wrong
    /// here. Nothing is lost that the repaint below does not put back out of the artifacts: after a
    /// roll, nothing on screen comes from before it.
    if (targetSize != sizeNow) {
        say(QStringLiteral("strip: the window is %1x%2, not %3x%4; resizing the document")
                .arg(targetSize.width()).arg(targetSize.height())
                .arg(sizeNow.width()).arg(sizeNow.height()));

        KisImageSP image = m_document->image();

        /// The resize's own undo command STAYS, and that is a recorded limitation rather than an
        /// oversight. KisDocument::setCurrentImage installs the document's KisDocumentUndoStore on
        /// this image, so cropImage()/resizeImage() push a command, and there are two costs: a
        /// Ctrl+Z after a window move takes the resize back instead of the reader's stroke (it
        /// corrects itself on the next roll), and the crop command RETAINS the pixels it removed so
        /// they can be restored -- the memory the crop was meant to free.
        ///
        /// Detaching the store around the resize was TRIED AND CANNOT BE DONE: KisImage::setUndoStore()
        /// does m_d->undoStore.reset(store) (kis_image.cc:1862) on a QScopedPointer (:265), so
        /// detaching with nullptr DELETES the document's own store and any pointer kept to restore it
        /// dangles -- the crash was a KisImage constructor lambda purging the freed store. Dropping
        /// just that command needs a store of our own that filters it, which is a new class whose one
        /// rule must be that it never loses a stroke; not worth building for a Ctrl+Z that only
        /// misbehaves when the user's last action was a window move.
        if (targetSize.width() < sizeNow.width() || targetSize.height() < sizeNow.height()) {
            image->cropImage(QRect(QPoint(0, 0), targetSize));
        } else {
            image->resizeImage(QRect(QPoint(0, 0), targetSize));
        }

        /// Both are asynchronous -- the operation runs on the image's own scheduler -- and the
        /// repaint below must not write into a layer that is still being resized, so the wait is
        /// part of the operation. It waits inside an event loop, and anything the application
        /// queued can run there: the document is asked about again afterwards, exactly as it is
        /// above.
        image->waitForDone();
        if (documentGoneAfterWritesForTests() || m_document.data() != documentThisRollIsFor
            || !m_document || !m_document->image()) {
            m_savingPages = wasSaving;
            fail(why, QStringLiteral("the document went away while the strip was being resized; "
                                     "the strip is unchanged"));
            say(QStringLiteral("strip: roll refused: the document went away during the resize"));
            return false;
        }

        /// And the zoom, which the resize has just invalidated.
        ///
        /// The fit is placed ONCE per document (showImage), which was right while the image could
        /// not change size: a turn inside a strip keeps the document, and re-fitting moves the centre
        /// with the zoom. A resize changes the canvas underneath that zoom, and measured on the
        /// tablet it left the strip a dot -- 0.3-0.7% in a 773x653 viewport, where fitting the
        /// active page into a 700x11672 window wants about 5.5%. The centre is deliberately NOT set
        /// here: the anchor logic below puts the reader back on the page they were reading, and
        /// preferredCenterFor() converts with whatever zoom is set by then.
        if (m_view && m_view->canvasBase() && m_view->canvasController()) {
            QWidget *widget = m_view->canvasBase()->canvasWidget();
            const QRect pageRect = target.slots().at(target.activeSlot()).rect;
            const qreal zoom = fitZoomFor(widget ? widget->size() : QSize(), pageRect,
                                        readingShare());
            if (zoom > 0.0) {
                say(QStringLiteral("zoom: the window is %1x%2 now, so a %3x%4 page fits at %5")
                        .arg(targetSize.width()).arg(targetSize.height())
                        .arg(pageRect.width()).arg(pageRect.height()).arg(zoom));
                m_view->canvasController()->setZoom(KoZoomMode::ZOOM_CONSTANT, zoom);
                m_viewSettleUntil = QDateTime::currentMSecsSinceEpoch() + 800;
            }
        }

        /// The strip is a VIEW of the notebook, not a document to write.
        ///
        /// Cleared here, where the resize made the mark; see clearModifiedFlag() for why, and for the
        /// other two places this roll clears it.
        clearModifiedFlag();

        /// Nothing else has to be repainted for the new size: there is no desk layer any more
        /// (PdfStripBuilder makes each slot's band carry the colour), and the repaint below fills
        /// every slot's paper over its cell -- and the cells tile the image the window arrives with,
        /// so every row of the new size is covered by a band.
    }

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

        /// The DIRECTORY and the full path are said, not only the page number: a restore that reads
        /// another notebook's artifact is the report this line has to be able to answer, and
        /// "page 2" alone cannot say which notebook's page 2 it was.
        const QString kraPath =
            QDir(m_projectDir).filePath(m_manifest.pages.at(slot.page).kraFile);
        say(QStringLiteral("strip: reading the layers of page %1 from %2")
                .arg(slot.page + 1).arg(kraPath));
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
        ///
        /// The cells of the window arriving tile the whole image, so a content layer is wiped over
        /// every row of it whatever band each slot holds -- there is no old band for a content
        /// layer to leave behind. The paper layer below is the one that needs one, and for exactly
        /// that reason: it is wiped band by band, not as one layer.
        for (KisPaintLayer *layer : content) {
            layer->paintDevice()->fill(slots.at(i).cell,
                                       KoColor(Qt::transparent, m_document->image()->colorSpace()));
        }

        /// const_cast because KisSharedPtr::data() hands back a const node, and the paper layer is
        /// this code's to repaint.
        KisPaintLayer *paper = qobject_cast<KisPaintLayer *>(
            const_cast<KisNode *>(m_stripPaper.at(i).data()));

        /// The paper is wiped over the band it owned BEFORE the window moved as well as over the one
        /// it owns now.
        ///
        /// Wiping the new band only is the stale strip the tablet reported -- "part of THAT one is
        /// intruding into the current one". A paper holds one slot's band, and moving the window
        /// back to the one holding a freely rotated page pushes every band below it down: the page's
        /// displayed box is much taller than any other page's, so the cell of every slot after the
        /// first starts lower than it did in the window that did not hold it. The rows at the top of
        /// each paper's previous band are then OUTSIDE the band it owns now -- in the cell of the
        /// slot below, whose paper layer sits underneath -- so they are drawn over the page that is
        /// supposed to be there. Measured on the reported notebook (18 letter pages with page 1
        /// turned 221 degrees, a five slot window): 576 rows on every slot but the first.
        ///
        /// The old band is read here, before m_stripCells is replaced at the end of the roll.
        const QRect wasHeld = i < m_stripCells.size() ? m_stripCells.at(i) : QRect();
        const QRect paperWipe = (wasHeld.isValid() && !wasHeld.isEmpty())
            ? wasHeld.united(slots.at(i).cell)
            : slots.at(i).cell;
        if (paper) {
            /// Cleared over the band the paper owned BEFORE the window moved -- the task-9 fix, or
            /// those pixels stay on screen -- and then filled with the desk colour over the cell it
            /// owns now: the room around the page is the colour the user chose over Krita's
            /// transparency checkerboard. The layer is still gone; the colour lives in each band.
            paper->paintDevice()->fill(paperWipe,
                                       KoColor(Qt::transparent, m_document->image()->colorSpace()));
            /// The desk colour for the room around the page, then the page itself.
            paper->paintDevice()->fill(slots.at(i).cell,
                                       KoColor(QColor(96, 96, 96), m_document->image()->colorSpace()));
        }

        /// Krita is told the slot changed. Writing into a paint device directly does not do that,
        /// and without it the canvas goes on showing what was there before: the window rolled, the
        /// slots held the right pages, and the screen did not. The band cleared above counts as
        /// changed as much as the one repainted: rows that no longer carry paint have to be redrawn
        /// too, or the stale ones stay on the canvas exactly as they were in the image.
        /// The rectangles handed to Krita are in IMAGE coordinates and are clamped to the image:
        /// slot 0's band starts half a gap above it (cell y is -56) so that the bands meet in the
        /// middle of the gap, and a dirty rectangle with a negative origin is not something the
        /// canvas, the thumbnail cache or the Layers docker's node model is entitled to be given.
        /// The paint-device wipe and the desk fill above are deliberately NOT clamped -- the cell
        /// has to be covered exactly as it is laid out -- and only the notification is trimmed.
        const QRect imageRect(0, 0, m_document->image()->width(), m_document->image()->height());
        if (paper) {
            paper->setDirty(paperWipe.intersected(imageRect));
        }
        for (KisPaintLayer *layer : content) {
            layer->setDirty(slots.at(i).cell.intersected(imageRect));
        }

        if (newPage < 0) {
            continue;
        }

        const QImage rendered =
            m_sourceRenderers.renderPage(m_manifest, m_projectDir, newPage, dpi, nullptr);
        if (paper && !rendered.isNull()) {
            paper->paintDevice()->convertFromQImage(rendered, nullptr,
                                                    slots.at(i).rect.x(), slots.at(i).rect.y());
            paper->setName(PdfStripBuilder::backgroundLayerName(
                m_manifest.pages.at(newPage).index));
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

            /// The artifact is page-local at whatever resolution it was WRITTEN at, and this
            /// repaint is at the resolution derived above -- which the page-size bound can have
            /// changed since the ink was written. So
            /// it is scaled to the page's own rectangle, which is what keeps the ink on the page it
            /// was drawn on. A difference of two pixels or less is pasted as it is: that is the two
            /// roundings of one box disagreeing, and scaling for it would only blur the ink.
            const bool sameSize = qAbs(entry.second.width() - slots.at(i).rect.width()) <= 2
                && qAbs(entry.second.height() - slots.at(i).rect.height()) <= 2;
            const QImage placed = sameSize
                ? entry.second
                : entry.second.scaled(slots.at(i).rect.size(), Qt::IgnoreAspectRatio,
                                      Qt::SmoothTransformation);
            target->paintDevice()->convertFromQImage(placed, nullptr,
                                                    slots.at(i).rect.x(), slots.at(i).rect.y());
            target->setDirty(slots.at(i).cell);
        }
        if (paper) {
            paper->setDirty(slots.at(i).cell);
        }
    }

    /// The clear is said at its original place as well, before the adoption below.
    ///
    /// It used to be the ONLY one, and moving it after the adoption is what the ink-loss sweep
    /// chased: keeping both positions means the roll behaves exactly as it did when it was green
    /// while the mark the adoption raises is still cleared afterwards. Setting a bool twice costs
    /// nothing; a roll that loses a page's ink costs everything.
    clearModifiedFlag();

    /// And the placement rule the notebook asked for: a content layer that ended up outside the
    /// Ink group -- one the user made, or one restored from an artifact under a name the strip did
    /// not have, which the block above adds at the root -- is moved INTO it, keeping its name and
    /// its pixels. Never deleted, never flattened, and the number still outside is logged so the
    /// rule is measurable.
    adoptContentIntoInk(m_document->image());

    /// And the strip is not a document to write, said once more after the repaint -- and AFTER the
    /// layer adoption just above, because moving a node into the Ink group is itself an image change
    /// and marks the document modified. Clearing before it is how a roll that adopted a content layer
    /// came back from the menu with an asterisk on the tab. See clearModifiedFlag().
    clearModifiedFlag();

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

int PdfPageNavigator::stripPageCount() const
{
    return m_stripPageCount;
}

bool PdfPageNavigator::stripIsOpen() const
{
    /// A document AND a window of slots. The slot list alone is not enough: it outlives the view
    /// that was showing it (a tab closed by hand, or a rebuild between its two halves), and a stale
    /// list read as "a strip is open" is how a count change would decide nothing needed rebuilding.
    return m_document && !m_stripPages.isEmpty();
}

void PdfPageNavigator::setScope(int scope)
{
    /// The count and the on/off state are TWO settings, and this is where they are kept apart:
    /// turning the strip off (1) writes only the on/off key, so the count is still there when the
    /// switch turns it back on. See StripPagesKey / StripOnKey.
    const bool on = scope > 1;
    QSettings settings;
    settings.setValue(QLatin1String(StripOnKey), on);

    int pages = qMax(1, scope);
    if (on) {
        /// The strip is symmetric -- a page above and a page below the active one -- so an even
        /// count is laid out as the next odd one, and what is stored is what the layout really
        /// makes rather than a number nothing would ever show.
        if (pages % 2 == 0) {
            ++pages;
        }
        m_stripPageCount = pages;
        settings.setValue(QLatin1String(StripPagesKey), pages);
    }

    /// The count is a SETTING, not a property of whichever notebook happens to be open when it is
    /// asked for.
    ///
    /// It is asked for BEFORE the notebook it is meant for as often as after -- the plugin and the
    /// tests both set the scope and then open a notebook -- and clamping it here to the notebook that
    /// happens to be open at that moment is how a leftover three page notebook rewrote a request for
    /// five into three (the sweep's two failures: a five page window built three pages tall). What
    /// bounds a count is the notebook the WINDOW is built in, and PdfStripLayout::forWindow clamps it
    /// there, every time, so the number the user chose survives and the slots on screen are the
    /// notebook's answer.
    const bool changed = (on ? pages : 1) != m_scope;
    m_scope = on ? pages : 1;

    /// The window bound follows the scope: design A is one page, design B is a strip of that many.
    /// Pages already open are not thrown out here; the extra slots are given back on the next page
    /// turn, and a smaller window is nobody's problem.
    m_window.setCapacity(m_scope);

    /// Nothing to apply and nothing to say when the window that is up is already that one: a
    /// switch turned on where the strip already was is not a window move.
    if (!changed) {
        return;
    }

    say(QStringLiteral("the strip's page count is now %1 (the strip is %2)")
            .arg(on ? QString::number(m_scope) : QString::number(m_stripPageCount))
            .arg(on ? QStringLiteral("on") : QStringLiteral("off")));

    /// OFF: one page at a time is a different DOCUMENT rather than a different window -- no bands,
    /// no slots -- so it is not this setter's to apply. The switch that asked for it rebuilds, and
    /// nothing here may take a strip down on its own.
    if (!on) {
        return;
    }
    if (!m_document || !hasNotebook() || m_index < 0 || m_stripPages.isEmpty()) {
        /// A single page has no window to re-cut, no document has nothing to re-cut, and no
        /// notebook has nothing to apply it to: the next build takes the count (the plugin's own
        /// scope path).
        return;
    }

    /// A count the notebook has no room for lays out the window that is already up. The setting
    /// changes and the screen does not: without this, picking a number larger than the notebook from
    /// the menu would still cost a whole window write, a render and a repaint to arrive at the same
    /// pixels. The layout is asked at the resolution the strip is already at, which is the one the
    /// roll would derive again from the same slot count.
    const PdfStripLayout arriving =
        PdfStripLayout::forWindow(m_manifest, m_index, m_scope, currentRenderDpi());
    const QSize sizeNow(m_document->image()->width(), m_document->image()->height());
    if (arriving.isValid() && arriving.slots().size() == m_stripPaper.size()
        && arriving.imageSize() == sizeNow) {
        return;
    }

    /// The count changed while a strip is up, so it is applied through the roll's own resize path:
    /// the bands are re-cut and the document resized, rather than the document and view rebuilt.
    QString why;
    if (!rollToPage(m_index, &why, m_index, true)) {
        say(QStringLiteral("the strip's page count was saved but the strip was not re-cut: %1")
                .arg(why));
    } else {
        /// The roll clears the mark where Krita's resize made it and at its own end; this clears
        /// anything raised after it returned, through the same helper, so this path cannot drift
        /// from the roll's own.
        clearModifiedFlag();
    }
}

int PdfPageNavigator::memoryBudgetMb() const
{
    return m_memoryBudgetMb;
}

qreal PdfPageNavigator::currentRenderDpi() const
{
    /// What the document that is up was built at, stored when it was built or rolled. The default
    /// before anything has been built is the reference dpi, which is what an empty navigator means.
    return m_renderedDpi > 0.0 ? m_renderedDpi : DefaultRenderDpi;
}

int PdfPageNavigator::longestPagePixelsForBudget(int megabytes) const
{
    const int activePage = m_index >= 0 ? m_index : 0;
    const qreal longestPt = longestSidePtInWindow(activePage, m_scope);
    if (longestPt <= 0.0) {
        /// No notebook, or a window with no pages: there is no page size to name.
        return 0;
    }

    /// The same derivation a roll would use, with the same layer count, without applying anything:
    /// the number the menu shows beside a budget has to be the number picking it would produce.
    const qreal dpi = dpiForBudget(megabytes, activePage, stripLayerCount());
    return qMax(0, qRound(longestPt * dpi / 72.0));
}

int PdfPageNavigator::minMemoryBudgetMb()
{
    return MinMemoryBudgetMb;
}

int PdfPageNavigator::deviceRamMb()
{
    /// Krita's own answer, which knows the device rather than the container it runs in on the
    /// desktop; 0 when it cannot say, and the guard then falls back to a modest machine.
    return qMax(0, KisImageConfig::totalRAM());
}

int PdfPageNavigator::maxMemoryBudgetMb()
{
    /// The device's own guard, not a constant: half of the physical memory (see
    /// MemoryBudgetRamFraction for why a half), or half of 1 GB when the machine will not say -- the
    /// conservative answer, which refuses rather than promises memory that is not there.
    const int ramMb = deviceRamMb() > 0 ? deviceRamMb() : 1024;
    return qMax(MinMemoryBudgetMb, int(qreal(ramMb) * MemoryBudgetRamFraction));
}

int PdfPageNavigator::windowCostMbForBudget(int megabytes) const
{
    const int activePage = m_index >= 0 ? m_index : 0;

    /// Held to the same range the setter holds a typed value to -- the floor and this device's own
    /// guard -- or the figure shown for 5 MB would be one the menu can never produce.
    const int budget =
        megabytes <= 0 ? 0 : qBound(MinMemoryBudgetMb, megabytes, maxMemoryBudgetMb());

    /// The same derivation the strip itself uses, without applying it, and the same layer count the
    /// roll would use: one derivation for the pixels and one for the figure, never two.
    const qreal dpi = dpiForBudget(budget, activePage, stripLayerCount());
    const PdfStripLayout layout = PdfStripLayout::forWindow(m_manifest, activePage, m_scope, dpi);
    const QSize size = layout.imageSize();
    if (size.isEmpty()) {
        return 0;
    }

    const qreal bytes =
        qreal(size.width()) * qreal(size.height()) * qreal(layersForBudget(layout)) * 4.0;

    /// Rounded UP: a figure that says "about 200 MB" must never be under the number the user typed
    /// for the case where the budget binds.
    return int(qCeil(bytes / 1000000.0));
}

int PdfPageNavigator::longestPagePixelsForScope(int scope) const
{
    const int activePage = m_index >= 0 ? m_index : 0;
    const qreal longestPt = longestSidePtInWindow(activePage, scope);
    if (longestPt <= 0.0) {
        /// No notebook, or a scope the notebook has no room for: there is no page size to name.
        return 0;
    }

    /// The same derivation a build of that window would use, without applying anything: the number
    /// the menu shows beside a choice has to be the number picking it would produce. The layer count
    /// is the shape the builder would make -- one band per slot plus Ink -- because the bands of a
    /// window that does not exist yet cannot be counted.
    const int slots = PdfStripLayout::forWindow(m_manifest, activePage, scope, 1.0).slots().size();
    if (slots <= 0) {
        return 0;
    }
    const qreal dpi = dpiForBudgetInScope(m_memoryBudgetMb, activePage, scope, qMax(1, slots + 1));
    return qMax(0, qRound(longestPt * dpi / 72.0));
}

int PdfPageNavigator::windowCostMbForScope(int scope) const
{
    const int activePage = m_index >= 0 ? m_index : 0;
    const PdfStripLayout reference = PdfStripLayout::forWindow(m_manifest, activePage, scope, 1.0);
    if (!reference.isValid()) {
        return 0;
    }

    /// The shape the builder would make for that window: one band per slot plus Ink. The bands of a
    /// window that does not exist yet cannot be counted, and a content layer an artifact would
    /// restore is not known either.
    const int layers = qMax(1, reference.slots().size() + 1);
    const qreal dpi = dpiForBudgetInScope(m_memoryBudgetMb, activePage, scope, layers);
    const QSize size = PdfStripLayout::forWindow(m_manifest, activePage, scope, dpi).imageSize();
    if (size.isEmpty()) {
        return 0;
    }

    /// Rounded UP: a figure that says "about 300 MB" must never be under the number the window would
    /// really hold.
    return int(qCeil(qreal(size.width()) * qreal(size.height()) * qreal(layers) * 4.0 / 1000000.0));
}

int PdfPageNavigator::layersForBudget(const PdfStripLayout &window) const
{
    /// The document's own count when one is open -- exact for the strip that is up -- and the shape
    /// the builder is about to make otherwise: one band per slot plus the Ink layer.
    const int counted = stripLayerCount();
    return counted > 0 ? counted : qMax(1, window.slots().size() + 1);
}

void PdfPageNavigator::clearModifiedFlag()
{
    if (m_document) {
        m_document->setModified(false);
    }
}

void PdfPageNavigator::setMemoryBudgetMb(int megabytes)
{
    /// 0 is "no limit" and is left alone. A typed value is held between the floor and this DEVICE's
    /// own guard -- not an artificial ceiling: the number is accepted up to what the machine can hold,
    /// and what it was held at is said with the RAM that decided it.
    const int guard = maxMemoryBudgetMb();
    int budget = megabytes <= 0 ? 0 : qMax(MinMemoryBudgetMb, megabytes);
    if (budget > guard) {
        say(QStringLiteral("%1 MB is more than this device can hold: %2 MB of RAM, half of it for the "
                           "strip, so the budget is %3 MB")
                .arg(megabytes)
                .arg(deviceRamMb())
                .arg(guard));
        budget = guard;
    }

    /// Written whether or not the value changed: the menu offers choices, and the one that is
    /// already in force in memory may not be the one on disk when another build wrote the file.
    QSettings settings;
    settings.setValue(QLatin1String(MemoryBudgetMbKey), budget);

    if (budget == m_memoryBudgetMb) {
        return;
    }
    m_memoryBudgetMb = budget;

    say(QStringLiteral("the rendered window's memory budget is now %1")
            .arg(budget > 0 ? QStringLiteral("%1 MB").arg(budget)
                            : QStringLiteral("unlimited (the fixed 200 dpi)")));

    /// The user chose it, so the screen has to show it, and the roll's resize path is the ONE
    /// mechanism that can change the resolution of a strip: it writes every page the window holds,
    /// resizes the document to the window at the new resolution and repaints every slot from its
    /// artifact. Nothing here is a second way of changing a strip.
    ///
    /// A single page (design A) is not a strip: there is no window to resize, and the page takes the
    /// new budget through buildForSinglePage() the next time it is built.
    if (!hasNotebook() || m_index < 0 || m_stripPages.isEmpty()) {
        return;
    }

    /// The window does not move; only the pixels it is drawn at change, so the reading page is kept
    /// where it is.
    QString why;
    if (!rollToPage(m_index, &why, m_index, true)) {
        say(QStringLiteral("the memory budget was saved but the strip was not rebuilt: %1").arg(why));
    } else {
        /// And the menu's own path ends here. The roll cleared the mark where Krita's resize made it
        /// and at its own end; this clears anything raised after the roll returned -- through the ONE
        /// helper, so this path cannot drift from the roll's. Only when the roll succeeded: a refused
        /// roll wrote nothing, and a document that still holds unsaved ink has to keep saying so.
        clearModifiedFlag();
    }
}

qreal PdfPageNavigator::dpiForBudget(int megabytes, int activePage, int layers) const
{
    /// The window that is up, or the one a roll is about to lay out (m_scope is set before the roll
    /// runs). The menu's own "what would N pages cost" goes through dpiForBudgetInScope() directly,
    /// because that window is not the one in force.
    return dpiForBudgetInScope(megabytes, activePage, m_scope, layers);
}

qreal PdfPageNavigator::dpiForBudgetInScope(int megabytes, int activePage, int scope,
                                            int layers) const
{
    if (megabytes <= 0) {
        /// No budget: exactly what this rendered at before the setting existed, to the dpi.
        return DefaultRenderDpi;
    }

    /// The window at the reference dpi. The layout is where the window's geometry lives -- how many
    /// pages it holds, how big they are, how much the gaps cost -- so the pixels it needs are read
    /// off one layout instead of being guessed from page sizes here.
    const PdfStripLayout reference =
        PdfStripLayout::forWindow(m_manifest, activePage, scope, DefaultRenderDpi);
    const QSize referenceSize = reference.imageSize();
    if (referenceSize.isEmpty()) {
        /// Nothing to lay out: no notebook, or a window with no pages in it.
        return DefaultRenderDpi;
    }

    /// The layers: the caller's own count when it has one, or the shape the builder is about to
    /// make -- one band per slot plus the Ink layer. A content layer an artifact carries makes the
    /// strip one bigger, which the roll that follows counts for real.
    /// THE FIX for the budget test that built 515.6 MB for a 200 MB budget: this used to fall back
    /// to layersForBudget(), which counts the document that is OPEN -- and when a strip is being
    /// BUILT the open document is the notebook being replaced, a different window with a different
    /// number of layers (a single page has two). The derivation then divided the budget by two
    /// instead of six and produced a window two and a half times over it. The shape the builder is
    /// about to make is the only honest count for a window that does not exist yet; a roll, where the
    /// open document IS the strip, passes its real count in.
    const int counted = layers > 0 ? layers : qMax(1, reference.slots().size() + 1);
    const qreal budgetBytes = qreal(megabytes) * 1000.0 * 1000.0;
    const qreal targetArea = budgetBytes / (4.0 * qreal(counted));
    const qreal referenceArea = qreal(referenceSize.width()) * qreal(referenceSize.height());
    if (targetArea <= 0.0 || referenceArea <= 0.0) {
        return DefaultRenderDpi;
    }

    if (targetArea >= referenceArea) {
        /// The budget already pays for the window at the reference resolution: the answer is the
        /// ceiling, and it is EXACTLY 200 dpi rather than a hair under it, so a budget that costs
        /// nothing in quality is today's behaviour to the dpi.
        return DefaultRenderDpi;
    }

    /// The square root is the estimate: at the reference dpi the window needs referenceArea pixels,
    /// the budget buys targetArea of them, and a layout's area goes as the square of its dpi.
    ///
    /// It is only an estimate, because the gaps between the pages do NOT scale with the dpi: a window
    /// solved this way spends the whole budget on the pages and then pays for the gaps on top, about
    /// 3% over for a five page window. So the dpi is then walked down against the layout itself --
    /// the same forWindow(), no second arithmetic -- until the area it really has fits, which is what
    /// makes a budget a bound rather than a hope. A few layout passes, not a render.
    qreal low = 0.0;
    qreal high = qMin(DefaultRenderDpi * qSqrt(targetArea / referenceArea), DefaultRenderDpi);
    for (int i = 0; i < 24; ++i) {
        const qreal middle = (low + high) / 2.0;
        const QSize probeSize =
            PdfStripLayout::forWindow(m_manifest, activePage, scope, middle).imageSize();
        const qreal probeArea = qreal(probeSize.width()) * qreal(probeSize.height());
        if (probeArea > targetArea) {
            high = middle;
        } else {
            low = middle;
        }
    }

    /// A dpi that lays a page out no pixels wide is not a lower quality, it is a broken layout: the
    /// geometry refuses at zero and the page would not open at all. One pixel for the longest page is
    /// the floor that needs, and it is not a quality clamp -- the menu offers nothing near it.
    const qreal smallest = 72.0 / qMax(qreal(1.0), longestSidePtInWindow(activePage, scope));
    return qMax(smallest, low);
}

int PdfPageNavigator::stripLayerCount() const
{
    /// What is open, when something is: every paint layer of the document is a full-size allocation,
    /// so counting them is the real answer -- the per-slot bands, the ink layer the pages are drawn
    /// on, and any content layer a page's artifact restored.
    if (!m_document || !m_document->image()) {
        /// Nothing open: the caller builds from the planned shape instead.
        return 0;
    }

    int layers = 0;
    countPaintLayers(m_document->image()->root(), &layers);
    return layers;
}

qreal PdfPageNavigator::longestSidePtInWindow(int activePage, int scope) const
{
    /// The layout is asked which pages the window holds, rather than that range being worked out a
    /// second time here: forWindow() already clamps the scope to the notebook and centres the window
    /// as far as the ends allow, and a second copy of that arithmetic would be a second answer. Any
    /// positive dpi does for the question; the pixels are not read.
    const PdfStripLayout window = PdfStripLayout::forWindow(m_manifest, activePage, scope, 1.0);

    qreal longest = 0.0;
    for (const PdfStripLayout::Slot &slot : window.slots()) {
        if (slot.page < 0 || slot.page >= m_manifest.pages.size()) {
            continue;
        }
        /// displaySizePt(): the page as the window will really hold it, which for a page the
        /// notebook turned is the rectangle that holds the turned sheet.
        const QSizeF size = m_manifest.pages.at(slot.page).displaySizePt();
        longest = qMax(longest, qMax(size.width(), size.height()));
    }
    return longest;
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
    /// The page's own source: the record says which PDF this page's background comes from, and
    /// the render is of the record's own page inside that file.
    PdfRenderBackend *backend = m_sourceRenderers.forPage(m_manifest, m_projectDir, index, why);
    if (!backend) {
        return false;
    }

    /// Step by step on purpose. Opening a document on Android crashed inside Qt without saying
    /// where, and these lines are what turned "somewhere after the copy" into a stage.
    say(QStringLiteral("rendering page %1").arg(index + 1));

    /// The same budget the strip answers to, for the one page this mode opens: a page document is
    /// its paper and the ink drawn on it, which is the count the planned window (one slot plus Ink)
    /// gives.
    const qreal dpi = dpiForBudget(m_memoryBudgetMb, index, 0);
    m_renderedDpi = dpi;
    KisImageSP image = PdfProjectBuilder::buildPageImage(m_manifest.pages.at(index), *backend, dpi, why);
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
    ///
    /// The notebook's own name, not the file it was copied from: the tab has to agree with the
    /// docker's title, the Recent entry and the export suggestion.
    const QString notebook = m_manifest.displayName();
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

            /// Sized so the active page takes the reading share of the viewport (three fifths by
            /// default): see fitZoomFor().
            const qreal zoom = fitZoomFor(viewport, pageRect, readingShare());

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

bool PdfPageNavigator::saveStripPages(QString *why)
{
    if (m_stripPages.isEmpty()) {
        /// A document holding one page has nothing to crop; the usual save is the whole of it --
        /// and through the queue, so this save cannot start while an idle write is still in the
        /// air (two in the air is the wedge, and this action is exactly the one a user reaches
        /// for while the idle write may be running).
        QString saveWhy;
        if (!saveThroughQueue(m_index, &saveWhy)) {
            say(QStringLiteral("could not save the page: %1").arg(saveWhy));
            if (why) {
                *why = QStringLiteral("page %1: %2").arg(m_index + 1).arg(saveWhy);
            }
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
        QString saveWhy;
        if (!savePageAndWait(page, &saveWhy)) {
            say(QStringLiteral("could not save page %1 (%2)").arg(page + 1).arg(saveWhy));
            if (why) {
                *why = QStringLiteral("page %1: %2").arg(page + 1).arg(saveWhy);
            }
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
    clearModifiedFlag();
    say(QStringLiteral("closing: page %1 of %2 written through the notebook, nothing for Krita to ask about")
            .arg(m_index + 1).arg(m_manifest.pages.size()));
    return true;
}

bool PdfPageNavigator::closeNotebook(QString *why)
{
    if (!hasNotebook()) {
        /// Nothing open: there is nothing to close and nothing to forget.
        return true;
    }

    /// The ink first, through the gate that exists for it. A notebook whose pages cannot be written
    /// is left open -- closing it would drop ink that never reached disk, and Krita's own prompt is
    /// what should ask about it.
    if (!prepareForClose()) {
        fail(why, QStringLiteral("the open pages could not be written, so the notebook was left open"));
        return false;
    }

    /// A reload waiting for the event loop would put the notebook straight back; finishReload()
    /// stands down on this flag.
    m_reloadPending = false;
    m_reloadManifest = PdfSessionManifest();
    m_reloadAnchor = 0;

    /// The view takes the document with it, and the document is the largest thing this class holds:
    /// the strip's image and every layer of it. This is the point of the whole close.
    closeCurrentPage();

    /// Everything that describes the notebook goes, in the same shape adoptNotebook() clears it --
    /// the window, the strip, the slot bookkeeping, the write stamps and the thumbnails queued for
    /// pages that are no longer there.
    m_window.clear();
    m_stripPages.clear();
    m_stripRects.clear();
    m_stripCells.clear();
    m_stripPaper.clear();
    m_stripActiveSlot = -1;
    m_windowSlot = -1;
    m_saveStamps.clear();
    m_thumbnailQueue.clear();
    m_zoomPlacedFor = nullptr;

    /// The document is gone, so the image whose ink changes were being watched is gone with it.
    if (m_inkChangeConnection) {
        disconnect(m_inkChangeConnection);
        m_inkChangeConnection = QMetaObject::Connection();
    }

    /// The renderers hold the notebook's source PDFs open. A store that is about to be removed must
    /// not be held open by a cache.
    m_sourceRenderers.clear();

    /// The clocks and the two timers: neither the follow nor the idle write may chase a notebook
    /// that is gone.
    m_lastInkChange = 0;
    m_lastAutoSave = 0;
    m_lastWriteError.clear();
    if (m_scrollWatch) {
        m_scrollWatch->stop();
    }
    if (m_autoSaveTimer) {
        m_autoSaveTimer->stop();
    }

    m_index = -1;
    m_projectDir.clear();
    m_manifest = PdfSessionManifest();

    say(QStringLiteral("the notebook is closed; nothing is open now"));
    Q_EMIT pageChanged(m_index, pageCount(), QString());
    return true;
}

bool PdfPageNavigator::prepareForNotebookChange(QString *why)
{
    if (!hasNotebook()) {
        fail(why, QStringLiteral("no notebook is open"));
        return false;
    }

    /// A reload on its way in is BENIGN: it is this same notebook re-read, and it lands in a
    /// moment. Wait for it rather than refuse the work that is asking.
    if (m_reloadPending) {
        waitForReloadToLand();
    }
    if (m_reloadPending) {
        fail(why, QStringLiteral("the notebook is being reloaded; try again in a moment"));
        return false;
    }

    /// The ink first, and waited for. Everything an operation does is decided by the manifest, and
    /// a page written after the manifest changed would be written against a page list that no
    /// longer describes the document in front of it.
    ///
    /// The reason comes back with the page that failed: a refusal that says only "the notebook could
    /// not be written" cannot be acted on by the user and cannot be diagnosed from the log.
    QString saveWhy;
    if (!saveStripPages(&saveWhy)) {
        fail(why, saveWhy.isEmpty()
                      ? QStringLiteral("the notebook could not be written, so nothing was changed")
                      : QStringLiteral("the notebook could not be written: %1").arg(saveWhy));
        return false;
    }

    /// A write already in the air is waited for, not refused: drainWrites() is the wait.
    QString drainWhy;
    if (!drainWrites(&drainWhy)) {
        fail(why, QStringLiteral("a page write was still in flight: %1").arg(drainWhy));
        return false;
    }

    /// Only now, and only because every page above reached its artifact: leaving the document
    /// modified would have Krita ask whether to save it while the notebook is being closed under
    /// it -- and the answer would be "save the old page list".
    clearModifiedFlag();
    return true;
}

bool PdfPageNavigator::reloadPending() const
{
    return m_reloadPending;
}

bool PdfPageNavigator::reloadNotebook(int anchorPage, QString *why)
{
    if (!hasNotebook()) {
        fail(why, QStringLiteral("no notebook is open"));
        return false;
    }
    if (m_reloadPending) {
        fail(why, QStringLiteral("the notebook is already being reloaded"));
        return false;
    }
    if (m_inPageTurn) {
        fail(why, QStringLiteral("a page turn is in progress"));
        return false;
    }

    /// Deliberately no write here. The page list has just changed, so the open document describes
    /// pages by numbers that now mean something else: writing it now would put one page's ink into
    /// another page's artifact. Ink that is still unsaved is therefore a refusal, not a save --
    /// prepareForNotebookChange() is what the caller runs before it changes the notebook.
    if (m_document && m_document->isModified()) {
        fail(why, QStringLiteral("the open page still carries ink that is not on disk; write the "
                                 "notebook before changing it"));
        return false;
    }

    /// Whatever an earlier write already started still gets to land.
    QString drainWhy;
    drainWrites(&drainWhy);

    /// Read now, so a notebook that cannot be read back is the caller's answer instead of a reload
    /// that half happened on the event loop.
    PdfSessionManifest manifest = PdfSession::openProject(m_projectDir, why);
    if (!manifest.isValid(why)) {
        return false;
    }

    m_reloadManifest = manifest;
    m_reloadAnchor = qBound(0, anchorPage, manifest.pages.size() - 1);
    m_reloadPending = true;

    say(QStringLiteral("reloading the notebook: %1 page(s), opening page %2")
            .arg(manifest.pages.size()).arg(m_reloadAnchor + 1));

    /// The old view has to be gone before the new document is built; Krita closes it on the event
    /// loop, which is the same reason every notebook open defers. finishReload() does the rest.
    closeCurrentPage();
    QTimer::singleShot(ReloadSettleMs, this, &PdfPageNavigator::finishReload);
    return true;
}

void PdfPageNavigator::waitForReloadToLand()
{
    /// Bounded at two of the reload's own timers: a reload that has not landed by then is not on its
    /// way in any more, and the caller refuses with a reason rather than spinning for ever.
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 2 * ReloadSettleMs;
    while (m_reloadPending && QDateTime::currentMSecsSinceEpoch() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void PdfPageNavigator::finishReload()
{
    if (!m_reloadPending) {
        return;
    }
    m_reloadPending = false;

    /// Everything that describes the OLD page list goes. A page number is a position, and the
    /// positions just changed: the window, the strip and its cells and paper layers, the slot
    /// bookkeeping, the write stamps and the thumbnails queued for pages that may now be other
    /// pages. Keeping any of it is how a strip shows the notebook as it was before the change.
    m_window.clear();
    m_stripPages.clear();
    m_stripRects.clear();
    m_stripCells.clear();
    m_stripPaper.clear();
    m_stripActiveSlot = -1;
    m_windowSlot = -1;
    m_saveStamps.clear();
    m_thumbnailQueue.clear();

    m_manifest = m_reloadManifest;

    /// The clocks start over for the same reason openNotebook() resets them: a mark left by a page
    /// that is no longer in this notebook would have the idle write chase a page that is not there.
    m_lastInkChange = 0;
    m_lastAutoSave = QDateTime::currentMSecsSinceEpoch();

    QString why;
    const bool shown = showPage(m_reloadAnchor, &why);
    if (!shown) {
        say(QStringLiteral("the notebook was reloaded but page %1 did not open: %2")
                .arg(m_reloadAnchor + 1).arg(why));
    }

    Q_EMIT pageChanged(m_index, pageCount(), QFileInfo(m_manifest.sourceFile).completeBaseName());
    Q_EMIT reloadFinished(m_index, shown);
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

QRect PdfPageNavigator::pageAreaFor(int index) const
{
    const int slot = m_stripPages.indexOf(index);
    if (slot < 0 || slot >= m_stripRects.size()) {
        return QRect();
    }
    return m_stripRects.at(slot);
}

KisDocument *PdfPageNavigator::pageLayersDocument(KisDocument *page, int index, QString *why)
{
    if (!page || !page->image()) {
        fail(why, QStringLiteral("the page has no image to write"));
        return nullptr;
    }

    /// The window's geometry describes this window's document and nothing else. A page document the
    /// navigator does not have open -- another notebook's page, shown in another view -- has no
    /// rectangle here, and is written whole rather than cropped by a stranger's numbers.
    const QRect area = page == m_document.data() ? pageAreaFor(index) : QRect();

    /// A document holding one page has no rectangle and needs no cropping.
    return area.isValid()
        ? PdfPageSaver::createPageLayersDocument(page->image(), area, why)
        : PdfPageSaver::createPageLayersDocument(page->image(), why);
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
    /// needs no cropping. Worked out in one place, so every writer of a page crops by the same one.
    const QRect pageArea = pageAreaFor(page);

    /// A STRIP whose page rectangles are gone is not a document to write.
    ///
    /// Without a rectangle this writes the WHOLE image into one page's artifact -- every page, with
    /// every page's ink, under one page's file name. That is the shape a notebook that was half
    /// adopted leaves behind (the manifest swapped, the strip bookkeeping cleared, the old document
    /// still up), and on the device it was a fifteen second export that timed out and then refused
    /// every action behind the write gate. A single-page document is the other case of an invalid
    /// rectangle, and it is fine: it has exactly one page background and the whole image IS the page.
    if (!pageArea.isValid() && m_document && m_document->image()) {
        int bands = 0;
        for (quint32 i = 0; i < m_document->image()->root()->childCount(); ++i) {
            if (PdfPageSaver::isPageBackground(m_document->image()->root()->at(i))) {
                ++bands;
            }
        }
        if (bands > 1) {
            fail(why, QStringLiteral("the strip's page rectangles are gone, so page %1 cannot be "
                                     "cropped out of it")
                          .arg(page + 1));
            return false;
        }
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
        /// produced exactly this shape (an A4 page comes out 814x1152 from the preview box above).
        const QImage thumb =
            projection->createThumbnailUncached(scaled.width(), scaled.height(), thumbArea);
        if (!thumb.isNull()) {
            QDir().mkpath(QFileInfo(thumbPath).absolutePath());
            thumb.save(thumbPath, "PNG");
        }
    }

    /// The copy is made while the page is still alive, and it owns its own pixels, so the editing
    /// document can be closed immediately afterwards. The same door "Insert image..." writes
    /// through, so the two crop by one rectangle.
    KisDocument *pageDocument = pageLayersDocument(m_document, page, why);
    if (!pageDocument) {
        return false;
    }

    /// The name the manifest records, not one rebuilt from the page's number. The two agree in a
    /// notebook that was just created and stop agreeing the moment an operation moves a page's ink
    /// from one artifact to another -- and a save that lands where no reader looks is ink that is
    /// silently gone. Every reader in the plugin has always used kraFile; the writer is the last
    /// one to be brought in line.
    const QString path = QDir(m_projectDir).filePath(m_manifest.pages.at(page).kraFile);
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
