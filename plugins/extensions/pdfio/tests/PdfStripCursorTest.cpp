/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfInkLoader.h"
#include "session/PdfPageSaver.h"
#include "session/PdfSourceRenderers.h"
#include "session/PdfStripBuilder.h"
#include "session/PdfStripLayout.h"
#include "session/PdfSessionManifest.h"

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisResourceCacheDb.h>
#include <KisResourceLocator.h>
#include <KisView.h>
#include <KisViewManager.h>

#include <kis_coordinates_converter.h>
#include <kis_file_layer.h>
#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_node_manager.h>
#include <kis_painter.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_tool_utils.h>
#include <kis_types.h>

#include <KoColor.h>
#include <KoColorSpaceConstants.h>
#include <KoColorSpaceRegistry.h>
#include <KoTestConfig.h>

#include <kra_converter.h>

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KZip>

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>

/**
 * The three-slot strip, asked the questions the design has to answer before it ships.
 *
 * The strip is design B, and until now it has been judged from the outside: PdfStripLayoutTest
 * proves the arithmetic, PdfStripBuilderTest proves the layer tree, and the running application
 * decides everything else. This test closes part of the gap between "the probe says it worked" and
 * "the application says otherwise", which is how the per-page-group design was abandoned
 * (66dd60c0dc: activating a node from a plugin "does not take in the running application").
 *
 * Three questions, each answered by measurement rather than opinion:
 *
 *  1. Which call moves the node a tool would draw into, on a real view, and what happens when there
 *     is more than one view. Then whether locking every page folder but the intended one turns a
 *     failed activation into a refusal rather than a stroke on the wrong page.
 *  2. Cursor to page: the mapping the navigator uses in strip mode, over a real three-slot strip,
 *     including the gaps, the empty slots at the ends, and zoom, with the boundary behaviour
 *     measured so a hover-switch throttle can be decided from numbers.
 *  3. Several real layers in one page folder: what a save and a reload actually preserve, and what
 *     reading the layer stack back costs compared with reading the merged image.
 *
 * It is a KisPart test because questions 1 and 3 are about documents, views and the file format.
 */
class PdfStripCursorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testThreeSlotStripRendersEachPageInItsOwnSlot();
    void testCursorToPageOnTheStripAcrossGapsAndZoom();
    void testBoundariesAndScopeClamping();
    void testActivationMovesTheNodeTheToolsRead();
    void testViewsInOneWindowShareTheActiveNode();
    void testLockingTheFoldersIsWhatMakesTheStrokeRefusable();
    void testStaleActivationRefusesRatherThanMisdirects();
    void testAPageFolderWithSeveralLayersComesBackFlattened();
    void testAnInsertedImageLayerReachesThePageArtifact();
    void testTheSidecarKeepsEachLayersOwnPixels();
    void testRestoringTheLayerStackCostsAFullKraLoad();

private:
    struct PageFolders {
        KisImageSP image;
        QList<KisNodeSP> groups;
        QList<KisNodeSP> layers;
    };

    QString fixturePath() const { return QStringLiteral(FILES_DATA_DIR) + QStringLiteral("mr-strip-3page.pdf"); }
    PdfSessionManifest manifestFor(PdfRenderBackend &backend, int pages = -1) const;

    KisNodeSP childNamed(const KisImageSP &image, const QString &name) const;
    static bool colourNear(const QColor &a, const QColor &b, int tolerance = 16);
    static QRect bandRect(int band, const QSize &pageSize);
    static QColor bandColour(int band);

    PageFolders buildPageFolderImage(KisDocument *document, int pages, int pageWidth, int pageHeight) const;
    PageFolders buildBandedPage(KisDocument *document, int pageWidth, int pageHeight) const;
    void openDocument(KisDocument *document, const PageFolders &folders) const;

    KisView *addView(KisDocument *document) const;
    QList<KisView *> viewsFor(KisDocument *document) const;

    /// Closes what a test opened. The node manager belongs to the main window, so documents left
    /// open by earlier tests make a later test's activation land on another image view -- measured:
    /// activeNode() came back null for a folder that had just been activated, in a full run but not
    /// in isolation. Each test that opens a document closes it again.
    void closeNotebook(KisDocument *document, KisView *view) const;
    static QStringList paintLayerNames(const KisImageSP &image);
    static bool waitForFlag(bool &flag, int timeoutMs);

    QTemporaryDir m_dir;
    QString m_fixture;
    KisMainWindow *m_mainWindow = nullptr;

    /// Dismisses anything modal. A dialog nobody can press hangs a headless run, and the first one
    /// can arrive while a view is being built -- the same guard PdfNavigatorIntegrationTest uses.
    QTimer *m_dialogWatchdog = nullptr;
};

PdfSessionManifest PdfStripCursorTest::manifestFor(PdfRenderBackend &backend, int pages) const
{
    PdfSessionManifest manifest;
    manifest.sourceFile = QStringLiteral("mr-strip-3page.pdf");
    manifest.sourceSha256 = QByteArrayLiteral("0123456789abcdef");
    manifest.sourceByteSize = 1;

    const int count = pages >= 0 ? qMin(pages, backend.pageCount()) : backend.pageCount();
    for (int i = 0; i < count; ++i) {
        const PdfPageInfo info = backend.pageInfo(i);
        PdfPageRecord page;
        page.index = info.index;
        page.sizePt = info.sizePt;
        page.rotation = info.rotation;
        page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
        page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
        manifest.pages.append(page);
    }
    return manifest;
}

KisNodeSP PdfStripCursorTest::childNamed(const KisImageSP &image, const QString &name) const
{
    if (!image || !image->root()) {
        return KisNodeSP();
    }
    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        if (image->root()->at(i)->name() == name) {
            return image->root()->at(i);
        }
    }
    return KisNodeSP();
}

bool PdfStripCursorTest::colourNear(const QColor &a, const QColor &b, int tolerance)
{
    return qAbs(a.red() - b.red()) <= tolerance
        && qAbs(a.green() - b.green()) <= tolerance
        && qAbs(a.blue() - b.blue()) <= tolerance;
}

QRect PdfStripCursorTest::bandRect(int band, const QSize &pageSize)
{
    /// Three equal bands down the page, so the merged image carries all of them.
    const int height = pageSize.height() / 3;
    return QRect(0, band * height, pageSize.width(), height);
}

QColor PdfStripCursorTest::bandColour(int band)
{
    switch (band) {
    case 0:
        return QColor(220, 40, 40);
    case 1:
        return QColor(40, 200, 60);
    default:
        return QColor(40, 80, 220);
    }
}

PdfStripCursorTest::PageFolders PdfStripCursorTest::buildPageFolderImage(KisDocument *document,
                                                                         int pages,
                                                                         int pageWidth,
                                                                         int pageHeight) const
{
    PageFolders folders;

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    Q_ASSERT(colorSpace);
    if (!colorSpace) {
        return folders;
    }

    const QSize pageSize(pageWidth, pageHeight);
    folders.image = new KisImage(document->createUndoStore(), pageSize.width(),
                                 pageSize.height() * pages, colorSpace, QStringLiteral("folders"));

    KisPaintLayerSP desk = new KisPaintLayer(folders.image, QStringLiteral("Desk"), OPACITY_OPAQUE_U8);
    desk->paintDevice()->fill(QRect(QPoint(0, 0), QSize(pageSize.width(), pageSize.height() * pages)),
                              KoColor(QColor(96, 96, 96), colorSpace));
    desk->setUserLocked(true);
    folders.image->addNode(desk, folders.image->root());

    for (int i = 0; i < pages; ++i) {
        KisGroupLayerSP group = new KisGroupLayer(folders.image, PdfStripBuilder::inkGroupName(i),
                                                  OPACITY_OPAQUE_U8);
        folders.image->addNode(group, folders.image->root());

        KisPaintLayerSP layer = new KisPaintLayer(folders.image, PdfStripBuilder::inkLayerName(i),
                                                  OPACITY_OPAQUE_U8);
        folders.image->addNode(layer, group);

        folders.groups.append(group);
        folders.layers.append(layer);
    }

    return folders;
}

PdfStripCursorTest::PageFolders PdfStripCursorTest::buildBandedPage(KisDocument *document,
                                                                    int pageWidth,
                                                                    int pageHeight) const
{
    PageFolders folders;
    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        return folders;
    }

    folders.image = new KisImage(document->createUndoStore(), pageWidth, pageHeight, colorSpace,
                                 QStringLiteral("page"));

    KisPaintLayerSP paper = new KisPaintLayer(folders.image, QStringLiteral("PDF page 1"),
                                              OPACITY_OPAQUE_U8);
    paper->paintDevice()->fill(QRect(0, 0, pageWidth, pageHeight), KoColor(Qt::white, colorSpace));
    paper->setUserLocked(true);
    folders.image->addNode(paper, folders.image->root());

    KisGroupLayerSP ink = new KisGroupLayer(folders.image, QStringLiteral("Ink"), OPACITY_OPAQUE_U8);
    folders.image->addNode(ink, folders.image->root());
    folders.groups.append(ink);

    for (int band = 0; band < 3; ++band) {
        KisPaintLayerSP layer = new KisPaintLayer(folders.image,
                                                  QStringLiteral("Ink band %1").arg(band + 1),
                                                  OPACITY_OPAQUE_U8);
        layer->paintDevice()->fill(bandRect(band, QSize(pageWidth, pageHeight)),
                                   KoColor(bandColour(band), colorSpace));
        folders.image->addNode(layer, ink);
        folders.layers.append(layer);
    }

    return folders;
}

void PdfStripCursorTest::openDocument(KisDocument *document, const PageFolders &folders) const
{
    document->setCurrentImage(folders.image, true, folders.groups.value(0));
    KisPart::instance()->addDocument(document);
}

QList<KisView *> PdfStripCursorTest::viewsFor(KisDocument *document) const
{
    QList<KisView *> found;
    const QList<QPointer<KisView>> views = KisPart::instance()->views();
    for (const QPointer<KisView> &view : views) {
        if (view && view->document() == document) {
            found.append(view);
        }
    }
    return found;
}

KisView *PdfStripCursorTest::addView(KisDocument *document) const
{
    KisView *view = m_mainWindow->addViewAndNotifyLoadingCompleted(document);
    /// The view that is really showing the document, not whatever that call returns -- the
    /// navigator learned the same lesson (PdfPageNavigator.cpp, viewForDocument).
    const QList<KisView *> views = viewsFor(document);
    if (!views.isEmpty() && (!view || !views.contains(view))) {
        view = views.first();
    }

    /// And make it the one the window is working in, exactly as PdfPageNavigator::showImage does.
    /// The node manager belongs to the main window and follows the *active* view, so activating a
    /// node while another document is the active one changes the other document's node -- the
    /// "activation does not take" symptom, reproduced here rather than described.
    if (view) {
        m_mainWindow->setActiveView(view);
        /// The activation is handed to the MDI area, so it lands on the next event-loop turn. The
        /// node manager follows the *active* view; without this wait a test that activates
        /// immediately after creating its view can be activating the previous test's image view --
        /// measured as activeNode() coming back null for a node that was just asked for.
        QTest::qWait(120);
    }
    return view;
}

void PdfStripCursorTest::closeNotebook(KisDocument *document, KisView *view) const
{
    /// The view takes the document with it -- removing both is the double teardown Krita crashes
    /// in, and it is why PdfPageNavigator::showImage picks one or the other, never both.
    if (view) {
        view->closeView();
    } else if (document) {
        KisPart::instance()->removeDocument(document, true);
    }
    QApplication::sendPostedEvents();
    QTest::qWait(60);
}

QStringList PdfStripCursorTest::paintLayerNames(const KisImageSP &image)
{
    QStringList names;
    if (!image || !image->root()) {
        return names;
    }
    /// KisSharedPtr hands back a const node through a const reference, and qobject_cast refuses to
    /// cast constness away, so the node is taken by value here.
    const std::function<void(KisNodeSP)> visit = [&names, &visit](KisNodeSP node) {
        if (!node) {
            return;
        }
        if (qobject_cast<KisPaintLayer *>(node.data())) {
            names.append(node->name());
        }
        for (quint32 i = 0; i < node->childCount(); ++i) {
            visit(node->at(i));
        }
    };
    visit(image->root());
    return names;
}

bool PdfStripCursorTest::waitForFlag(bool &flag, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!flag && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return flag;
}

void PdfStripCursorTest::initTestCase()
{
    Q_INIT_RESOURCE(krita);
    QVERIFY(m_dir.isValid());

    m_fixture = fixturePath();
    QVERIFY2(QFileInfo::exists(m_fixture), qPrintable(m_fixture));
    QVERIFY(QStandardPaths::isTestModeEnabled());

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QVERIFY(QDir().mkpath(appData));
    if (!KisResourceCacheDb::initialize(appData)) {
        qFatal("could not initialise the resource cache database under %s", qPrintable(appData));
    }
    KisResourceLocator::instance()->initialize(
        QStringLiteral(KRITA_RESOURCE_DIRS_FOR_TESTS).section(QLatin1Char(';'), 0, 0)
        + QStringLiteral("/krita"));

    m_dialogWatchdog = new QTimer(this);
    m_dialogWatchdog->setInterval(20);
    connect(m_dialogWatchdog, &QTimer::timeout, this, []() {
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            if (widget->isVisible() && qobject_cast<QDialog *>(widget)) {
                qWarning("[strip-cursor] dismissing a modal dialog: %s",
                         qPrintable(widget->windowTitle()));
                widget->close();
            }
        }
    });
    m_dialogWatchdog->start();

    m_mainWindow = KisPart::instance()->createMainWindow();
    QVERIFY(m_mainWindow);
}

void PdfStripCursorTest::cleanupTestCase()
{
    /// Measured, with the stack: deleting the main window while a view is still alive destroys the
    /// view from the MDI area's destructor, and KisView::~KisView reaches
    /// KoToolManager::removeCanvasController with a null canvas resource provider. The window is
    /// therefore only hidden; the process is about to end and Krita's own teardown has nothing it
    /// needs from this test. Krita's ui tests can delete theirs because they detach the view
    /// manager first (libs/ui/tests/kis_view_signals_test.cpp:62-90), which the navigator's own
    /// teardown note says is a crash of a different shape for documents opened the way these are.
    ///
    /// Every document is marked unmodified first: closing a modified one raises the "save it?"
    /// dialog, and a dialog answered in the middle of a close is how this hung before.
    const QList<QPointer<KisDocument>> documents = KisPart::instance()->documents();
    for (const QPointer<KisDocument> &document : documents) {
        if (document) {
            document->setModified(false);
        }
    }

    /// The main window is only hidden, not deleted, and the views are closed through the
    /// navigator's own path. Both alternatives were measured and both crash in Krita's teardown:
    /// deleting the window with a live view reaches KoToolManager::removeCanvasController with a
    /// null resource provider, and detaching the view manager first (which is what Krita's own ui
    /// test does, libs/ui/tests/kis_view_signals_test.cpp:62-90) crashes earlier still on documents
    /// opened this way. What remains is the process's own teardown, which is handled in main().
    const QList<QPointer<KisView>> views = KisPart::instance()->views();
    for (const QPointer<KisView> &view : views) {
        if (view) {
            view->closeView();
        }
    }
    QApplication::sendPostedEvents();

    const QList<QPointer<KisDocument>> remaining = KisPart::instance()->documents();
    for (const QPointer<KisDocument> &document : remaining) {
        if (document) {
            KisPart::instance()->removeDocument(document, true);
        }
    }
    QApplication::sendPostedEvents();

    if (m_mainWindow) {
        m_mainWindow->hide();
    }

    /// Stopped last: the dialog that hangs the teardown is raised while the window is going down.
    if (m_dialogWatchdog) {
        m_dialogWatchdog->stop();
    }
}

/**
 * The rendering proof: a real three-slot strip over the coloured fixture, every slot compared with
 * the page rendered on its own, and the strip written out as a PNG so it can be looked at.
 */
void PdfStripCursorTest::testThreeSlotStripRendersEachPageInItsOwnSlot()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(m_fixture));
    QCOMPARE(backend.pageCount(), 3);

    QTemporaryDir project;
    QVERIFY(project.isValid());

    /// The source is resolved against the project directory now -- the renderer is asked for the
    /// record's own page of the record's own source -- so the project has to be one that holds the
    /// file the manifest records, exactly as a real notebook's does.
    QVERIFY(QFile::copy(m_fixture, QDir(project.path()).filePath(QStringLiteral("mr-strip-3page.pdf"))));

    QString why;
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                                renderers, project.path(), &why);
    QVERIFY2(strip.image, qPrintable(why));
    QVERIFY(strip.layout.isValid());

    const QList<PdfStripLayout::Slot> slots = strip.layout.slots();
    QCOMPARE(slots.size(), 3);

    QImage composed(strip.layout.imageSize(), QImage::Format_RGB32);
    composed.fill(QColor(96, 96, 96));
    QPainter painter(&composed);

    for (int i = 0; i < slots.size(); ++i) {
        const PdfStripLayout::Slot &slot = slots.at(i);
        QVERIFY(slot.page >= 0);

        KisNodeSP paper = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        QVERIFY2(paper, qPrintable(PdfStripBuilder::backgroundLayerName(slot.page)));

        const QImage inSlot = paper->paintDevice()->convertToQImage(nullptr, slot.rect);
        const QImage onItsOwn = backend.renderPage(slot.page, 200.0);
        QVERIFY(!inSlot.isNull());
        QVERIFY(!onItsOwn.isNull());
        QCOMPARE(inSlot.size(), onItsOwn.size());
        QCOMPARE(inSlot.size(), slot.rect.size());

        /// Every sampled pixel, not the centre alone: a slot holding a different page would agree
        /// in the middle and differ everywhere the pages differ.
        int worst = 0;
        const int stepX = qMax(1, inSlot.width() / 23);
        const int stepY = qMax(1, inSlot.height() / 29);
        for (int y = stepY / 2; y < inSlot.height(); y += stepY) {
            for (int x = stepX / 2; x < inSlot.width(); x += stepX) {
                const QColor a = inSlot.pixelColor(x, y);
                const QColor b = onItsOwn.pixelColor(x, y);
                worst = qMax(worst, qMax(qAbs(a.red() - b.red()),
                                         qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue()))));
            }
        }
        QVERIFY2(worst <= 2, qPrintable(QStringLiteral("slot %1 differs from its own page render by %2")
                                            .arg(slot.page + 1).arg(worst)));

        painter.drawImage(slot.rect.topLeft(), inSlot);

        const QColor middle = inSlot.pixelColor(inSlot.width() / 2, inSlot.height() / 2);
        qInfo("slot %d holds page %d at %d,%d %dx%d; centre rgb(%d,%d,%d)",
              i, slot.page + 1, slot.rect.x(), slot.rect.y(),
              slot.rect.width(), slot.rect.height(), middle.red(), middle.green(), middle.blue());
    }
    painter.end();

    /// The pages really are mixed sizes, so the strip is not three copies of one cell.
    QVERIFY2(slots.at(0).rect.size() != slots.at(1).rect.size()
                 || slots.at(1).rect.size() != slots.at(2).rect.size(),
             "the fixture is supposed to have mixed page sizes");

    /// The ink layer is above every page, which is what makes a stroke on the active page visible.
    auto rootIndex = [&strip](const QString &name) {
        for (quint32 i = 0; i < strip.image->root()->childCount(); ++i) {
            if (strip.image->root()->at(i)->name() == name) {
                return int(i);
            }
        }
        return -1;
    };
    const int inkIndex = rootIndex(QStringLiteral("Ink"));
    QVERIFY(inkIndex >= 0);
    for (const PdfStripLayout::Slot &slot : slots) {
        QVERIFY(inkIndex > rootIndex(PdfStripBuilder::backgroundLayerName(slot.page)));
    }

    /// A temporary file by default: writing the strip into tests/data left a modified fixture
    /// behind after every run, which makes the next run's tree dirty and the measurement
    /// non-deterministic. PDFIO_STRIP_PNG still names an explicit place when one is wanted.
    const QString out = qEnvironmentVariable(
        "PDFIO_STRIP_PNG", m_dir.filePath(QStringLiteral("mr-strip-three-slot.png")));
    QVERIFY2(composed.save(out, "PNG"), qPrintable(out));
    qInfo("strip %dx%d written to %s", composed.width(), composed.height(), qPrintable(out));
}

void PdfStripCursorTest::testCursorToPageOnTheStripAcrossGapsAndZoom()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(m_fixture));

    const PdfStripLayout layout = PdfStripLayout::forWindow(manifestFor(backend), 1, 3, 200.0);
    QVERIFY(layout.isValid());

    QTemporaryDir project;
    QVERIFY(QFile::copy(m_fixture, QDir(project.path()).filePath(QStringLiteral("mr-strip-3page.pdf"))));
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                                renderers, project.path(), nullptr);
    QVERIFY(strip.image);
    QCOMPARE(QSize(strip.image->width(), strip.image->height()), layout.imageSize());

    KisCoordinatesConverter converter;
    converter.setImage(strip.image);
    converter.setImageResolution(200, 200);
    converter.setCanvasWidgetSize(QSizeF(900, 1400));

    const QList<PdfStripLayout::Slot> slots = layout.slots();

    for (qreal zoom : { 0.2, 0.5, 1.0, 2.0 }) {
        converter.setZoom(zoom);

        /// The navigator's strip branch is QRectF(m_stripRects.at(slot)).contains(point) over the
        /// same rectangles (PdfPageNavigator.cpp:307-317), so the layout's pageAt is that branch.
        for (const PdfStripLayout::Slot &slot : slots) {
            QVERIFY(slot.page >= 0);
            const QPointF widget = converter.documentToWidget(QPointF(slot.rect.center()));
            const QPointF document = converter.widgetToDocument(widget);
            const int mapped = layout.pageAt(document.toPoint());
            QVERIFY2(mapped == slot.page,
                     qPrintable(QStringLiteral("zoom %1: the centre of slot %2 mapped to page %3")
                                    .arg(zoom).arg(slot.page + 1).arg(mapped + 1)));
        }

        /// The dead zone between two pages, in document pixels and on screen at this zoom. This is
        /// the number a hover-to-switch throttle has to be chosen against.
        const int gapDocument = slots.at(1).rect.top() - slots.at(0).rect.bottom() - 1;
        const QPointF lowWidget = converter.documentToWidget(
            QPointF(slots.at(0).rect.center().x(), slots.at(0).rect.bottom()));
        const QPointF highWidget = converter.documentToWidget(
            QPointF(slots.at(0).rect.center().x(), slots.at(1).rect.top()));
        const qreal gapWidget = qAbs(highWidget.y() - lowWidget.y());

        const QPoint gapPoint(slots.at(0).rect.center().x(),
                              (slots.at(0).rect.bottom() + slots.at(1).rect.top()) / 2);
        QCOMPARE(layout.pageAt(gapPoint), -1);

        qInfo("zoom %-4.2f gap %d document px = %.1f widget px; a point in it is on no page",
              zoom, gapDocument, gapWidget);
    }
}

void PdfStripCursorTest::testBoundariesAndScopeClamping()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(m_fixture));

    const PdfStripLayout layout = PdfStripLayout::forWindow(manifestFor(backend), 1, 3, 200.0);
    QVERIFY(layout.isValid());

    const QRect r = layout.slots().at(1).rect;

    /// Inside, and the last pixel that is inside.
    QCOMPARE(layout.pageAt(QPoint(r.left(), r.top())), 1);
    QCOMPARE(layout.pageAt(QPoint(r.right(), r.bottom())), 1);

    /// One past the right edge. QRect::contains (what PdfStripLayout::pageAt uses) says no, but
    /// QRectF::contains -- what the navigator's strip branch uses -- says yes. The discrepancy is
    /// one pixel wide and the navigator currently reports the page there.
    QVERIFY2(!r.contains(QPoint(r.right() + 1, r.center().y())), "QRect says outside");
    QVERIFY2(QRectF(r).contains(QPointF(r.right() + 1, r.center().y())), "QRectF says inside");
    QCOMPARE(layout.pageAt(QPoint(r.right() + 1, r.center().y())), -1);

    /// The same one pixel at the bottom.
    QVERIFY2(QRectF(r).contains(QPointF(r.center().x(), r.bottom() + 1)), "QRectF says inside");
    QCOMPARE(layout.pageAt(QPoint(r.center().x(), r.bottom() + 1)), -1);

    /// A two-page notebook asked for a three-slot window: forWindow() clamps the scope to the page
    /// count (PdfStripLayout.cpp, scope = qMin(scope, manifest.pages.size())), so no slot is left
    /// without a page and the Slot::page == -1 branch is not reachable through it today. Recorded
    /// because it decides whether "the centre is in an empty slot" can be a real cause of the
    /// scroll-follow bug: through this path, it cannot.
    const PdfStripLayout shortLayout = PdfStripLayout::forWindow(manifestFor(backend, 2), 0, 3, 200.0);
    QVERIFY(shortLayout.isValid());
    const QList<PdfStripLayout::Slot> slots = shortLayout.slots();
    QCOMPARE(slots.size(), 2);
    for (const PdfStripLayout::Slot &slot : slots) {
        QVERIFY2(slot.page >= 0, "the scope is clamped to the page count, so every slot has a page");
        QVERIFY(!slot.cell.isEmpty());
    }

    qInfo("boundary: page rect %d,%d %dx%d; one past the edge is on no page; a two-page notebook "
          "asked for three slots gets %d and no page-less slot",
          r.x(), r.y(), r.width(), r.height(), int(slots.size()));
}

/**
 * Question 1, part one: which call moves the node a tool would draw into.
 *
 * KisViewManager::activeNode() delegates to KisNodeManager::activeNode(), which returns
 * KisImageView::currentNode(); the activation slots end in that same setCurrentNode. Reading the
 * manager back is reading the node the tool reads.
 */
void PdfStripCursorTest::testActivationMovesTheNodeTheToolsRead()
{
    qInfo("step: createDocument");
    KisDocument *document = KisPart::instance()->createDocument();
    qInfo("step: buildPageFolderImage");
    const PageFolders folders = buildPageFolderImage(document, 3, 200, 200);
    QVERIFY(folders.image);
    QCOMPARE(folders.groups.size(), 3);
    qInfo("step: openDocument");
    openDocument(document, folders);
    qInfo("step: addView");
    KisView *view = addView(document);
    qInfo("step: view %s", view ? "created" : "NOT created");
    QVERIFY(view);
    QCOMPARE(view->document(), document);

    KisViewManager *viewManager = view->viewManager();
    QVERIFY(viewManager);
    KisNodeManager *nodeManager = viewManager->nodeManager();
    QVERIFY(nodeManager);

    const KisNodeSP initial = nodeManager->activeNode();
    qInfo("after opening the document the active node is \"%s\"",
          initial ? qPrintable(initial->name()) : "none");

    /// slotNonUiActivatedNode: the call the abandoned design used.
    nodeManager->slotNonUiActivatedNode(folders.groups.at(2));
    QCOMPARE(nodeManager->activeNode(), KisNodeSP(folders.groups.at(2)));
    QCOMPARE(viewManager->activeNode(), KisNodeSP(folders.groups.at(2)));

    /// slotUiActivatedNode: the UI path, which additionally walks the shape controller.
    nodeManager->slotUiActivatedNode(folders.groups.at(1));
    QCOMPARE(nodeManager->activeNode(), KisNodeSP(folders.groups.at(1)));
    QCOMPARE(viewManager->activeNode(), KisNodeSP(folders.groups.at(1)));

    qInfo("slotNonUiActivatedNode and slotUiActivatedNode both moved the active node to the page "
          "folder asked for; the node the tools read is the same object");

}

/**
 * Question 1, part two: where the active node lives when there is more than one view.
 *
 * Measured, and it changes the story the abandoned commit told. The view manager belongs to the
 * main window, not to the view -- the navigator's own comment about viewManager()->document()
 * returning whichever view is active says the same -- so two views of one document in one window
 * share one node manager and one active node. Inside a single window there is therefore no "wrong
 * view's manager" to activate on: a call that succeeds is seen by every view in that window. The
 * "it does not take in the running application" symptom has to come from somewhere else, and the
 * candidate left is the document or the window, not the view.
 */
void PdfStripCursorTest::testViewsInOneWindowShareTheActiveNode()
{
    KisDocument *document = KisPart::instance()->createDocument();
    const PageFolders folders = buildPageFolderImage(document, 3, 200, 200);
    QVERIFY(folders.image);
    openDocument(document, folders);

    KisView *first = addView(document);
    KisView *second = addView(document);
    QVERIFY(first);
    QVERIFY(second);
    QVERIFY(first != second);
    QCOMPARE(first->document(), document);
    QCOMPARE(second->document(), document);

    KisNodeManager *firstManager = first->viewManager()->nodeManager();
    KisNodeManager *secondManager = second->viewManager()->nodeManager();
    QVERIFY(firstManager);
    QVERIFY(secondManager);

    /// One manager for the window, so one active node behind both views.
    QCOMPARE(firstManager, secondManager);

    const KisNodeSP secondBefore = secondManager->activeNode();
    firstManager->slotNonUiActivatedNode(folders.groups.at(2));
    QCOMPARE(firstManager->activeNode(), KisNodeSP(folders.groups.at(2)));

    /// Both views read the same node, so an activation that took is seen by both -- there is no
    /// per-view answer in this window for a stroke to disagree with.
    QCOMPARE(second->viewManager()->activeNode(), KisNodeSP(folders.groups.at(2)));
    QVERIFY(second->viewManager()->activeNode() != secondBefore);

    qInfo("two views of one document in one window: one node manager (%s), so activating on it "
          "moved the active node for both views",
          firstManager == secondManager ? "shared" : "per view");

}

/**
 * Question 1, part three: is locking enough, and where does the refusal live.
 *
 * isEditable() walks the parent chain, so locking a page's folder makes every layer inside it
 * uneditable -- one flag per page, not one per layer.
 */
void PdfStripCursorTest::testLockingTheFoldersIsWhatMakesTheStrokeRefusable()
{
    KisDocument *document = KisPart::instance()->createDocument();
    const PageFolders folders = buildPageFolderImage(document, 3, 200, 200);
    QVERIFY(folders.image);
    openDocument(document, folders);

    KisView *view = addView(document);
    QVERIFY(view);
    KisNodeManager *nodeManager = view->viewManager()->nodeManager();
    QVERIFY(nodeManager);

    /// Every folder locked, then the intended one opened. The node is copied out of the list
    /// because KisSharedPtr hands back a const node from a const reference.
    for (int i = 0; i < folders.groups.size(); ++i) {
        KisNodeSP group = folders.groups.at(i);
        group->setUserLocked(i != 1);
    }
    nodeManager->slotNonUiActivatedNode(folders.groups.at(1));
    QCOMPARE(nodeManager->activeNode(), KisNodeSP(folders.groups.at(1)));

    QVERIFY2(folders.groups.at(1)->isEditable(), "the intended page has to be paintable");
    QVERIFY2(folders.layers.at(1)->isEditable(), "including the layer a stroke lands in");

    for (int i : { 0, 2 }) {
        QVERIFY2(!folders.groups.at(i)->isEditable(), "an inactive page folder is locked");
        QVERIFY2(!folders.layers.at(i)->isEditable(),
                 "and the lock reaches its children through the parent walk of isEditable()");
        QVERIFY2(!KisToolUtils::nodeEditableMessage(folders.groups.at(i), false).isEmpty(),
                 "the tool-facing predicate refuses a locked folder");
    }
    QVERIFY2(KisToolUtils::nodeEditableMessage(folders.groups.at(1), false).isEmpty(),
             "the intended folder passes the same predicate");

    qInfo("one userLocked flag per page folder is enough: %d folders locked, the children of each "
          "report isEditable() false, and the intended page passes nodeEditableMessage",
          int(folders.groups.size()));

}

/**
 * Question 1, part four: what the guarantee actually is when activation does not take.
 *
 * With every folder but the intended one locked, a stale active node is a locked folder, so the
 * stroke is refused. It cannot land on another page, because there is no other unlocked page.
 */
void PdfStripCursorTest::testStaleActivationRefusesRatherThanMisdirects()
{
    KisDocument *document = KisPart::instance()->createDocument();
    const PageFolders folders = buildPageFolderImage(document, 3, 200, 200);
    QVERIFY(folders.image);
    openDocument(document, folders);

    KisView *view = addView(document);
    QVERIFY(view);
    KisNodeManager *nodeManager = view->viewManager()->nodeManager();
    QVERIFY(nodeManager);

    /// Opening the notebook on page 1 leaves that page's folder active -- the navigator hands the
    /// node to setCurrentImage -- but a fresh view reports no active node until something asks for
    /// one, which is itself worth knowing. Ask for page 1 explicitly, then turn to page 3.
    nodeManager->slotNonUiActivatedNode(folders.groups.at(0));
    QCOMPARE(nodeManager->activeNode(), KisNodeSP(folders.groups.at(0)));

    /// The user has turned to page 3, so page 3 is the one that should be writable...
    for (int i = 0; i < folders.groups.size(); ++i) {
        KisNodeSP group = folders.groups.at(i);
        group->setUserLocked(i != 2);
    }
    /// ...and the activation silently did not take: the active node is still page 1, now locked.
    const KisNodeSP active = nodeManager->activeNode();
    QVERIFY2(active, "the page that was open has to still be the active node");
    QVERIFY(active != KisNodeSP(folders.groups.at(2)));
    QVERIFY(!active->isEditable());
    QVERIFY2(!KisToolUtils::nodeEditableMessage(active, false).isEmpty(),
             "a stroke has to be refused, not sent to another page");

    for (int i : { 0, 1 }) {
        QVERIFY(!folders.groups.at(i)->isEditable());
    }

    /// And where the refusal does not live: the image itself happily writes to a locked layer's
    /// device. Locking is a tool-level guarantee, enforced on the active node, not a device lock.
    /// Measured so the claim is not overstated.
    const QImage before = folders.layers.at(0)->paintDevice()->convertToQImage(nullptr,
                                                                              folders.image->bounds());
    {
        KisPainter painter(folders.layers.at(0)->paintDevice());
        painter.fill(0, 0, 16, 16, KoColor(Qt::black, folders.image->colorSpace()));
        painter.end();
    }
    const QImage after = folders.layers.at(0)->paintDevice()->convertToQImage(nullptr,
                                                                             folders.image->bounds());
    const bool deviceRefused = (before == after);

    qInfo("stale activation: active node \"%s\" is locked, so the tool refuses; a raw KisPainter "
          "write into that locked folder's device changed the pixels: %s (the lock is enforced on "
          "the active node, not by the paint device)",
          qPrintable(active->name()), deviceRefused ? "no" : "yes");
    QVERIFY2(!deviceRefused, "if this ever becomes true, the guarantee is stronger than measured");

}

/**
 * Question 3: several real layers in one page folder, saved and reloaded.
 *
 * PdfInkLoader reads mergedimage.png and nothing else, so what comes back is the flatten. The test
 * asserts the flatten carries all three layers -- an image that only had the top one would miss two
 * bands -- and that the archive really does hold a layer stack beside it.
 */
void PdfStripCursorTest::testAPageFolderWithSeveralLayersComesBackFlattened()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int pageWidth = 300;
    const int pageHeight = 300;

    KisDocument *document = KisPart::instance()->createDocument();
    const PageFolders folders = buildBandedPage(document, pageWidth, pageHeight);
    QVERIFY(folders.image);
    QCOMPARE(folders.layers.size(), 3);

    KisDocument *pageDocument = PdfPageSaver::createPageLayersDocument(folders.image, nullptr);
    QVERIFY(pageDocument);

    /// The merged image is the document's projection (kis_kra_saver.cpp:658), and the projection is
    /// built by an update job. The production path has a document that has been on screen for a
    /// while; a document built and saved in the same few statements does not, and writes a
    /// transparent mergedimage.png. Measured: without this the reload comes back rgba(0,0,0,0) at
    /// every band. refreshGraphAsync + waitForDone is the pair KisImage documents for exactly this.
    pageDocument->image()->refreshGraphAsync(pageDocument->image()->root(),
                                        { pageDocument->image()->bounds() },
                                        pageDocument->image()->bounds());
    pageDocument->image()->waitForDone();

    const QString path = dir.filePath(QStringLiteral("page.kra"));
    bool finished = false;
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, this, [&finished](const QString &) {
        finished = true;
    });

    QString why;
    QVERIFY2(PdfPageSaver::saveDocument(pageDocument, path, &why), qPrintable(why));
    QVERIFY2(waitForFlag(finished, 30000), "the save never reported back");
    QVERIFY(QFileInfo::exists(path));
    KisPart::instance()->removeDocument(pageDocument, true);

    /// What the notebook reads back.
    QElapsedTimer loaderClock;
    loaderClock.start();
    const QImage flat = PdfInkLoader::loadInk(path, &why);
    const qint64 loaderMs = loaderClock.elapsed();
    QVERIFY2(!flat.isNull(), qPrintable(why));

    for (int band = 0; band < 3; ++band) {
        const QRect rect = bandRect(band, QSize(pageWidth, pageHeight));
        const QColor got = flat.pixelColor(rect.center());
        qInfo("band %d of the reload: rect %d,%d %dx%d of a %dx%d image, rgba(%d,%d,%d,%d)",
              band + 1, rect.x(), rect.y(), rect.width(), rect.height(),
              flat.width(), flat.height(), got.red(), got.green(), got.blue(), got.alpha());
        QVERIFY2(colourNear(got, bandColour(band)),
                 qPrintable(QStringLiteral("band %1 is rgb(%2,%3,%4), not the colour it was painted")
                                .arg(band + 1).arg(got.red()).arg(got.green()).arg(got.blue())));
    }

    /// The archive beside that single image: the layer stack, and the merged image it was read from.
    KZip zip(path);
    QVERIFY(zip.open(QIODevice::ReadOnly));
    const KArchiveDirectory *root = zip.directory();
    QVERIFY(root);

    qint64 mergedBytes = 0;
    int layerEntries = 0;
    qint64 layerBytes = 0;
    QStringList entryNames;
    const std::function<void(const KArchiveDirectory *, const QString &)> walk =
        [&](const KArchiveDirectory *directory, const QString &prefix) {
            const QStringList entries = directory->entries();
            for (const QString &entry : entries) {
                const KArchiveEntry *child = directory->entry(entry);
                const QString full = prefix + QLatin1Char('/') + entry;
                if (const KArchiveDirectory *sub = dynamic_cast<const KArchiveDirectory *>(child)) {
                    walk(sub, full);
                    continue;
                }
                if (const KArchiveFile *file = dynamic_cast<const KArchiveFile *>(child)) {
                    entryNames.append(full);

                    /// Krita stores each paint layer as .../layers/layerN (its own tile format, not
                    /// a PNG) with the icc and defaultpixel companions beside it. Counting only the
                    /// bare layerN entries counts layers, which is what the stack costs to read back.
                    const QString base = full.section(QLatin1Char('/'), -1);
                    if (full.endsWith(QStringLiteral("mergedimage.png"))) {
                        mergedBytes = file->size();
                    } else if (base.startsWith(QStringLiteral("layer"))
                               && !base.contains(QLatin1Char('.'))) {
                        ++layerEntries;
                        layerBytes += file->size();
                    }
                }
            }
        };
    walk(root, QString());
    zip.close();

    QVERIFY(mergedBytes > 0);
    QVERIFY2(layerEntries >= 3,
             qPrintable(QStringLiteral("expected the three layer entries, saw %1: %2")
                            .arg(layerEntries).arg(entryNames.join(QStringLiteral(", ")))));

    qInfo("page.kra: %lld bytes of layer data across %d layer entries, beside a %lld byte "
          "mergedimage.png; PdfInkLoader read the %dx%d flatten in %lld ms and returned all three "
          "bands. Entries: %s",
          layerBytes, layerEntries, mergedBytes, flat.width(), flat.height(), loaderMs,
          qPrintable(entryNames.join(QStringLiteral(" "))));
}

/**
 * Question: an image inserted into a page -- File > Insert Image..., which is a FILE layer -- has to
 * reach the page's artifact like any other layer. copyPageLayers() skipped everything that was not a
 * paint layer, so the inserted image lived only in the editor: not in the .kra, not in the exported
 * PDF, and nothing said so. The file layer's projection is rasterised into a paint layer with the
 * same name, and a file layer whose source is gone is skipped rather than failing the save.
 */
void PdfStripCursorTest::testAnInsertedImageLayerReachesThePageArtifact()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString inserted = dir.filePath(QStringLiteral("inserted.png"));
    QImage picture(64, 64, QImage::Format_ARGB32_Premultiplied);
    picture.fill(Qt::red);
    QVERIFY(picture.save(inserted));

    KisDocument *document = KisPart::instance()->createDocument();
    PageFolders folders = buildBandedPage(document, 300, 300);
    QVERIFY(folders.image);

    auto *fileLayer = new KisFileLayer(folders.image, dir.path(), QStringLiteral("inserted.png"),
                                       KisFileLayer::None, QString(),
                                       QStringLiteral("Inserted image"), OPACITY_OPAQUE_U8);
    QVERIFY(folders.image->addNode(fileLayer, folders.image->rootLayer()));

    /// And one whose source is gone, which must be skipped with a log rather than fail the save.
    auto *goneLayer = new KisFileLayer(folders.image, dir.path(), QStringLiteral("not-there.png"),
                                       KisFileLayer::None, QString(),
                                       QStringLiteral("Missing image"), OPACITY_OPAQUE_U8);
    QVERIFY(folders.image->addNode(goneLayer, folders.image->rootLayer()));

    KisDocument *pageDocument = PdfPageSaver::createPageLayersDocument(folders.image, nullptr);
    QVERIFY2(pageDocument, "a file layer whose source is gone must not fail the whole save");

    bool carried = false;
    bool missingCarried = false;
    KisNodeSP root = pageDocument->image()->root();
    for (quint32 i = 0; i < root->childCount(); ++i) {
        const QString name = root->at(i)->name();
        if (name == QStringLiteral("Inserted image")) {
            carried = true;
        }
        if (name == QStringLiteral("Missing image")) {
            missingCarried = true;
        }
    }
    qInfo("layers in the page artifact: %d, inserted image carried: %d, missing-source layer "
          "carried: %d",
          int(root->childCount()), int(carried), int(missingCarried));
    QVERIFY2(carried, "the inserted image layer did not reach the page artifact");
    QVERIFY2(!missingCarried, "a file layer whose source is gone must be skipped, not carried");

    /// And it survives the save and the read-back the notebook actually does: the layer entry is
    /// not enough, the flatten has to carry the inserted image's pixels.
    pageDocument->image()->refreshGraphAsync(pageDocument->image()->root(),
                                             { pageDocument->image()->bounds() },
                                             pageDocument->image()->bounds());
    pageDocument->image()->waitForDone();

    const QString path = dir.filePath(QStringLiteral("page.kra"));
    bool finished = false;
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, this,
                     [&finished](const QString &) { finished = true; });
    QString saveWhy;
    QVERIFY2(PdfPageSaver::saveDocument(pageDocument, path, &saveWhy), qPrintable(saveWhy));
    QVERIFY2(waitForFlag(finished, 30000), "the save never reported back");
    KisPart::instance()->removeDocument(pageDocument, true);

    const QImage flat = PdfInkLoader::loadInk(path, &saveWhy);
    QVERIFY2(!flat.isNull(), qPrintable(saveWhy));

    /// The inserted image is a 64x64 red block at the page's top left; nothing else in the banded
    /// page is that red.
    int redPixels = 0;
    for (int y = 0; y < qMin(64, flat.height()); ++y) {
        for (int x = 0; x < qMin(64, flat.width()); ++x) {
            const QColor pixel = flat.pixelColor(x, y);
            if (pixel.red() > 200 && pixel.green() < 60 && pixel.blue() < 60) {
                ++redPixels;
            }
        }
    }
    qInfo("file layer save/reload: %dx%d flatten from %lld bytes, red pixels in the inserted "
          "image's corner: %d",
          flat.width(), flat.height(), qint64(QFileInfo(path).size()), redPixels);
    QVERIFY2(redPixels > 100, "the inserted image's pixels are not in the flatten after the save");
}

/**
 * The sidecar, per layer: two layers with different pixels come back as two layers with their own
 * pixels, in their own places.
 *
 * This is the cheap proof of both halves of the artifact's layer path -- PdfPageSaver writes one
 * PNG per layer, PdfInkLoader::loadInkLayersFromSidecar reads them back by name -- and it is also
 * the measurement that shows what the writer used to do: a layer carrying a position of its own
 * (an inserted picture is placed by its layer offset) had that offset thrown away by the copy, so
 * its PNG came back at the page's top left, and the layers beside it were read out of pixels that
 * were not theirs.
 *
 * The page is the one PdfStripCursorTest already builds for this question: a white paper, an Ink
 * group, three coloured bands as the page's layers. The inserted picture goes in beside them, the
 * way placeInsertedImage() puts one there.
 */
void PdfStripCursorTest::testTheSidecarKeepsEachLayersOwnPixels()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    KisDocument *document = KisPart::instance()->createDocument();
    PageFolders folders = buildBandedPage(document, 300, 300);
    QVERIFY(folders.image);
    QCOMPARE(folders.groups.size(), 1);

    const KoColorSpace *colorSpace = folders.image->colorSpace();

    /// Pixels at the device's origin, the place on the page carried by the layer offset -- exactly
    /// the shape placeInsertedImage() leaves behind.
    const QRect pictureOnPage(100, 120, 40, 40);
    KisPaintLayerSP picture = new KisPaintLayer(folders.image, QStringLiteral("Inserted image"),
                                                OPACITY_OPAQUE_U8);
    picture->paintDevice()->fill(QRect(QPoint(0, 0), pictureOnPage.size()),
                                 KoColor(QColor(255, 0, 0), colorSpace));
    picture->setX(pictureOnPage.x());
    picture->setY(pictureOnPage.y());
    QVERIFY(folders.image->addNode(picture, folders.groups.at(0)));

    KisDocument *pageDocument = PdfPageSaver::createPageLayersDocument(folders.image, nullptr);
    QVERIFY(pageDocument);
    pageDocument->image()->refreshGraphAsync(pageDocument->image()->root(),
                                             { pageDocument->image()->bounds() },
                                             pageDocument->image()->bounds());
    pageDocument->image()->waitForDone();

    const QString path = dir.filePath(QStringLiteral("page.kra"));
    bool finished = false;
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, this,
                     [&finished](const QString &) { finished = true; });
    QString why;
    QVERIFY2(PdfPageSaver::saveDocument(pageDocument, path, &why), qPrintable(why));
    QVERIFY2(waitForFlag(finished, 30000), "the save never reported back");
    KisPart::instance()->removeDocument(pageDocument, true);

    QFile index(path + QStringLiteral(".layers.txt"));
    QVERIFY2(index.open(QIODevice::ReadOnly | QIODevice::Text), qPrintable(index.fileName()));
    const QString listing = QString::fromUtf8(index.readAll());
    qInfo("the sidecar the writer left:\n%s", qPrintable(listing));

    const QList<QPair<QString, QImage>> layers = PdfInkLoader::loadInkLayersFromSidecar(path, &why);
    QCOMPARE(layers.size(), 4);
    QCOMPARE(layers.at(0).first, QStringLiteral("Ink band 1"));
    QCOMPARE(layers.at(1).first, QStringLiteral("Ink band 2"));
    QCOMPARE(layers.at(2).first, QStringLiteral("Ink band 3"));
    QCOMPARE(layers.at(3).first, QStringLiteral("Inserted image"));

    for (const QPair<QString, QImage> &entry : layers) {
        QCOMPARE(entry.second.size(), QSize(300, 300));
    }

    const QImage band2 = layers.at(1).second;
    const QImage inserted = layers.at(3).second;

    const auto opaqueAt = [](const QImage &pixels, const QPoint &at) {
        return pixels.valid(at) && qAlpha(pixels.pixel(at)) > 0;
    };
    const auto redAt = [](const QImage &pixels, const QPoint &at) {
        if (!pixels.valid(at)) {
            return false;
        }
        const QRgb pixel = pixels.pixel(at);
        return qAlpha(pixel) > 200 && qRed(pixel) > 200 && qGreen(pixel) < 60 && qBlue(pixel) < 60;
    };

    qInfo("the inserted layer's PNG: red at its own place %d, its own pixels left untouched %d; "
          "the band beside it red where the picture is %d",
          int(redAt(inserted, QPoint(120, 140))), int(!opaqueAt(inserted, QPoint(30, 30))),
          int(redAt(band2, QPoint(120, 140))));

    QVERIFY2(redAt(inserted, QPoint(120, 140)),
             "the inserted image's own PNG does not carry it where its layer offset puts it");
    QVERIFY2(!opaqueAt(inserted, QPoint(30, 30)),
             "the inserted image's PNG carries pixels from a layer it does not own");
    QVERIFY2(!redAt(band2, QPoint(120, 140)),
             "a band's PNG carries the inserted image's pixels");

    /// The recorded position is the page-local one, not the artifact origin: the picture sits at
    /// 100,120 of the page, and the artifact's origin is the page's.
    QVERIFY2(listing.contains(QStringLiteral("Inserted image\t3.png\t255\t100\t120")),
             qPrintable(QStringLiteral("the sidecar does not record the layer's own place:\n%1")
                            .arg(listing)));
}

/**
 * Question 3, part two: what restoring the stack costs instead.
 *
 * KraConverter::buildImage reads the whole .kra -- the XML and every layer -- and rebuilds a
 * KisImage. The test measures that path on the same file and counts what it brings back.
 */
void PdfStripCursorTest::testRestoringTheLayerStackCostsAFullKraLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const int pageWidth = 300;
    const int pageHeight = 300;

    KisDocument *source = KisPart::instance()->createDocument();
    const PageFolders folders = buildBandedPage(source, pageWidth, pageHeight);
    QVERIFY(folders.image);

    KisDocument *pageDocument = PdfPageSaver::createPageLayersDocument(folders.image, nullptr);
    QVERIFY(pageDocument);
    /// The merged image is the projection; see the note in the test above.
    pageDocument->image()->refreshGraphAsync(pageDocument->image()->root(),
                                        { pageDocument->image()->bounds() },
                                        pageDocument->image()->bounds());
    pageDocument->image()->waitForDone();
    const QString path = dir.filePath(QStringLiteral("page.kra"));

    bool finished = false;
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, this, [&finished](const QString &) {
        finished = true;
    });
    QString why;
    QVERIFY2(PdfPageSaver::saveDocument(pageDocument, path, &why), qPrintable(why));
    QVERIFY2(waitForFlag(finished, 30000), "the save never reported back");
    KisPart::instance()->removeDocument(pageDocument, true);

    /// The cheap path: one PNG out of the zip.
    QElapsedTimer loaderClock;
    loaderClock.start();
    const QImage flat = PdfInkLoader::loadInk(path, &why);
    const qint64 loaderMs = loaderClock.elapsed();
    QVERIFY(!flat.isNull());

    /// The expensive path: the whole document.
    KisDocument *restored = KisPart::instance()->createDocument();
    QVERIFY(restored);
    KraConverter converter(restored);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));

    QElapsedTimer kraClock;
    kraClock.start();
    const KisImportExportErrorCode code = converter.buildImage(&file);
    const qint64 kraMs = kraClock.elapsed();
    QVERIFY2(code.isOk(), "KraConverter could not read the page artifact back");

    KisImageSP loaded = converter.image();
    QVERIFY2(loaded, "KraConverter did not produce an image");
    const QStringList names = paintLayerNames(loaded);

    /// All three layers come back through this path, which is what a folder-per-page design needs
    /// if the ink has to be kept as a stack rather than as a picture.
    const int inkLayers = int(std::count_if(names.cbegin(), names.cend(), [](const QString &name) {
        return name.startsWith(QStringLiteral("Ink band"));
    }));
    QCOMPARE(inkLayers, 3);

    qInfo("restoring the stack: KraConverter::buildImage %lld ms, %d paint layers (%s); "
          "PdfInkLoader::loadInk %lld ms, 1 image -- the stack costs a full document load per page",
          kraMs, int(names.size()), qPrintable(names.join(QStringLiteral(", "))), loaderMs);
}

int main(int argc, char *argv[])
{
    qputenv("LANGUAGE", "en");
    QStandardPaths::setTestModeEnabled(true);
    qputenv("KRITA_NO_ASSERT_MSG", "1");
    qputenv("KRITA_PLUGIN_PATH", QByteArray(PDFIO_PLUGIN_DIR));
    qputenv("EXTRA_RESOURCE_DIRS", QByteArray(KRITA_RESOURCE_DIRS_FOR_TESTS));

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PdfStripCursorTest"));

    PdfStripCursorTest test;
    const int result = QTest::qExec(&test, argc, argv);

    /// The results are out and every test has finished. What runs next is Krita's static
    /// destruction of KisPart, KoToolManager and the canvas objects, and in this binary it dumps
    /// core inside KoToolManager::removeCanvasController with a null resource provider -- measured,
    /// and not something this test owns. The streams are flushed and the process exits with the
    /// test's own code, so a ctest run reports the result of the tests rather than the crash of the
    /// application after them.
    fflush(nullptr);
    std::_Exit(result);
    return result;
}

#include "PdfStripCursorTest.moc"
