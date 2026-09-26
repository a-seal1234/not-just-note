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

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceCacheDb.h>
#include <KisResourceLocator.h>
#include <KisView.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KoColor.h>

#include <KoTestConfig.h>

#include <QApplication>
#include <QDialog>
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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtMath>
#include <QtTest>

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
    /// Last on purpose: it swaps the fixture and the scope, restores both, and runs after every
    /// test that would care -- so a failure inside it cascades to nothing that runs after it.
    void testRollWritesEveryWindowPageAndRedrawsFromDisk();

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
    const QList<QPointer<KisView>> views = KisPart::instance()->views();
    for (const QPointer<KisView> &view : views) {
        if (view) {
            view->closeView();
        }
    }
    QApplication::sendPostedEvents();
    QApplication::processEvents();

    if (m_mainWindow) {
        m_mainWindow->hide();
        QApplication::processEvents();
        delete m_mainWindow;
        m_mainWindow = nullptr;
        QApplication::sendPostedEvents();
        QApplication::processEvents();
    }

    /// Only now: every dialog the teardown itself can raise -- the "save it?" prompt above, and the
    /// one a save that is refused raises -- has to be dismissed while it is up.
    if (m_dialogWatchdog) {
        m_dialogWatchdog->stop();
    }

    /// The refusal test leaves its page artifact read-only on purpose. Put it back so the store
    /// this run created can be removed.
    const QString artifact = artifactFor(0);
    if (QFileInfo::exists(artifact)) {
        QFile::setPermissions(artifact,
                              QFile::ReadOwner | QFile::WriteOwner
                                  | QFile::ReadUser | QFile::WriteUser);
    }
    QDir(projectsRoot()).removeRecursively();
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

    /// Every paper layer holds its own page and NOTHING else: no desk colour around it any more, so
    /// the room the page leaves is transparent and the canvas background shows through. That is also
    /// the strictest form of the check this test has been making since task-9 -- a row of the
    /// previous window left outside the band it owns would extend these bounds -- so the page's own
    /// rectangle, within the pixel the two roundings may disagree on, is what is asserted.
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
        const QRect pageNow = holding.slots().at(i).rect;
        const QRect painted = papers.at(i)->paintDevice()->exactBounds();
        QVERIFY2(qAbs(painted.width() - pageNow.width()) <= 2
                     && qAbs(painted.height() - pageNow.height()) <= 2
                     && qAbs(painted.left() - pageNow.left()) <= 2
                     && qAbs(painted.top() - pageNow.top()) <= 2,
                 qPrintable(QStringLiteral("the paper of slot %1 is painted %2,%3 %4x%5, where its "
                                           "page sits at %6,%7 %8x%9")
                                .arg(i)
                                .arg(painted.x()).arg(painted.y())
                                .arg(painted.width()).arg(painted.height())
                                .arg(pageNow.x()).arg(pageNow.y())
                                .arg(pageNow.width()).arg(pageNow.height())));
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
    return QTest::qExec(&test, argc, argv);
}

#include "PdfNavigatorIntegrationTest.moc"
