/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfNotebookOpsDialog.h"
#include "PdfPageNavigator.h"

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfInkLoader.h"
#include "session/PdfNotebookOps.h"
#include "session/PdfPageRotator.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"
#include "session/PdfSourceRenderers.h"
#include "session/PdfStripBuilder.h"

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceCacheDb.h>
#include <KisResourceLocator.h>
#include <KisView.h>
#include <kis_canvas_controller.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include "tiles3/kis_tile_data_store.h"

#include <KoColor.h>

#include <KoTestConfig.h>

#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QWidget>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtMath>
#include <QtTest>

/// For the parked exit: fflush() before _exit(), because _exit() runs no static destructors and
/// flushes nothing, and ctest reads QTest's summary from those streams.
#include <cstdio>
#include <limits>
#include <unistd.h>

#if defined(__linux__)
/// For malloc_trim(): the last step of the native-heap measurement, which asks the allocator to hand
/// back what Krita has already freed. Linux-only, like /proc/self/status.
#include <malloc.h>
#endif

/**
 * The page turn, over a real notebook, with a real document behind every page.
 *
 * PdfPageWindowTest proves the eviction rule on its own -- a dirty page is never dropped -- and
 * PdfProjectBuilderTest proves the layer contract, but neither ever turns a page. Everything the
 * rule protects against is a property of the turn as a whole: the document's own modified flag is
 * what the window is told, the save that has to happen before an eviction is the plugin's
 * ink-only save to a real artifact, and "the page turn was refused" only means something if the
 * page that was open is still the page that is open, still holding its ink.
 *
 * So this test does what the plugin does: it wraps a fixture PDF into a notebook the way
 * PdfIoPlugin does, lets PdfPageNavigator build its real KisDocument through KisPart, draws on
 * the page's own ink layer, and then turns pages with next()/previous()/showPage(). Each test
 * asserts the visible outcome -- the open page, KisDocument::isModified(), the artifact on disk
 * -- and the window's counters, which is the part that explains it.
 *
 * What it is not: a brush stroke. The mark is placed straight into the Ink layer's paint device
 * and the modified flag is set through KisDocument::setModified(), which is the flag showPage()
 * reads. The painting pipeline is covered by Krita's own tests; what is under test here is what
 * happens to a page that carries ink when it has to make room.
 */
class PdfNavigatorIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testCleanTurnEvictsWithoutSaving();
    void testDirtyPageIsSavedBeforeEvictionAndTheInkComesBack();
    void testAPageIsWrittenToTheArtifactTheManifestNames();
    void testRefusedSaveKeepsThePageOpenAndTheInkIntact();
    void testInkIsNotWrittenWhileThePenIsStillBusy();
    void testALayerThatIsNotInkIsSavedToo();
    void testQuittingWritesTheInkToo();
    void testClosingTheTabWritesTheInkAndAsksNothing();
    void testAnInsertedImageStaysItsOwnLayerAcrossAReopen();
    void testInsertingOnAStripWritesThePageAndNotTheStrip();
    void testAMovedPageKeepsTheReaderAndTheirInk();
    void testADeletedPageLeavesTheReaderOnTheNextOne();
    void testRotatingAPageTurnsTheInkWithThePaper();
    void testAnExtractedRangeOpensAsItsOwnNotebook();
    void testMergingANotebookInAddsItsPagesAndKeepsTheReader();
    void testTheScreenKeepsItsChangeUntilApply();

    /// The page list's own gesture: a swipe turns the row it was made on, by the same quarter turn
    /// the buttons use, and a movement that is not a swipe turns nothing.
    void testSwipingAPageTurnsIt();
    /// The canvas: the selected page turned by hand, by an angle no button can name, and the turn
    /// landing in the pending list like every other edit.
    void testTheCanvasTurnsTheSelectedPageByAnyAngle();
    /// The REAL rotator at an angle that is not a right angle, which no other test runs.
    void testTheRealRotatorTurnsAPageByAnAngleThatIsNotARightAngle();
    /// The page list's other gesture: press and hold a row, drag it to another place, drop it. The
    /// new order is ONE pending edit, and the swipe that shares the same press still turns a page.
    void testDraggingARowReordersItInOnePendingEdit();
    /// A page marked for deletion cannot be grabbed: it is on its way out.
    void testARowMarkedForDeletionCannotBeDragged();
    /// A drop where the row already is, and a drop outside the table, both leave the list alone.
    void testADropOnTheSamePositionIsANoOp();
    /// The preview is prepared for the screen rather than for the file: the pixels a card and the
    /// canvas are drawn from are the size they are drawn at, tagged with the screen's ratio.
    void testAPreviewIsPreparedForTheScreensDeviceRatio();
    /// The canvas asks the navigator for a fresh preview rather than stretching a small one.
    void testTheCanvasAsksTheNavigatorForAFreshPreview();
    /// Deleting a notebook: the open one is closed for real, its folder goes, and the recent list
    /// forgets it. A real fixture notebook, not a hand-made directory.
    void testDeletingANotebookClosesItAndRemovesItsFolder();
    /// Deleting refuses what is not the store's, and a refusal leaves the open notebook usable.
    void testDeletingRefusesWhatIsNotTheStores();
    /// The rendered window's memory budget: what the strip may cost, the resolution that buys, and
    /// that "no limit" is exactly the size and the dpi the strip always had. A mixed-orientation
    /// notebook, because that is the case a pixel target got wrong.
    /// The generated preview itself: enough pixels for the screen that draws it, and the page's ink
    /// in it rather than blank paper.
    void testAGeneratedPreviewIsSavedBigEnoughForTheScreen();
    void testAGeneratedPreviewCarriesTheInk();
    /// A window that holds a freely rotated page and one that does not, rolled between and back:
    /// the image is the same size by design, and part of the strip that was there used to stay.
    void testRollingBackToARotatedPageLeavesNoStripBehind();
    /// A roll whose document goes away between its writes and its redraw refuses cleanly, which is
    /// the state a reload landing inside the write phase's event loops reaches.
    void testARollRefusesWhenItsDocumentGoesAway();
    /// The other half of the resize: a notebook of one page size must never resize anything.
    void testARollBetweenSameSizeWindowsDoesNotResize();

    /// How many pages the strip holds is the user's to choose: the slots that count lays out, the
    /// default, the persisted count, a count that is not one of the presets, and that turning the
    /// strip off does not forget it.
    void testTheStripPageCountChoosesTheWindowSize();
    /// A count change while a strip is open lands the document at the new size through the ROLL --
    /// the same document, the same image, the bands re-cut -- rather than a rebuild.
    void testAStripPageCountChangeResizesTheSameDocument();
    /// The count is bounded by the notebook rather than by a constant of this build's.
    void testAStripPageCountIsBoundedByTheNotebook();
    /// A bigger strip at a fixed budget stays inside it (the pages get coarser); with no limit it
    /// simply costs more. The figures the report is written from are printed here.
    void testABiggerStripStaysInsideTheMemoryBudget();

    /// Resizing: the pane's three modes stay apart, a scale typed or dragged is one pending edit,
    /// Scale and Box reset independently, and a scaled page renders at a larger SOURCE dpi rather
    /// than being upscaled.
    void testTheOpsPaneKeepsTurnScaleAndBoxApart();
    void testAScaledPageIsRenderedAtALargerDpi();

    /// The page-loading trigger: the settle delay and the reading share, persisted, defaulted to
    /// today's behaviour, clamped at their ends, and -- for the share -- changing which page a scroll
    /// leaves under the centre of the viewport.
    void testThePageLoadingTriggerIsPersistedAndClamped();

    /// Importing a PDF while a notebook is open: the new notebook replaces the old one in every
    /// place that names it, and its pages are rendered from the NEW notebook's own PDF -- which the
    /// source-renderer cache, keyed on the relative file name, used to hand back as the old one's.
    void testImportingWhileANotebookIsOpenReplacesIt();


    /// Last on purpose: it swaps the fixture and the scope, restores both, and runs after every
    /// test that would care -- so a failure inside it cascades to nothing that runs after it.
    void testRollWritesEveryWindowPageAndRedrawsFromDisk();

    /// The memory and naming tests run HERE, after every test that measures the strip, and that is
    /// deliberate rather than tidy: they open heavy notebooks, walk the budget up and down, close
    /// documents and trim the allocator. Declared before the roll tests, as they were in the sweep
    /// that lost ink on rolls which had been green: the order was the one variable nobody was
    /// looking at, and a test that changes another test's outcome is a test that is wrong.
    /// A TYPED budget, not one of the presets: persisted, held between the floor and this device's
    /// guard, derived by the same derivation the presets use, and applied through the same resize
    /// path.
    void testATypedBudgetIsHeldAndDerived();
    /// A measurement, not an assertion: what holds the native heap after a budget change, and which
    /// of the two candidates does not give it back when the document closes.
    void testWhatHoldsTheNativeHeap();
    /// A notebook is never named after the picker's cache copy: the rule in one place, the reader
    /// half, the repair on open, and the human default for an import the provider could not name.
    void testANotebookIsNeverNamedAfterTheCacheCopy();
    /// The rendered window's memory budget, last: it changes the budget into and out of force and
    /// leaves a heavy notebook open.
    void testTheMemoryBudgetBoundsTheStrip();

private:
    PdfPageNavigator *navigator() const;

    /// Opens a notebook from its own copy of the fixture. The project directory is named after
    /// the source file, so a differently named copy is a different notebook: each test starts
    /// with an empty page store rather than whatever the test before it wrote.
    /// Opens a notebook and pins the scope it is read at. The application ships with the strip on
    /// (five pages) and most of these cases are about the single page document -- design A -- so
    /// the scope is asked for here instead of inherited from a default that is something else now.
    /// The strip case asks for five.
    bool useNotebook(const QString &name, int scope = 1);

    /// Paints a black square on the open page's ink layer and marks the document modified.
    void drawInk(KisDocument *document);

    QString artifactFor(int index) const;
    bool waitForInk(const QString &path, int timeoutMs = 30000);

    QTemporaryDir m_dir;
    QString m_fixture;
    KisMainWindow *m_mainWindow = nullptr;
    QTimer *m_dialogWatchdog = nullptr;

    /// How many times Krita asked "this document has been modified, do you want to save it?".
    /// Closing our tab must never add to it: the ink is written by the notebook before the
    /// question can be asked.
    int m_savePrompts = 0;
};

namespace {

/// A square of ink in the page's own pixels, small enough to be cheap to save and specific
/// enough that finding it back is not a coincidence.
const QRect InkMark(8, 8, 24, 24);

/// The root the plugin itself would use, asked of the plugin rather than spelled out again: the
/// notebook folder moved to Documents on the desktop, and a copy of the old rule here would have
/// gone on writing to the old place.
QString projectsRoot()
{
    return PdfSession::projectRoot();
}

KisPaintLayer *inkLayer(const KisImageSP &image)
{
    return qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data());
}

/// The STRIP's ink layer, found the way the plugin itself finds it: by name.
///
/// PdfProjectBuilder::inkStrokeLayer() answers for a page document -- a root whose second layer
/// is the Ink group -- and a strip image is one paper layer per slot, each carrying the desk colour
/// over its own band, and an Ink group above all of them with the stroke layer inside it. Asking
/// the page-shaped helper about a strip image hands back nothing, which is what this test hit on
/// its first run.
KisPaintLayer *stripInkLayer(const KisImageSP &image)
{
    if (!image) {
        return nullptr;
    }
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        if (child->name() != QStringLiteral("Ink")) {
            continue;
        }
        /// The group holds the stroke now; a bare paint layer of that name is the older shape.
        if (KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(child.data())) {
            return layer;
        }
        for (quint32 c = 0; c < child->childCount(); ++c) {
            if (KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(child->at(c).data())) {
                return layer;
            }
        }
    }
    return nullptr;
}

/**
 * Whether the mark is on the page, and whether the rest of the page is still blank.
 *
 * The second half matters: an artifact read back as an opaque page would satisfy a check that
 * only looked for dark pixels, and the test would then pass without any ink having survived.
 */
bool inkMarkInImage(const QImage &pixels)
{
    if (pixels.isNull() || !pixels.rect().contains(InkMark.center())) {
        return false;
    }

    const QColor mark = pixels.pixelColor(InkMark.center());
    const QColor away = pixels.pixelColor(2, 2);
    const bool marked = mark.alpha() > 0 && qGray(mark.rgb()) < 96;
    const bool blankElsewhere = away.alpha() == 0 || qGray(away.rgb()) > 200;
    return marked && blankElsewhere;
}

bool inkMarkPresent(const KisImageSP &image)
{
    KisPaintLayer *layer = inkLayer(image);
    if (!layer || !image) {
        return false;
    }

    return inkMarkInImage(layer->paintDevice()->convertToQImage(0, image->bounds()));
}

/**
 * The bounding box of the marked pixels of a layer, which is where the ink is. Empty when there is
 * none -- and an empty box would make every position comparison below vacuous, so callers check it.
 */
QRect markedBounds(const QImage &pixels)
{
    int x0 = pixels.width();
    int y0 = pixels.height();
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < pixels.height(); ++y) {
        for (int x = 0; x < pixels.width(); ++x) {
            const QRgb pixel = pixels.pixel(x, y);
            if (qAlpha(pixel) > 128 && qGray(pixel) < 128) {
                x0 = qMin(x0, x);
                y0 = qMin(y0, y);
                x1 = qMax(x1, x);
                y1 = qMax(y1, y);
            }
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

/// The layer called \a name out of what an artifact holds, or a null image.
QImage layerNamed(const QList<QPair<QString, QImage>> &layers, const QString &name)
{
    for (const QPair<QString, QImage> &entry : layers) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return QImage();
}

/// Every .kra sitting directly in the user's home directory.
///
/// That is where Krita puts the autosave of a document with no path on this platform, and it is
/// the one place a save we did not ask for would land without a dialog having to choose it.
QStringList homeKraFiles()
{
    const QDir home(QDir::homePath());
    QStringList files;
    const QStringList names = home.entryList(QStringList() << QStringLiteral("*.kra"), QDir::Files);
    for (const QString &name : names) {
        files << home.absoluteFilePath(name);
    }
    files.sort();
    return files;
}

} // namespace

PdfPageNavigator *PdfNavigatorIntegrationTest::navigator() const
{
    return PdfPageNavigator::instance();
}

void PdfNavigatorIntegrationTest::initTestCase()
{
    /// The main window is built from Krita's own resources -- the XMLGUI file that gives it a
    /// toolbar, and the configuration it starts from. They are not in kritaui, so the test has to
    /// pull them in itself, exactly as Krita's ui tests do (libs/ui/tests/kis_view_signals_test.cpp).
    Q_INIT_RESOURCE(krita);

    QVERIFY(m_dir.isValid());

    /// The strip's page count, whether it is on, and the page-loading trigger are PERSISTED settings,
    /// and the navigator is a process-wide singleton constructed the first time anything asks for it.
    /// This is the one place the DEFAULTS can be observed: with the keys absent, the notebook opens at
    /// five pages with the strip on, turns 450 ms after the view settles, and fits a page to three
    /// fifths (60%) of the viewport -- exactly as it always did. Cleared before the first ask -- a
    /// previous run of the suite leaves them behind -- rather than asserted in a test that runs later,
    /// by which time the singleton has already read them. The count and the on/off state are two keys
    /// on purpose; the tests below are what proves turning the strip off leaves the count alone.
    {
        QSettings settings;
        settings.remove(QStringLiteral("pdfio/stripPages"));
        settings.remove(QStringLiteral("pdfio/stripOn"));
        settings.remove(QStringLiteral("pdfio/scrollSettleMs"));
        settings.remove(QStringLiteral("pdfio/readingSharePercent"));
        settings.sync();
    }

    m_fixture = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));

    /// Test mode keeps the notebook store under ~/.qttest instead of the user's data directory,
    /// and the store is emptied first: a project directory is derived from its source, so an
    /// artifact left by an earlier run would otherwise be read back as this run's ink.
    QVERIFY(QStandardPaths::isTestModeEnabled());

    /// The notebook folder now defaults to Documents, which is the user's own directory. Point the
    /// Documents half of the policy at this run's temporary directory so the test creates and
    /// removes notebooks inside it and nowhere else. The root policy itself is PdfSessionTest's to
    /// prove; this test only needs a place to work.
    PdfSession::setDocumentsLocationForTests(m_dir.path());
    QVERIFY(projectsRoot().startsWith(m_dir.path()));

    /// Krita's own ui tests put the resource system up before they touch a document
    /// (sdk/tests/kistest.h, the TESTUI branch of registerResources); a document created without
    /// it would have a resource locator with nowhere to look. The database lives in the
    /// test-mode application data directory, so the run throws it away with the rest.
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(appData));
    if (!KisResourceCacheDb::initialize(appData)) {
        qFatal("could not initialise the resource cache database under %s", qPrintable(appData));
    }
    KisResourceLocator::instance()->initialize(
        QStringLiteral(KRITA_RESOURCE_DIRS_FOR_TESTS).section(QLatin1Char(';'), 0, 0)
        + QStringLiteral("/krita"));

    QDir store(projectsRoot());
    if (store.exists()) {
        QVERIFY(store.removeRecursively());
    }

    /// A dialog nobody can dismiss hangs a headless run, and the first one can arrive before the
    /// main window even exists, so this goes up before anything that can raise one. The
    /// navigator's own answer is the value under test; the test closes the dialog rather than
    /// waiting behind it.
    m_dialogWatchdog = new QTimer(this);
    m_dialogWatchdog->setInterval(10);
    connect(m_dialogWatchdog, &QTimer::timeout, this, [this]() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            if (widget->isVisible() && qobject_cast<QDialog *>(widget)) {
                /// Counted before it is dismissed: this is the dialog the notebook must never
                /// let Krita raise, and closing it is what would let the test run past it.
                if (QMessageBox *box = qobject_cast<QMessageBox *>(widget)) {
                    if (box->text().contains(QStringLiteral("has been modified"))) {
                        ++m_savePrompts;
                    }
                }
                qWarning("[nav-integration] dismissing modal dialog \"%s\"",
                         qPrintable(widget->windowTitle()));
                widget->close();
            }
        }
    });
    m_dialogWatchdog->start();

    /// The page turn needs a document and a view, and the plugin gets both from the current main
    /// window. Krita's own ui tests build one headlessly the same way
    /// (libs/ui/tests/kis_view_signals_test.cpp).
    qWarning("[nav-integration] creating a main window");
    m_mainWindow = KisPart::instance()->createMainWindow();
    qWarning("[nav-integration] main window created: %s", m_mainWindow ? "yes" : "no");
    QVERIFY(m_mainWindow);

    /// Turning a page by panning is driven by a timer watching the middle of the viewport. The
    /// canvas here has no meaningful viewport, and the timer must not turn a page while a
    /// refused save is being handled.
    navigator()->setScrollFollowEnabled(false);

    /// And the defaults the singleton read, asserted once it exists: five pages, the strip on, a
    /// 450 ms settle -- what the follow's own log calls "settled on page N after 450 ms" -- and the
    /// three fifths the fit has always used. The keys were removed above, before anything could
    /// construct it.
    QCOMPARE(navigator()->scope(), 5);
    QCOMPARE(navigator()->stripPageCount(), 5);
    QCOMPARE(navigator()->scrollSettleMs(), 450);
    QCOMPARE(navigator()->readingSharePercent(), 60);
}

void PdfNavigatorIntegrationTest::cleanupTestCase()
{
    /// The test is over and the notebook store is about to be removed, so nothing may be written
    /// on the way out. A document that is still modified makes KisView::queryClose() put up a
    /// "the document has been modified, do you want to save it?" dialog, and a dialog raised from
    /// inside the teardown is a hang or a crash rather than a test result.
    if (KisDocument *document = navigator()->currentDocument()) {
        document->setModified(false);
    }

    /// Krita's own ui tests close the view and delete the main window themselves
    /// (libs/ui/tests/kis_view_signals_test.cpp); leaving the documents, the views and the window
    /// to static destruction is what turned a green run into a segfault on the way out of the
    /// process, after the tests had already reported.
    ///
    /// The view manager is deliberately left in place: closing a view makes Krita's MDI area
    /// activate another one, and KisView::notifyCurrentStateChanged() reaches the input manager
    /// through it. With the manager detached (KisView::setViewManager(nullptr), which is what
    /// libs/ui/tests does) KisView::globalInputManager() returns null and that path crashes.
    /// Every view, not only the navigator's: a test that opened a notebook leaves one behind, and
    /// a view that outlives the window it belongs to is what makes the delete below crash instead
    /// of finish (KoToolManager tears the canvas controller down twice).
    /// PARKED: the main window and its views are deliberately NOT torn down here, and main() exits
    /// the process without letting their destructors run at all.
    ///
    /// The chain of Krita teardown defects this suite kept finding, in the order it was found:
    ///   1. KoCanvasResourceProvider::hasDerivedResourceConverter(this=0) under
    ///      KoToolManager::Private::disconnectActiveTool -- guarded in KoToolManager.cpp.
    ///   2. KisViewManager::canvasResourceProvider(this=0) under KisToolPaint::tryRestoreOpacitySnapshot
    ///      -- guarded in kis_tool_paint.cc, and reachable from the product, not only from a test.
    ///   3. A use-after-free in KisMainWindow::dockWidgets() from a canvas controller unset after the
    ///      window was half destroyed: fixed here by unsetting the controllers while the window was
    ///      still alive (that abort is gone).
    ///   4. Two more null converter paths behind it (canvasState, syncOnImageSizeChange) -- the
    ///      second reachable from the product: KisView.cpp calls it whenever the image's size changes.
    ///   5. What is left is a USE-AFTER-FREE, not a null: KoToolProxy::requestStrokeEnd at
    ///      KoToolProxy.cpp:569, reached from a Qt signal/slot during teardown, faulting on a
    ///      non-zero address. A dangling tool proxy on the way out; the tool proxy is Krita's, the
    ///      suite never touches it, and a test harness cannot order a fix for it.
    ///
    /// Both obvious ways out were tried and both bite: deleting the window walks the chain above,
    /// and leaving the window and the views to static destruction is the segfault on the way out of
    /// the process this comment used to record. So the third way is taken -- the process ends BEFORE
    /// that teardown runs, in main(), after QTest has printed its summary and with an exit status
    /// derived from its result. Recorded as its own defect task: four core guards landed, this fifth
    /// is Krita's, and the teardown chain is Krita's to fix.
    ///
    /// WHAT THIS LEAVES UNCOVERED, named rather than hidden: nothing about the plugin is untested --
    /// every notebook, document, page turn, roll, budget, naming and preview assertion still runs.
    /// What is no longer exercised is the APPLICATION's teardown: KisMainWindow destruction with a
    /// notebook open, the view's own close path inside that destruction, and Krita's document/view
    /// cleanup on exit. Those are Krita's code, they are covered by Krita's own ui tests, and they
    /// are exactly where the five defects above live.
    if (m_dialogWatchdog) {
        m_dialogWatchdog->stop();
    }
}

bool PdfNavigatorIntegrationTest::useNotebook(const QString &name, int scope)
{
    navigator()->setScope(scope);

    const QString source = m_dir.filePath(name + QStringLiteral(".pdf"));
    if (!QFileInfo::exists(source) && !QFile::copy(m_fixture, source)) {
        qWarning("[nav-integration] cannot copy the fixture to %s", qPrintable(source));
        return false;
    }

    QString why;
    if (!navigator()->openNotebook(source, &why)) {
        qWarning("[nav-integration] openNotebook failed: %s", qPrintable(why));
        return false;
    }

    return navigator()->currentDocument() != nullptr;
}

void PdfNavigatorIntegrationTest::drawInk(KisDocument *document)
{
    QVERIFY(document);

    const KisImageSP image = document->image();
    QVERIFY(image);

    KisPaintLayer *layer = inkLayer(image);
    QVERIFY(layer);
    layer->paintDevice()->fill(InkMark, KoColor(QColor(0, 0, 0), image->colorSpace()));
    QVERIFY2(inkMarkPresent(image), "the mark did not land on the ink layer");

    /// The flag the page turn reads. A brush stroke arrives at the same flag through
    /// KisDocument::setImageModified(); the pixels are placed directly here so that what is under
    /// test is the page window and the save.
    document->setModified(true);
    QVERIFY(document->isModified());
}

QString PdfNavigatorIntegrationTest::artifactFor(int index) const
{
    return QDir(navigator()->projectDir()).filePath(PdfSession::pageFileName(index));
}

bool PdfNavigatorIntegrationTest::waitForInk(const QString &path, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (!PdfInkLoader::loadInk(path).isNull()) {
            return true;
        }
        QTest::qWait(50);
    }
    return !PdfInkLoader::loadInk(path).isNull();
}

/**
 * A page turn with nothing to protect: the page that leaves has no unsaved ink, so the window
 * drops it and nothing is written.
 */
void PdfNavigatorIntegrationTest::testCleanTurnEvictsWithoutSaving()
{
    QVERIFY(useNotebook(QStringLiteral("clean")));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY(navigator()->pageCount() >= 2);
    QVERIFY(!navigator()->currentDocument()->isModified());
    QCOMPARE(navigator()->pageWindow().capacity(), navigator()->scope());

    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    QCOMPARE(navigator()->pageWindow().evictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().openPages(), QList<int>{1});

    /// A clean page costs a render to reopen and never a write, so no artifact was made.
    QVERIFY(!QFileInfo::exists(artifactFor(0)));

    QVERIFY2(navigator()->previous(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);
    QCOMPARE(navigator()->pageWindow().evictionCount(), 2);
}

/**
 * A page turn that has to save first: the ink is on disk before the page is let go, and it comes
 * back with the page when the page is opened again.
 */
void PdfNavigatorIntegrationTest::testDirtyPageIsSavedBeforeEvictionAndTheInkComesBack()
{
    QVERIFY(useNotebook(QStringLiteral("dirty")));

    KisDocument *firstPage = navigator()->currentDocument();
    QVERIFY(firstPage);
    drawInk(firstPage);

    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    /// The page could only be dropped because it was written first.
    QCOMPARE(navigator()->pageWindow().evictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 0);

    /// A different document is open, and the one that left is really on disk.
    QVERIFY(navigator()->currentDocument() != firstPage);
    const QString artifact = artifactFor(0);
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));

    /// And the ink comes back where it was drawn, which is the round trip the export rests on.
    QVERIFY2(navigator()->previous(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY2(inkMarkPresent(navigator()->currentDocument()->image()),
             "the ink saved before the eviction did not come back with the page");
}

/**
 * The page is written to the artifact the manifest names, and not to the name its page number
 * implies.
 *
 * The two agree in a notebook that was just created -- PdfSession::createProject() builds both
 * from the same counter -- which is exactly what made the divergence invisible: the reading half
 * of the plugin has always used record.kraFile, while the writing half rebuilt the name as
 * PdfSession::pageFileName(record.index). The moment a notebook operation moves a page's ink from
 * one name to another -- insert, duplicate, extract -- the write half goes on writing to the old
 * name, and the ink is saved into a file no reader ever opens. Silent, and only visible as "my
 * notes were not there when I came back".
 *
 * So the manifest is edited to name an artifact the number does not imply, and the page is then
 * drawn on and turned away from. The write has to land at the recorded name, and nothing may be
 * left at the implied one.
 */
void PdfNavigatorIntegrationTest::testAPageIsWrittenToTheArtifactTheManifestNames()
{
    QVERIFY(useNotebook(QStringLiteral("artifact-name")));

    const QString manifestPath = PdfSession::manifestPath(navigator()->projectDir());
    QString why;
    PdfSessionManifest manifest = PdfSessionManifest::readFrom(manifestPath, &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    const QString named = QStringLiteral("pages/renamed-ink.kra");
    const QString implied = artifactFor(0);
    QVERIFY(QDir(navigator()->projectDir()).filePath(named) != implied);
    manifest.pages[0].kraFile = named;
    QVERIFY2(manifest.writeTo(manifestPath, &why), qPrintable(why));

    /// Reopened so the navigator works from that manifest rather than the copy it took when the
    /// page was first shown, then the implied name is cleared: the save an open makes writes one,
    /// and leaving it there would make the assertion below pass for the wrong reason.
    QVERIFY(useNotebook(QStringLiteral("artifact-name")));
    QCOMPARE(navigator()->currentIndex(), 0);
    QFile::remove(implied);

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    const QString namedPath = QDir(navigator()->projectDir()).filePath(named);
    QVERIFY2(QFileInfo::exists(namedPath),
             qPrintable(QStringLiteral("%1 was never written").arg(namedPath)));
    QVERIFY2(waitForInk(namedPath), qPrintable(namedPath));
    QVERIFY2(!QFileInfo::exists(implied),
             qPrintable(QStringLiteral("the ink was written to %1, the name the page number "
                                       "implies, instead of the name the manifest records (%2)")
                            .arg(implied, namedPath)));
}

/**
 * A page turn that cannot be completed. The save fails, so the eviction is refused, so the turn
 * is refused: the page that is open stays open, still holding an ink that is on disk nowhere,
 * and the destination is not half-written.
 */
void PdfNavigatorIntegrationTest::testRefusedSaveKeepsThePageOpenAndTheInkIntact()
{
    QVERIFY(useNotebook(QStringLiteral("refused")));

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    /// A destination that exists and cannot be written: Krita refuses such a save before it
    /// starts, which is exactly the refusal the window has to act on.
    const QString artifact = artifactFor(0);
    QVERIFY(QDir().mkpath(QFileInfo(artifact).absolutePath()));
    const QByteArray sentinel("this is not a saved page\n");
    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(sentinel), qint64(sentinel.size()));
        file.close();
    }
    QVERIFY(QFile::setPermissions(artifact, QFile::ReadOwner | QFile::ReadUser));
    QVERIFY(!QFileInfo(artifact).isWritable());

    QString why;
    QVERIFY2(!navigator()->next(&why), "the page turn was allowed although its save had failed");
    QVERIFY2(why.contains(QStringLiteral("saving it failed")), qPrintable(why));

    /// Nothing moved: same page, same document, still dirty, ink still on it.
    QCOMPARE(navigator()->currentIndex(), 0);
    QCOMPARE(navigator()->currentDocument(), page);
    QVERIFY(page->isModified());
    QVERIFY2(inkMarkPresent(page->image()), "the refused page turn lost the ink");

    /// And the unwritable destination was not touched.
    {
        QFile file(artifact);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), sentinel);
    }

    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().evictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 0);
    QCOMPARE(navigator()->pageWindow().openPages(), QList<int>{0});

    /// Refused again rather than once by accident, and the page is still the one that is open.
    QVERIFY(!navigator()->next(&why));
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 2);
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY2(inkMarkPresent(page->image()), "the second refusal lost the ink");

    /// The refusal was the destination and nothing else: once the destination can be written the
    /// same turn succeeds and the ink reaches the disk.
    QVERIFY(QFile::setPermissions(artifact,
                                  QFile::ReadOwner | QFile::WriteOwner
                                      | QFile::ReadUser | QFile::WriteUser));
    QVERIFY(QFileInfo(artifact).isWritable());

    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));
    QCOMPARE(navigator()->pageWindow().evictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().savedBeforeEvictionCount(), 1);
    QCOMPARE(navigator()->pageWindow().blockedEvictionCount(), 2);
}

/**
 * The idle write waits for a pause, and a page turn is what writes promptly.
 *
 * The pipeline used to write the ink down after 600 ms of quiet, and in a strip that means every
 * page of the window -- which page a stroke belongs to is only decided at save time -- so taking
 * notes rewrote all five artifacts, some ten megabytes, every three seconds of the session
 * (pdfio.log, 2026-09-24: seven full five-page cycles inside thirty seconds). Ten seconds of
 * quiet is now the earliest an idle write runs and five minutes is the longest ink may sit
 * unsaved, so a second and a half after a stroke nothing may be on disk -- and the page turn
 * must still write it and wait for the landing, because that is what stands between a turn and a
 * lost stroke.
 */
void PdfNavigatorIntegrationTest::testInkIsNotWrittenWhileThePenIsStillBusy()
{
    QVERIFY(useNotebook(QStringLiteral("busy")));

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    /// Six ticks of the idle timer, and nothing to show for them.
    QTest::qWait(1500);
    const QString artifact = artifactFor(0);
    QVERIFY2(!QFileInfo::exists(artifact),
             "the idle write fired while the ink had barely stopped moving");

    /// The turn writes it, waits for it to land, and the mark is in what it wrote.
    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));
    QVERIFY2(inkMarkInImage(PdfInkLoader::loadInk(artifact)),
             "the turn wrote the page without the ink that was on it");
}

/**
 * A layer the user made is written too, wherever it is.
 *
 * The save used to take the contents of the Ink group and nothing else -- the strip's single layer
 * called "Ink" -- so a layer made outside it, which is the obvious thing to do with a highlights
 * layer or a second brush, was dropped without a word: the page came back as if those strokes had
 * never been made. An artifact now holds every layer of the page except the render of the source,
 * which is the one thing that is derivable and the one thing the file-size budget is about.
 */
void PdfNavigatorIntegrationTest::testALayerThatIsNotInkIsSavedToo()
{
    QVERIFY(useNotebook(QStringLiteral("layers")));

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);

    /// A layer of the user's own, added above the Ink group rather than inside it.
    KisPaintLayerSP mine = new KisPaintLayer(document->image(), QStringLiteral("Highlights"),
                                             OPACITY_OPAQUE_U8);
    mine->paintDevice()->fill(InkMark, KoColor(QColor(0, 0, 0), document->image()->colorSpace()));
    document->image()->addNode(mine, document->image()->root());
    document->setModified(true);
    Q_EMIT document->image()->sigImageModified();

    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    const QImage saved = PdfInkLoader::loadInk(artifactFor(0));
    QVERIFY2(inkMarkInImage(saved),
             "a layer the user made outside the Ink group was not written to the artifact");
}

/**
 * Closing our tab: the ink is written by the notebook before Krita can ask about it, so there is
 * no "do you want to save it?" prompt and no chance for Krita to write the editing document -- the
 * rendered page included -- as a .kra.
 *
 * The close is driven the way closing a tab drives it: closeView(), which is the subwindow close
 * that reaches KisView::closeEvent() and then queryClose().
 */
void PdfNavigatorIntegrationTest::testClosingTheTabWritesTheInkAndAsksNothing()
{
    QVERIFY(useNotebook(QStringLiteral("close")));

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    KisView *view = navigator()->currentView();
    QVERIFY(view);
    QVERIFY(navigator()->projectDir() != QString());

    const QString artifact = artifactFor(0);
    QVERIFY2(!QFileInfo::exists(artifact),
             "the page was written before anything asked for it to be");

    /// Both places a write we did not ask for could land are watched, so "nothing else was
    /// written" is an assertion rather than a hope.
    const QStringList homeKraBefore = homeKraFiles();
    const int promptsBefore = m_savePrompts;

    view->closeView();
    QApplication::sendPostedEvents();
    QApplication::processEvents();

    /// Nothing was asked, because by the time Krita looks there is nothing to ask about.
    QCOMPARE(m_savePrompts, promptsBefore);

    /// The ink is on disk, in the notebook's own artifact, with the mark in it.
    const QImage saved = PdfInkLoader::loadInk(artifact);
    QVERIFY2(!saved.isNull(), qPrintable(artifact));
    QVERIFY2(inkMarkInImage(saved),
             "the closed page's artifact does not hold the ink that was on the page");

    /// And no editing document was written: the notebook holds one artifact under pages/, nothing
    /// .kra-shaped in its own root, and the home directory gained no .kra at all.
    const QDir project(navigator()->projectDir());
    const QStringList rootKra = project.entryList(QStringList() << QStringLiteral("*.kra"), QDir::Files);
    QVERIFY2(rootKra.isEmpty(), qPrintable(rootKra.join(QLatin1Char(','))));
    QCOMPARE(homeKraFiles(), homeKraBefore);
}

/**
 * The inserted image stays its own layer inside the Ink group when the page is left and the
 * notebook is opened again. This is the user's report, reproduced headlessly.
 *
 * The flow is the one they perform: a five-page strip (the shipped scope), a picture inserted into
 * the open page's Ink group as a paint layer of its own, a turn to a page outside the window --
 * which writes every page the window holds and redraws the strip from those artifacts -- and then
 * the notebook opened again, which builds the strip from the artifacts a second time.
 *
 * The second build is where the merge happened. PdfStripBuilder::build() read each page with
 * PdfInkLoader::loadInk(), which is the artifact's MERGED image: the flatten of every layer the
 * page holds. Pouring that into the strip's single "Ink" layer turned the inserted picture into
 * ink -- one layer in the panel where there had been two. Measured before the fix: "after the
 * reopen: the Ink group holds 1 child(ren): Ink", and this test fails on the missing child. After
 * it: two children, the picture's 3600 pixels in the layer that owns them, none in Ink.
 */
void PdfNavigatorIntegrationTest::testAnInsertedImageStaysItsOwnLayerAcrossAReopen()
{
    /// A source long enough that a five-page window has somewhere to roll to.
    const QString usualFixture = m_fixture;
    m_fixture = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));

    QVERIFY(useNotebook(QStringLiteral("inserted-layer"), 5));
    m_fixture = usualFixture;

    QVERIFY(navigator()->pageCount() >= 8);
    QCOMPARE(navigator()->currentIndex(), 0);

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    KisImageSP image = document->image();
    QVERIFY(image);

    KisNodeSP group;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        if (child->name() == QStringLiteral("Ink")
            && qobject_cast<KisGroupLayer *>(child.data())) {
            group = child;
            break;
        }
    }
    QVERIFY2(group, "the strip has no Ink group");

    /// What placeInsertedImage() does: its own paint layer, its own pixels, the place on the page
    /// carried by the layer offset, inside the Ink group.
    const QRect picture(200, 300, 60, 60);
    const KoColorSpace *colorSpace = image->colorSpace();
    KisPaintLayerSP inserted = new KisPaintLayer(image, QStringLiteral("Inserted image"),
                                                 OPACITY_OPAQUE_U8);
    inserted->paintDevice()->fill(QRect(QPoint(0, 0), picture.size()),
                                  KoColor(QColor(255, 0, 0), colorSpace));
    inserted->setX(picture.x());
    inserted->setY(picture.y());
    QVERIFY2(image->addNode(inserted, group), "the picture could not be added to the Ink group");

    const auto childNames = [](const KisNodeSP &parent) {
        QStringList names;
        for (quint32 i = 0; i < parent->childCount(); ++i) {
            names.append(parent->at(i)->name());
        }
        return names;
    };
    qInfo("before the turn: the Ink group holds %d child(ren): %s", int(group->childCount()),
          qPrintable(childNames(group).join(QStringLiteral(", "))));

    document->setModified(true);
    Q_EMIT image->sigImageModified();

    /// A turn outside the window: the roll writes every page the window holds, page 1 included.
    QString why;
    QVERIFY2(navigator()->showPage(5, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 5);
    QVERIFY2(QFileInfo::exists(artifactFor(0)), "the roll did not write page 1");

    QFile list(artifactFor(0) + QStringLiteral(".layers.txt"));
    QVERIFY2(list.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(list.fileName()));
    const QString listing = QString::fromUtf8(list.readAll());
    qInfo("page 1's sidecar after the roll:\n%s", qPrintable(listing));
    QVERIFY2(listing.contains(QStringLiteral("Inserted image")),
             "the artifact does not list the inserted layer as a layer of its own");

    /// And the notebook opened again, which is the build a page switch reaches when the page is
    /// rebuilt from what is on disk.
    QVERIFY(useNotebook(QStringLiteral("inserted-layer"), 5));
    QCOMPARE(navigator()->currentIndex(), 0);

    KisDocument *reopened = navigator()->currentDocument();
    QVERIFY(reopened);
    KisImageSP strip = reopened->image();
    QVERIFY(strip);

    const PdfStripLayout layout = PdfStripLayout::forWindow(navigator()->manifest(), 0, 5, 200.0);
    QVERIFY(layout.isValid());
    const int slot = layout.slotForPage(0);
    QVERIFY(slot >= 0);
    const QRect slotRect = layout.slots().at(slot).rect;

    KisNodeSP reopenedGroup;
    for (quint32 i = 0; i < strip->root()->childCount(); ++i) {
        KisNodeSP child = strip->root()->at(i);
        if (qobject_cast<KisGroupLayer *>(child.data())) {
            reopenedGroup = child;
            break;
        }
    }
    QVERIFY2(reopenedGroup, "the rebuilt strip has no Ink group");
    const QStringList names = childNames(reopenedGroup);
    qInfo("after the reopen: the Ink group holds %d child(ren): %s", int(names.size()),
          qPrintable(names.join(QStringLiteral(", "))));

    QVERIFY2(names.contains(QStringLiteral("Inserted image")),
             "the inserted image came back inside the Ink layer instead of as its own child");
    QVERIFY2(names.contains(QStringLiteral("Ink")), "the strip's stroke layer is gone");

    const auto redPixelsIn = [](const QImage &pixels, const QRect &area) {
        int count = 0;
        const QRect clipped = area.intersected(pixels.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                const QRgb pixel = pixels.pixel(x, y);
                if (qAlpha(pixel) > 200 && qRed(pixel) > 200 && qGreen(pixel) < 60
                    && qBlue(pixel) < 60) {
                    ++count;
                }
            }
        }
        return count;
    };

    /// The stroke layer, and the layer that owns the picture: the picture's pixels must be in the
    /// second and nowhere in the first.
    KisPaintLayer *strokes = nullptr;
    KisPaintLayer *restored = nullptr;
    for (quint32 i = 0; i < reopenedGroup->childCount(); ++i) {
        KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(reopenedGroup->at(i).data());
        if (!layer) {
            continue;
        }
        if (layer->name() == QStringLiteral("Ink")) {
            strokes = layer;
        } else if (layer->name() == QStringLiteral("Inserted image")) {
            restored = layer;
        }
    }
    QVERIFY(strokes);
    QVERIFY2(restored, "the inserted image is not a paint layer of the rebuilt group");

    const int redInInk = redPixelsIn(strokes->paintDevice()->convertToQImage(0, strip->bounds()),
                                     slotRect);
    const QImage restoredPixels = restored->paintDevice()->convertToQImage(0, strip->bounds());
    const int redInPicture = redPixelsIn(restoredPixels, slotRect);

    qInfo("after the reopen: red pixels inside page 1's slot -- in the Ink stroke layer %d, in "
          "the inserted image's own layer %d (expected about %d)",
          redInInk, redInPicture, picture.width() * picture.height());

    QCOMPARE(redInInk, 0);
    QVERIFY2(redInPicture > picture.width() * picture.height() / 2,
             "the inserted picture's pixels did not come back into its own layer");

    /// Where it came to rest: the picture's own place inside page 1's slot, not the slot's corner.
    QVERIFY2(restoredPixels.valid(slotRect.topLeft() + picture.center()), "outside the slot");
    const QColor atPlace = restoredPixels.pixelColor(slotRect.topLeft() + picture.center());
    qInfo("the inserted picture in its own layer: rgba(%d,%d,%d,%d) at %d,%d",
          atPlace.red(), atPlace.green(), atPlace.blue(), atPlace.alpha(),
          slotRect.x() + picture.center().x(), slotRect.y() + picture.center().y());
    QVERIFY2(atPlace.red() > 200 && atPlace.green() < 60 && atPlace.blue() < 60,
             "the inserted picture is not at its own place inside the page's slot");

    /// And the tab is closed, the way the roll test closes its own: a view nobody closed takes the
    /// main window down with it in the teardown. The document is marked clean first -- its ink is on
    /// disk and nothing was drawn since -- so closing asks nothing.
    reopened->setModified(false);
    navigator()->setScope(1);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);
}

/**
 * "Insert image..." writes the page, not the strip.
 *
 * slotSavePage() built its document from the whole editing image, and a five-page strip is five
 * pages tall: page 1's artifact came out the size of the strip (measured in the user's notebook:
 * maindoc 1653x5273 where page 1 renders 1653x2339). The next roll rewrote it cropped, so it
 * self-corrected on the first page switch -- but a close, a reopen or an export right after the
 * insert read a page-sized rectangle full of strip.
 *
 * The write now goes through PdfPageNavigator::pageLayersDocument(), the same crop the roll uses.
 * This test measures both on one image: the call the insert used to make, which is still the whole
 * strip, and the one it makes now, which is the page. Then it opens the notebook again with no page
 * switch in between and checks that the picture came back as its own layer, at its own place.
 */
void PdfNavigatorIntegrationTest::testInsertingOnAStripWritesThePageAndNotTheStrip()
{
    /// A source long enough that a five-page window is a real strip.
    const QString usualFixture = m_fixture;
    m_fixture = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));
    QVERIFY(useNotebook(QStringLiteral("insert-crop"), 5));
    m_fixture = usualFixture;

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    KisImageSP image = document->image();
    QVERIFY(image);

    const PdfStripLayout layout = PdfStripLayout::forWindow(navigator()->manifest(), 0, 5, 200.0);
    QVERIFY(layout.isValid());
    const int slot = layout.slotForPage(0);
    QVERIFY(slot >= 0);
    const QRect pageRect = layout.slots().at(slot).rect;

    /// The strip has to be taller than one page, or the two writes would be the same size and the
    /// measurement would say nothing.
    QVERIFY2(image->height() > pageRect.height(),
             qPrintable(QStringLiteral("the strip is %1 px tall and page 1 is %2 -- not a strip")
                            .arg(image->height()).arg(pageRect.height())));

    KisNodeSP group;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        if (child->name() == QStringLiteral("Ink")
            && qobject_cast<KisGroupLayer *>(child.data())) {
            group = child;
            break;
        }
    }
    QVERIFY2(group, "the strip has no Ink group");

    /// The picture, as placeInsertedImage() puts one there: its own layer, its own pixels, the place
    /// carried by the layer offset, well inside page 1.
    const QPoint pictureOnStrip(200, 300);
    const QRect pictureOnStripRect(pictureOnStrip, QSize(60, 60));
    QVERIFY2(pageRect.contains(pictureOnStripRect),
             qPrintable(QStringLiteral("the picture at %1,%2 is not inside page 1 (%3,%4 %5x%6)")
                            .arg(pictureOnStrip.x()).arg(pictureOnStrip.y())
                            .arg(pageRect.x()).arg(pageRect.y())
                            .arg(pageRect.width()).arg(pageRect.height())));
    KisPaintLayerSP inserted = new KisPaintLayer(image, QStringLiteral("Inserted image"),
                                                 OPACITY_OPAQUE_U8);
    inserted->paintDevice()->fill(QRect(QPoint(0, 0), pictureOnStripRect.size()),
                                  KoColor(QColor(255, 0, 0), image->colorSpace()));
    inserted->setX(pictureOnStrip.x());
    inserted->setY(pictureOnStrip.y());
    QVERIFY2(image->addNode(inserted, group), "the picture could not be added to the Ink group");

    QString why;

    /// The call the insert made before, kept here as its measurement: the whole editing image.
    KisDocument *whole = PdfPageSaver::createPageLayersDocument(image, &why);
    QVERIFY2(whole, qPrintable(why));
    const QSize wholeSize(whole->image()->width(), whole->image()->height());
    qInfo("the call the insert used to make: %dx%d; page 1's rectangle: %dx%d; the strip: %dx%d",
          wholeSize.width(), wholeSize.height(), pageRect.width(), pageRect.height(),
          image->width(), image->height());
    QCOMPARE(wholeSize, QSize(image->width(), image->height()));
    QVERIFY(wholeSize.height() > pageRect.height());
    KisPart::instance()->removeDocument(whole, true);

    /// And the one it makes now: page 1's own rectangle, through the door the roll crops by.
    KisDocument *pageDocument = navigator()->pageLayersDocument(document, 0, &why);
    QVERIFY2(pageDocument, qPrintable(why));
    const QSize pageSize(pageDocument->image()->width(), pageDocument->image()->height());
    qInfo("pageLayersDocument(): %dx%d, page 1's rectangle %dx%d",
          pageSize.width(), pageSize.height(), pageRect.width(), pageRect.height());
    QCOMPARE(pageSize, pageRect.size());

    const QString artifact = artifactFor(0);
    bool finished = false;
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, this,
                     [&finished](const QString &) { finished = true; });
    QVERIFY2(PdfPageSaver::saveDocument(pageDocument, artifact, &why), qPrintable(why));
    QElapsedTimer saveClock;
    saveClock.start();
    while (!finished && saveClock.elapsed() < 30000) {
        QTest::qWait(25);
    }
    QVERIFY2(finished, "the save never reported back");
    KisPart::instance()->removeDocument(pageDocument, true);

    QFile list(artifact + QStringLiteral(".layers.txt"));
    QVERIFY2(list.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(list.fileName()));
    const QString listing = QString::fromUtf8(list.readAll());
    qInfo("page 1's sidecar right after the insert:\n%s", qPrintable(listing));
    QVERIFY2(listing.contains(QStringLiteral("Inserted image")),
             "the artifact does not list the inserted layer");

    const QList<QPair<QString, QImage>> layers =
        PdfInkLoader::loadInkLayersFromSidecar(artifact, &why);
    QVERIFY2(layers.size() >= 2, qPrintable(why));

    const QPoint pictureInPage = pictureOnStrip - pageRect.topLeft();
    QImage insertedPixels;
    for (const QPair<QString, QImage> &entry : layers) {
        QCOMPARE(entry.second.size(), pageRect.size());
        if (entry.first == QStringLiteral("Inserted image")) {
            insertedPixels = entry.second;
        }
    }
    QVERIFY2(!insertedPixels.isNull(), "the artifact has no inserted image entry");

    const QPoint inPicture = pictureInPage + QPoint(pictureOnStripRect.width() / 2,
                                                    pictureOnStripRect.height() / 2);
    const QRgb atPlace = insertedPixels.pixel(inPicture);
    const QRgb corner = insertedPixels.pixel(2, 2);
    qInfo("right after the insert: %d layer(s), each %dx%d; the picture's own place %d,%d is "
          "rgba(%d,%d,%d,%d); a corner of its layer is rgba(%d,%d,%d,%d)",
          int(layers.size()), pageRect.width(), pageRect.height(), inPicture.x(), inPicture.y(),
          qRed(atPlace), qGreen(atPlace), qBlue(atPlace), qAlpha(atPlace),
          qRed(corner), qGreen(corner), qBlue(corner), qAlpha(corner));
    QVERIFY2(qAlpha(atPlace) > 200 && qRed(atPlace) > 200 && qGreen(atPlace) < 60
                 && qBlue(atPlace) < 60,
             "the inserted picture is not at its own place inside the page's rectangle");
    QCOMPARE(qAlpha(corner), 0);

    /// No page switch in between: the notebook is opened again straight away, which is the read the
    /// defect broke.
    QVERIFY(useNotebook(QStringLiteral("insert-crop"), 5));
    QCOMPARE(navigator()->currentIndex(), 0);

    KisDocument *reopened = navigator()->currentDocument();
    QVERIFY(reopened);
    KisImageSP strip = reopened->image();
    QVERIFY(strip);

    KisNodeSP reopenedGroup;
    for (quint32 i = 0; i < strip->root()->childCount(); ++i) {
        KisNodeSP child = strip->root()->at(i);
        if (qobject_cast<KisGroupLayer *>(child.data())) {
            reopenedGroup = child;
            break;
        }
    }
    QVERIFY2(reopenedGroup, "the rebuilt strip has no Ink group");

    QStringList names;
    KisPaintLayer *restored = nullptr;
    KisPaintLayer *strokes = nullptr;
    for (quint32 i = 0; i < reopenedGroup->childCount(); ++i) {
        names.append(reopenedGroup->at(i)->name());
        KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(reopenedGroup->at(i).data());
        if (!layer) {
            continue;
        }
        if (layer->name() == QStringLiteral("Inserted image")) {
            restored = layer;
        } else if (layer->name() == QStringLiteral("Ink")) {
            strokes = layer;
        }
    }
    qInfo("after the reopen with no page switch: the Ink group holds %d child(ren): %s",
          int(names.size()), qPrintable(names.join(QStringLiteral(", "))));
    QVERIFY2(names.contains(QStringLiteral("Inserted image")),
             "the picture did not come back as its own layer");
    QVERIFY(strokes);
    QVERIFY(restored);

    const auto redPixelsIn = [](const QImage &pixels, const QRect &area) {
        int count = 0;
        const QRect clipped = area.intersected(pixels.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                const QRgb pixel = pixels.pixel(x, y);
                if (qAlpha(pixel) > 200 && qRed(pixel) > 200 && qGreen(pixel) < 60
                    && qBlue(pixel) < 60) {
                    ++count;
                }
            }
        }
        return count;
    };

    const QImage restoredPixels = restored->paintDevice()->convertToQImage(0, strip->bounds());
    const int redInInk = redPixelsIn(strokes->paintDevice()->convertToQImage(0, strip->bounds()),
                                     pageRect);
    const QColor backAtPlace = restoredPixels.pixelColor(
        pictureOnStrip + QPoint(pictureOnStripRect.width() / 2, pictureOnStripRect.height() / 2));
    qInfo("after the reopen: red pixels inside page 1's slot -- Ink %d, the inserted layer %d; the "
          "picture's place rgba(%d,%d,%d,%d)",
          redInInk, redPixelsIn(restoredPixels, pageRect), backAtPlace.red(), backAtPlace.green(),
          backAtPlace.blue(), backAtPlace.alpha());
    QCOMPARE(redInInk, 0);
    QVERIFY2(backAtPlace.red() > 200 && backAtPlace.green() < 60 && backAtPlace.blue() < 60,
             "the picture did not come back at its own place on the page");

    /// The tab is closed, the way the roll test closes its own: a view nobody closed takes the main
    /// window down with it in the teardown.
    reopened->setModified(false);
    navigator()->setScope(1);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);
}

/**
 * After the notebook is changed under the open page, the reader keeps their page and its ink.
 *
 * A move changes what every page NUMBER means, so the page that is open cannot simply be turned to
 * a new index: it is closed, the manifest is re-read, and the anchor the operation reported is
 * opened in a fresh document. What is under test is the whole round trip -- ink written before the
 * change, the manifest changed underneath it, the notebook reloaded, and the ink read back out of
 * the artifact that travelled with its record.
 */
void PdfNavigatorIntegrationTest::testAMovedPageKeepsTheReaderAndTheirInk()
{
    QVERIFY(useNotebook(QStringLiteral("ops-move")));
    QVERIFY(navigator()->pageCount() >= 3);
    QCOMPARE(navigator()->currentIndex(), 0);

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    /// The ink has to be on disk before the manifest that describes it changes, or the change is
    /// applied on top of ink that is going somewhere else.
    QString why;
    QVERIFY2(navigator()->prepareForNotebookChange(&why), qPrintable(why));
    QVERIFY2(!page->isModified(), "the document stayed modified after its ink was written");

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::movePage(navigator()->projectDir(), 0, 2, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    QCOMPARE(outcome.anchorPage, 2);

    QVERIFY2(navigator()->reloadNotebook(outcome.anchorPage, &why), qPrintable(why));
    QVERIFY(navigator()->reloadPending());

    /// The rebuild runs on the event loop: the old view has to be gone before the new document is
    /// built, which is the same wait every notebook open uses.
    QElapsedTimer clock;
    clock.start();
    while (navigator()->reloadPending() && clock.elapsed() < 30000) {
        QTest::qWait(50);
    }
    QVERIFY2(!navigator()->reloadPending(), "the reload never finished");

    QCOMPARE(navigator()->currentIndex(), 2);
    QCOMPARE(navigator()->pageCount(), 3);

    /// The record travelled with its file name, and it is the page that is open -- so the ink that
    /// was drawn on it before the move came back out of the artifact it was written to.
    QCOMPARE(navigator()->manifest().pages.at(2).kraFile, PdfSession::pageFileName(0));
    QVERIFY2(inkMarkPresent(navigator()->currentDocument()->image()),
             "the moved page came back without its ink");

    /// And nothing is left waiting: the reload is a one-shot, not a state the notebook sits in.
    QVERIFY(!navigator()->reloadPending());
}

/**
 * Deleting the page the reader is on leaves them on the page that took its place, and the pages
 * around it keep their own ink.
 */
void PdfNavigatorIntegrationTest::testADeletedPageLeavesTheReaderOnTheNextOne()
{
    QVERIFY(useNotebook(QStringLiteral("ops-delete")));
    QVERIFY(navigator()->pageCount() >= 3);

    /// Ink on page 1, then turn to page 2 and delete it: the reader has to land on the page that
    /// takes its place -- and page 1's ink must still be in page 1's artifact afterwards.
    KisDocument *first = navigator()->currentDocument();
    QVERIFY(first);
    drawInk(first);

    QString why;
    QVERIFY2(navigator()->prepareForNotebookChange(&why), qPrintable(why));
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::deletePages(navigator()->projectDir(), 1, 1, 1);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    QCOMPARE(outcome.anchorPage, 1);

    QVERIFY2(navigator()->reloadNotebook(outcome.anchorPage, &why), qPrintable(why));
    QElapsedTimer clock;
    clock.start();
    while (navigator()->reloadPending() && clock.elapsed() < 30000) {
        QTest::qWait(50);
    }
    QVERIFY2(!navigator()->reloadPending(), "the reload never finished");

    QCOMPARE(navigator()->pageCount(), 2);
    QCOMPARE(navigator()->currentIndex(), 1);

    /// Page 1 was not the page that was deleted, and its artifact still holds its ink.
    const QString artifact = QDir(navigator()->projectDir()).filePath(PdfSession::pageFileName(0));
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));
    QVERIFY2(navigator()->previous(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY2(inkMarkPresent(navigator()->currentDocument()->image()),
             "the page that was not deleted lost its ink to the delete");
}

/**
 * Turning a page turns the paper AND the ink: a stroke stays on the line it was drawn on.
 *
 * The paper is easy -- it is rendered from the source every time, so a turn is a turn of the render.
 * The ink is the part that can silently go wrong: it lives in the artifact as pixels, and a page
 * whose paper turned by 90 degrees while its ink did not is a page whose notes are suddenly along
 * the wrong edge. So this test measures both, on the same page:
 *
 *  - the artifact's Ink layer is the quarter-turn of what it was, at the pixel level, and the page
 *    is the turned shape;
 *  - the background of the reopened page is the turned render of the source, not the upright one.
 *
 * The ink is drawn in the top-left corner, which a quarter turn moves to the top-right: a mark that
 * had stayed where it was would fail both the box comparison and the paper comparison.
 */
void PdfNavigatorIntegrationTest::testRotatingAPageTurnsTheInkWithThePaper()
{
    QVERIFY(useNotebook(QStringLiteral("ops-rotate")));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY(navigator()->pageCount() >= 2);

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    QString why;
    QVERIFY2(navigator()->prepareForNotebookChange(&why), qPrintable(why));

    const QString artifact = artifactFor(0);
    const QList<QPair<QString, QImage>> artifactLayers = PdfInkLoader::loadInkLayers(artifact, &why);
    QStringList names;
    for (const QPair<QString, QImage> &entry : artifactLayers) {
        names << QStringLiteral("%1(%2x%3)").arg(entry.first).arg(entry.second.width())
                     .arg(entry.second.height());
    }
    qInfo("the artifact %s exists=%d holds %d layer(s): %s",
          qPrintable(artifact), int(QFileInfo::exists(artifact)), int(artifactLayers.size()),
          qPrintable(names.join(QStringLiteral(", "))));
    /// The stroke layer's own name: the page's Ink GROUP holds a paint layer called
    /// PdfProjectBuilder::inkStrokeLayerName(), and the artifact holds the flattened leaves.
    const QImage inkBefore =
        layerNamed(artifactLayers, PdfProjectBuilder::inkStrokeLayerName());
    QVERIFY2(!inkBefore.isNull(), qPrintable(QStringLiteral("%1: layers are [%2] (%3)")
                                                 .arg(why, names.join(QStringLiteral(", ")),
                                                      artifact)));
    const QRect markBefore = markedBounds(inkBefore);
    QVERIFY2(!markBefore.isEmpty(), "the page has no marked pixels to follow through the turn");

    /// The real rotator, through the operation the menu runs: journalled, committed, atomic.
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::rotatePages(
        navigator()->projectDir(), 0, 1, 90, PdfPageRotator::rotateInto, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    QCOMPARE(outcome.anchorPage, 0);

    QVERIFY2(navigator()->reloadNotebook(0, &why), qPrintable(why));
    QElapsedTimer clock;
    clock.start();
    while (navigator()->reloadPending() && clock.elapsed() < 30000) {
        QTest::qWait(50);
    }
    QVERIFY2(!navigator()->reloadPending(), "the reload never finished");

    /// The page the reader is on is the turned one: the record says so, and the reader sees it.
    QCOMPARE(navigator()->manifest().pages.at(0).extraRotation, 90);
    const KisImageSP image = navigator()->currentDocument()->image();
    QVERIFY(image);
    QCOMPARE(QSize(image->width(), image->height()),
             QSize(inkBefore.height(), inkBefore.width()));

    /// The ink turned with it. For a 90 degree clockwise turn of a WxH image, the pixel at (x, y)
    /// is the pixel at (H-1-y, x) -- so the mark's box moves from the top left to the top right.
    const QImage inkAfter =
        layerNamed(PdfInkLoader::loadInkLayers(artifact, &why),
                   PdfProjectBuilder::inkStrokeLayerName());
    QVERIFY2(!inkAfter.isNull(), qPrintable(why));
    QCOMPARE(inkAfter.size(), inkBefore.size().transposed());

    const QRect markAfter = markedBounds(inkAfter);
    const QRect expected(QPoint(inkBefore.height() - 1 - markBefore.bottom(), markBefore.left()),
                         QSize(markBefore.height(), markBefore.width()));
    qInfo("the mark: %d,%d %dx%d before -> %d,%d %dx%d after; a clockwise turn puts it at %d,%d %dx%d",
          markBefore.x(), markBefore.y(), markBefore.width(), markBefore.height(),
          markAfter.x(), markAfter.y(), markAfter.width(), markAfter.height(),
          expected.x(), expected.y(), expected.width(), expected.height());
    QCOMPARE(markAfter, expected);

    /// And the paper under it is the turned render of the source, not the upright one. Compared on
    /// a grid with a tolerance: the two went through different colour paths, and what is being
    /// asserted is that the page turned, not that two codecs agree byte for byte.
    PopplerRenderBackend pdf;
    QVERIFY(pdf.open(navigator()->sourcePath()));
    const QImage turnedRender =
        pdf.renderPage(0, 200.0).transformed(QTransform().rotate(90), Qt::FastTransformation);

    KisNodeSP background;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        if (image->root()->at(i)->name() == PdfProjectBuilder::backgroundLayerName()) {
            background = image->root()->at(i);
            break;
        }
    }
    QVERIFY2(background, "the reopened page has no background layer");
    const QImage paper = background->paintDevice()->convertToQImage(0, image->bounds());
    QCOMPARE(paper.size(), turnedRender.size());

    int compared = 0;
    int close = 0;
    for (int y = 8; y < paper.height(); y += 37) {
        for (int x = 8; x < paper.width(); x += 41) {
            const QColor mine = paper.pixelColor(x, y);
            const QColor theirs = turnedRender.pixelColor(x, y);
            ++compared;
            if (qAbs(mine.red() - theirs.red()) <= 48 && qAbs(mine.green() - theirs.green()) <= 48
                && qAbs(mine.blue() - theirs.blue()) <= 48) {
                ++close;
            }
        }
    }
    QVERIFY2(compared > 50, "too few pixels were compared for the answer to mean anything");
    qInfo("the turned paper: %d of %d sampled pixels agree with the turned render", close, compared);
    QVERIFY2(close * 10 >= compared * 9,
             qPrintable(QStringLiteral("%1 of %2 sampled pixels differ: the paper is not the turned "
                                       "page").arg(compared - close).arg(compared)));

    /// The tab is closed, the way the other tests that leave a page open close theirs: the document
    /// is marked clean first -- its ink is on disk -- so closing asks nothing.
    navigator()->currentDocument()->setModified(false);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);
}

/**
 * A range extracted from the open notebook opens as a notebook of its own -- by directory.
 *
 * This is F5 of the design: the extracted notebook draws on the SAME source PDF as the notebook it
 * came from, and a project is normally keyed by the source's own hash. Opening it by source would
 * find the original, so the extraction opens it by directory instead -- which is also what Recent
 * notebooks and the Start screen remember.
 */
void PdfNavigatorIntegrationTest::testAnExtractedRangeOpensAsItsOwnNotebook()
{
    QVERIFY(useNotebook(QStringLiteral("ops-extract")));
    QCOMPARE(navigator()->currentIndex(), 0);
    QVERIFY(navigator()->pageCount() >= 3);

    /// Ink on a page that is going to be extracted, so "the new notebook is the range" is about
    /// pixels and not only about a page count.
    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    QString why;
    QVERIFY2(navigator()->prepareForNotebookChange(&why), qPrintable(why));

    const QString original = navigator()->projectDir();
    const QString destination = QDir(original).absolutePath() + QStringLiteral("-range");
    QDir(destination).removeRecursively();

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::extractRange(original, 0, 2, destination);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));

    /// The notebook that is open has to be gone before the new one is built: the same close-first
    /// rule every open follows.
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);

    /// By directory: the source path is the same file for both notebooks.
    QVERIFY2(navigator()->openNotebookDir(destination, &why), qPrintable(why));
    QCOMPARE(QFileInfo(navigator()->projectDir()).absoluteFilePath(),
             QFileInfo(destination).absoluteFilePath());
    QCOMPARE(navigator()->pageCount(), 2);
    QCOMPARE(navigator()->currentIndex(), 0);

    /// And it is the range: the page that was drawn on came with its ink.
    QVERIFY2(inkMarkPresent(navigator()->currentDocument()->image()),
             "the extracted notebook lost the ink of the page it carries");

    /// The notebook it came from is still itself, read straight from disk: extracting is a
    /// read-only operation on it, and this test does not need a third view to say so.
    QCOMPARE(PdfSession::openProject(original, &why).pages.size(), 3);

    /// The tab is closed, the way the other tests that leave a page open close theirs.
    navigator()->currentDocument()->setModified(false);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);
}

/**
 * Merging a notebook in grows the open notebook, and the reader stays on the page they were on.
 *
 * The engine's own test covers what a merge copies. This one covers the reload: the notebook the
 * reader is holding gains pages under it, and after reloadNotebook() the page that is open is the
 * same page -- not the same index into a longer list, and not a page of the notebook merged in.
 */
void PdfNavigatorIntegrationTest::testMergingANotebookInAddsItsPagesAndKeepsTheReader()
{
    QVERIFY(useNotebook(QStringLiteral("ops-merge")));
    QCOMPARE(navigator()->currentIndex(), 0);
    const int before = navigator()->pageCount();
    QVERIFY(before >= 2);

    /// A second notebook of its own, made from another fixture, so the pages that arrive are
    /// visibly not the ones the reader already had.
    QTemporaryDir other;
    QVERIFY(other.isValid());
    const QString otherDir = other.filePath(QStringLiteral("incoming"));
    PopplerRenderBackend backend;
    const QString incomingPdf = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-rotations.pdf");
    QVERIFY(backend.open(incomingPdf));
    const PdfSessionManifest incoming = PdfSession::createProject(otherDir, incomingPdf, backend);
    QVERIFY2(incoming.isValid(), "the notebook this test wants to merge in could not be made");
    QVERIFY(incoming.pages.size() >= 2);

    QString why;
    QVERIFY2(navigator()->prepareForNotebookChange(&why), qPrintable(why));

    /// Appended: the page the reader is on is not pushed anywhere, which the anchor says.
    const PdfNotebookOps::Outcome outcome =
        PdfNotebookOps::mergeNotebook(navigator()->projectDir(), before, otherDir, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    QCOMPARE(outcome.anchorPage, 0);

    QVERIFY2(navigator()->reloadNotebook(outcome.anchorPage, &why), qPrintable(why));
    QElapsedTimer clock;
    clock.start();
    while (navigator()->reloadPending() && clock.elapsed() < 30000) {
        QTest::qWait(50);
    }
    QVERIFY2(!navigator()->reloadPending(), "the reload never finished");

    QCOMPARE(navigator()->pageCount(), before + incoming.pages.size());
    QCOMPARE(navigator()->currentIndex(), 0);
    /// The page that is open is still one of the notebook that was open; the pages that arrived are
    /// behind it and say which source they came from.
    QCOMPARE(navigator()->manifest().pages.at(0).source, 0);
    QCOMPARE(navigator()->manifest().pages.at(before).source, 1);

    /// The tab is closed, like the other tests that leave a page open.
    navigator()->currentDocument()->setModified(false);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);
}

/**
 * The screen keeps a whole change in memory until Apply, and says why an action is not available.
 *
 * The two things the design insists on are assertions here rather than intentions: Apply is off while
 * nothing has changed (a no-op would still close and reopen the notebook), and a page that cannot be
 * deleted or moved says so instead of leaving a greyed button mute. This test exercises the dialog's
 * in-memory copy only; the plugin pre-saves open pages before showing it, so plugin-level Cancel is
 * not necessarily byte-for-byte disk-neutral.
 */
void PdfNavigatorIntegrationTest::testTheScreenKeepsItsChangeUntilApply()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("screen"));

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QCOMPARE(manifest.pages.size(), 3);
    QFile manifestBeforeFile(PdfSession::manifestPath(project));
    QVERIFY(manifestBeforeFile.open(QIODevice::ReadOnly));
    const QByteArray manifestBefore = manifestBeforeFile.readAll();

    /// Ink on the first page, so the change has a real file to carry: "the duplicate brings its
    /// files" is then a statement about bytes rather than about a name.
    {
        QFile ink(QDir(project).filePath(manifest.pages.at(0).kraFile));
        QVERIFY(ink.open(QIODevice::WriteOnly));
        ink.write("ink of the first page");
    }
    {
        const QString layerPath = QDir(project).filePath(
            manifest.pages.at(0).kraFile + QStringLiteral(".layers/Inserted image.png"));
        QVERIFY(QDir().mkpath(QFileInfo(layerPath).absolutePath()));
        QFile layer(layerPath);
        QVERIFY(layer.open(QIODevice::WriteOnly));
        layer.write("test layer pixels");

        QFile index(QDir(project).filePath(manifest.pages.at(0).kraFile + QStringLiteral(".layers.txt")));
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("# index\tname\tfile\topacity\tx\ty\n0\tInserted image\tInserted image.png\t255\t0\t0\n");
    }

    PdfNotebookOpsDialog dialog(project, manifest, 0);
    const auto button = [&dialog](const char *name) {
        auto *found = dialog.findChild<QPushButton *>(QString::fromLatin1(name));
        Q_ASSERT(found);
        return found;
    };
    const auto label = [&dialog](const char *name) {
        auto *found = dialog.findChild<QLabel *>(QString::fromLatin1(name));
        Q_ASSERT(found);
        return found->text();
    };
    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);

    /// Nothing has changed: Apply is off and the legend says so, while the whole-notebook entries --
    /// insert, extract, merge -- are available because the list is untouched.
    QVERIFY2(!button("pdfio_ops_apply")->isEnabled(),
             "Apply was offered with nothing changed: that would close and reopen the notebook for "
             "nothing");
    QVERIFY2(label("pdfio_ops_summary").contains(QStringLiteral("No changes")),
             qPrintable(label("pdfio_ops_summary")));
    QVERIFY(button("pdfio_ops_insert")->isEnabled());
    QVERIFY(button("pdfio_ops_extract")->isEnabled());
    QVERIFY(button("pdfio_ops_merge_notebook")->isEnabled());

    /// The first page cannot move up, and the hint says why rather than leaving the button mute.
    table->selectRow(0);
    QVERIFY(!button("pdfio_ops_move_up")->isEnabled());
    QVERIFY2(label("pdfio_ops_hint").contains(QStringLiteral("first page")),
             qPrintable(label("pdfio_ops_hint")));

    /// Duplicate the first page: one more page, named from the notebook's own allocator, with its
    /// files brought along -- and still nothing on disk.
    button("pdfio_ops_duplicate")->click();
    PdfNotebookOps::PageEdits edits = dialog.edits();
    QCOMPARE(edits.pages.size(), 4);
    QCOMPARE(edits.sources.size(), 1);
    QCOMPARE(edits.pages.at(1).kraFile, PdfSession::pageFileNameForNumber(4));
    QVERIFY(edits.pages.at(1).kraFile != manifest.pages.at(0).kraFile);
    QCOMPARE(edits.pages.at(1).index, manifest.pages.at(0).index);
    /// Every file the copy needs is listed against the name it will have, and every source is a file
    /// that is really there: the artifact, and the preview when the page has one.
    bool carriesTheArtifact = false;
    bool carriesTheLayerIndex = false;
    for (const QPair<QString, QString> &copy : edits.copyExternal) {
        if (copy.second == edits.pages.at(1).kraFile + QStringLiteral(".layers.txt")) {
            carriesTheLayerIndex = true;
            QCOMPARE(copy.first, QDir(project).filePath(manifest.pages.at(0).kraFile
                                                        + QStringLiteral(".layers.txt")));
            QVERIFY2(QFileInfo::exists(copy.first), qPrintable(copy.first));
        }
        if (copy.second == edits.pages.at(1).kraFile) {
            carriesTheArtifact = true;
            /// The artifact is there because the test made one; a preview is only a name until a save
            /// has actually drawn one, and the engine carries it when it exists.
            QVERIFY2(QFileInfo::exists(copy.first), qPrintable(copy.first));
        }
    }
    QVERIFY2(carriesTheArtifact, "the duplicate's artifact is not in the change at all");
    QVERIFY2(carriesTheLayerIndex, "the duplicate's sidecar layer index is not in the change");
    QVERIFY2(!QFileInfo::exists(QDir(project).filePath(edits.pages.at(1).kraFile)),
             "the screen wrote a file: it must only edit its own copy");
    QVERIFY(button("pdfio_ops_apply")->isEnabled());

    /// Insert and merge are edits of THIS list, so they stay available with a change pending; extract
    /// writes a notebook of its own and waits, with the reason in the hint rather than a mute button.
    QVERIFY2(button("pdfio_ops_insert")->isEnabled(),
             "insert is an edit of this notebook's list and must not wait for the change");
    QVERIFY2(button("pdfio_ops_merge_notebook")->isEnabled(),
             "a merge-in is an edit of this notebook's list and must not wait for the change");
    QVERIFY(!button("pdfio_ops_extract")->isEnabled());
    QVERIFY2(label("pdfio_ops_hint").contains(QStringLiteral("Extract writes a notebook of its own")),
             qPrintable(label("pdfio_ops_hint")));

    /// Turning the open page: the record carries the turn, the preview goes with the change, and the
    /// summary names it.
    table->selectRow(0);
    button("pdfio_ops_turn_right")->click();
    edits = dialog.edits();
    QCOMPARE(edits.pages.at(0).extraRotation, 90);
    QVERIFY(edits.removeAfter.contains(manifest.pages.at(0).thumbFile));
    QVERIFY2(label("pdfio_ops_summary").contains(QStringLiteral("turned")),
             qPrintable(label("pdfio_ops_summary")));

    /// Deleting a page keeps it in the list, struck through, until Apply -- and Keep page brings it
    /// back, which is what makes a mistake here cost one click rather than a Cancel.
    table->selectRow(3);
    button("pdfio_ops_delete_page")->click();
    /// Still listed -- struck through -- so the mistake is visible and one click from being undone.
    QCOMPARE(table->rowCount(), 4);
    QCOMPARE(dialog.edits().pages.size(), 3);
    QVERIFY(dialog.edits().removeAfter.contains(manifest.pages.at(2).kraFile));
    QVERIFY(dialog.edits().removeAfter.contains(manifest.pages.at(2).kraFile + QStringLiteral(".layers")));
    QVERIFY(dialog.edits().removeAfter.contains(manifest.pages.at(2).kraFile + QStringLiteral(".layers.txt")));
    QVERIFY2(label("pdfio_ops_hint").contains(QStringLiteral("Keep page")),
             qPrintable(label("pdfio_ops_hint")));
    button("pdfio_ops_keep_page")->click();
    QCOMPARE(dialog.edits().pages.size(), 4);
    QVERIFY(!dialog.edits().removeAfter.contains(manifest.pages.at(2).kraFile));
    QVERIFY(!dialog.edits().removeAfter.contains(manifest.pages.at(2).kraFile + QStringLiteral(".layers")));
    QVERIFY(!dialog.edits().removeAfter.contains(manifest.pages.at(2).kraFile + QStringLiteral(".layers.txt")));

    /// The insert button is a real edit: given a PDF to add, its pages join THIS list and the PDF is
    /// named in the change, so it commits with everything else rather than as its own operation.
    {
        PdfNotebookOpsDialog withPdf(project, manifest, 0);
        const QString addedPdf = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-rotations.pdf");
        PdfNotebookOpsDialog::SourceAdder adder;
        adder.pickAndRead = [&addedPdf](PdfNotebookOpsDialog::PdfToAdd *pdf) {
            pdf->path = addedPdf;
            pdf->displayedSizes << QSizeF(300, 400) << QSizeF(300, 400);
            return true;
        };
        adder.askRange = [](int, int *first, int *count) {
            *first = 0;
            *count = 2;
            return true;
        };
        withPdf.setSourceAdder(adder);
        withPdf.findChild<QPushButton *>(QStringLiteral("pdfio_ops_insert"))->click();

        const PdfNotebookOps::PageEdits withInsertion = withPdf.edits();
        QCOMPARE(withInsertion.pages.size(), 5);
        QCOMPARE(withInsertion.additions.size(), 1);
        QCOMPARE(withInsertion.additions.at(0), addedPdf);
        /// source sources.size() names the first addition, and the pages say so.
        QCOMPARE(withInsertion.pages.at(1).source, 1);
        QCOMPARE(withInsertion.pages.at(1).index, 0);
        QCOMPARE(withInsertion.pages.at(2).source, 1);
        QCOMPARE(withInsertion.pages.at(2).index, 1);
        QCOMPARE(withInsertion.pages.at(1).sizePt, QSizeF(300, 400));
        /// Named from the notebook's allocator, so nothing lands on a page the notebook has.
        QCOMPARE(withInsertion.pages.at(1).kraFile, PdfSession::pageFileNameForNumber(4));
        QVERIFY2(!QFileInfo::exists(QDir(project).filePath(withInsertion.pages.at(1).kraFile)),
                 "the screen wrote a file for the inserted page");
        QVERIFY(withPdf.findChild<QPushButton *>(QStringLiteral("pdfio_ops_apply"))->isEnabled());
    }

    /// And the merge button: another notebook's pages arrive with their files named in the change.
    {
        const QString otherDir = dir.filePath(QStringLiteral("incoming"));
        PopplerRenderBackend otherBackend;
        const PdfSessionManifest otherManifest = PdfSession::createProject(
            otherDir, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-rotations.pdf"), otherBackend);
        QVERIFY2(otherManifest.isValid(), "the notebook this test merges in could not be made");

        PdfNotebookOpsDialog merging(project, manifest, 0);
        PdfNotebookOpsDialog::NotebookMerger merger;
        merger.pickNotebook = [&otherDir, &otherManifest](
                                  PdfNotebookOpsDialog::NotebookToMerge *notebook) {
            notebook->dir = otherDir;
            notebook->manifest = otherManifest;
            return true;
        };
        merging.setNotebookMerger(merger);
        merging.findChild<QPushButton *>(QStringLiteral("pdfio_ops_merge_notebook"))->click();

        const PdfNotebookOps::PageEdits withMerge = merging.edits();
        QCOMPARE(withMerge.pages.size(), 3 + otherManifest.pages.size());
        QCOMPARE(withMerge.additions.size(), 1);
        QCOMPARE(withMerge.additions.at(0), QDir(otherDir).filePath(otherManifest.sourceFile));
        /// After the page the selection is on, which is row 1 in a fresh screen.
        QCOMPARE(withMerge.pages.at(1).source, 1);
        QCOMPARE(withMerge.pages.at(1).index, otherManifest.pages.at(0).index);
        QCOMPARE(withMerge.pages.at(1).sizePt, otherManifest.pages.at(0).sizePt);
        QVERIFY2(!withMerge.copyExternal.isEmpty(), "the incoming page's files are not in the change");
        bool carriesLayerIndex = false;
        for (const QPair<QString, QString> &copy : withMerge.copyExternal) {
            if (copy.second == withMerge.pages.at(1).kraFile + QStringLiteral(".layers.txt")) {
                carriesLayerIndex = true;
                QCOMPARE(copy.first, QDir(otherDir).filePath(otherManifest.pages.at(0).kraFile
                                                              + QStringLiteral(".layers.txt")));
            }
        }
        QVERIFY2(carriesLayerIndex, "the merged page's sidecar layer index is not in the change");
        QVERIFY2(!withMerge.copyExternalDirs.isEmpty(), "the incoming sidecar is not in the change");
    }

    /// A one-page notebook cannot lose its only page, and says so.
    PdfSessionManifest single = manifest;
    single.pages.removeLast();
    single.pages.removeLast();
    QCOMPARE(single.pages.size(), 1);
    PdfNotebookOpsDialog only(project, single, 0);
    auto *deleteOnly = only.findChild<QPushButton *>(QStringLiteral("pdfio_ops_delete_page"));
    auto *hintOnly = only.findChild<QLabel *>(QStringLiteral("pdfio_ops_hint"));
    QVERIFY(deleteOnly && hintOnly);
    QVERIFY(!deleteOnly->isEnabled());
    QVERIFY2(hintOnly->text().contains(QStringLiteral("at least one page")),
             qPrintable(hintOnly->text()));

    /// Reject after building a composite page-list change: the dialog itself never writes its
    /// pending copy. The plugin's separate preflight save is outside this dialog-only test.
    dialog.reject();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QFile manifestAfterFile(PdfSession::manifestPath(project));
    QVERIFY(manifestAfterFile.open(QIODevice::ReadOnly));
    QCOMPARE(manifestAfterFile.readAll(), manifestBefore);
    const QString pendingDuplicate = QDir(project).filePath(PdfSession::pageFileNameForNumber(4));
    QVERIFY(!QFileInfo::exists(pendingDuplicate));
    QVERIFY(!QFileInfo::exists(pendingDuplicate + QStringLiteral(".layers")));
    QVERIFY(!QFileInfo::exists(pendingDuplicate + QStringLiteral(".layers.txt")));
}

/**
 * Rolling back to the window that holds a freely rotated page leaves nothing of the window left
 * behind.
 *
 * Reported from the tablet: "it is as if the layout does not clear the background completely. It
 * happens when I scroll the tilted page 1 down, switch to the strip of pages 3-8, then switch back,
 * and find that part of THAT one is intruding into the current one."
 *
 * The image is the same size in both windows by design -- the layout sizes it from the tallest
 * window of the whole notebook -- so the roll runs and every slot's cell is wiped. What that wipe
 * did not reach was a PAPER layer's own content from the window before: a paper was wiped only over
 * the band it owned NOW, and moving back to the window holding the big rotated page pushes every
 * band below it down. The top of each paper's previous band was then left above its new one -- in
 * the cell of the slot below, whose paper layer sits underneath -- and drawn over the page that was
 * supposed to be there.
 *
 * The page also carries a second content layer kind, an inserted image, so the test proves the
 * content layers -- wiped over every slot's cell, and the cells tile the whole image -- really are
 * cleared, and that what leaked was only the paper.
 */
void PdfNavigatorIntegrationTest::testRollingBackToARotatedPageLeavesNoStripBehind()
{
    /// The user's notebook, built rather than opened from a fixture so the geometry is exactly the
    /// reported one: eighteen same-size letter pages, page 1 set down at 221 degrees, so its box is
    /// 2726x2776 px against 1700x2200 for the rest at 200 dpi.
    /// The project lives under the run's own temporary root, beside every other test's, rather
    /// than in a directory of its own that dies when this function returns: a project removed while
    /// the navigator still knows about it is a state no other test leaves behind, and the next
    /// test's openNotebook() writes the page being replaced before it adopts its own notebook.
    const QString project = m_dir.filePath(QStringLiteral("stale-strip-project"));
    const QString source = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(source), qPrintable(source));

    PopplerRenderBackend backend;
    QString why;
    PdfSessionManifest manifest = PdfSession::createProject(project, source, backend, &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    /// Every page the same size, and only page 1 turned. The fixture's Letter page is the source of
    /// all of them, so nothing but the angle can make one window different from another.
    /// The fixture's own geometry cycle: MANY_GEOMETRY[2] is ((0, 0, 612, 792), 0), a letter
    /// sheet, so every page below is the same size out of the same source page.
    const int letterPage = 2;
    const int pages = 18;
    manifest.pages.clear();
    for (int i = 0; i < pages; ++i) {
        PdfPageRecord page;
        page.index = letterPage;
        page.sizePt = QSizeF(612, 792);
        page.kraFile = PdfSession::pageFileName(i);
        page.thumbFile = PdfSession::thumbFileName(i);
        page.extraRotation = i == 0 ? 221 : 0;
        manifest.pages.append(page);
    }
    manifest.refreshNextPageNumber();
    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    QVERIFY2(manifest.writeTo(PdfSession::manifestPath(project), &why), qPrintable(why));

    navigator()->setScope(5);
    QVERIFY2(navigator()->openNotebookDir(project, &why), qPrintable(why));
    QCOMPARE(navigator()->pageCount(), pages);
    QCOMPARE(navigator()->currentIndex(), 0);

    const PdfSessionManifest opened = navigator()->manifest();
    QCOMPARE(opened.pages.at(0).extraRotation, 221);

    /// The window holding the tilted page and the one the user switched to. They need DIFFERENT
    /// sizes -- the tilted page's box is the bigger one -- which is what the roll resizes the
    /// document between, and their bands do not line up either, which is what the rest of this test
    /// is about.
    const PdfStripLayout holding = PdfStripLayout::forWindow(opened, 0, 5, 200.0);
    const PdfStripLayout away = PdfStripLayout::forWindow(opened, 8, 5, 200.0);
    QVERIFY(holding.isValid());
    QVERIFY(away.isValid());
    QVERIFY2(holding.imageSize().width() > away.imageSize().width()
                 && holding.imageSize().height() > away.imageSize().height(),
             "the two windows have to need different sizes or this test says nothing about a resize");
    QCOMPARE(holding.slots().at(0).page, 0);
    QVERIFY2(holding.slots().at(0).rect.height() > away.slots().at(0).rect.height(),
             "the tilted page is not the tall one, so this test would prove nothing");
    QVERIFY2(holding.slots().at(1).cell.top() != away.slots().at(1).cell.top(),
             "the two windows have the same bands, so this test would prove nothing");

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    KisImageSP strip = document->image();
    QVERIFY(strip);
    QCOMPARE(QSize(strip->width(), strip->height()), holding.imageSize());
    QVERIFY2(strip->width() > qRound(612.0 * 200.0 / 72.0),
             "the strip was not built wide enough for the turned page's box");

    /// The Ink group, and a second content layer kind inside it: the inserted image the user's own
    /// page carries. The first roll writes it into the page's artifact; the second has to bring it
    /// back as its own layer, at its own place, and clear it everywhere else.
    KisNodeSP inkGroup;
    for (quint32 i = 0; i < strip->root()->childCount(); ++i) {
        KisNodeSP child = strip->root()->at(i);
        if (qobject_cast<KisGroupLayer *>(child.data())) {
            inkGroup = child;
            break;
        }
    }
    QVERIFY2(inkGroup, "the strip has no Ink group");

    const QRect picture(40, 60, 48, 48);
    KisPaintLayerSP inserted = new KisPaintLayer(strip, QStringLiteral("Inserted image"),
                                                 OPACITY_OPAQUE_U8);
    inserted->paintDevice()->fill(QRect(QPoint(0, 0), picture.size()),
                                  KoColor(QColor(255, 0, 0), strip->colorSpace()));
    inserted->setX(picture.x());
    inserted->setY(picture.y());
    QVERIFY2(strip->addNode(inserted, inkGroup), "the picture could not be added to the Ink group");

    document->setModified(true);
    Q_EMIT strip->sigImageModified();

    /// Away: the window moves off the tilted page, writing every page it held -- the inserted image
    /// among them -- and the document SHRINKS to the window it moved to. That shrink is the memory
    /// the user watched climb: the tilted page's box was in every layer until this happened.
    QVERIFY2(navigator()->showPage(8, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 8);
    QVERIFY2(QFileInfo::exists(artifactFor(0)), "the roll did not write the tilted page");
    QCOMPARE(QSize(navigator()->currentDocument()->image()->width(),
                   navigator()->currentDocument()->image()->height()),
             away.imageSize());

    /// And the resize must not leave the strip looking like a document to write. Krita's own
    /// cropImage()/resizeImage() go through KisProcessingApplicator, which pushes an undo command
    /// and marks the document modified -- and Krita then autosaves it: on the tablet the tab gained
    /// an asterisk, the status bar said "Saving Document... 76%", and that save is the prime suspect
    /// for the memory the user watched climb after every roll. The strip is a view of the notebook,
    /// and the roll wrote every page it holds, so it is clean.
    QVERIFY2(!navigator()->currentDocument()->isModified(),
             "the roll left the strip looking modified, which is what Krita autosaves");

    /// And back. This second roll is the one the report is about, and it grows the document again.
    QVERIFY2(navigator()->showPage(0, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);
    QCOMPARE(QSize(navigator()->currentDocument()->image()->width(),
                   navigator()->currentDocument()->image()->height()),
             holding.imageSize());

    /// The document is the same one: a roll moves the window, it does not rebuild it. Without this
    /// the assertions below could be reading a freshly built strip and pass without proving
    /// anything.
    QVERIFY(navigator()->currentDocument() == document);
    /// .data() on both sides: image() hands back a KisImageWSP (weak) and strip is a KisImageSP
    /// (shared), and there is no single overload for comparing the two. The question is identity --
    /// the same image, not a rebuilt one -- and the raw pointers ask exactly that.
    QVERIFY(navigator()->currentDocument()->image().data() == strip.data());

    /// Every paper layer is exactly the band it owns now: the desk colour over the whole cell, the
    /// page inside it, and nothing anywhere else. A row of the previous window left outside the new
    /// band is what the user saw as the other strip intruding.
    ///
    /// The cell is compared as the layout lays it out, NOT clipped to the image. Slot 0's band
    /// starts half a gap above the image -- cell y is -56 -- so that the bands meet in the middle of
    /// the gap, and a paint device keeps those rows even though the image never shows them (the
    /// reported failure is exactly that: painted 0,-56 2726x2888, which IS slot 0's cell). Clipping
    /// the expectation would fail on a paper that is painted perfectly. Equality is also the
    /// strictest form of the check the test exists for: a stale band outside the cell, a band one
    /// row short, or the wrong slot's paper all fail it.
    QList<KisPaintLayer *> papers;
    for (quint32 i = 0; i < strip->root()->childCount(); ++i) {
        KisNodeSP child = strip->root()->at(i);
        if (child->name() == QStringLiteral("Desk") || child == inkGroup) {
            continue;
        }
        if (KisPaintLayer *paper = qobject_cast<KisPaintLayer *>(child.data())) {
            papers.append(paper);
        }
    }
    QCOMPARE(papers.size(), holding.slots().size());

    for (int i = 0; i < papers.size(); ++i) {
        const QRect ownedNow = holding.slots().at(i).cell;
        const QRect painted = papers.at(i)->paintDevice()->exactBounds();
        QVERIFY2(painted == ownedNow,
                 qPrintable(QStringLiteral("the paper of slot %1 is painted %2,%3 %4x%5, where the "
                                           "band it owns now is %6,%7 %8x%9")
                                .arg(i)
                                .arg(painted.x()).arg(painted.y())
                                .arg(painted.width()).arg(painted.height())
                                .arg(ownedNow.x()).arg(ownedNow.y())
                                .arg(ownedNow.width()).arg(ownedNow.height())));
    }

    /// And there really is a second content layer kind to clear: the inserted image came back as
    /// its own layer, in its own place on the tilted page, and nowhere else.
    KisPaintLayer *insertedBack = nullptr;
    for (quint32 i = 0; i < inkGroup->childCount(); ++i) {
        KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(inkGroup->at(i).data());
        if (layer && layer->name() == QStringLiteral("Inserted image")) {
            insertedBack = layer;
            break;
        }
    }
    QVERIFY2(insertedBack, "the inserted image did not come back as a layer of its own");

    const QRect tiltedSlot = holding.slots().at(0).rect;
    const QRect expectedPicture(tiltedSlot.topLeft() + picture.topLeft(), picture.size());
    QVERIFY2(insertedBack->paintDevice()->exactBounds() == expectedPicture,
             qPrintable(QStringLiteral("the inserted image is at %1,%2 %3x%4, not at its own place "
                                       "%5,%6 %7x%8 in the page's slot")
                            .arg(insertedBack->paintDevice()->exactBounds().x())
                            .arg(insertedBack->paintDevice()->exactBounds().y())
                            .arg(insertedBack->paintDevice()->exactBounds().width())
                            .arg(insertedBack->paintDevice()->exactBounds().height())
                            .arg(expectedPicture.x()).arg(expectedPicture.y())
                            .arg(expectedPicture.width()).arg(expectedPicture.height())));

    const QImage picturePixels =
        insertedBack->paintDevice()->convertToQImage(0, expectedPicture);
    QVERIFY(!picturePixels.isNull());
    const QColor at = picturePixels.pixelColor(QPoint(picture.width() / 2, picture.height() / 2));
    QVERIFY2(at.red() > 200 && at.green() < 60 && at.blue() < 60,
             qPrintable(QStringLiteral("the inserted image came back as rgb(%1,%2,%3)")
                            .arg(at.red()).arg(at.green()).arg(at.blue())));

    /// And the tab is closed the way every test that leaves one open closes it: a view nobody
    /// closed takes the main window down with it in the teardown. Nothing was drawn that is not on
    /// disk -- the roll wrote the window -- so closing asks nothing.
    ///
    /// Drained on both sides of the close, for longer than the Layers docker's node model
    /// compresses a dummy change. That model calls m_d->indexConverter without a guard when the
    /// queue fires (libs/ui/kis_node_model.cpp:440), and setDummiesFacade deletes and nulls that
    /// converter without stopping the compressor or clearing the queue (:316, :144-152) -- only
    /// slotBeginRemoveDummy does, and it is marked FIXME (:383). So a node update still in the air
    /// when the document goes away crashes from whatever nested event loop is running, and
    /// KisImage::waitForDone() spins one through KisBusyWaitBroker -> KisDelayedSaveDialog. This
    /// test runs its strokes and rolls in one image and then closes it, which is the most likely
    /// place for that queue to be non-empty at close; letting it run while the model is still
    /// attached, and again after the close, is what keeps this test from handing that work to
    /// whatever runs next. It is not a fix for the Krita defect -- the missing guard is.
    QApplication::processEvents();
    QTest::qWait(200);
    document->setModified(false);
    navigator()->setScope(1);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * A window move as a whole, as a flow: write every page the window holds, then redraw the whole
 * window from what was written.
 *
 * The strip opens at scope five over a fifty-page source -- the ordinary fixture is three pages,
 * and a window of five over three never has anywhere to go -- and a mark is placed inside a page
 * that will STAY in the window when it moves. Turning to a page outside the window is what rolls
 * it. Afterwards:
 *
 *  - every page the old window held has its artifact on disk: the roll writes all five, not only
 *    the two whose band was about to leave,
 *  - the mark is in the artifact of the page it was drawn on, and
 *  - the mark is ON SCREEN in the new window, in a slot whose page did not change.
 *
 * The third is what fails without the redraw-from-saved flow. The roll wipes every slot it
 * repaints, so a mark visible in a slot whose page did not change can only have come back out of
 * the artifact -- the incremental roll kept such slots' pixels in memory instead, and this
 * asserts the pixels came from the disk.
 */
void PdfNavigatorIntegrationTest::testRollWritesEveryWindowPageAndRedrawsFromDisk()
{
    /// A source long enough that a five-page window has somewhere to roll to.
    const QString usualFixture = m_fixture;
    m_fixture = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));
    navigator()->setScope(5);

    QVERIFY(useNotebook(QStringLiteral("roll"), 5));

    /// The copy is made; the swap has done its job. Restored here instead of at the end so a
    /// failure inside this test cannot leave the next one opening a fifty-page notebook nobody
    /// asked for: the swap is this test's business and nothing else's.
    m_fixture = usualFixture;

    QVERIFY(navigator()->pageCount() >= 8);
    QCOMPARE(navigator()->currentIndex(), 0);

    /// Page 1 is inside the window the open built, so asking for it activates its slot rather
    /// than rolling: the window under test is the one page 0 opened -- pages 0..4.
    QString why;
    QVERIFY2(navigator()->showPage(1, &why), qPrintable(why));

    const PdfStripLayout before = PdfStripLayout::forWindow(navigator()->manifest(), 0, 5, 200.0);
    QVERIFY(before.isValid());
    const int staying = 3;
    const int slot = before.slotForPage(staying);
    QVERIFY(slot >= 0);

    /// A mark inside the page that will stay, together with the change signal a stroke would
    /// bring: the roll's clean pass reads the marks that signal raises, so the test drives them
    /// the same way the canvas does -- pixels placed directly, the signal invoked directly.
    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    KisPaintLayer *layer = stripInkLayer(document->image());
    QVERIFY2(layer, "the strip has no layer called Ink to draw on");
    const QRect mark(before.slots().at(slot).rect.topLeft() + QPoint(8, 8), QSize(24, 24));
    layer->paintDevice()->fill(mark, KoColor(QColor(0, 0, 0), document->image()->colorSpace()));
    document->setModified(true);
    Q_EMIT document->image()->sigImageModified();

    /// The marks cover every page the window holds -- the roll has all of them to clear.
    QVERIFY(!navigator()->pageWindow().dirtyPages().isEmpty());

    /// And the roll: page 5 is outside the window [0..4], so this is a window move and not a
    /// turn to a page already in the strip.
    QVERIFY2(navigator()->showPage(5, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 5);

    /// Every page the old window reached the disk -- all five, the three whose band was not
    /// going anywhere included. The write has LANDED by the time the roll returns; that wait is
    /// the pipeline the flow rests on.
    for (int page = 0; page < 5; ++page) {
        QVERIFY2(QFileInfo::exists(artifactFor(page)),
                 qPrintable(QStringLiteral("page %1 was not written by the roll").arg(page + 1)));
    }

    /// The mark is in the artifact of the page it was drawn on...
    const QImage saved = PdfInkLoader::loadInk(artifactFor(staying));
    QVERIFY2(inkMarkInImage(saved), "the mark did not reach the artifact of the page that stayed");

    /// ...and on screen, in a slot the roll wiped and repainted. The new window is five pages
    /// centred on 5, so it starts at page 3: this page did not change bands, and what shows in
    /// its slot can only be what was read back out of the artifact above.
    const PdfStripLayout after = PdfStripLayout::forWindow(navigator()->manifest(), 5, 5, 200.0);
    const int afterSlot = after.slotForPage(staying);
    QVERIFY(afterSlot >= 0);
    KisPaintLayer *redrawn = stripInkLayer(navigator()->currentDocument()->image());
    QVERIFY(redrawn);
    const QImage onScreen = redrawn->paintDevice()->convertToQImage(
        0, QRect(after.slots().at(afterSlot).rect.topLeft(),
                 after.slots().at(afterSlot).rect.size()));
    QVERIFY2(inkMarkInImage(onScreen),
             "the mark did not come back from the artifact into the redrawn slot");

    /// And the window is clean again: every mark the signal raised was cleared by the write that
    /// put it on disk -- the clean pass the redraw is allowed to run behind.
    QVERIFY2(navigator()->pageWindow().dirtyPages().isEmpty(),
             "the roll finished with pages still marked unsaved");

    /// Back to the shipped default -- and the tab closed, the way the closing test closes it.
    ///
    /// This test runs last, and teardown destroys the main window with whatever the tests left
    /// in it: a view nobody closed took that window down on its way out (a null canvas resource
    /// provider inside KoToolManager, reached from KisView's destructor), which is why the
    /// closing test leaves nothing open and this one now does the same. The document is marked
    /// clean first on purpose: its ink IS on disk -- the roll wrote it, and nothing was drawn
    /// since -- so there is nothing to ask about and nothing to write on the way out.
    navigator()->setScope(1);
    KisDocument *finalDocument = navigator()->currentDocument();
    QVERIFY(finalDocument);
    finalDocument->setModified(false);

    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(50);
}

/**
 * Quitting, which does not have to close the view first: the navigator hangs the same save on the
 * application's own aboutToQuit, so ink drawn and never turned away from is still written.
 *
 * The signal is invoked directly -- it is the one Krita emits on the way out -- rather than
 * calling qApp->quit(), which would tear the test process down with it.
 */
void PdfNavigatorIntegrationTest::testQuittingWritesTheInkToo()
{
    QVERIFY(useNotebook(QStringLiteral("quit")));

    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    const QString artifact = artifactFor(0);
    QVERIFY2(!QFileInfo::exists(artifact),
             "the page was written before anything asked for it to be");

    const QStringList homeKraBefore = homeKraFiles();
    const int promptsBefore = m_savePrompts;

    QVERIFY(QMetaObject::invokeMethod(qApp, "aboutToQuit"));
    QApplication::processEvents();

    QCOMPARE(m_savePrompts, promptsBefore);

    const QImage saved = PdfInkLoader::loadInk(artifact);
    QVERIFY2(!saved.isNull(), qPrintable(artifact));
    QVERIFY2(inkMarkInImage(saved), "the ink was not written on the way out");
    QCOMPARE(homeKraFiles(), homeKraBefore);

    /// And the document is clean afterwards, so whatever closes it next has nothing to ask about.
    QCOMPARE(navigator()->currentDocument(), page);
    QVERIFY(!page->isModified());
}

/**
 * A swipe across a page turns it, and the buttons and the gesture agree about what turning is.
 *
 * Turning a page is the common case on this screen and a button is a small thing to find on a
 * tablet, so the gesture is the one worth having. What is asserted is that it is the SAME change:
 * one quarter turn, recorded on the row that was swiped. A screen where two ways to turn a page
 * disagree is worse than a screen with one, and a screen where a slipped click turns a page is worse
 * than either.
 */
void PdfNavigatorIntegrationTest::testSwipingAPageTurnsIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("swipe"));

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QCOMPARE(manifest.pages.size(), 3);

    PdfNotebookOpsDialog dialog(project, manifest, 0);

    /// Shown, because a swipe is a position: a table that has never been laid out has no row under a
    /// point, and the gesture would then be tested against a geometry the user never sees.
    dialog.show();
    QTest::qWait(50);

    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY2(table, "the screen has no page list to swipe");
    QWidget *viewport = table->viewport();
    QVERIFY2(table->visualRect(table->model()->index(1, 0)).isValid(),
             "the page list has no laid-out row to swipe");

    const auto swipe = [table, viewport](int row, int dx, int dy) {
        const QPoint from = table->visualRect(table->model()->index(row, 0)).center();
        QTest::mousePress(viewport, Qt::LeftButton, Qt::KeyboardModifiers(), from);
        QTest::mouseRelease(viewport, Qt::LeftButton, Qt::KeyboardModifiers(),
                            from + QPoint(dx, dy));
    };

    /// A swipe to the right turns the page right, on the row it was made on and no other.
    swipe(1, 90, 0);
    QCOMPARE(dialog.edits().pages.at(1).extraRotation, 90);
    QCOMPARE(dialog.edits().pages.at(0).extraRotation, 0);
    QCOMPARE(dialog.edits().pages.at(2).extraRotation, 0);

    /// The other direction is the other quarter turn, on the same row again.
    swipe(0, -90, 0);
    QCOMPARE(dialog.edits().pages.at(0).extraRotation, 270);

    /// A movement too small to be a gesture is a slipped click: it selects, and turns nothing.
    swipe(2, 10, 0);
    QCOMPARE(dialog.edits().pages.at(2).extraRotation, 0);

    /// Nor is a drag down the list, which is how a list is scrolled with a finger.
    swipe(2, 10, 220);
    QCOMPARE(dialog.edits().pages.at(2).extraRotation, 0);

    /// And the button turns exactly what the gesture turns: one quarter turn, one place it happens.
    table->selectRow(2);
    QPushButton *turnRight =
        dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_turn_right"));
    QVERIFY2(turnRight, "the screen has no turn button to compare the gesture with");
    turnRight->click();
    QCOMPARE(dialog.edits().pages.at(2).extraRotation, 90);
}

namespace {

/**
 * The rectangle the page's card occupies on the canvas: every pixel that is not the canvas backdrop.
 *
 * The backdrop is read off the top-left corner, which the card never reaches, so a card that was
 * really turned measures as a different shape rather than only being described differently. That is
 * the difference between "the preview was rotated" and "a number changed".
 */
QRect cardOnCanvas(const QImage &image)
{
    if (image.isNull()) {
        return QRect();
    }

    const QColor backdrop = image.pixelColor(0, 0);
    const auto isBackdrop = [&backdrop](const QColor &pixel) {
        return qAbs(pixel.red() - backdrop.red()) <= 8
            && qAbs(pixel.green() - backdrop.green()) <= 8
            && qAbs(pixel.blue() - backdrop.blue()) <= 8;
    };

    int x0 = image.width();
    int y0 = image.height();
    int x1 = -1;
    int y1 = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (isBackdrop(image.pixelColor(x, y))) {
                continue;
            }
            x0 = qMin(x0, x);
            y0 = qMin(y0, y);
            x1 = qMax(x1, x);
            y1 = qMax(y1, y);
        }
    }
    return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
}

} // namespace

/**
 * The canvas turns the selected page by an angle no button can name, as a pending edit.
 *
 * The buttons can only mean a quarter turn and a swipe carries a direction rather than an angle, so
 * the canvas is where a page is set down at an angle like 135 degrees -- not a right angle by any
 * reading. A gesture is a PREVIEW while the hand is down -- the picture and the readout turn, and
 * nothing else -- and lands as ONE pending edit on release, so a whole drag is one change and one
 * undo takes it back. What is asserted is that this pending edit is the same kind as every other:
 * edits() carries the angle, the manifest on disk does not, and Cancel leaves the notebook byte for
 * byte what it was. The preview is really turned too, which is why the card's SHAPE on the canvas is
 * measured rather than only the number beside it.
 *
 * The gesture is analog, so the angle it lands on is compared with the readout and with the list
 * rather than with a literal degree: an arbitrary pixel drag cannot promise one, and making it snap
 * to a right angle is exactly what this screen must not do. The exact quarter turn is the buttons'
 * contract, and it is asserted here through the button.
 */
void PdfNavigatorIntegrationTest::testTheCanvasTurnsTheSelectedPageByAnyAngle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("canvas"));

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QCOMPARE(manifest.pages.size(), 3);
    QVERIFY2(!manifest.pages.at(0).thumbFile.isEmpty(), "the first page records no preview name");

    /// A preview to turn: a wide card with a red block at its left end, so a card that was really
    /// rotated has a different shape and different pixels from one whose number changed.
    /// PdfSession::createProject() names a preview but draws none until a save has happened, so the
    /// test draws this one itself.
    QImage card(160, 40, QImage::Format_ARGB32_Premultiplied);
    card.fill(Qt::white);
    {
        QPainter painter(&card);
        painter.fillRect(QRect(0, 0, 40, 40), QColor(Qt::red));
    }
    const QString thumbPath = QDir(project).filePath(manifest.pages.at(0).thumbFile);
    QVERIFY2(card.save(thumbPath), qPrintable(thumbPath));

    QFile manifestBeforeFile(PdfSession::manifestPath(project));
    QVERIFY(manifestBeforeFile.open(QIODevice::ReadOnly));
    const QByteArray manifestBefore = manifestBeforeFile.readAll();

    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *canvas = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_canvas"));
    QVERIFY2(canvas, "the ops screen has no canvas to turn a page on");
    auto *readout = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_angle"));
    QVERIFY2(readout, "the canvas has no live readout of the angle");
    auto *summary = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_summary"));
    QVERIFY(summary);
    auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_apply"));
    QVERIFY(apply);
    QVERIFY2(!apply->isEnabled(), "Apply was offered before anything changed");

    /// Laid out and shown: a gesture is a position, and a canvas that was never laid out has no
    /// centre to turn around.
    dialog.show();
    QTest::qWait(50);

    /// The gesture as the widget receives it: press, move, release. Sent straight to the canvas
    /// rather than through the window system, so the positions under test are the widget's own.
    const auto press = [canvas](const QPoint &at) {
        QMouseEvent event(QEvent::MouseButtonPress, at, canvas->mapToGlobal(at),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    const auto moveTo = [canvas](const QPoint &at) {
        QMouseEvent event(QEvent::MouseMove, at, canvas->mapToGlobal(at),
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    const auto releaseAt = [canvas](const QPoint &at) {
        QMouseEvent event(QEvent::MouseButtonRelease, at, canvas->mapToGlobal(at),
                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };

    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);
    /// The Turn column, in the order the table builds them: thumbnail, position, source, size, turn,
    /// ink.
    constexpr int TurnColumn = 4;

    /// Before anything is turned the page's own preview is on the canvas, upright: the card is wide,
    /// and the page's own shape is portrait, so a canvas that ignored the preview could not be
    /// showing this.
    const QRect uprightCard = cardOnCanvas(canvas->grab().toImage());
    QVERIFY2(uprightCard.isValid(), "the canvas drew nothing at all");
    QVERIFY2(uprightCard.width() > uprightCard.height(),
             "the canvas is not showing the preview the page records");

    /// The point the widget measures its angles from: the same centre its own painting uses.
    /// Driving the gesture from anywhere else adds a degree or two to both of its ends -- an earlier
    /// run of this test pressed from QRect::center(), half a pixel away from the centre the widget
    /// turns around, and landed on 92 degrees while asking for 90. That is what an analog gesture
    /// does; this test is about what the gesture records, not about an exact degree from a pixel
    /// drag.
    const QPointF hub = QRectF(canvas->rect()).center();
    const QPoint threeOClock = (hub + QPointF(100, 0)).toPoint();
    const QPoint sixOClock = (hub + QPointF(0, 100)).toPoint();

    /// One gesture, a quarter around that centre: taken hold of at three o'clock and let go at six.
    /// While the hand is down it is a PREVIEW: the drawn page has already turned and the readout has
    /// already followed it, while the record, the list and the pending change are exactly as they
    /// were.
    const QString standing = readout->text();
    press(threeOClock);
    moveTo(sixOClock);

    QCOMPARE(dialog.edits().pages.at(0).extraRotation, 0);
    QVERIFY2(!apply->isEnabled(), "a gesture in progress was treated as a pending edit");
    QVERIFY2(readout->text() != standing, "the readout did not follow the hand");
    QVERIFY(table->item(0, TurnColumn));
    QVERIFY2(table->item(0, TurnColumn)->text().contains(QStringLiteral("as it is")),
             qPrintable(table->item(0, TurnColumn)->text()));
    const QRect previewCard = cardOnCanvas(canvas->grab().toImage());
    QVERIFY2(previewCard.isValid() && previewCard.height() > previewCard.width(),
             "the preview did not turn while the hand was still down");

    /// The hand is up: now it is ONE pending edit -- one change in the summary, so one undo takes
    /// the whole turn back -- and nothing has been written. The gesture is analog, so what is
    /// asserted is that the readout, the list and the pending edit all say the SAME angle, and that
    /// this drag really is the quarter turn it looks like.
    releaseAt(sixOClock);
    const int quarter = dialog.edits().pages.at(0).extraRotation;
    QVERIFY2(readout->text().contains(QString::number(quarter)),
             "the readout and the pending edit disagree");
    QVERIFY2(table->item(0, TurnColumn)->text().contains(QString::number(quarter)),
             qPrintable(table->item(0, TurnColumn)->text()));
    QVERIFY2(qAbs(quarter - 90) <= 4,
             qPrintable(QStringLiteral("a drag from three o'clock to six landed at %1").arg(quarter)));
    QVERIFY2(apply->isEnabled(), "a turn made on the canvas is not a pending edit");
    QVERIFY2(summary->text().contains(QStringLiteral("1 change(s) pending")),
             qPrintable(summary->text()));

    /// And an angle that is not a right angle at all: from three o'clock to half past four is 45
    /// degrees on top of what is already there, and it too is only a preview until the hand comes up.
    const QString settled = readout->text();
    const QPoint halfPastFour = (hub + QPointF(70, 70)).toPoint();
    press(threeOClock);
    moveTo(halfPastFour);
    QCOMPARE(dialog.edits().pages.at(0).extraRotation, quarter);
    QVERIFY2(readout->text() != settled, "the readout did not follow the second gesture");
    releaseAt(halfPastFour);
    const int angled = dialog.edits().pages.at(0).extraRotation;
    QVERIFY2(angled != quarter, "the second gesture recorded no turn at all");
    QVERIFY2(angled % 90 != 0, "the angle under test turned out to be a right angle");
    QVERIFY2(readout->text().contains(QString::number(angled)),
             "the readout and the pending edit disagree at an arbitrary angle");

    /// The button still adds exactly a quarter turn on top of the angle the gesture left, which is
    /// the contract the buttons have always had.
    const int afterButton = (angled + 90) % 360;
    dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_turn_right"))->click();
    QCOMPARE(dialog.edits().pages.at(0).extraRotation, afterButton);

    /// The canvas follows the selection: page 2 was never turned, so the readout says so and edits()
    /// still has it upright.
    table->selectRow(1);
    QCOMPARE(dialog.edits().pages.at(1).extraRotation, 0);
    QVERIFY2(readout->text().contains(QStringLiteral("0")), qPrintable(readout->text()));
    QCOMPARE(dialog.edits().pages.at(0).extraRotation, afterButton);

    /// Nothing has been written: the turn is pending, and the notebook on disk is as it was.
    QFile manifestAfterFile(PdfSession::manifestPath(project));
    QVERIFY(manifestAfterFile.open(QIODevice::ReadOnly));
    QCOMPARE(manifestAfterFile.readAll(), manifestBefore);

    /// Cancel writes nothing either, which is the whole promise of the screen. Apply is where the
    /// change leaves it, and the plugin then commits it as one change (the screen test above covers
    /// that half).
    dialog.reject();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QFile manifestRejectedFile(PdfSession::manifestPath(project));
    QVERIFY(manifestRejectedFile.open(QIODevice::ReadOnly));
    QCOMPARE(manifestRejectedFile.readAll(), manifestBefore);
}

/**
 * The real rotator at an angle that is not a right angle, over a page that was really drawn on.
 *
 * Every rotation test at the ops level passes a stub, so the arithmetic the stub stands in for --
 * Krita turning the ink, the turned sheet measuring the box the page says it measures, and the
 * artifact reading back at that size -- had never been run. 37 degrees is the smallest interesting
 * angle: it is not a right angle, its box is bigger than the page in both directions, and a notebook
 * that recorded it while the artifact stayed upright would put the ink on the wrong lines.
 */
void PdfNavigatorIntegrationTest::testTheRealRotatorTurnsAPageByAnAngleThatIsNotARightAngle()
{
    QVERIFY(useNotebook(QStringLiteral("real-angle")));
    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);

    /// Turning away from the page is what writes it, so there is a real artifact to turn.
    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    const QString project = navigator()->projectDir();
    const QString artifact = artifactFor(0);
    QVERIFY2(waitForInk(artifact), qPrintable(artifact));

    const QSize upright = PdfInkLoader::artifactSize(artifact, &why);
    QVERIFY2(!upright.isEmpty(), qPrintable(why));

    const PdfNotebookOps::Outcome turned =
        PdfNotebookOps::rotatePages(project, 0, 1, 37, PdfPageRotator::rotateInto, 0);
    QVERIFY2(turned.ok, qPrintable(turned.why));

    /// The record says 37, and the size the reader sees is the box the turned sheet fits in -- not
    /// the page's own size, and not the swapped size of a right angle.
    PdfSessionManifest after = PdfSessionManifest::readFrom(PdfSession::manifestPath(project), &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.at(0).extraRotation, 37);

    const QSizeF size = after.pages.at(0).sizePt;
    const QSizeF box = after.pages.at(0).displaySizePt();
    const qreal radians = qDegreesToRadians(37.0);
    const qreal cosine = qAbs(qCos(radians));
    const qreal sine = qAbs(qSin(radians));
    QVERIFY2(qAbs(box.width() - (size.width() * cosine + size.height() * sine)) < 0.01,
             qPrintable(QStringLiteral("the box is %1 wide, where the turned sheet says %2")
                            .arg(box.width())
                            .arg(size.width() * cosine + size.height() * sine)));
    QVERIFY2(qAbs(box.height() - (size.width() * sine + size.height() * cosine)) < 0.01,
             qPrintable(QStringLiteral("the box is %1 tall, where the turned sheet says %2")
                            .arg(box.height())
                            .arg(size.width() * sine + size.height() * cosine)));
    QVERIFY2(box.width() > size.width() && box.height() > size.height(),
             "a page set down at 37 degrees has to be BIGGER than its sizePt");

    /// And the page's image, reopened from the artifact the manifest names, measures the same box in
    /// the pixels it was written at: the ink really was turned with the paper.
    const QSize reopened = PdfInkLoader::artifactSize(QDir(project).filePath(after.pages.at(0).kraFile),
                                                      &why);
    QVERIFY2(!reopened.isEmpty(), qPrintable(why));
    const QSize expected(qRound(upright.width() * cosine + upright.height() * sine),
                         qRound(upright.width() * sine + upright.height() * cosine));
    QVERIFY2(qAbs(reopened.width() - expected.width()) <= 1
                 && qAbs(reopened.height() - expected.height()) <= 1,
             qPrintable(QStringLiteral("the turned page reopens as %1x%2, where the box says %3x%4")
                            .arg(reopened.width())
                            .arg(reopened.height())
                            .arg(expected.width())
                            .arg(expected.height())));
}

namespace {

/// A press, a move or a release on \a widget at \a at, sent straight to it rather than through the
/// window system so the position under test is the widget's own.
void sendMouseAt(QWidget *widget, QEvent::Type type, const QPoint &at, Qt::MouseButton button,
                 Qt::MouseButtons buttons)
{
    QMouseEvent event(type, at, widget->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

/// The middle of page row \a row in the table's viewport.
QPoint rowPoint(QTableWidget *table, int row)
{
    return table->visualRect(table->model()->index(row, 0)).center();
}

/// The order the change would write, as artifact names: the only thing a reorder is about.
QStringList pageOrder(const PdfNotebookOps::PageEdits &edits)
{
    QStringList order;
    for (const PdfPageRecord &page : edits.pages) {
        order << page.kraFile;
    }
    return order;
}

/**
 * Stops the fixture's dialog watchdog for the length of a test, and puts it back however the test
 * ends.
 *
 * The watchdog dismisses any shown QDialog, which is exactly what the screen under test is -- so
 * without this the screen is closed a few milliseconds in, and "the drop target is on screen" would
 * be a statement about a hidden widget. Nothing in these tests can raise the modal dialog the
 * watchdog exists for, so pausing it here is safe; the destructor starts it again even when a
 * QVERIFY returns early.
 */
struct WatchdogPause {
    explicit WatchdogPause(QTimer *watchdog)
        : m_watchdog(watchdog)
    {
        m_watchdog->stop();
    }
    ~WatchdogPause() { m_watchdog->start(); }

    WatchdogPause(const WatchdogPause &) = delete;
    WatchdogPause &operator=(const WatchdogPause &) = delete;

    QTimer *m_watchdog;
};

/**
 * Puts the rendered window's memory budget back however a test ends.
 *
 * The budget is PERSISTED: a test that walked away from one would change what the NEXT RUN of the
 * suite opens a notebook at, and a failure half way through the test would leave it there for good.
 * The destructor runs on every way out, a failed QVERIFY included.
 */
struct BudgetRestore {
    explicit BudgetRestore(int original)
        : m_original(original)
    {
    }
    ~BudgetRestore() { PdfPageNavigator::instance()->setMemoryBudgetMb(m_original); }

    BudgetRestore(const BudgetRestore &) = delete;
    BudgetRestore &operator=(const BudgetRestore &) = delete;

    int m_original;
};

/// How many paint layers \a image holds, however they are nested.
///
/// Each one is a full-size allocation in the strip -- the per-slot bands, the ink the pages are
/// drawn on, any content layer an artifact restored -- so this is the count a memory budget is
/// divided by. Counted here rather than asked of the navigator, so the number the budget was
/// computed with is checked against the document that was really built.
int paintLayersOf(const KisImageSP &image)
{
    if (!image) {
        return 0;
    }

    /// Not const pointers: a const KisNodeSP hands back a const KisNode* through data(), and
    /// qobject_cast refuses to cast the constness away. The walking pointers are ours.
    int count = 0;
    QList<KisNodeSP> pending;
    pending.append(image->root());
    while (!pending.isEmpty()) {
        KisNodeSP node = pending.takeLast();
        for (quint32 i = 0; i < node->childCount(); ++i) {
            KisNodeSP child = node->at(i);
            if (qobject_cast<KisPaintLayer *>(child.data())) {
                ++count;
            }
            pending.append(child);
        }
    }
    return count;
}

/// The process's resident set, in kB, from /proc/self/status -- 0 when it cannot be read.
///
/// The same figure the device's dumpsys reports, and the one the memory measurement is about: what
/// the process really holds, as opposed to what a document says it is made of.
qint64 residentKb()
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) {
        return 0;
    }

    while (!status.atEnd()) {
        const QByteArray line = status.readLine().simplified();
        if (line.startsWith("VmRSS:")) {
            const QList<QByteArray> parts = line.split(' ');
            return parts.size() > 1 ? parts.at(1).toLongLong() : 0;
        }
    }
    return 0;
}

/// Writes the mixed-orientation notebook the two memory tests use: five real Letter pages from
/// ex-manypage-50, the first and the third turned a right angle, so the window holds landscape pages
/// beside portrait ones -- the case a pixel target got wrong. \a project is created and its manifest
/// filled; the caller opens it.
bool writeMixedFivePageNotebook(const QString &project, QString *why)
{
    const QString source = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    if (!QFileInfo::exists(source)) {
        if (why) {
            *why = QStringLiteral("the fixture %1 is missing").arg(source);
        }
        return false;
    }

    PopplerRenderBackend backend;
    PdfSessionManifest manifest = PdfSession::createProject(project, source, backend, why);
    if (!manifest.isValid(why)) {
        return false;
    }
    if (manifest.pages.size() < 5) {
        if (why) {
            *why = QStringLiteral("the fixture has %1 pages, five are needed").arg(manifest.pages.size());
        }
        return false;
    }

    while (manifest.pages.size() > 5) {
        manifest.pages.removeLast();
    }
    manifest.pages[0].extraRotation = 90;
    manifest.pages[2].extraRotation = 90;
    return manifest.writeTo(PdfSession::manifestPath(project), why);
}

/// How many pixels of \a image carry any paint. The drop indicator is transparent except for the
/// held row's highlight and the insertion line, so this is the proof that it draws something.
int paintedPixels(const QImage &image)
{
    int painted = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, y).alpha() > 0) {
                ++painted;
            }
        }
    }
    return painted;
}

/// Creates a notebook from the text fixture with \a name written into its manifest -- the shape a
/// notebook has when the picker's cache copy, or a build that kept it, named the notebook. Shape, not
/// content: the naming tests open it one page at a time, so nothing here is about memory.
bool writeNamedNotebook(const QString &project, const QString &name, QString *why)
{
    const QString source = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    if (!QFileInfo::exists(source)) {
        if (why) {
            *why = QStringLiteral("the fixture %1 is missing").arg(source);
        }
        return false;
    }

    PopplerRenderBackend backend;
    PdfSessionManifest manifest = PdfSession::createProject(project, source, backend, why);
    if (!manifest.isValid(why)) {
        return false;
    }
    manifest.name = name;
    return manifest.writeTo(PdfSession::manifestPath(project), why);
}

/// How many of the image's root layers are page backgrounds, counted with the saver's own rule.
///
/// A band IS a slot in a strip: the builder makes one paper layer per page of the window, and the
/// roll re-cuts them when the count changes. So this is the number of pages the document really
/// holds, and PdfPageSaver::isPageBackground() is what decides it -- never a name check written
/// again here, which is the rule the task asks the count to be made with.
int bandCountOf(const KisImageSP &image)
{
    if (!image) {
        return 0;
    }

    int bands = 0;
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        if (PdfPageSaver::isPageBackground(image->root()->at(i))) {
            ++bands;
        }
    }
    return bands;
}

/// The root index of the Ink group, or -1. Every paper band has to stay BELOW it: a band added above
/// the ink hides the strokes on the page it covers, which is why the roll's band surgery inserts at
/// the top of the paper rather than at the top of the root.
int inkGroupIndex(const KisImageSP &image)
{
    if (!image) {
        return -1;
    }

    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        if (image->root()->at(i)->name() == QStringLiteral("Ink")) {
            return int(i);
        }
    }
    return -1;
}

/// Writes a notebook of \a pages same-size Letter (612x792 pt) pages out of the fixture's own letter
/// page: the shape the reported notebook has, and the one the strip-size memory figures are about.
bool writeLetterNotebook(const QString &project, int pages, QString *why)
{
    const QString source = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    if (!QFileInfo::exists(source)) {
        if (why) {
            *why = QStringLiteral("the fixture %1 is missing").arg(source);
        }
        return false;
    }
    if (pages < 1) {
        if (why) {
            *why = QStringLiteral("%1 pages is not a notebook").arg(pages);
        }
        return false;
    }

    PopplerRenderBackend backend;
    PdfSessionManifest manifest = PdfSession::createProject(project, source, backend, why);
    if (!manifest.isValid(why)) {
        return false;
    }

    /// The fixture's own geometry cycle: MANY_GEOMETRY[2] is a letter sheet, so every page below is
    /// the same size out of the same source page and the only thing a window can change is how many
    /// of them it holds.
    const int letterPage = 2;
    manifest.pages.clear();
    for (int i = 0; i < pages; ++i) {
        PdfPageRecord page;
        page.index = letterPage;
        page.sizePt = QSizeF(612, 792);
        page.kraFile = PdfSession::pageFileName(i);
        page.thumbFile = PdfSession::thumbFileName(i);
        manifest.pages.append(page);
    }
    manifest.refreshNextPageNumber();
    if (!manifest.isValid(why)) {
        return false;
    }
    return manifest.writeTo(PdfSession::manifestPath(project), why);
}

/// Puts the strip's page count, its on/off state AND the scope in force back, however a test ends.
///
/// Three things leak, and each needs its own half:
///
///  - pdfio/stripPages and pdfio/stripOn are PERSISTED, and a test that walked away from one would
///    change what the next RUN of the suite opens a notebook at. The destructor writes both back;
///  - the navigator is a process-wide SINGLETON that has already read those keys, so what the next
///    test sees is its in-memory scope, not the keys. setScope() is the one door that puts the count,
///    the on/off state and the window bound back, and it is called here;
///  - and the destructor runs on EVERY way out -- a failed QVERIFY included -- which is the whole
///    point of an RAII guard rather than a trailing statement in each test.
///
/// The count is restored BEFORE the scope, so a restore to "off" does not leave the test's own count
/// behind: setScope(count) records it, setScope(1) turns the strip off and keeps it. A test that
/// fails part way through leaves an open document; the first call then rolls it back to the restored
/// count, which is one window write against a suite that would otherwise be wrong for every test
/// after it.
struct StripRestore {
    StripRestore()
        : m_pages(QSettings().value(QStringLiteral("pdfio/stripPages"), 5).toInt())
        , m_on(QSettings().value(QStringLiteral("pdfio/stripOn"), true).toBool())
        , m_scope(PdfPageNavigator::instance()->scope())
        , m_count(PdfPageNavigator::instance()->stripPageCount())
    {
    }
    ~StripRestore()
    {
        /// The navigator first -- setScope() writes both keys itself -- and the original keys LAST, so
        /// what is on disk at the end is exactly what was there at the start.
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        navigator->setScope(m_count);
        navigator->setScope(m_scope);

        QSettings settings;
        settings.setValue(QStringLiteral("pdfio/stripPages"), m_pages);
        settings.setValue(QStringLiteral("pdfio/stripOn"), m_on);
    }

    StripRestore(const StripRestore &) = delete;
    StripRestore &operator=(const StripRestore &) = delete;

    int m_pages;
    bool m_on;
    int m_scope;
    int m_count;
};

/// A notebook of \a pages same-size Letter (612x792 pt) records, for geometry that needs a manifest
/// but no document: at the 200 dpi reference that is a 1700x2200 px page.
PdfSessionManifest letterManifest(int pages)
{
    PdfSessionManifest manifest;
    manifest.sourceFile = QStringLiteral("strip-page-loading.pdf");
    manifest.sourceSha256 = QByteArrayLiteral("0123456789abcdef");
    manifest.sourceByteSize = 1;

    for (int i = 0; i < pages; ++i) {
        PdfPageRecord page;
        page.index = i;
        page.sizePt = QSizeF(612, 792);
        page.kraFile = PdfSession::pageFileName(i);
        page.thumbFile = PdfSession::thumbFileName(i);
        manifest.pages.append(page);
    }
    return manifest;
}

/// Puts the page-loading trigger back however a test ends: the two persisted keys and the values in
/// force, which the singleton has already read and the keys alone cannot restore.
///
/// RAII on purpose, the same lesson StripRestore records: a failing assertion must not leave the next
/// test with this one's settle delay or reading share.
struct PageLoadingRestore {
    PageLoadingRestore()
        : m_settleMs(QSettings().value(QStringLiteral("pdfio/scrollSettleMs"), 450).toInt())
        , m_share(QSettings().value(QStringLiteral("pdfio/readingSharePercent"), 60).toInt())
        , m_navigatorSettleMs(PdfPageNavigator::instance()->scrollSettleMs())
        , m_navigatorShare(PdfPageNavigator::instance()->readingSharePercent())
    {
    }
    ~PageLoadingRestore()
    {
        /// The navigator first -- the setters write both keys -- and the original keys LAST, so what
        /// is on disk at the end is exactly what was there at the start.
        PdfPageNavigator *navigator = PdfPageNavigator::instance();
        navigator->setScrollSettleMs(m_navigatorSettleMs);
        navigator->setReadingSharePercent(m_navigatorShare);

        QSettings settings;
        settings.setValue(QStringLiteral("pdfio/scrollSettleMs"), m_settleMs);
        settings.setValue(QStringLiteral("pdfio/readingSharePercent"), m_share);
    }

    PageLoadingRestore(const PageLoadingRestore &) = delete;
    PageLoadingRestore &operator=(const PageLoadingRestore &) = delete;

    int m_settleMs;
    int m_share;
    int m_navigatorSettleMs;
    int m_navigatorShare;
};

/// Writes a one-source notebook into \a project whose source is copied in under \a sourceName --
/// the RELATIVE name the manifest records and the renderer cache keys on -- with one page per entry
/// of \a pageIndices, its displayed size in \a sizes (parallel lists). \a name is the notebook's
/// display name, so two notebooks built this way can be told apart on the tab.
///
/// Hand-built rather than through PdfSession::createProject because the collision under test is
/// exactly two notebooks whose source files have the SAME NAME in different directories, which is
/// what the Android picker makes of every import.
bool writeCollidingSourceNotebook(const QString &project, const QString &sourcePdf,
                                  const QString &sourceName, const QString &name,
                                  const QList<int> &pageIndices, const QList<QSizeF> &sizes,
                                  QString *why)
{
    if (pageIndices.size() != sizes.size() || pageIndices.isEmpty()) {
        if (why) {
            *why = QStringLiteral("the page and size lists do not agree");
        }
        return false;
    }
    QDir().mkpath(project);
    const QString copied = QDir(project).filePath(sourceName);
    if (!QFile::copy(sourcePdf, copied)) {
        if (why) {
            *why = QStringLiteral("cannot copy %1 to %2").arg(sourcePdf, copied);
        }
        return false;
    }

    PdfSessionManifest manifest;
    manifest.name = name;
    manifest.sourceFile = sourceName;
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(copied);
    manifest.sourceByteSize = QFileInfo(copied).size();

    PdfSourceRecord source;
    source.file = sourceName;
    source.sha256 = manifest.sourceSha256;
    source.byteSize = manifest.sourceByteSize;
    manifest.sources.append(source);

    for (int i = 0; i < pageIndices.size(); ++i) {
        PdfPageRecord page;
        page.index = pageIndices.at(i);
        /// The DISPLAYED size (the source's /Rotate already applied), which is what displaySizePt()
        /// returns and what the renderer produces -- so every page lands in a slot its own size.
        page.sizePt = sizes.at(i);
        page.kraFile = PdfSession::pageFileName(i);
        page.thumbFile = PdfSession::thumbFileName(i);
        manifest.pages.append(page);
    }
    manifest.refreshNextPageNumber();

    if (!manifest.isValid(why)) {
        return false;
    }
    return manifest.writeTo(PdfSession::manifestPath(project), why);
}

/// Whether \a band holds anything but the desk colour: a slot whose page was rendered carries paper
/// and text inside its rectangle, and one whose render came back EMPTY keeps the desk fill and
/// nothing else. The same rule, and the same tolerance, PdfStripBuilderTest uses to find where a
/// page landed.
bool bandHasARenderedPage(KisNodeSP band)
{
    /// By value and non-const on purpose: a const KisNodeSP hands back a const KisNode*, and
    /// qobject_cast refuses to cast the constness away (this exact error has cost two builds now).
    KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(band.data());
    if (!layer) {
        return false;
    }

    const QImage pixels = layer->paintDevice()->convertToQImage(0, layer->paintDevice()->extent());
    if (pixels.isNull()) {
        return false;
    }

    for (int y = 0; y < pixels.height(); ++y) {
        for (int x = 0; x < pixels.width(); ++x) {
            const QColor at = pixels.pixelColor(x, y);
            if (qAbs(at.red() - 96) > 1 || qAbs(at.green() - 96) > 1 || qAbs(at.blue() - 96) > 1) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

/**
 * Press and hold a page row, drag it to another place, let go: the list takes the new order as ONE
 * pending edit -- and the swipe that shares the same press still turns a page when the press moves
 * at once.
 *
 * The buttons move a page one place per click, which is a lot of clicking for "this sheet belongs at
 * the end", and a tablet has no room for a drag handle. So the gesture is the one everyone already
 * knows: press, hold, drag, drop. What is asserted is that the drop lands in the pending list
 * exactly as a button's move does -- one change, one undo, nothing written -- that a quick sideways
 * drag is still the swipe rather than a reorder, and that once the hold has fired the drag owns the
 * whole gesture, so a sideways drag reorders and cannot turn the page.
 */
void PdfNavigatorIntegrationTest::testDraggingARowReordersItInOnePendingEdit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("reorder"));

    /// Shown for the whole test, so the watchdog has to be out of the way: see WatchdogPause.
    WatchdogPause watchdogPaused(m_dialogWatchdog);

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QCOMPARE(manifest.pages.size(), 3);

    QFile manifestBeforeFile(PdfSession::manifestPath(project));
    QVERIFY(manifestBeforeFile.open(QIODevice::ReadOnly));
    const QByteArray manifestBefore = manifestBeforeFile.readAll();

    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);
    auto *indicator = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_drop_indicator"));
    QVERIFY2(indicator, "the screen has no drop indicator for the held row and the insertion point");
    auto *summary = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_summary"));
    QVERIFY(summary);
    auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_apply"));
    QVERIFY(apply);

    dialog.show();
    QTest::qWait(50);
    QWidget *viewport = table->viewport();
    QVERIFY2(table->visualRect(table->model()->index(2, 0)).isValid(),
             "the page list has no laid-out row to drag");

    const auto pressRow = [viewport, table](int row) {
        sendMouseAt(viewport, QEvent::MouseButtonPress, rowPoint(table, row), Qt::LeftButton,
                    Qt::LeftButton);
    };
    const auto moveTo = [viewport](const QPoint &at) {
        sendMouseAt(viewport, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton);
    };
    const auto releaseAt = [viewport](const QPoint &at) {
        sendMouseAt(viewport, QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    };

    const QStringList before = pageOrder(dialog.edits());
    QVERIFY2(!indicator->isVisible(), "the drop indicator was on screen before any drag");

    /// Hold the first page and drop it past the last one. The hold has to have fired before the move
    /// is made, which is what the wait gives it; the indicator must be up by then.
    pressRow(0);
    QTest::qWait(600);
    QVERIFY2(indicator->isVisible(), "the hold fired but the drag shows nothing");

    const QPoint dropAt = rowPoint(table, 2) + QPoint(0, table->rowHeight(2));
    moveTo(dropAt);

    /// The held page is marked and the insertion line is drawn: a grab and a drop target that cannot
    /// be missed. The line sits at the bottom of the last row, which is the end of the list.
    const QImage feedback = indicator->grab().toImage();
    QVERIFY2(paintedPixels(feedback) > 0, "the drag painted nothing at all");
    const int heldTop = table->rowViewportPosition(0);
    QVERIFY2(feedback.pixelColor(feedback.width() - 4, heldTop + 5).alpha() > 0,
             "the row in the hand is not marked");
    const int lineAt = table->rowViewportPosition(2) + table->rowHeight(2);
    QVERIFY2(feedback.pixelColor(4, qBound(0, lineAt, feedback.height() - 1)).alpha() > 0,
             "the insertion point is not marked");

    releaseAt(dropAt);
    QVERIFY2(!indicator->isVisible(), "the drop indicator was left on screen after the drop");

    const QStringList after = pageOrder(dialog.edits());
    QCOMPARE(after.size(), 3);
    QCOMPARE(after.at(0), before.at(1));
    QCOMPARE(after.at(1), before.at(2));
    QCOMPARE(after.at(2), before.at(0));
    QVERIFY2(apply->isEnabled(), "the reorder is not a pending edit");
    QVERIFY2(summary->text().contains(QStringLiteral("1 change(s) pending")),
             qPrintable(summary->text()));
    QVERIFY2(summary->text().contains(QStringLiteral("order changed")), qPrintable(summary->text()));

    /// A quick sideways drag is still the swipe: the press never stayed put, so nothing was grabbed,
    /// the page turns, and the order does not move.
    const QStringList beforeSwipe = pageOrder(dialog.edits());
    const QPoint swipeFrom = rowPoint(table, 1);
    pressRow(1);
    moveTo(swipeFrom + QPoint(90, 0));
    releaseAt(swipeFrom + QPoint(90, 0));
    QVERIFY2(!indicator->isVisible(), "a quick drag put the drop indicator up");
    QCOMPARE(pageOrder(dialog.edits()), beforeSwipe);
    QCOMPARE(dialog.edits().pages.at(1).extraRotation, 90);

    /// And a long hold followed by a drag that is mostly sideways -- |dx| >= 60 and |dx| > |dy|, so
    /// the swipe would have taken it -- reorders and turns nothing: once the hold has fired, the drag
    /// owns the whole gesture.
    const QStringList beforeHold = pageOrder(dialog.edits());
    pressRow(0);
    QTest::qWait(600);
    QVERIFY2(indicator->isVisible(), "the hold fired but the drag shows nothing");
    const QPoint sideways = rowPoint(table, 1) + QPoint(120, table->rowHeight(1) / 2 - 2);
    moveTo(sideways);
    releaseAt(sideways);

    const QStringList afterHold = pageOrder(dialog.edits());
    QCOMPARE(afterHold.size(), 3);
    QCOMPARE(afterHold.at(0), beforeHold.at(1));
    QCOMPARE(afterHold.at(1), beforeHold.at(0));
    QCOMPARE(afterHold.at(2), beforeHold.at(2));
    /// No page was turned by that drag: the one the swipe turned above is still the only one holding
    /// a turn.
    int turned = 0;
    for (const PdfPageRecord &page : dialog.edits().pages) {
        if (page.extraRotation != 0) {
            ++turned;
        }
    }
    QCOMPARE(turned, 1);

    /// Nothing was written: the new order is pending, and the notebook on disk is as it was.
    QFile manifestAfterFile(PdfSession::manifestPath(project));
    QVERIFY(manifestAfterFile.open(QIODevice::ReadOnly));
    QCOMPARE(manifestAfterFile.readAll(), manifestBefore);
}

/**
 * A page marked for deletion cannot be dragged: it is on its way out.
 *
 * The struck-through row is still a row, so a hold on one is easy to allow by accident. Moving it
 * would move something the change is about to take away -- the same reason "Move up"/"Move down" are
 * off for it -- so the hold does not fire for it at all, and a drag over it leaves the list alone.
 * The press still selects it, so "Keep page" stays one click away.
 */
void PdfNavigatorIntegrationTest::testARowMarkedForDeletionCannotBeDragged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("reorder-deleted"));

    /// Shown for the whole test, so the watchdog has to be out of the way: see WatchdogPause.
    WatchdogPause watchdogPaused(m_dialogWatchdog);

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QCOMPARE(manifest.pages.size(), 3);

    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);
    auto *indicator = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_drop_indicator"));
    QVERIFY2(indicator, "the screen has no drop indicator");
    auto *summary = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_summary"));
    QVERIFY(summary);

    dialog.show();
    QTest::qWait(50);
    QWidget *viewport = table->viewport();
    QVERIFY2(table->visualRect(table->model()->index(1, 0)).isValid(),
             "the page list has no laid-out row to hold");

    /// Mark the middle page for deletion. The list keeps it, struck through, until Apply.
    table->selectRow(1);
    dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_delete_page"))->click();
    QVERIFY2(summary->text().contains(QStringLiteral("1 page deleted")),
             qPrintable(summary->text()));
    const QStringList kept = pageOrder(dialog.edits());
    QCOMPARE(kept.size(), 2);

    /// A press and hold on the struck-through row: it is selected, and nothing is grabbed.
    sendMouseAt(viewport, QEvent::MouseButtonPress, rowPoint(table, 1), Qt::LeftButton,
                Qt::LeftButton);
    QTest::qWait(600);
    QVERIFY2(!indicator->isVisible(), "a page marked for deletion was grabbed");

    /// Dragging it to the top anyway changes nothing: there is nothing held to drop.
    const QPoint top = rowPoint(table, 0) - QPoint(0, table->rowHeight(0) / 2);
    sendMouseAt(viewport, QEvent::MouseMove, top, Qt::NoButton, Qt::LeftButton);
    sendMouseAt(viewport, QEvent::MouseButtonRelease, top, Qt::LeftButton, Qt::NoButton);

    QVERIFY2(!indicator->isVisible(), "the drag indicator appeared for a page being deleted");
    QCOMPARE(pageOrder(dialog.edits()), kept);
    QVERIFY2(!summary->text().contains(QStringLiteral("order changed")),
             qPrintable(summary->text()));
    QVERIFY2(summary->text().contains(QStringLiteral("1 page deleted")),
             qPrintable(summary->text()));
}

/**
 * A drop where the row already is, and a drop outside the table, both leave the list alone.
 *
 * A hold that never moves is the most likely accident on a touch screen -- a press that lingers
 * before the page is turned -- and it must not become a "change" that turns Apply on and makes the
 * notebook reopen for nothing. A release outside the table is the other half: the drag is abandoned,
 * rather than dropped at whatever row happens to be nearest.
 */
void PdfNavigatorIntegrationTest::testADropOnTheSamePositionIsANoOp()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("reorder-noop"));

    /// Shown for the whole test, so the watchdog has to be out of the way: see WatchdogPause.
    WatchdogPause watchdogPaused(m_dialogWatchdog);

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QCOMPARE(manifest.pages.size(), 3);

    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);
    auto *indicator = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_drop_indicator"));
    QVERIFY2(indicator, "the screen has no drop indicator");
    auto *summary = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_summary"));
    QVERIFY(summary);
    auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_apply"));
    QVERIFY(apply);

    dialog.show();
    QTest::qWait(50);
    QWidget *viewport = table->viewport();

    const QStringList before = pageOrder(dialog.edits());
    QVERIFY2(!apply->isEnabled(), "Apply was on before anything was done");

    /// Hold the middle page and let go without moving it: the gap is the one it already occupies.
    sendMouseAt(viewport, QEvent::MouseButtonPress, rowPoint(table, 1), Qt::LeftButton,
                Qt::LeftButton);
    QTest::qWait(600);
    QVERIFY2(indicator->isVisible(), "the hold fired but the drag shows nothing");
    sendMouseAt(viewport, QEvent::MouseButtonRelease, rowPoint(table, 1), Qt::LeftButton,
                Qt::NoButton);

    QVERIFY2(!indicator->isVisible(), "the drop indicator survived the drop");
    QCOMPARE(pageOrder(dialog.edits()), before);
    QVERIFY2(!apply->isEnabled(), "a drop where the row already was became a pending change");
    QVERIFY2(summary->text().contains(QStringLiteral("No changes")), qPrintable(summary->text()));

    /// Hold the last page, drag it above the list and let go there: that is outside the table, so
    /// the drag is cancelled and the order is untouched.
    sendMouseAt(viewport, QEvent::MouseButtonPress, rowPoint(table, 2), Qt::LeftButton,
                Qt::LeftButton);
    QTest::qWait(600);
    QVERIFY2(indicator->isVisible(), "the hold fired but the drag shows nothing");
    const QPoint outside(rowPoint(table, 2).x(), -20);
    sendMouseAt(viewport, QEvent::MouseMove, outside, Qt::NoButton, Qt::LeftButton);
    sendMouseAt(viewport, QEvent::MouseButtonRelease, outside, Qt::LeftButton, Qt::NoButton);

    QVERIFY2(!indicator->isVisible(), "the drop indicator survived a cancelled drag");
    QCOMPARE(pageOrder(dialog.edits()), before);
    QVERIFY2(!apply->isEnabled(), "a cancelled drop changed the list");
}

/**
 * A preview is prepared for the screen, not for the file.
 *
 * A preview a notebook already holds can be as small as 180x256 for A4; the card it goes into is 46x62 LOGICAL pixels,
 * which on a 2-2.5x tablet is 115x155 DEVICE pixels, and the canvas wants several times that again.
 * Handing Qt the file's size for the box and letting the painter stretch it by the screen's ratio is
 * what the user saw as pixelation. What is asserted here is the arithmetic of the fix at the ratios
 * a 3K panel really has, and then that the screen's two surfaces use it: the card's pixels are the
 * card's size at the table's ratio, the pane's pixels are the pane's size at the canvas's ratio, and
 * for the card the source is at least as big as what is drawn -- which is the crispness claim, and a
 * size claim.
 */
void PdfNavigatorIntegrationTest::testAPreviewIsPreparedForTheScreensDeviceRatio()
{
    /// The arithmetic, at the ratios the tablet has. The result is the drawn size in DEVICE pixels,
    /// tagged with the ratio, never the file's size handed through.
    QImage sheet(180, 256, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::white);
    const QPixmap file = QPixmap::fromImage(sheet);
    const QSize logical(46, 62);
    for (qreal ratio : { 1.0, 2.0, 2.5 }) {
        const QPixmap prepared = pdfioPreviewForDisplay(file, logical, ratio);
        QVERIFY2(!prepared.isNull(), "a preview with pixels was prepared as nothing");
        QCOMPARE(prepared.devicePixelRatio(), ratio);
        QCOMPARE(prepared.size(),
                 QSize(qRound(logical.width() * ratio), qRound(logical.height() * ratio)));
    }

    /// Nothing in, nothing out: no pixels are invented for an empty source or an empty box.
    QVERIFY(pdfioPreviewForDisplay(QPixmap(), logical, 2.5).isNull());
    QVERIFY(pdfioPreviewForDisplay(file, QSize(), 2.5).isNull());

    /// And now the screen's own two surfaces, which is where the numbers have to be real. The
    /// preview on disk is a small one, of the kind an older notebook holds: 180x256 for an A4 page.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("dense-preview"));

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");
    QVERIFY(!manifest.pages.at(0).thumbFile.isEmpty());

    const QString thumbPath = QDir(project).filePath(manifest.pages.at(0).thumbFile);
    QVERIFY2(sheet.save(thumbPath), qPrintable(thumbPath));

    WatchdogPause watchdogPaused(m_dialogWatchdog);
    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *table = dialog.findChild<QTableWidget *>();
    QVERIFY(table);
    auto *canvas = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_canvas"));
    QVERIFY(canvas);

    dialog.show();
    QTest::qWait(50);

    /// The card: the file is 180x256, the card is the table's icon size, and the pixels prepared for
    /// it are that box at the table's ratio -- not the file handed through.
    const PdfNotebookOpsDialog::PreparedPreview card = dialog.cardPreview(0);
    QVERIFY2(!card.pixmap.isNull(), "the card has no picture");
    QCOMPARE(card.sourcePixels, QSize(180, 256));
    QCOMPARE(card.logicalSize, QSize(180, 256).scaled(table->iconSize(), Qt::KeepAspectRatio));
    QCOMPARE(card.devicePixelRatio, table->devicePixelRatioF());
    QCOMPARE(card.pixmap.size(), QSize(qRound(card.logicalSize.width() * table->devicePixelRatioF()),
                                       qRound(card.logicalSize.height() * table->devicePixelRatioF())));
    QVERIFY2(card.sourcePixels.width() >= card.pixmap.width()
                 && card.sourcePixels.height() >= card.pixmap.height(),
             "the card is drawn from fewer pixels than it is drawn at, so it will be stretched");

    /// The canvas: the same rule at the pane's own size. The pane is bigger than the file, which is
    /// exactly the case the other test covers -- here what matters is that the pixels prepared for
    /// it are the pane's drawn size, not the file's.
    canvas->grab();
    const PdfNotebookOpsDialog::PreparedPreview pane = dialog.canvasPreview();
    QVERIFY2(!pane.pixmap.isNull(), "the canvas has no picture");
    QCOMPARE(pane.sourcePixels, QSize(180, 256));
    QCOMPARE(pane.devicePixelRatio, canvas->devicePixelRatioF());
    QCOMPARE(pane.pixmap.size(), QSize(qRound(pane.logicalSize.width() * canvas->devicePixelRatioF()),
                                       qRound(pane.logicalSize.height() * canvas->devicePixelRatioF())));
    QVERIFY2(pane.logicalSize != pane.sourcePixels,
             "the canvas handed the file through instead of preparing it for the pane");
    QVERIFY2(pane.logicalSize.width() > 0 && pane.logicalSize.height() > 0,
             "the canvas prepared nothing to draw into");
}

/**
 * The canvas asks the navigator for a fresh preview instead of stretching a small one.
 *
 * A page's preview file can be as small as the 180x256 an older notebook holds, and a 3K tablet's pane wants
 * several times those pixels. When the pane is about to draw at a size its row's preview cannot
 * fill, the page is asked for again rather than the small picture stretched. The ask has to go to
 * the navigator, and this is that: a notebook the navigator itself has open, its preview deleted, and
 * the file back on disk because the screen asked for it -- nothing else in this test writes one.
 */
void PdfNavigatorIntegrationTest::testTheCanvasAsksTheNavigatorForAFreshPreview()
{
    QVERIFY(useNotebook(QStringLiteral("crisp-preview")));
    const QString project = navigator()->projectDir();
    const PdfSessionManifest manifest = navigator()->manifest();
    QVERIFY2(!manifest.pages.at(0).thumbFile.isEmpty(), "the page records no preview name");

    const QString preview = QDir(project).filePath(manifest.pages.at(0).thumbFile);
    QFile::remove(preview);
    QVERIFY2(!QFileInfo::exists(preview), qPrintable(preview));

    WatchdogPause watchdogPaused(m_dialogWatchdog);
    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *canvas = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_canvas"));
    QVERIFY(canvas);

    dialog.show();
    QTest::qWait(50);

    /// The pane has no preview for the open page, so the screen asks the navigator for one -- and
    /// the ask is what writes the file, because nothing else in this test does.
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(preview), 10000);

    /// And the fresh pixels reach the canvas rather than staying in the file.
    canvas->grab();
    const PdfNotebookOpsDialog::PreparedPreview pane = dialog.canvasPreview();
    QVERIFY2(!pane.pixmap.isNull(), "the fresh preview never reached the canvas");
    QVERIFY2(!pane.sourcePixels.isEmpty(), "the canvas is still drawing an empty sheet");
    QCOMPARE(pane.devicePixelRatio, canvas->devicePixelRatioF());

    /// The pixels the pane took are the pixels the navigator wrote, not something it drew itself.
    const QImage written(preview);
    QVERIFY2(!written.isNull(), qPrintable(preview));
    QCOMPARE(pane.sourcePixels, written.size());
}

/**
 * A generated preview is saved with enough pixels for the screen that draws it.
 *
 * The tablet's page card is up to 320x452 LOGICAL px and the panel runs at a device ratio of 2-2.5,
 * so the card is drawn from up to 800x1130 DEVICE pixels. The file used to be fitted into a 256 box
 * -- 180x256 for A4 -- and stretched 4.4x to fill that, which is the "thumbnail too pixellated" the
 * user reported. It is now fitted into a 1152 box, and the render is aimed at the same number
 * between its 96 dpi floor and its 200 dpi ceiling, so a bigger file does not mean a coarser page.
 */
void PdfNavigatorIntegrationTest::testAGeneratedPreviewIsSavedBigEnoughForTheScreen()
{
    QVERIFY(useNotebook(QStringLiteral("preview-size")));

    const PdfSessionManifest manifest = navigator()->manifest();
    QVERIFY2(!manifest.pages.at(0).thumbFile.isEmpty(), "the page records no preview name");
    const QString preview = QDir(navigator()->projectDir()).filePath(manifest.pages.at(0).thumbFile);

    /// The GENERATED path and only it: whatever was there is dropped, and nothing but the
    /// navigator's own ask writes the file back.
    QFile::remove(preview);
    QVERIFY2(!QFileInfo::exists(preview), qPrintable(preview));

    /// The FILE is what is waited for, not the signal. Opening the notebook also queued a preview
    /// for the NEIGHBOURING page (showImage asks for index + 1), so thumbnailReady fires for page 2
    /// about 40 ms after the open and quite possibly before page 1's file exists: a wait on any
    /// signal returns on that one and reads a file that is not there yet.
    navigator()->ensureThumbnail(0);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(preview), 30000);

    const QImage written(preview);
    QVERIFY2(!written.isNull(), qPrintable(preview));
    qInfo("page 1's preview is %dx%d (%s bytes)", written.width(), written.height(),
          qPrintable(QString::number(QFileInfo(preview).size())));

    /// The worst card the tablet draws, in device pixels.
    QVERIFY2(written.width() >= 800 && written.height() >= 1130,
             qPrintable(QStringLiteral("the preview is %1x%2, too small for a 320x452 card at a 2.5 "
                                       "ratio (800x1130 device pixels)")
                            .arg(written.width()).arg(written.height())));

    /// And it is the whole preview box, which it can only be because the render was not lowered to
    /// get there: a 595 pt page saved 1152 px high was rendered at about 139 dpi, well above the
    /// 96 dpi floor text needs to stay readable.
    QCOMPARE(written.height(), 1152);
}

/**
 * A generated preview carries the page's ink.
 *
 * Generating a preview rather than writing one from an open page is what happens to every page a
 * turn has just dropped the preview of, and the generated one used to be the source render alone:
 * a page that had been drawn on came back as blank paper, which in the panel and on the ops card is
 * the page's identity. The ink is composited from the artifact's PNG sidecar, scaled from the
 * artifact's own pixels onto the render's.
 */
void PdfNavigatorIntegrationTest::testAGeneratedPreviewCarriesTheInk()
{
    QVERIFY(useNotebook(QStringLiteral("preview-ink")));

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    KisPaintLayer *layer = inkLayer(document->image());
    QVERIFY2(layer, "the page has no ink layer to draw on");

    /// A block of a colour a page of black text cannot produce, so finding it in the preview is
    /// finding the ink and not the paper's own printing.
    const QRect mark(300, 300, 200, 200);
    layer->paintDevice()->fill(mark, KoColor(QColor(255, 0, 0), document->image()->colorSpace()));
    document->setModified(true);
    Q_EMIT document->image()->sigImageModified();

    /// Turning away writes the artifact -- and a preview of its own, from the open page. That one
    /// is deleted below, so what is read back here is the GENERATED preview.
    QString why;
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QVERIFY2(waitForInk(artifactFor(0)), qPrintable(artifactFor(0)));

    const PdfSessionManifest manifest = navigator()->manifest();
    const QString preview = QDir(navigator()->projectDir()).filePath(manifest.pages.at(0).thumbFile);
    QFile::remove(preview);
    QVERIFY2(!QFileInfo::exists(preview), qPrintable(preview));

    /// The FILE is what is waited for, not the signal. Opening the notebook also queued a preview
    /// for the NEIGHBOURING page (showImage asks for index + 1), so thumbnailReady fires for page 2
    /// about 40 ms after the open and quite possibly before page 1's file exists: a wait on any
    /// signal returns on that one and reads a file that is not there yet.
    navigator()->ensureThumbnail(0);
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(preview), 30000);

    const QImage written(preview);
    QVERIFY2(!written.isNull(), qPrintable(preview));

    int red = 0;
    QRect where;
    for (int y = 0; y < written.height(); ++y) {
        for (int x = 0; x < written.width(); ++x) {
            const QColor at = written.pixelColor(x, y);
            if (at.red() > 200 && at.green() < 60 && at.blue() < 60) {
                ++red;
                const QRect pixel(QPoint(x, y), QSize(1, 1));
                where = where.isNull() ? pixel : where.united(pixel);
            }
        }
    }

    qInfo("the generated preview is %dx%d and carries %d red pixels, around %d,%d",
          written.width(), written.height(), red, where.center().x(), where.center().y());

    /// The block is about a hundredth of the page, so a few thousand preview pixels. A preview of
    /// blank paper has none of them.
    QVERIFY2(red > 3000, "the generated preview has no ink in it");
    /// And it is where the page put it: the block is in the page's top left quarter.
    QVERIFY2(where.center().x() < written.width() / 2 && where.center().y() < written.height() / 2,
             "the ink came back in the wrong part of the page");
}

/**
 * A roll whose document goes away between its writes and its redraw refuses, and refuses cleanly.
 *
 * The write phase runs nested event loops -- one inside every page write -- and a reload queued
 * behind a notebook change runs inside them and takes the document away. This is the SIGSEGV from
 * the real session: merge 50 pages into a 36 page notebook, scroll the strip, crash during a roll.
 * m_document is a QPointer and goes null when its document is destroyed; a reload REPLACES it with
 * another one, which is worse, because a null check alone would let the roll repaint the new
 * document with the old window's rectangles and then write the new notebook's slot bookkeeping
 * over it. The guard therefore compares the document with the one the roll started for.
 *
 * That state is reached in the real world only by timing no test can promise, so this reaches it
 * through setDocumentGoneAfterWritesForTests(): the write phase runs for real and the guard fires
 * where a reload would have left it.
 */
void PdfNavigatorIntegrationTest::testARollRefusesWhenItsDocumentGoesAway()
{
    /// A source long enough that a five page window has somewhere to roll to.
    const QString usualFixture = m_fixture;
    m_fixture = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));
    QVERIFY(useNotebook(QStringLiteral("roll-gone"), 5));
    m_fixture = usualFixture;

    QVERIFY(navigator()->pageCount() >= 8);
    QCOMPARE(navigator()->currentIndex(), 0);

    KisDocument *const document = navigator()->currentDocument();
    QVERIFY(document);
    KisImageSP strip = document->image();
    QVERIFY(strip);

    /// The paper layers' names, in strip order, are what the window holds: a roll that moved
    /// repaints and renames them, so their staying put is the window staying put.
    const auto paperNames = [](const KisImageSP &image) {
        QStringList names;
        if (!image) {
            return names;
        }
        for (quint32 i = 0; i < image->root()->childCount(); ++i) {
            const QString name = image->root()->at(i)->name();
            if (name != QStringLiteral("Desk") && name != QStringLiteral("Ink")) {
                names << name;
            }
        }
        return names;
    };
    const QStringList before = paperNames(strip);
    QCOMPARE(before.size(), 5);

    /// Page 5 is already in the window, so asking for it activates its slot -- and it is the last
    /// slot, so the screen queues a roll to re-centre the window. That queued roll is the one the
    /// report crashed in.
    QString why;
    QVERIFY2(navigator()->showPage(4, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 4);

    {
        /// The seam, for this block only. Whatever happens below, the flag goes back: a test that
        /// leaves it set would disarm the guard for every test after it.
        struct DocumentGoneForTests {
            DocumentGoneForTests() { PdfPageNavigator::setDocumentGoneAfterWritesForTests(true); }
            ~DocumentGoneForTests() { PdfPageNavigator::setDocumentGoneAfterWritesForTests(false); }
        } goneForTests;

        /// The queued roll writes every page of the window first, and that is what makes the
        /// artifacts appear; the guard then fires before a single pixel is repainted.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(artifactFor(0)), 30000);

        /// Generously longer than the drain and the guard that follow the last write. The waits
        /// cannot make this pass by themselves: with the seam set the repaint cannot happen at all,
        /// and with the seam broken the repaint renames the papers within this window and the
        /// comparison below fails.
        QTest::qWait(1500);

        KisDocument *const stillOpen = navigator()->currentDocument();
        QVERIFY2(stillOpen, "the roll took the document with it");
        QVERIFY(stillOpen->image());
        const QStringList after = paperNames(stillOpen->image());
        QVERIFY2(after == before,
                 qPrintable(QStringLiteral("the window moved: the strip holds papers [%1] where it "
                                           "held [%2]")
                                .arg(after.join(QStringLiteral(", ")),
                                     before.join(QStringLiteral(", ")))));
    }

    /// The honest assertions: refused, still open, and the page where it was.
    QVERIFY(navigator()->currentDocument());
    QVERIFY(navigator()->currentDocument()->image());
    QCOMPARE(navigator()->currentDocument(), document);
    QCOMPARE(navigator()->currentIndex(), 4);

    /// And the tab is closed the way the other tests that leave one open close it, with the queued
    /// work drained on both sides of the close.
    navigator()->currentDocument()->setModified(false);
    navigator()->setScope(1);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * A roll between two windows of the same size resizes nothing.
 *
 * This is the common case and the one the roll exists for: a notebook whose pages are all one size
 * never needs the document resized, and the same-size path must stay exactly as cheap as it was --
 * the resize is new, and it must not creep into the case that never needed it. The image size and
 * every layer device's extent have to be identical before and after.
 */
void PdfNavigatorIntegrationTest::testARollBetweenSameSizeWindowsDoesNotResize()
{
    /// A notebook of one page size, built here rather than taken from a fixture: every fixture with
    /// enough pages to roll through is a mix of sizes, which is the other half of this test.
    const QString project = m_dir.filePath(QStringLiteral("roll-same-size-project"));
    const QString source = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(source), qPrintable(source));

    PopplerRenderBackend backend;
    QString why;
    PdfSessionManifest manifest = PdfSession::createProject(project, source, backend, &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    /// Eight letter pages, all out of the fixture's letter page, so nothing but the window can make
    /// one window a different size from another.
    manifest.pages.clear();
    for (int i = 0; i < 8; ++i) {
        PdfPageRecord page;
        page.index = 2;
        page.sizePt = QSizeF(612, 792);
        page.kraFile = PdfSession::pageFileName(i);
        page.thumbFile = PdfSession::thumbFileName(i);
        manifest.pages.append(page);
    }
    manifest.refreshNextPageNumber();
    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    QVERIFY2(manifest.writeTo(PdfSession::manifestPath(project), &why), qPrintable(why));

    navigator()->setScope(5);
    QVERIFY2(navigator()->openNotebookDir(project, &why), qPrintable(why));
    QCOMPARE(navigator()->pageCount(), 8);
    QCOMPARE(navigator()->currentIndex(), 0);

    /// The two windows really are the same size, or this test would prove nothing.
    const PdfStripLayout first = PdfStripLayout::forWindow(navigator()->manifest(), 0, 5, 200.0);
    const PdfStripLayout second = PdfStripLayout::forWindow(navigator()->manifest(), 6, 5, 200.0);
    QCOMPARE(first.imageSize(), second.imageSize());

    KisDocument *const document = navigator()->currentDocument();
    QVERIFY(document);
    KisImageSP strip = document->image();
    QVERIFY(strip);
    QCOMPARE(QSize(strip->width(), strip->height()), first.imageSize());

    /// The layer devices as their allocated extents: a crop or a resize changes them, a repaint
    /// over the same bands does not.
    const auto deviceSizes = [](const KisImageSP &image) {
        QList<QSize> sizes;
        for (quint32 i = 0; i < image->root()->childCount(); ++i) {
            KisNodeSP child = image->root()->at(i);
            if (KisPaintLayer *layer = qobject_cast<KisPaintLayer *>(child.data())) {
                sizes.append(layer->paintDevice()->extent().size());
            }
        }
        return sizes;
    };
    const QList<QSize> sizesBefore = deviceSizes(strip);

    /// Page 7 is outside [0..4], so this is a window move and not a turn to a page already in the
    /// strip.
    QVERIFY2(navigator()->showPage(6, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 6);
    QVERIFY(navigator()->currentDocument() == document);

    KisImageSP after = navigator()->currentDocument()->image();
    QVERIFY(after);
    QCOMPARE(QSize(after->width(), after->height()), first.imageSize());
    const QList<QSize> sizesAfter = deviceSizes(after);
    QVERIFY2(sizesAfter == sizesBefore,
             qPrintable(QStringLiteral("the layer devices changed size (%1 before, %2 after) with "
                                       "no size change to make")
                            .arg(sizesBefore.size()).arg(sizesAfter.size())));

    /// The tab is closed the way the other tests that leave one open close it, with the queued work
    /// drained on both sides of the close.
    navigator()->currentDocument()->setModified(false);
    navigator()->setScope(1);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * How many pages the strip holds is the user's to choose.
 *
 * The window that is up is the unit: the layout's slots are the pages, one paper band per slot, so a
 * chosen count has to lay out exactly that many. This drives the count through setScope() -- which is
 * what the menu's presets and its typed dialog call -- and checks the document that results, the two
 * settings it is persisted in, and that turning the strip off keeps the count rather than forgetting
 * it. The spinner WIDGET itself is not in this binary: it lives in PdfIoPlugin.cpp, so what is tested
 * here is the value handling the dialog hands the navigator, not the widget that produces it.
 */
void PdfNavigatorIntegrationTest::testTheStripPageCountChoosesTheWindowSize()
{
    StripRestore restore;
    /// A budget, so the ONE thing under test is the count: eleven Letter pages at the reference 200
    /// dpi would be two gigabytes across their layers, and this test is about slots, not memory. It
    /// changes no slot count, and the budget is put back however the test ends.
    BudgetRestore budgetRestore(PdfPageNavigator::instance()->memoryBudgetMb());
    PdfPageNavigator::instance()->setMemoryBudgetMb(150);

    const QString project = m_dir.filePath(QStringLiteral("strip-pages"));
    QString why;
    QVERIFY2(writeLetterNotebook(project, 12, &why), qPrintable(why));

    /// Five, which is what the notebook ships with, asked for before it is opened so the strip is
    /// built at that count.
    navigator()->setScope(5);
    QVERIFY2(navigator()->openNotebookDir(project, &why), qPrintable(why));
    QCOMPARE(navigator()->pageCount(), 12);
    QCOMPARE(navigator()->scope(), 5);
    QCOMPARE(navigator()->stripPageCount(), 5);

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    QVERIFY(document->image());
    QCOMPARE(bandCountOf(document->image()), 5);

    /// A chosen count lays out that many slots: the bands are the slots, one paper layer per page of
    /// the window, so counting them with the saver's own rule is counting the window.
    for (int pages : { 3, 7, 9 }) {
        navigator()->setScope(pages);
        QCOMPARE(navigator()->scope(), pages);
        QCOMPARE(bandCountOf(navigator()->currentDocument()->image()), pages);

        const PdfStripLayout layout = PdfStripLayout::forWindow(navigator()->manifest(),
                                                                navigator()->currentIndex(), pages,
                                                                navigator()->currentRenderDpi());
        QVERIFY(layout.isValid());
        QCOMPARE(layout.slots().size(), pages);
    }

    /// A count that is NOT one of the presets works the same way: eleven pages, laid out as eleven,
    /// and the document really holds eleven bands.
    navigator()->setScope(11);
    QCOMPARE(navigator()->scope(), 11);
    QCOMPARE(navigator()->stripPageCount(), 11);
    QCOMPARE(bandCountOf(navigator()->currentDocument()->image()), 11);

    /// And it is written where a restart reads it, under the two keys the count and the on/off state
    /// are kept apart in.
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/stripPages")).toInt(), 11);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/stripOn")).toBool(), true);

    /// Turning the strip off changes only the on/off half: the count is still there, so the switch
    /// turns it back on at eleven rather than at five. This is the "must not forget the count" rule.
    navigator()->setScope(1);
    QCOMPARE(navigator()->scope(), 1);
    QCOMPARE(navigator()->stripPageCount(), 11);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/stripPages")).toInt(), 11);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/stripOn")).toBool(), false);

    /// And the tab is closed the way the tests that leave one open close it.
    if (KisDocument *open = navigator()->currentDocument()) {
        open->setModified(false);
    }
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * A count change while a strip is open lands the document at the new size through the ROLL, not a
 * rebuild.
 *
 * A different page count is a different WINDOW -- more slots, each with its own paper band -- and the
 * roll re-cuts the bands and resizes the document to the window arriving. It used to REFUSE a window
 * with a different number of slots, which is what handed the change to a full rebuild: a new
 * document, a new view, and the cost of both. What is asserted here is that identity: the same
 * KisDocument, the same KisImage, the bands equal to the new layout's slots, every band below the Ink
 * group, the reading page where it was, and no modified flag left for Krita to autosave.
 */
void PdfNavigatorIntegrationTest::testAStripPageCountChangeResizesTheSameDocument()
{
    StripRestore restore;
    /// A budget, and it is not what is under test: it keeps a nine page Letter window from being
    /// built at the reference 200 dpi, which is 1.4 GB across its layers. The slots, the identity of
    /// the document and the resize path are all the budget cannot change.
    BudgetRestore budgetRestore(PdfPageNavigator::instance()->memoryBudgetMb());
    PdfPageNavigator::instance()->setMemoryBudgetMb(400);

    const QString project = m_dir.filePath(QStringLiteral("strip-pages-resize"));
    QString why;
    QVERIFY2(writeLetterNotebook(project, 12, &why), qPrintable(why));

    navigator()->setScope(5);
    QVERIFY2(navigator()->openNotebookDir(project, &why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 0);

    KisDocument *const document = navigator()->currentDocument();
    QVERIFY(document);
    KisImageSP const strip = document->image();
    QVERIFY(strip);

    const int indexBefore = navigator()->currentIndex();
    QCOMPARE(bandCountOf(strip), 5);

    const PdfStripLayout five = PdfStripLayout::forWindow(navigator()->manifest(), indexBefore, 5,
                                                          navigator()->currentRenderDpi());
    QVERIFY(five.isValid());
    QCOMPARE(QSize(strip->width(), strip->height()), five.imageSize());

    /// Five to nine: the window grows at both ends, so four bands have to be added and the document
    /// grows with them.
    navigator()->setScope(9);

    /// The SAME document and the SAME image: a count change is a roll of the window, not a rebuild of
    /// the document. Without this the assertions below could be reading a freshly built strip and pass
    /// without proving anything.
    QCOMPARE(navigator()->currentDocument(), document);
    QVERIFY2(navigator()->currentDocument()->image().data() == strip.data(),
             "the image was replaced, so the count change rebuilt the document");
    QCOMPARE(navigator()->currentIndex(), indexBefore);

    const PdfStripLayout nine = PdfStripLayout::forWindow(navigator()->manifest(), indexBefore, 9,
                                                          navigator()->currentRenderDpi());
    QVERIFY(nine.isValid());
    QVERIFY2(nine.imageSize() != five.imageSize(),
             "the two windows have the same size, so this test says nothing about a resize");
    QCOMPARE(QSize(navigator()->currentDocument()->image()->width(),
                   navigator()->currentDocument()->image()->height()),
             nine.imageSize());

    /// The bands are exactly the slots of the new window, counted with the rule the page saver uses.
    QCOMPARE(bandCountOf(navigator()->currentDocument()->image()), nine.slots().size());
    QCOMPARE(bandCountOf(navigator()->currentDocument()->image()), 9);

    /// Every band is BELOW the Ink group. A band inserted above the ink hides the strokes on the page
    /// it covers, which is the one place the surgery may not put it.
    const int ink = inkGroupIndex(navigator()->currentDocument()->image());
    QVERIFY2(ink >= 0, "the strip has no Ink group");
    for (quint32 i = 0; i < navigator()->currentDocument()->image()->root()->childCount(); ++i) {
        if (PdfPageSaver::isPageBackground(navigator()->currentDocument()->image()->root()->at(i))) {
            QVERIFY2(int(i) < ink,
                     qPrintable(QStringLiteral("a band at root index %1 is above the Ink group at %2")
                                    .arg(i)
                                    .arg(ink)));
        }
    }

    /// And the surgery left nothing for Krita to autosave: the strip is a view of the notebook, and
    /// an asterisk on the tab is what makes Krita write the whole strip out.
    QVERIFY2(!navigator()->currentDocument()->isModified(),
             "the band surgery left the strip marked modified");

    /// The tab is closed the way the tests that leave one open close it.
    navigator()->currentDocument()->setModified(false);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * The page count is bounded by the NOTEBOOK, not by a constant of this build's.
 *
 * The user asked to be able to load several tens of pages on a powerful machine, so seven is not a
 * ceiling. What bounds a WINDOW is the notebook -- it cannot extend past the first and last page --
 * and the count itself is a setting that is NOT clamped to whichever notebook is open when it is
 * asked for: it is asked for before the notebook it is meant for as often as after, and a leftover
 * small notebook rewriting a request for nine into five is exactly the bug the sweep caught. So a
 * count larger than the open notebook is kept as the setting, and the slots on screen are the
 * notebook's answer.
 */
void PdfNavigatorIntegrationTest::testAStripPageCountIsBoundedByTheNotebook()
{
    StripRestore restore;
    /// A budget for the same reason as its neighbours: five Letter pages at 200 dpi are about
    /// 476 MB, and this test is about the notebook being the bound rather than a constant.
    BudgetRestore budgetRestore(PdfPageNavigator::instance()->memoryBudgetMb());
    PdfPageNavigator::instance()->setMemoryBudgetMb(200);

    const QString project = m_dir.filePath(QStringLiteral("strip-pages-bounded"));
    QString why;
    QVERIFY2(writeLetterNotebook(project, 5, &why), qPrintable(why));

    navigator()->setScope(5);
    QVERIFY2(navigator()->openNotebookDir(project, &why), qPrintable(why));
    QCOMPARE(navigator()->pageCount(), 5);

    /// Nine asked for in a five page notebook: the SETTING is nine, and the WINDOW is the five pages
    /// the notebook has. Asking for more than the notebook holds changes no pixels, so the five slots
    /// that are up stay up.
    navigator()->setScope(9);
    QCOMPARE(navigator()->stripPageCount(), 9);
    QCOMPARE(navigator()->scope(), 9);
    QCOMPARE(bandCountOf(navigator()->currentDocument()->image()), 5);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/stripPages")).toInt(), 9);

    /// And the remembered count survives the strip being turned off and on: the switch brings back
    /// nine, which this notebook again holds five of.
    navigator()->setScope(1);
    QCOMPARE(navigator()->scope(), 1);
    QCOMPARE(navigator()->stripPageCount(), 9);

    navigator()->setScope(navigator()->stripPageCount());
    QCOMPARE(navigator()->stripPageCount(), 9);
    QCOMPARE(navigator()->scope(), 9);
    QCOMPARE(bandCountOf(navigator()->currentDocument()->image()), 5);

    /// The tab is closed the way the tests that leave one open close it.
    navigator()->currentDocument()->setModified(false);
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * A bigger strip at a fixed budget stays inside it; with no limit it simply costs more.
 *
 * This is the half of the strip-size setting the menu label exists for. At a fixed budget the window
 * keeps its memory and spends it across more pages at a lower resolution each -- the pages get
 * coarser, the megabytes do not grow. With no limit there is nothing to hold it back and the memory
 * climbs with the page count, which is the cost the label has to make legible.
 *
 * The notebook is the reported one: same-size Letter (612x792 pt) pages. The figures at three, five,
 * seven and nine pages, with no limit and with 400 MB, are printed here so the device numbers can be
 * checked against them.
 */
void PdfNavigatorIntegrationTest::testABiggerStripStaysInsideTheMemoryBudget()
{
    StripRestore stripRestore;
    BudgetRestore budgetRestore(PdfPageNavigator::instance()->memoryBudgetMb());

    const QString project = m_dir.filePath(QStringLiteral("strip-pages-budget"));
    QString why;
    QVERIFY2(writeLetterNotebook(project, 12, &why), qPrintable(why));

    /// 400 MB, set before the notebook is opened so the first build is already inside it.
    PdfPageNavigator::instance()->setMemoryBudgetMb(400);
    navigator()->setScope(3);
    QVERIFY2(navigator()->openNotebookDir(project, &why), qPrintable(why));

    struct Figures {
        QSize size;
        int layers = 0;
        qreal bytes = 0.0;
        qreal dpi = 0.0;
        int longest = 0;
    };

    /// What the document really holds at \a pages, read off the document rather than off the
    /// derivation: the image size and the layer count decide the memory, the dpi the roll landed on
    /// decides the page size. No QTest macro in here on purpose -- one would return from the lambda,
    /// not from the test -- the caller asserts on what comes back.
    const auto measure = [this](int pages) {
        Figures f;
        navigator()->setScope(pages);

        KisDocument *document = navigator()->currentDocument();
        if (!document || !document->image()) {
            return f;
        }

        f.size = document->image()->size();
        f.layers = paintLayersOf(document->image());
        f.bytes = qreal(f.size.width()) * qreal(f.size.height()) * qreal(f.layers) * 4.0;
        f.dpi = navigator()->currentRenderDpi();

        const PdfStripLayout layout = PdfStripLayout::forWindow(
            navigator()->manifest(), navigator()->currentIndex(), pages, f.dpi);
        for (const PdfStripLayout::Slot &slot : layout.slots()) {
            f.longest = qMax(f.longest, qMax(slot.rect.width(), slot.rect.height()));
        }

        /// printf-style on purpose: qWarning() is the printf family and does not take %1.
        qWarning("[strip-pages] %d pages: %dx%d, %d layer(s), %.1f MB, %.1f dpi, longest %d px",
                 pages,
                 f.size.width(),
                 f.size.height(),
                 f.layers,
                 f.bytes / 1000000.0,
                 f.dpi,
                 f.longest);
        return f;
    };

    /// WITH the budget: every window grown to is inside it, and the pages get smaller as the strip
    /// grows rather than the window breaking the budget.
    qreal longestBefore = 0.0;
    for (int pages : { 3, 5, 7, 9 }) {
        const Figures f = measure(pages);
        QCOMPARE(navigator()->scope(), pages);
        QVERIFY2(f.layers > 0, "the strip has no layers, so the memory figure would be meaningless");

        const qreal budgetBytes = 400.0 * 1000000.0;
        QVERIFY2(f.bytes <= budgetBytes,
                 qPrintable(QStringLiteral("a 400 MB budget built %1x%2 x %3 layers = %4 MB")
                                .arg(f.size.width())
                                .arg(f.size.height())
                                .arg(f.layers)
                                .arg(f.bytes / 1000000.0, 0, 'f', 1)));

        /// The figure the menu names beside the count is the memory the window really costs, to the
        /// rounding of the megabytes.
        QVERIFY2(qAbs(navigator()->windowCostMbForScope(pages) - f.bytes / 1000000.0) <= 1.0,
                 qPrintable(QStringLiteral("the menu names %1 MB where the window costs %2 MB")
                                .arg(navigator()->windowCostMbForScope(pages))
                                .arg(f.bytes / 1000000.0, 0, 'f', 1)));

        if (longestBefore > 0.0) {
            QVERIFY2(f.longest <= longestBefore,
                     qPrintable(QStringLiteral("%1 pages gives a %2 px longest page where fewer pages "
                                               "gave %3 px at the same budget")
                                    .arg(pages)
                                    .arg(f.longest)
                                    .arg(longestBefore)));
        }
        longestBefore = f.longest;

        QVERIFY2(!navigator()->currentDocument()->isModified(),
                 "a count change under a budget left the strip marked modified");

        /// The crop a growth-derived resize makes RETAINS the pixels it removed in its undo command
        /// (a recorded limitation, see rollToPage), and four windows in a row would hold all four.
        /// Clearing the history frees exactly that; the document's own size and layer count -- what
        /// the assertions above are about -- are untouched by it.
        navigator()->currentDocument()->clearUndoHistory();
    }

    /// And with NO limit the same growth simply costs more: the pages stay at the reference
    /// resolution and the memory climbs with the page count. This is the half of the label that says
    /// a big strip is expensive rather than forbidden.
    ///
    /// Seven and nine pages are NOT built here. At 200 dpi a nine page Letter window is about 1.4 GB
    /// across its layers, which is exactly the cost the menu label exists to make legible -- and not
    /// something this harness should allocate to prove a number. The figures are read off the same
    /// layout the build would use, and the three page case is built so the two can be seen to agree.
    /// The roll's own budget would be set on the current, nine page window, so the scope comes down
    /// first: at 400 MB and then at no limit, three pages is 200 dpi either way.
    navigator()->setScope(3);
    PdfPageNavigator::instance()->setMemoryBudgetMb(0);
    QCOMPARE(navigator()->currentRenderDpi(), 200.0);

    const auto derivedBytesAtNoLimit = [this](int pages) {
        const PdfStripLayout layout = PdfStripLayout::forWindow(
            navigator()->manifest(), navigator()->currentIndex(), pages, 200.0);
        const int layers = qMax(1, layout.slots().size() + 1);
        const QSize size = layout.imageSize();
        return qreal(size.width()) * qreal(size.height()) * qreal(layers) * 4.0;
    };

    /// The built document at three pages IS the derivation's number, to the byte: that is what says
    /// the reported figures are the document's own size rather than an estimate of it.
    {
        const Figures f = measure(3);
        QCOMPARE(navigator()->currentRenderDpi(), 200.0);
        const qreal derived = derivedBytesAtNoLimit(3);
        QVERIFY2(qAbs(f.bytes - derived) < 1.0,
                 qPrintable(QStringLiteral("the built three page window is %1 MB where the layout says "
                                           "%2 MB")
                                .arg(f.bytes / 1000000.0, 0, 'f', 1)
                                .arg(derived / 1000000.0, 0, 'f', 1)));
    }

    qreal previous = derivedBytesAtNoLimit(3);
    for (int pages : { 5, 7, 9 }) {
        const qreal bytes = derivedBytesAtNoLimit(pages);
        qWarning("[strip-pages] %d pages: no limit, %.1f MB derived at 200 dpi, about %d px longest",
                 pages, bytes / 1000000.0, navigator()->longestPagePixelsForScope(pages));
        QVERIFY2(bytes > previous,
                 qPrintable(QStringLiteral("%1 pages costs %2 MB, no more than the %3 MB fewer pages "
                                           "cost with no limit")
                                .arg(pages)
                                .arg(bytes / 1000000.0, 0, 'f', 1)
                                .arg(previous / 1000000.0, 0, 'f', 1)));
        /// Which is the point of the warning label: with no limit the fourth step is already past
        /// what the 400 MB budget held it to.
        QVERIFY2(bytes > 400.0 * 1000000.0,
                 "a bigger strip with no limit did not cost more than the 400 MB budget held it to");
        previous = bytes;
    }

    /// The tab is closed the way the tests that leave one open close it.
    if (KisDocument *document = navigator()->currentDocument()) {
        document->setModified(false);
    }
    if (KisView *view = navigator()->currentView()) {
        view->closeView();
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }
    QTest::qWait(200);
    QApplication::processEvents();
}

/**
 * The page-loading trigger is the user's: how long the view must settle before the strip turns, and
 * how much of the viewport the active page takes when it is fitted.
 *
 * Both are PERSISTED like the strip's page count and the memory budget, both default to exactly what
 * every build before the setting did (450 ms, three fifths), and both are CLAMPED rather than refused:
 * the settle delay at 0 -- a negative pause has no meaning, and there is NO CEILING because no value
 * breaks anything -- and the reading share between 1% and 100%, the fraction's own ends (zero is a
 * page with no height; above 100% the page is taller than the viewport and the neighbour the rule
 * exists to show is gone again).
 *
 * The share is not only a quality knob: it decides how much scrolling turns the reading page, and the
 * geometry at the end of this test pins that down with the code's own layout and the code's own fit
 * arithmetic. The spinner widgets themselves live in PdfIoPlugin.cpp and are not in this binary; what
 * is tested here is the value handling the dialogs call.
 */
void PdfNavigatorIntegrationTest::testThePageLoadingTriggerIsPersistedAndClamped()
{
    PageLoadingRestore restore;

    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// The strip's own scope, to be shown at the end to be untouched: the page-loading trigger is a
    /// separate setting and must not move the window.
    const int scopeBefore = navigator->scope();

    /// The defaults this test can see -- initTestCase asserted the ones the singleton read at
    /// construction: 450 ms and 60%, what every build before the setting used.
    QCOMPARE(navigator->scrollSettleMs(), 450);
    QCOMPARE(navigator->readingSharePercent(), 60);
    QCOMPARE(PdfPageNavigator::minReadingSharePercent(), 1);
    QCOMPARE(PdfPageNavigator::maxReadingSharePercent(), 100);

    /// Persisted and read back, under the two keys, exactly like the memory budget.
    navigator->setScrollSettleMs(300);
    QCOMPARE(navigator->scrollSettleMs(), 300);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/scrollSettleMs")).toInt(), 300);

    navigator->setReadingSharePercent(40);
    QCOMPARE(navigator->readingSharePercent(), 40);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/readingSharePercent")).toInt(), 40);

    /// Clamped, not refused: a negative settle is held at 0, and an hour is stored as asked -- there
    /// is no ceiling for the settle delay, and the switch is what says "off".
    navigator->setScrollSettleMs(-100);
    QCOMPARE(navigator->scrollSettleMs(), 0);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/scrollSettleMs")).toInt(), 0);
    navigator->setScrollSettleMs(3600000);
    QCOMPARE(navigator->scrollSettleMs(), 3600000);

    /// The share is held at both ends: 0 is not a share at all and 100 is the last value that leaves
    /// the neighbour on screen.
    navigator->setReadingSharePercent(0);
    QCOMPARE(navigator->readingSharePercent(), PdfPageNavigator::minReadingSharePercent());
    navigator->setReadingSharePercent(1000);
    QCOMPARE(navigator->readingSharePercent(), PdfPageNavigator::maxReadingSharePercent());

    /// The default the fit has always used, pinned against the real arithmetic: a 1700x2200 px page
    /// (612x792 pt at the 200 dpi reference) in a 1200x1600 viewport, three fifths of the height.
    const QSize viewport(1200, 1600);
    const QRect pageRect(0, 0, 1700, 2200);
    navigator->setReadingSharePercent(60);
    const qreal atSixty = navigator->fitZoomForViewport(viewport, pageRect);
    const qreal threeFifths = 0.6 * 1600.0 / 2200.0;
    QVERIFY2(qAbs(atSixty - threeFifths) < 1e-9,
             qPrintable(QStringLiteral("the 60% fit is %1, not the three fifths %2")
                            .arg(atSixty).arg(threeFifths)));

    navigator->setReadingSharePercent(100);
    const qreal atFull = navigator->fitZoomForViewport(viewport, pageRect);
    QVERIFY2(atFull > atSixty, "100% has to make the page bigger on screen than 60%");
    /// At 100% the width fits before the height does, which is the ceiling's own meaning: the page is
    /// no longer limited by the share, and nothing of the next page is left.
    QCOMPARE(atFull, 1200.0 / 1700.0);

    /// And the share changes WHICH PAGE a scroll leaves under the centre -- the reading page.
    ///
    /// Three same-size pages are laid out by the code's own layout, the centre starts on page 2 (the
    /// middle slot) and the user scrolls 600 widget px. The strip travels 600/zoom DOCUMENT px, so
    /// the bigger the page on screen (the larger the share) the less far the centre goes: at 100% it
    /// is still on page 2, at 60% it has reached page 3.
    const PdfSessionManifest manifest = letterManifest(3);
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest, 1, 3, 200.0);
    QVERIFY(layout.isValid());
    const QList<PdfStripLayout::Slot> slots = layout.slots();
    QCOMPARE(slots.size(), 3);

    const QPointF start = slots.at(1).rect.center();
    const QList<int> pages = { slots.at(0).page, slots.at(1).page, slots.at(2).page };
    const QList<QRect> rects = { slots.at(0).rect, slots.at(1).rect, slots.at(2).rect };
    const auto readingPageAfterScroll = [&pages, &rects, &layout, &start](qreal zoom) {
        const QPointF centre(start.x(), start.y() + 600.0 / zoom);
        return layout.nearestPage(pages, rects, centre, std::numeric_limits<qreal>::max());
    };

    navigator->setReadingSharePercent(100);
    const qreal biggerPage = navigator->fitZoomForViewport(viewport, slots.at(1).rect);
    navigator->setReadingSharePercent(60);
    const qreal smallerPage = navigator->fitZoomForViewport(viewport, slots.at(1).rect);
    QVERIFY2(smallerPage < biggerPage, "the 60% page has to be smaller on screen than the 100% one");

    qInfo("reading page after a 600 widget px scroll: 100%% (zoom %f) stays on page %d; 60%% "
          "(zoom %f) is on page %d",
          biggerPage, readingPageAfterScroll(biggerPage) + 1, smallerPage,
          readingPageAfterScroll(smallerPage) + 1);

    QCOMPARE(readingPageAfterScroll(biggerPage), slots.at(1).page);
    QCOMPARE(readingPageAfterScroll(smallerPage), slots.at(2).page);

    /// And the trigger is a setting of its own: moving either value changed nothing about which
    /// window the strip holds.
    QCOMPARE(navigator->scope(), scopeBefore);
}

/**
 * Importing a PDF while a notebook is open: the new notebook has to replace the old one everywhere.
 *
 * The report was "the proportion/rescale is the new one but the content is the old notebook", and the
 * mechanism was NOT the reuse path in the open flow -- it was the source-renderer cache.
 * PdfSourceRenderers keeps one open backend per RELATIVE source file name the manifest records, and
 * every Android import is a copy called "pdfio-picked.pdf": with a notebook already open, the second
 * import's pages were rendered through the FIRST notebook's still-open PDF, giving the new manifest's
 * page count, sizes and rotation (the new proportions) with the old notebook's pixels drawn into
 * them. adoptNotebook() now clears the renderers -- and the stale thumbnail queue and write stamps --
 * when a notebook replaces another, which is what closeNotebook() had always done for a close.
 *
 * The two projects here are built to make that visible: different PDFs under the SAME relative source
 * name, and the second notebook's pages point at source pages the first notebook's PDF does not have,
 * so a render served by the wrong backend comes back EMPTY and the band keeps only the desk colour.
 * With realistic (in-range) page indices the same fault shows as the old notebook's pages drawn into
 * the new notebook, which is what the user saw; out of range is what a test can see.
 *
 * The ink half is here too: the open notebook's pages are written by the gate the import path now
 * runs before it closes it (prepareForNotebookChange), and the page that was drawn on comes back with
 * its mark when the notebook is opened again.
 *
 * What this test does NOT cover: the plugin's own close-first helper (openNotebookReplacing) is not
 * in this binary, so what is tested is the gate it calls and the navigator's own replace. Nor does it
 * cover the second, latent half of the cache fault -- the key is still the relative name, so two
 * notebooks are only safe because the cache is cleared between them (see the report).
 */
void PdfNavigatorIntegrationTest::testImportingWhileANotebookIsOpenReplacesIt()
{
    StripRestore restore;

    const QString fixtureA = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    const QString fixtureB = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-manypage-50.pdf");
    QVERIFY2(QFileInfo::exists(fixtureA) && QFileInfo::exists(fixtureB), "the fixtures are missing");

    const QString dirA = m_dir.filePath(QStringLiteral("import-open-a"));
    const QString dirB = m_dir.filePath(QStringLiteral("import-open-b"));
    QString why;

    /// A: three pages of text-fixture (its own PDF has three), source recorded as "same-name.pdf".
    /// The displayed sizes are the fixture's own (page 1 is turned a right angle by the file).
    QVERIFY2(writeCollidingSourceNotebook(dirA, fixtureA, QStringLiteral("same-name.pdf"),
                                          QStringLiteral("Notebook A"), { 0, 1, 2 },
                                          { QSizeF(595, 842), QSizeF(420, 595), QSizeF(300, 300) },
                                          &why), qPrintable(why));
    /// B: SIX pages of ex-manypage -- source pages 10..15, past the end of A's three page PDF --
    /// under the same relative source name, with the fixture's displayed sizes for those pages. Six
    /// rather than three so a window move after the import can be exercised as well.
    QVERIFY2(writeCollidingSourceNotebook(dirB, fixtureB, QStringLiteral("same-name.pdf"),
                                          QStringLiteral("Notebook B"), { 10, 11, 12, 13, 14, 15 },
                                          { QSizeF(612, 792), QSizeF(595, 420), QSizeF(595, 842),
                                            QSizeF(420, 595), QSizeF(612, 792), QSizeF(595, 420) },
                                          &why), qPrintable(why));

    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// A is open at three pages, with ink on its first page that is not on disk yet.
    navigator->setScope(3);
    QVERIFY2(navigator->openNotebookDir(dirA, &why), qPrintable(why));
    QCOMPARE(navigator->projectDir(), dirA);
    QCOMPARE(navigator->manifest().pages.size(), 3);

    {
        KisDocument *document = navigator->currentDocument();
        QVERIFY(document);
        KisPaintLayer *ink = stripInkLayer(document->image());
        QVERIFY2(ink, "the strip has no layer called Ink to draw on");

        const PdfStripLayout layout = PdfStripLayout::forWindow(navigator->manifest(), 0, 3,
                                                                navigator->currentRenderDpi());
        QVERIFY(layout.isValid());
        const QRect mark(layout.slots().at(0).rect.topLeft() + QPoint(8, 8), QSize(24, 24));
        ink->paintDevice()->fill(mark, KoColor(QColor(0, 0, 0), document->image()->colorSpace()));
        document->setModified(true);
        Q_EMIT document->image()->sigImageModified();
    }

    /// The gate the import path now runs before it closes the old notebook: every page the document
    /// holds is written, and only then may the close happen. The ink must reach the artifact -- this
    /// is the "written before it is closed" half of the fix.
    QVERIFY2(navigator->prepareForNotebookChange(&why), qPrintable(why));
    QVERIFY2(QFileInfo::exists(artifactFor(0)), "the first notebook's page was not written");
    QVERIFY2(inkMarkInImage(PdfInkLoader::loadInk(artifactFor(0))),
             "the mark did not reach the first notebook's artifact");

    /// And the import itself: open B while A's document is still standing, which is the sequence the
    /// report is about (the plugin closes the view first; the navigator must replace either way).
    QVERIFY2(navigator->openNotebookDir(dirB, &why), qPrintable(why));

    /// Every place that names the notebook is B's: the navigator's directory and manifest, the
    /// document's own recorded directory, and the tab.
    QCOMPARE(navigator->projectDir(), dirB);
    QCOMPARE(navigator->manifest().pages.size(), 6);
    QCOMPARE(navigator->manifest().pages.at(0).index, 10);
    QCOMPARE(navigator->manifest().pages.at(5).index, 15);
    QCOMPARE(navigator->manifest().displayName(), QStringLiteral("Notebook B"));

    KisDocument *documentB = navigator->currentDocument();
    QVERIFY(documentB);
    QCOMPARE(documentB->property("pdfioProjectDir").toString(), dirB);
    QVERIFY2(documentB->caption().contains(QStringLiteral("Notebook B")),
             qPrintable(QStringLiteral("the tab still reads \"%1\"").arg(documentB->caption())));

    /// And the CONTENT is B's: every band holds a page rendered from ex-manypage. If the old
    /// notebook's renderer was reused, all three renders come back empty (A's PDF has no page 11)
    /// and every band is desk colour and nothing else.
    const KisImageSP imageB = documentB->image();
    QVERIFY(imageB);
    QList<KisNodeSP> bandsB;
    for (quint32 i = 0; i < imageB->root()->childCount(); ++i) {
        if (PdfPageSaver::isPageBackground(imageB->root()->at(i))) {
            bandsB.append(imageB->root()->at(i));
        }
    }
    QCOMPARE(bandsB.size(), 3);
    for (int slot = 0; slot < bandsB.size(); ++slot) {
        QVERIFY2(bandHasARenderedPage(bandsB.at(slot)),
                 qPrintable(QStringLiteral("band %1 of the imported notebook has no page in it: the "
                                           "old notebook's PDF was rendered into the new window")
                                .arg(slot + 1)));
    }

    /// And scrolling on, into a SECOND window of the new notebook: it is built from the new
    /// notebook's own pages too, with no band left over from the window before it or from the old
    /// notebook. Page 6 is outside the window [1..3], so this is a roll.
    QVERIFY2(navigator->showPage(5, &why), qPrintable(why));
    QCOMPARE(navigator->currentIndex(), 5);

    const KisImageSP rolled = navigator->currentDocument()->image();
    QVERIFY(rolled);
    QList<KisNodeSP> rolledBands;
    QStringList rolledNames;
    for (quint32 i = 0; i < rolled->root()->childCount(); ++i) {
        if (PdfPageSaver::isPageBackground(rolled->root()->at(i))) {
            rolledBands.append(rolled->root()->at(i));
            rolledNames.append(rolled->root()->at(i)->name());
        }
    }
    QCOMPARE(rolledBands.size(), 3);
    /// The window around page 6 holds notebook positions 4..6, whose source pages are 13..15, and the
    /// band names are the LAYER names the builder gives those records -- so this says which pages the
    /// new window is holding, not only that it painted something.
    QCOMPARE(rolledNames,
             QStringList({ PdfStripBuilder::backgroundLayerName(13),
                           PdfStripBuilder::backgroundLayerName(14),
                           PdfStripBuilder::backgroundLayerName(15) }));
    for (int slot = 0; slot < rolledBands.size(); ++slot) {
        QVERIFY2(bandHasARenderedPage(rolledBands.at(slot)),
                 qPrintable(QStringLiteral("band %1 of the second window after the import has no page "
                                           "in it").arg(slot + 1)));
    }

    /// The inverse, while we are here: going BACK to A rebuilds A -- the renderer cache cleared the
    /// other way round -- and the ink written before the import is back on the page it was drawn on.
    QVERIFY2(navigator->openNotebookDir(dirA, &why), qPrintable(why));
    QCOMPARE(navigator->projectDir(), dirA);
    QCOMPARE(navigator->manifest().displayName(), QStringLiteral("Notebook A"));

    KisDocument *documentA = navigator->currentDocument();
    QVERIFY(documentA);
    QCOMPARE(documentA->property("pdfioProjectDir").toString(), dirA);

    const PdfStripLayout layoutA = PdfStripLayout::forWindow(navigator->manifest(), 0, 3,
                                                             navigator->currentRenderDpi());
    QVERIFY(layoutA.isValid());
    KisPaintLayer *inkA = stripInkLayer(documentA->image());
    QVERIFY(inkA);
    QVERIFY2(inkMarkInImage(inkA->paintDevice()->convertToQImage(0, layoutA.slots().at(0).rect)),
             "the ink written before the import did not come back with the notebook");

    /// And the same notebook opened twice in a row: one document, still A's, no stale B left over.
    QVERIFY2(navigator->openNotebookDir(dirA, &why), qPrintable(why));
    QCOMPARE(navigator->projectDir(), dirA);
    QVERIFY(navigator->currentDocument());
    QCOMPARE(navigator->currentDocument()->property("pdfioProjectDir").toString(), dirA);
}

/**
 * Deleting a notebook: the open one is closed for real, its folder goes, and the recent list forgets
 * it.
 *
 * The notebook store only ever grew -- import, open a bundle, extract, merge all make a notebook and
 * nothing ever removed one -- so the delete is the missing half. Two things have to be true at once:
 * the folder is gone, and the navigator no longer has the notebook open. The second is the one that
 * costs something: the strip's document and its layers are the largest thing the application holds,
 * so a delete that leaves them behind is not a delete, it is a leak. This drives the order the menu
 * entry uses -- write the open pages, remove the folder, close the notebook -- and then asks the
 * navigator whether it still knows about any of it.
 */
void PdfNavigatorIntegrationTest::testDeletingANotebookClosesItAndRemovesItsFolder()
{
    QVERIFY(useNotebook(QStringLiteral("deletable")));
    const QString project = navigator()->projectDir();
    QVERIFY2(!project.isEmpty(), "the notebook has no directory to delete");
    QVERIFY2(PdfPageNavigator::isInsideNotebookStore(project), qPrintable(project));
    QVERIFY2(QFileInfo::exists(PdfSession::manifestPath(project)), qPrintable(project));

    /// The store's own entry, the way the menus remember one -- the recent list is what the delete
    /// has to forget along with the folder.
    const QString entry =
        project + PdfPageNavigator::recentNotebookSeparator() + QStringLiteral("deletable");
    QStringList entries = PdfPageNavigator::recentNotebooks();
    entries.removeAll(entry);
    entries.prepend(entry);
    PdfPageNavigator::setRecentNotebooks(entries);
    QVERIFY2(PdfPageNavigator::recentNotebooks().contains(entry), "the recent entry was not written");

    /// The open notebook has a page document behind it, and ink that is not on disk yet: the close
    /// has both something to let go of and something to write first.
    QVERIFY(navigator()->hasNotebook());
    QVERIFY(navigator()->pageCount() > 0);
    KisDocument *page = navigator()->currentDocument();
    QVERIFY(page);
    drawInk(page);
    QVERIFY(page->isModified());

    /// Exactly what the menu entry does, without the confirmation dialog: the ink is written, the
    /// folder is removed while the notebook is still open, and then the notebook is closed.
    QString why;
    QVERIFY2(navigator()->prepareForClose(), "the open page could not be written before the delete");
    QVERIFY2(PdfPageNavigator::removeNotebookStore(project, &why), qPrintable(why));
    QVERIFY2(navigator()->closeNotebook(&why), qPrintable(why));

    QVERIFY2(!QFileInfo::exists(project), qPrintable(project));
    QVERIFY2(!navigator()->hasNotebook(), "the navigator still reports a notebook after the delete");
    QVERIFY2(!navigator()->currentDocument(), "the page document outlived the deleted notebook");
    QVERIFY2(!navigator()->currentView(), "the page view outlived the deleted notebook");
    QVERIFY(navigator()->projectDir().isEmpty());
    QVERIFY(navigator()->manifest().pages.isEmpty());
    QCOMPARE(navigator()->pageCount(), 0);
    QCOMPARE(navigator()->currentIndex(), -1);
    QVERIFY2(!PdfPageNavigator::recentNotebooks().contains(entry),
             "the recent list still names a notebook that is gone");
}

/**
 * Deleting refuses what is not the store's, and a refusal leaves the open notebook exactly as it was.
 *
 * The refusal has to come before anything is touched: a half-removed notebook is worse than one that
 * was not removed, and the rule that keeps this from being aimed at the user's own documents is that
 * only a directory inside the project root is the store's. The last part makes a removal actually
 * fail -- a directory in the notebook's own tree with the write bit off -- and shows the notebook is
 * still the one that is open and still works; a run that ignores the permission bits (root in a
 * container) cannot make that failure and says so rather than pretending.
 */
void PdfNavigatorIntegrationTest::testDeletingRefusesWhatIsNotTheStores()
{
    /// A directory outside the notebook store: not ours, refused, left alone.
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    QVERIFY2(!PdfPageNavigator::isInsideNotebookStore(outside.path()),
             "a directory outside the store was accepted as the store's");
    QString why;
    QVERIFY2(!PdfPageNavigator::removeNotebookStore(outside.path(), &why),
             "a directory outside the store was removed");
    QVERIFY2(!why.isEmpty(), "the refusal gave no reason");
    QVERIFY2(QFileInfo::exists(outside.path()), "the refusal removed the directory anyway");

    /// The store's own root is not a notebook either.
    QVERIFY(!PdfPageNavigator::isInsideNotebookStore(PdfSession::projectRoot()));

    /// And something inside the store that is not a notebook directory: a plain file, which must not
    /// be removed just because it is in the right place.
    const QString stray = QDir(PdfSession::projectRoot()).filePath(QStringLiteral("not-a-notebook"));
    {
        QFile file(stray);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a notebook");
    }
    why.clear();
    QVERIFY2(!PdfPageNavigator::removeNotebookStore(stray, &why),
             "a file inside the store was accepted as a notebook");
    QVERIFY2(!why.isEmpty(), "the refusal gave no reason");
    QVERIFY2(QFileInfo::exists(stray), "the file was removed");
    QFile::remove(stray);

    /// A real notebook, open, whose tree cannot be emptied: its pages directory is made unwritable.
    QVERIFY(useNotebook(QStringLiteral("undeletable")));
    const QString project = navigator()->projectDir();
    const QString pages = QDir(project).filePath(QStringLiteral("pages"));
    QVERIFY2(QFileInfo(pages).isDir(), qPrintable(pages));

    const QFile::Permissions original = QFile::permissions(pages);
    QFile::setPermissions(pages,
                          QFile::ReadOwner | QFile::ReadUser | QFile::ExeOwner | QFile::ExeUser);
    const bool unwritable = !QFileInfo(pages).isWritable();
    if (!unwritable) {
        QFile::setPermissions(pages, original);
        QSKIP("this run ignores the permission bits, so a removal cannot be made to fail here");
    }

    why.clear();
    const bool refused = !PdfPageNavigator::removeNotebookStore(project, &why);
    /// Put back first, whatever the rest of the test does: the run's own cleanup removes this tree.
    QFile::setPermissions(pages, original);

    QVERIFY2(refused, "a notebook whose tree cannot be emptied was removed anyway");
    QVERIFY2(!why.isEmpty(), "the refusal gave no reason");
    QVERIFY2(QFileInfo::exists(PdfSession::manifestPath(project)),
             "the refusal left the notebook half-removed: its manifest is gone");

    /// Still the same open notebook, and still working: the page turn goes through the document and
    /// the saves the way it always does.
    QVERIFY(navigator()->hasNotebook());
    QVERIFY(navigator()->currentDocument());
    QCOMPARE(navigator()->projectDir(), project);
    why.clear();
    QVERIFY2(navigator()->next(&why), qPrintable(why));
    QCOMPARE(navigator()->currentIndex(), 1);
}

/**
 * The rendered window's memory budget: what the strip may cost, and the resolution that buys.
 *
 * A page can be LANDSCAPE, which is why the knob is the memory and not a pixel target: a bound on
 * the longest side buys a different amount of memory for every shape, so the same target is 1.8 Mpx
 * on a 16:9 page and 2.5 Mpx on an A4 portrait, and a window of five slides costs several times a
 * window of five A4 sheets. This notebook is deliberately MIXED -- two of its five pages are turned
 * a right angle, so the window holds landscape pages beside portrait ones -- because that is the case
 * the pixel target got wrong.
 *
 * What is asserted is what the user asked for: the window the strip really built costs no more than
 * the budget it was given, the budget is spent rather than thrown away when it is small enough to
 * bind, a budget above what 200 dpi already costs changes nothing, and "no limit" is exactly the dpi
 * and the image size this notebook has always opened at.
 */
void PdfNavigatorIntegrationTest::testTheMemoryBudgetBoundsTheStrip()
{
    /// A mixed-orientation notebook from the real many-page fixture: five Letter pages, the first and
    /// the third turned a right angle -- landscape beside portrait, from the fixture's own pages and
    /// through the real renderer, not a doctored sizePt.
    const QString project = m_dir.filePath(QStringLiteral("memory-budget"));
    QString why;
    QVERIFY2(writeMixedFivePageNotebook(project, &why), qPrintable(why));

    /// The budget that is already saved is put back however this test ends: it is persisted, and a
    /// failure walking away from one would change what the next RUN of the suite opens at.
    BudgetRestore restore(PdfPageNavigator::instance()->memoryBudgetMb());

    /// The budget for this window is set BEFORE it is opened: a window of this size at 200 dpi costs
    /// about 560 MB of layers, and the first build must not allocate that just so the test can shrink
    /// it afterwards.
    PdfPageNavigator::instance()->setMemoryBudgetMb(200);
    PdfPageNavigator::instance()->setScope(5);
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(project, &why), qPrintable(why));

    const PdfSessionManifest opened = navigator()->manifest();
    QCOMPARE(opened.pages.size(), 5);
    const int scope = navigator()->scope();
    QCOMPARE(scope, 5);

    /// The mix really is a mix: a landscape page and a portrait page in the same window.
    const QSizeF landscape = opened.pages.at(0).displaySizePt();
    QVERIFY2(landscape.width() > landscape.height(), "page 1 is not the landscape one");
    const QSizeF portrait = opened.pages.at(1).displaySizePt();
    QVERIFY2(portrait.height() > portrait.width(), "page 2 is not the portrait one");

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    QVERIFY(document->image());

    /// How many full-size layers the strip really has, counted from the document that was built.
    const int layers = paintLayersOf(document->image());
    QVERIFY2(layers >= 2, "the strip has fewer layers than a window with ink on it needs");

    /// The size the window has at the reference dpi, which is the size this notebook has always
    /// opened at -- read off the layout, so no 200 dpi document has to be allocated to know it.
    const PdfStripLayout reference =
        PdfStripLayout::forWindow(opened, navigator()->currentIndex(), scope, 200.0);
    QVERIFY(reference.isValid());
    const QSize atDefault = reference.imageSize();

    /// Every finite budget bounds the memory the window really holds: area x layers x 4 bytes, in the
    /// decimal megabytes the menu names. Small enough to bind and it is spent, not thrown away; above
    /// what 200 dpi already costs and it changes nothing at all.
    for (int megabytes : { 200, 400, 800 }) {
        PdfPageNavigator::instance()->setMemoryBudgetMb(megabytes);
        QCOMPARE(PdfPageNavigator::instance()->memoryBudgetMb(), megabytes);

        const QSize size = document->image()->size();
        const qreal bytes = qreal(size.width()) * qreal(size.height()) * qreal(layers) * 4.0;
        const qreal budgetBytes = qreal(megabytes) * 1000.0 * 1000.0;
        QVERIFY2(bytes <= budgetBytes,
                 qPrintable(QStringLiteral("a %1 MB budget built %2x%3 x %4 layers = %5 MB")
                                .arg(megabytes)
                                .arg(size.width())
                                .arg(size.height())
                                .arg(layers)
                                .arg(bytes / 1000000.0, 0, 'f', 1)));

        const qreal dpi = navigator()->currentRenderDpi();
        const PdfStripLayout landed =
            PdfStripLayout::forWindow(opened, navigator()->currentIndex(), scope, dpi);
        QVERIFY(landed.isValid());
        QCOMPARE(size, landed.imageSize());

        /// And the change must not leave the tab marked modified. The roll clears the flag where
        /// Krita's resize set it and again at its own end (after the layer adoption, which can mark
        /// it), and the menu's own apply path clears it through that same helper -- a strip that says
        /// it is modified is a strip Krita autosaves, and that autosave is the memory the user
        /// watched climb.
        QVERIFY2(!document->isModified(), "a budget change left the document marked modified");

        if (dpi < 200.0) {
            /// It binds, so the window is what the budget buys and not less: a derivation that came
            /// out half empty would be throwing pixels away for nothing.
            QVERIFY2(bytes >= budgetBytes * 0.9,
                     qPrintable(QStringLiteral("a %1 MB budget only spent %2 MB")
                                    .arg(megabytes)
                                    .arg(bytes / 1000000.0, 0, 'f', 1)));

            /// And the page size the menu names beside the budget is the longest page in the window
            /// that budget built, within the layout's own rounding.
            int longestRendered = 0;
            for (const PdfStripLayout::Slot &slot : landed.slots()) {
                longestRendered = qMax(longestRendered, qMax(slot.rect.width(), slot.rect.height()));
            }
            const int named = navigator()->longestPagePixelsForBudget(megabytes);
            QVERIFY2(qAbs(longestRendered - named) <= 1,
                     qPrintable(QStringLiteral("the menu names %1 px and the window has %2 px")
                                    .arg(named)
                                    .arg(longestRendered)));
        } else {
            /// The ceiling: a budget above what 200 dpi already costs changes nothing, so the window
            /// is the size it has always been.
            QCOMPARE(size, atDefault);
        }
    }

    /// "No limit" is today exactly: 200 dpi, and the window the layout at 200 dpi has.
    PdfPageNavigator::instance()->setMemoryBudgetMb(0);
    QCOMPARE(PdfPageNavigator::instance()->memoryBudgetMb(), 0);
    QCOMPARE(navigator()->currentRenderDpi(), 200.0);
    QCOMPARE(document->image()->size(), atDefault);
    /// And the page size the menu names for it is the 200 dpi one, from the same derivation.
    int longestAtDefault = 0;
    for (const PdfStripLayout::Slot &slot : reference.slots()) {
        longestAtDefault = qMax(longestAtDefault, qMax(slot.rect.width(), slot.rect.height()));
    }
    QVERIFY2(qAbs(navigator()->longestPagePixelsForBudget(0) - longestAtDefault) <= 1,
             "the page size the menu names for no limit is not the 200 dpi one");

    /// The choice is written where a restart reads it.
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/memoryBudgetMb")).toInt(), 0);
    PdfPageNavigator::instance()->setMemoryBudgetMb(400);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/memoryBudgetMb")).toInt(), 400);

    /// And the key the previous build used for its pixel target is ignored, not read: a settings file
    /// left behind must not bring the pixel behaviour back.
    QSettings stale;
    stale.setValue(QStringLiteral("pdfio/maxPagePixels"), 1200);
    stale.sync();
    PdfPageNavigator::instance()->setMemoryBudgetMb(0);
    QCOMPARE(navigator()->currentRenderDpi(), 200.0);
    stale.remove(QStringLiteral("pdfio/maxPagePixels"));
}

/**
 * A TYPED budget: a value that is not one of the presets, through the same setter and the same
 * derivation.
 *
 * The presets are three steps of a knob whose useful range depends on the window -- a window of five
 * slides costs several times a window of five A4 sheets at the same dpi, which is why the knob is the
 * memory at all -- so the value that suits the user is often not on the menu. This is the value
 * handling the typed dialog hands to the navigator, tested where it can be: the dialog widget itself
 * lives in the plugin, which is not in this binary.
 */
void PdfNavigatorIntegrationTest::testATypedBudgetIsHeldAndDerived()
{
    const QString project = m_dir.filePath(QStringLiteral("typed-budget"));
    QString why;
    QVERIFY2(writeMixedFivePageNotebook(project, &why), qPrintable(why));

    /// The budget that is already saved is put back however this test ends.
    BudgetRestore restore(PdfPageNavigator::instance()->memoryBudgetMb());

    /// A budget BEFORE it is opened, so a window that costs about 560 MB at 200 dpi is not allocated
    /// at full size first.
    PdfPageNavigator::instance()->setMemoryBudgetMb(100);
    PdfPageNavigator::instance()->setScope(5);
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(project, &why), qPrintable(why));

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    QVERIFY(document->image());
    const PdfSessionManifest opened = navigator()->manifest();
    const int scope = navigator()->scope();

    /// The presets on either side of the typed value, for the window that is up.
    PdfPageNavigator::instance()->setMemoryBudgetMb(200);
    const qreal dpiAt200 = navigator()->currentRenderDpi();
    PdfPageNavigator::instance()->setMemoryBudgetMb(400);
    const qreal dpiAt400 = navigator()->currentRenderDpi();

    /// A typed value, not one of the presets: persisted like any other, and derived by the SAME
    /// derivation -- its dpi sits between the two presets, which is what says there is not a second
    /// one for typed numbers.
    PdfPageNavigator::instance()->setMemoryBudgetMb(300);
    QCOMPARE(PdfPageNavigator::instance()->memoryBudgetMb(), 300);
    QCOMPARE(QSettings().value(QStringLiteral("pdfio/memoryBudgetMb")).toInt(), 300);

    const qreal typedDpi = navigator()->currentRenderDpi();
    QVERIFY2(typedDpi > dpiAt200 && typedDpi < dpiAt400,
             qPrintable(QStringLiteral("300 MB gave %1 dpi, outside the %2..%3 the presets gave")
                            .arg(typedDpi)
                            .arg(dpiAt200)
                            .arg(dpiAt400)));

    /// And it went through the same resize path: the document is the layout at that dpi, and the
    /// budget change did not leave it marked modified.
    const PdfStripLayout typed =
        PdfStripLayout::forWindow(opened, navigator()->currentIndex(), scope, typedDpi);
    QVERIFY(typed.isValid());
    QCOMPARE(document->image()->size(), typed.imageSize());
    QVERIFY2(!document->isModified(), "a typed budget left the document marked modified");

    /// The floor and the ceiling hold it, and 0 is still "no limit" rather than a tiny budget.
    PdfPageNavigator::instance()->setMemoryBudgetMb(PdfPageNavigator::minMemoryBudgetMb() - 15);
    QCOMPARE(PdfPageNavigator::instance()->memoryBudgetMb(), PdfPageNavigator::minMemoryBudgetMb());
    PdfPageNavigator::instance()->setMemoryBudgetMb(PdfPageNavigator::maxMemoryBudgetMb() * 4);
    QCOMPARE(PdfPageNavigator::instance()->memoryBudgetMb(), PdfPageNavigator::maxMemoryBudgetMb());
    PdfPageNavigator::instance()->setMemoryBudgetMb(0);
    QCOMPARE(PdfPageNavigator::instance()->memoryBudgetMb(), 0);
    QCOMPARE(navigator()->currentRenderDpi(), 200.0);

    /// What the dialog shows under the number: the memory the window really costs. While the budget
    /// binds that is the number typed, to the megabyte -- what the dialog promises.
    const int costAtNoLimit = navigator()->windowCostMbForBudget(0);
    QVERIFY(costAtNoLimit > 0);
    QCOMPARE(navigator()->windowCostMbForBudget(PdfPageNavigator::maxMemoryBudgetMb()), costAtNoLimit);
    for (int megabytes : { 200, 300, 400 }) {
        const int cost = navigator()->windowCostMbForBudget(megabytes);
        QVERIFY2(qAbs(cost - megabytes) <= 1,
                 qPrintable(QStringLiteral("a %1 MB budget is shown as costing %2 MB")
                                .arg(megabytes)
                                .arg(cost)));
    }

    /// And the page size the dialog names grows with the budget, through that one derivation.
    QVERIFY(navigator()->longestPagePixelsForBudget(200)
            < navigator()->longestPagePixelsForBudget(300));
    QVERIFY(navigator()->longestPagePixelsForBudget(300)
            < navigator()->longestPagePixelsForBudget(400));
}

/**
 * WHAT HOLDS THE NATIVE HEAP -- a measurement, not an assertion.
 *
 * The device showed the document's own size barely touching the resident figure: 573 MB native with a
 * 531.9 MiB document open, 593 MB fifteen seconds after the budget took it to 163.8 MiB, and 588 MB
 * after the document was closed -- the document's pixels were not returned. Two candidates: the crop
 * command retaining the pixels it removed, and Krita's own tile store holding tiles after the image is
 * gone. This walks the process through both and prints what the process and Krita's tile store hold at
 * each step:
 *
 *   - the crop's undo is measured by clearing the undo history, which frees exactly what a resize
 *     command retains;
 *   - the store is measured by closing the document, which destroys its image and every tile it owns;
 *   - and the allocator -- a third candidate -- by malloc_trim(), which hands back what Krita has
 *     already freed. glibc keeps freed blocks in its arenas; Android's allocator is a different one,
 *     which is why the device and the harness can differ.
 *
 * No threshold is asserted: these figures are machine-dependent, and a test that failed on a device it
 * was never measured on would be worse than no test. Read the [mem] lines.
 */
void PdfNavigatorIntegrationTest::testWhatHoldsTheNativeHeap()
{
    const auto report = [](const char *stage) {
        KisTileDataStore::instance()->tryForceUpdateMemoryStatisticsWhileIdle();
        const KisTileDataStore::MemoryStatistics stats =
            KisTileDataStore::instance()->memoryStatistics();
        qWarning("[mem] %-34s rss %8lld kB | tiles total %8lld real %8lld pool %8lld swap %8lld kB",
                 stage,
                 residentKb(),
                 stats.totalMemorySize / 1024,
                 stats.realMemorySize / 1024,
                 stats.poolSize / 1024,
                 stats.swapSize / 1024);
    };

    /// A baseline with nothing open, whatever the tests before this one left behind in the store.
    QString why;
    if (navigator()->hasNotebook()) {
        QVERIFY2(navigator()->closeNotebook(&why), qPrintable(why));
    }
    QTest::qWait(300);
    report("baseline, nothing open");

    const QString project = m_dir.filePath(QStringLiteral("native-heap"));
    QVERIFY2(writeMixedFivePageNotebook(project, &why), qPrintable(why));

    PdfPageNavigator::instance()->setMemoryBudgetMb(0);
    PdfPageNavigator::instance()->setScope(5);
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(project, &why), qPrintable(why));

    KisDocument *document = navigator()->currentDocument();
    QVERIFY(document);
    QVERIFY(document->image());
    qWarning("[mem] the no-limit document is %dx%d, about %d MB across its layers",
             document->image()->width(),
             document->image()->height(),
             navigator()->windowCostMbForBudget(0));
    report("open at no limit (200 dpi)");

    PdfPageNavigator::instance()->setMemoryBudgetMb(200);
    report("immediately after 200 MB");

    /// The device's own shape: the temporaries went after a few seconds, the retained pixels did not.
    QTest::qWait(1500);
    report("1.5 s after 200 MB");

    /// (a) THE CROP'S UNDO. The resize pushed a command that retains the pixels it removed so they can
    /// be restored; clearing the history is exactly that memory. A drop here is candidate (a).
    document->clearUndoHistory();
    QTest::qWait(300);
    report("undo history cleared");

    /// (b) THE STORE. Closing destroys the image and unregisters every tile it owned. What is not
    /// given back now is process-global -- Krita's tile data store and its pooler -- or the allocator.
    QVERIFY2(navigator()->closeNotebook(&why), qPrintable(why));
    QTest::qWait(500);
    report("document closed");

#if defined(__linux__)
    /// And which of those two: this asks the allocator for what it has already been given.
    malloc_trim(0);
    QTest::qWait(200);
    report("after malloc_trim(0)");
#endif

    /// A second, small notebook: does opening one reuse what the first left, or add to it?
    const QString small = m_dir.filePath(QStringLiteral("native-heap-small"));
    QVERIFY2(writeMixedFivePageNotebook(small, &why), qPrintable(why));
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(small, &why), qPrintable(why));
    QTest::qWait(300);
    report("second notebook open");
}

/**
 * A notebook is never named after the picker's cache copy.
 *
 * On Android the picker copies the chosen PDF into the app's cache under a name the app supplies --
 * "pdfio-picked.pdf" -- and the notebook used to be named after it whenever the provider could not be
 * asked for the name a person actually sees. Two of the device's five notebooks carry one: a Start
 * screen row, a tab and an export suggestion reading "pdfio-picked-notes (2)", which is a path, not a
 * title. The rule lives in one place (PdfPageNavigator::isInternalNotebookName and the default it
 * falls back to) and the manifest is where it is applied, because the tab, the Start screen, the
 * recent list and the export suggestion all read the name from there.
 */
void PdfNavigatorIntegrationTest::testANotebookIsNeverNamedAfterTheCacheCopy()
{
    /// The rule: the names this application really writes, with the extension and the "(2)" a copy
    /// gets removed, and nothing else.
    QVERIFY(PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked")));
    QVERIFY(PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked.pdf")));
    QVERIFY(PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked-notes (2)")));
    QVERIFY(PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked-pages")));
    QVERIFY(PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked-notebook")));
    QVERIFY(PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked-image")));

    /// And a name a person would plausibly type is not one of ours -- including one that starts the
    /// same way, which is why this is a list of whole names rather than a prefix rule.
    QVERIFY(!PdfPageNavigator::isInternalNotebookName(QStringLiteral("posn1-67-com")));
    QVERIFY(!PdfPageNavigator::isInternalNotebookName(QString::fromUtf8("ใบงานเขียนอ้างอิงงานวิจัย")));
    QVERIFY(!PdfPageNavigator::isInternalNotebookName(QStringLiteral("pdfio-picked-up-the-wrong-file")));
    QVERIFY(!PdfPageNavigator::isInternalNotebookName(QStringLiteral("My notes")));
    QVERIFY(!PdfPageNavigator::isInternalNotebookName(QString()));

    /// The human default, and that it is neither a path nor an internal name.
    const QString fallback = PdfPageNavigator::defaultNotebookName();
    QVERIFY(!fallback.isEmpty());
    QVERIFY(!PdfPageNavigator::isInternalNotebookName(fallback));
    QVERIFY(!fallback.contains(QLatin1Char('/')));
    QVERIFY(!fallback.contains(QStringLiteral("pdfio-picked")));

    /// The reader half, which is what every reader of a notebook's name goes through: an internal or
    /// missing name shows the default, a name a person chose shows itself.
    QCOMPARE(PdfPageNavigator::usableNotebookName(QString()), fallback);
    QCOMPARE(PdfPageNavigator::usableNotebookName(QStringLiteral("pdfio-picked")), fallback);
    QCOMPARE(PdfPageNavigator::usableNotebookName(QStringLiteral("pdfio-picked-notes (2)")), fallback);
    QCOMPARE(PdfPageNavigator::usableNotebookName(QStringLiteral("posn1-67-com")),
             QStringLiteral("posn1-67-com"));

    /// One page at a time: these tests are about a name, not about memory.
    PdfPageNavigator::instance()->setScope(1);
    QString why;

    /// A manifest carrying an internal name is repaired BY OPENING IT -- on disk and in the copy the
    /// navigator holds, which is what the tab and the docker show.
    const QString cacheNamed = m_dir.filePath(QStringLiteral("cache-named"));
    QVERIFY2(writeNamedNotebook(cacheNamed, QStringLiteral("pdfio-picked"), &why), qPrintable(why));
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(cacheNamed, &why), qPrintable(why));
    QCOMPARE(PdfPageNavigator::instance()->manifest().name, fallback);

    const PdfSessionManifest repaired =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(cacheNamed), &why);
    QVERIFY(repaired.isValid());
    QCOMPARE(repaired.name, fallback);
    QVERIFY2(!repaired.name.contains(QStringLiteral("pdfio-picked")),
             "the notebook is still named after the picker's cache copy");

    /// The other shape the device showed, with the "(2)" a second copy gets.
    const QString notesNamed = m_dir.filePath(QStringLiteral("cache-named-notes"));
    QVERIFY2(writeNamedNotebook(notesNamed, QStringLiteral("pdfio-picked-notes (2)"), &why),
             qPrintable(why));
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(notesNamed, &why), qPrintable(why));
    const PdfSessionManifest notes =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(notesNamed), &why);
    QVERIFY(notes.isValid());
    QCOMPARE(notes.name, fallback);

    /// A name a person chose -- or a provider gave -- is left exactly as it is, which is the rule the
    /// rename path ("Rename notebook...") also follows.
    const QString chosen = m_dir.filePath(QStringLiteral("named-by-a-person"));
    QVERIFY2(writeNamedNotebook(chosen, QStringLiteral("posn1-67-com"), &why), qPrintable(why));
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(chosen, &why), qPrintable(why));
    QCOMPARE(PdfPageNavigator::instance()->manifest().name, QStringLiteral("posn1-67-com"));
    const PdfSessionManifest kept =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(chosen), &why);
    QVERIFY(kept.isValid());
    QCOMPARE(kept.name, QStringLiteral("posn1-67-com"));

    const QString typed = m_dir.filePath(QStringLiteral("renamed-by-hand"));
    QVERIFY2(writeNamedNotebook(typed, QStringLiteral("My notes"), &why), qPrintable(why));
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(typed, &why), qPrintable(why));
    QCOMPARE(PdfPageNavigator::instance()->manifest().name, QStringLiteral("My notes"));

    /// The import fallback: a notebook whose manifest has NO name -- what the picker path leaves when
    /// the provider is still to be asked -- is settled with the human default when the app settles it,
    /// and never with the file it was imported from. Opening alone leaves the name alone on purpose:
    /// on Android the provider's own name is asked for straight afterwards.
    const QString unnamed = m_dir.filePath(QStringLiteral("no-name-yet"));
    QVERIFY2(writeNamedNotebook(unnamed, QString(), &why), qPrintable(why));
    QVERIFY2(PdfPageNavigator::instance()->openNotebookDir(unnamed, &why), qPrintable(why));
    QVERIFY(PdfPageNavigator::instance()->manifest().name.isEmpty());

    QString namedWhy;
    QVERIFY2(PdfPageNavigator::instance()->ensureNotebookName(true, &namedWhy), qPrintable(namedWhy));
    QCOMPARE(PdfPageNavigator::instance()->manifest().name, fallback);
    const PdfSessionManifest named =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(unnamed), &why);
    QVERIFY(named.isValid());
    QCOMPARE(named.name, fallback);
    QVERIFY2(!named.name.contains(QStringLiteral("text-fixture")),
             "the notebook was named after the file it was imported from");
    QVERIFY2(!PdfPageNavigator::isInternalNotebookName(named.name), "the default is an internal name");

    /// And settling it again never rewrites a name that is already a person's.
    QVERIFY2(PdfPageNavigator::instance()->ensureNotebookName(true, &namedWhy), qPrintable(namedWhy));
    QCOMPARE(PdfPageNavigator::instance()->manifest().name, fallback);
}

int main(int argc, char *argv[])
{
    qputenv("LANGUAGE", "en");
    QStandardPaths::setTestModeEnabled(true);

    /// A safe assert in a headless run has nobody to press Ignore, and the dialog it would put up
    /// is answered by the watchdog below as if Abort had been pressed -- Krita aborts. This is the
    /// switch for exactly that: an ignorable assert warns and recovers, as it would for a user who
    /// pressed Ignore. See libs/global/kis_assert.cpp.
    qputenv("KRITA_NO_ASSERT_MSG", "1");

    /// The ink-only save is Krita's .kra export, which is a plugin: without it the save the test
    /// is about cannot even start. The build puts the plugins in one directory, which is what the
    /// plugin trader reads when the variable is set.
    qputenv("KRITA_PLUGIN_PATH", QByteArray(PDFIO_PLUGIN_DIR));
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PdfNavigatorIntegrationTest"));

    PdfNavigatorIntegrationTest test;
    const int failed = QTest::qExec(&test, argc, argv);

    /// And OUT of the process from here, deliberately, without returning through the static
    /// destructors: the application's teardown is Krita's and it is unsound (see the park in
    /// cleanupTestCase() for the five defects, the last of them a use-after-free in KoToolProxy), so
    /// the window and the views this suite built are left standing and the process ends now.
    ///
    /// QTest has already printed its summary and failed holds how many tests failed, so the exit
    /// status still means what it always meant -- a red suite stays red. The buffers are flushed by
    /// hand because _exit() does not do it, and ctest reads the summary from those streams.
    std::fflush(nullptr);
    ::_exit(failed == 0 ? 0 : 1);
}


/**
 * The pane's three modes stay apart: one in force at a time, named before a drag starts.
 *
 * Turn is the default and is untouched; Scale is the emphasis, reachable by typing a percentage as
 * well as by dragging; Box is its own mode with its own warning. The two resets are separate, which
 * is the user's own rule: putting the scale back must leave the crop in force and the other way
 * round.
 */
void PdfNavigatorIntegrationTest::testTheOpsPaneKeepsTurnScaleAndBoxApart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("ops-resize"));

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest = PdfSession::createProject(
        project, QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"), backend);
    QVERIFY2(manifest.isValid(), "the notebook this screen test needs could not be made");

    /// A preview the pane can draw, so the Box gesture has a page to take hold of.
    const QString thumbPath = QDir(project).filePath(manifest.pages.at(0).thumbFile);
    QImage sheet(180, 256, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(Qt::white);
    {
        QPainter painter(&sheet);
        painter.fillRect(QRect(10, 10, 40, 40), QColor(Qt::red));
        painter.fillRect(QRect(10, 200, 160, 20), QColor(Qt::black));
    }
    QVERIFY2(sheet.save(thumbPath), qPrintable(thumbPath));

    WatchdogPause watchdogPaused(m_dialogWatchdog);
    PdfNotebookOpsDialog dialog(project, manifest, 0);
    auto *turn = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_mode_turn"));
    auto *scale = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_mode_scale"));
    auto *boxMode = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_mode_box"));
    auto *readout = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_angle"));
    auto *warning = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_box_warning"));
    auto *field = dialog.findChild<QDoubleSpinBox *>(QStringLiteral("pdfio_ops_scale_value"));
    auto *resetScale = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_reset_scale"));
    auto *resetBox = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_reset_box"));
    auto *apply = dialog.findChild<QPushButton *>(QStringLiteral("pdfio_ops_apply"));
    auto *canvas = dialog.findChild<QWidget *>(QStringLiteral("pdfio_ops_canvas"));
    QVERIFY(turn && scale && boxMode && readout && warning && field && resetScale && resetBox && apply
            && canvas);

    /// Turn is in force to begin with, the pane says so, and that is what a drag would do.
    QVERIFY2(turn->isChecked() && !scale->isChecked() && !boxMode->isChecked(),
             "the pane did not open on Turn");
    QCOMPARE(dialog.dragMode(), PdfNotebookOpsDialog::TurnDrag);
    QVERIFY2(readout->text().contains(QStringLiteral("Turn")), qPrintable(readout->text()));
    QVERIFY2(!apply->isEnabled(), "Apply was offered before anything changed");

    /// Scale: the mode switch is exclusive, the readout names the mode, and the typed field is the
    /// second way in. A scale is a change, and the summary names it as a scale rather than a turn.
    scale->click();
    QVERIFY2(scale->isChecked() && !turn->isChecked() && !boxMode->isChecked(),
             "the mode switch is not exclusive");
    QCOMPARE(dialog.dragMode(), PdfNotebookOpsDialog::ScaleDrag);
    QVERIFY2(readout->text().contains(QStringLiteral("Scale")), qPrintable(readout->text()));
    field->setValue(150.0);
    QCOMPARE(dialog.edits().pages.at(0).extraScale, 1.5);
    QCOMPARE(dialog.edits().pages.at(0).displaySizePt(), manifest.pages.at(0).sizePt * 1.5);
    QVERIFY2(apply->isEnabled(), "a typed scale is not a pending edit");
    auto *summary = dialog.findChild<QLabel *>(QStringLiteral("pdfio_ops_summary"));
    QVERIFY(summary);
    QVERIFY2(summary->text().contains(QStringLiteral("scaled")), qPrintable(summary->text()));

    /// Box is its own mode with its own readout and warning, and the pane says what the crop would
    /// remove before Apply is pressed.
    boxMode->click();
    QVERIFY2(boxMode->isChecked() && !scale->isChecked(), "Box and Scale are not exclusive");
    QVERIFY2(readout->text().contains(QStringLiteral("Box")), qPrintable(readout->text()));
    QVERIFY2(!warning->text().isEmpty(), "Box mode said nothing about what the crop removes");

    dialog.show();
    QTest::qWait(50);

    /// A drag on the left edge: the crop the mode is for. The grip is where the drawn page's left
    /// edge is, measured off the pane itself rather than assumed.
    const QImage painted = canvas->grab().toImage();
    const QRect drawn = cardOnCanvas(painted);
    QVERIFY2(drawn.isValid(), "the pane drew no page to take hold of");
    const QPoint leftEdge(drawn.left() + 1, drawn.center().y());
    const QPoint pushedIn = leftEdge + QPoint(40, 0);
    const auto send = [canvas](QEvent::Type type, const QPoint &at, Qt::MouseButton button,
                               Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, canvas->mapToGlobal(at), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    send(QEvent::MouseButtonPress, leftEdge, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, pushedIn, Qt::NoButton, Qt::LeftButton);
    QVERIFY2(!dialog.edits().pages.at(0).boxPt.isValid(),
             "the crop was recorded while the hand was still down");
    QVERIFY2(readout->text().contains(QStringLiteral("Box")), qPrintable(readout->text()));
    send(QEvent::MouseButtonRelease, pushedIn, Qt::LeftButton, Qt::NoButton);

    const QRectF crop = dialog.edits().pages.at(0).boxPt;
    QVERIFY2(crop.isValid() && crop.x() > 10.0 && crop.width() < manifest.pages.at(0).sizePt.width(),
             qPrintable(QStringLiteral("the crop is %1,%2 %3x%4")
                            .arg(crop.x()).arg(crop.y()).arg(crop.width()).arg(crop.height())));
    QVERIFY2(summary->text().contains(QStringLiteral("cropped")), qPrintable(summary->text()));
    /// The pane warned BEFORE the drag and still warns after it: the ink inside the cut-away part is
    /// what the user is about to lose.
    QVERIFY2(!warning->text().isEmpty(), "the crop said nothing about the ink it removes");

    /// Resetting one mode leaves the other: Box goes back to the whole sheet and the scale of 1.5
    /// stays; then the scale goes back and the page is the one the notebook has.
    resetBox->click();
    QCOMPARE(dialog.edits().pages.at(0).boxPt, QRectF());
    QCOMPARE(dialog.edits().pages.at(0).extraScale, 1.5);
    resetScale->click();
    QCOMPARE(dialog.edits().pages.at(0).extraScale, 1.0);
    QVERIFY2(!apply->isEnabled(),
             "the page is back to what the notebook has, and Apply is still offered");

    /// And the screen hands the operation the one thing a crop cannot be applied without.
    QVERIFY2(static_cast<bool>(dialog.edits().clipper),
             "the screen did not name a clipper, so a crop would be refused");
}

/**
 * A scaled page is rendered at a LARGER DPI, not upscaled: the same source, more pixels.
 *
 * This is what keeps the pen's ink sharp and what makes the memory budget the thing that decides how
 * far a scale can go. The proof is that a 2x page at 100 dpi is byte for byte the source rendered at
 * 200 dpi -- and NOT the 100 dpi render stretched, which is a different picture however carefully it
 * is stretched.
 */
void PdfNavigatorIntegrationTest::testAScaledPageIsRenderedAtALargerDpi()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sourcePath = dir.filePath(QStringLiteral("source.pdf"));
    QVERIFY(QFile::copy(QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf"),
                        sourcePath));

    PdfSessionManifest manifest;
    manifest.sourceFile = QStringLiteral("source.pdf");
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(sourcePath);
    manifest.sourceByteSize = QFileInfo(sourcePath).size();
    manifest.pages.append(PdfPageRecord{0, QSizeF(595, 842), 0, QStringLiteral("pages/p0001.kra"),
                                        QString(), 0});

    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    QString why;
    const QImage once = renderers.renderPage(manifest, dir.path(), 0, 100.0, &why);
    QVERIFY2(!once.isNull(), qPrintable(why));

    manifest.pages[0].extraScale = 2.0;
    const QImage twice = renderers.renderPage(manifest, dir.path(), 0, 100.0, &why);
    QVERIFY2(!twice.isNull(), qPrintable(why));

    /// Twice the pixels in each direction: the same page at the same dpi, bigger.
    QVERIFY2(qAbs(twice.width() - once.width() * 2) <= 2
                 && qAbs(twice.height() - once.height() * 2) <= 2,
             qPrintable(QStringLiteral("a 2x page at 100 dpi rendered %1x%2, where twice the 1x "
                                       "render is %3x%4")
                            .arg(twice.width()).arg(twice.height())
                            .arg(once.width() * 2).arg(once.height() * 2)));

    /// And those pixels are the SOURCE's at twice the dpi, not the first render stretched: the same
    /// page at scale 1 and 200 dpi is the same picture, byte for byte.
    PdfSessionManifest unscaled = manifest;
    unscaled.pages[0].extraScale = 1.0;
    const QImage atTwiceTheDpi = renderers.renderPage(unscaled, dir.path(), 0, 200.0, &why);
    QVERIFY2(!atTwiceTheDpi.isNull(), qPrintable(why));
    QCOMPARE(twice, atTwiceTheDpi);
    QVERIFY2(twice != once.scaled(twice.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
             "the 2x page is the 1x render stretched, not rendered again at a larger dpi");

    /// A box renders its box only, at the frame the box's own points say.
    unscaled.pages[0].boxPt = QRectF(100, 100, 200, 300);
    const QImage boxed = renderers.renderPage(unscaled, dir.path(), 0, 100.0, &why);
    QVERIFY2(!boxed.isNull(), qPrintable(why));
    QVERIFY2(qAbs(boxed.width() - qRound(200.0 * 100.0 / 72.0)) <= 2
                 && qAbs(boxed.height() - qRound(300.0 * 100.0 / 72.0)) <= 2,
             qPrintable(QStringLiteral("a 200x300 point box at 100 dpi rendered %1x%2")
                            .arg(boxed.width()).arg(boxed.height())));
}

#include "PdfNavigatorIntegrationTest.moc"
