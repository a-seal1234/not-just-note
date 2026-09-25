/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageNavigator.h"

#include "session/PdfInkLoader.h"
#include "session/PdfNotebookOps.h"
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
#include <QMessageBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
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
/// is the Ink group -- and a strip image is a Desk, one paper layer per slot, and an Ink group
/// above all of them with the stroke layer inside it. Asking the page-shaped helper about a strip
/// image hands back nothing, which is what this test hit on its first run.
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
