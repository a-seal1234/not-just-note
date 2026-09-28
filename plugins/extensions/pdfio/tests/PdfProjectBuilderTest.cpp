/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/image/ImageRenderBackend.h"
#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"
#include "session/PdfSourceRenderers.h"

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_layer.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

/**
 * The layer contract is what makes the rest of the session safe: if the page ever stops being
 * locked, or the notes stop landing in a group of their own, the ink-only save that follows
 * would quietly write the original artwork into the project.
 */
class PdfProjectBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPageImageStructure();
    void testRotatedPageUsesItsDisplayedSize();
    void testScaledPageUsesScaledSourceDpi();
    void testABlankPageIsWhitePaperOfItsOwnSize();
    void testAnImageSourceIsOnePageSizedByItsPixels();
    void testAnImportedPictureIsRescaledAtImport();
    void testFailsWithoutGeometry();
    void testSourceRendererCacheUsesProjectAndContentIdentity();

    /// A page set down at an angle has nothing at its corners, rather than the black an opaque
    /// render comes back with when the turn exposes the pixels around it.
    void testAnAngledPageHasEmptyCorners();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }

    PdfPageRecord recordFor(int index) const
    {
        PopplerRenderBackend backend;
        backend.open(fixturePath(QStringLiteral("text-fixture.pdf")));
        return PdfPageRecord{index, backend.pageInfo(index).sizePt,
                             backend.pageInfo(index).rotation,
                             PdfSession::pageFileName(index), QString(), 0};
    }
};

void PdfProjectBuilderTest::testPageImageStructure()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QString why;
    const KisImageSP image =
        PdfProjectBuilder::buildPageImage(recordFor(0), backend, 72.0, &why);
    QVERIFY2(image, qPrintable(why));

    QCOMPARE(image->xRes(), 72.0);
    QCOMPARE(image->yRes(), 72.0);

    KisNodeSP root = image->root();
    QCOMPARE(root->childCount(), 2u);

    /// Bottom layer: the locked page artwork.
    KisNodeSP bottom = root->at(0);
    QVERIFY(bottom);
    QCOMPARE(bottom->name(), PdfProjectBuilder::backgroundLayerName());
    QVERIFY(bottom->userLocked());
    QVERIFY(qobject_cast<KisPaintLayer *>(bottom.data()));

    /// Top layer: the empty group the notes go into, and only that one is writable.
    KisNodeSP top = root->at(1);
    QVERIFY(top);
    QCOMPARE(top->name(), PdfProjectBuilder::inkLayerName());
    QVERIFY(!top->userLocked());
    QVERIFY(qobject_cast<KisGroupLayer *>(top.data()));

    /// A group is not paintable: the Ink group has to carry a paint layer, otherwise selecting
    /// it and drawing does nothing.
    QCOMPARE(top->childCount(), 1u);
    KisNodeSP stroke = top->at(0);
    QVERIFY(stroke);
    QVERIFY(qobject_cast<KisPaintLayer *>(stroke.data()));
    QCOMPARE(stroke->name(), PdfProjectBuilder::inkStrokeLayerName());
    QVERIFY(!stroke->userLocked());
    QCOMPARE(PdfProjectBuilder::inkStrokeLayer(image), stroke);

    /// The rendered page really landed: the fixture draws text, so something is not white.
    QImage flattened = bottom->paintDevice()->convertToQImage(0, image->bounds());
    QVERIFY(!flattened.isNull());

    bool foundInk = false;
    for (int y = 0; y < flattened.height() && !foundInk; ++y) {
        for (int x = 0; x < flattened.width(); ++x) {
            if (qGray(flattened.pixel(x, y)) < 200) {
                foundInk = true;
                break;
            }
        }
    }
    QVERIFY2(foundInk, "the background layer is blank, the page raster did not land in it");
}

void PdfProjectBuilderTest::testRotatedPageUsesItsDisplayedSize()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    const PdfPageRecord rotated = recordFor(1);
    QCOMPARE(rotated.rotation, 90);

    const KisImageSP image = PdfProjectBuilder::buildPageImage(rotated, backend, 72.0);
    QVERIFY(image);

    /// 420 x 595 points at 72 dpi, i.e. the displayed (rotated) page, within a pixel.
    QCOMPARE(image->width(), 420);
    QCOMPARE(image->height(), 595);
}

void PdfProjectBuilderTest::testScaledPageUsesScaledSourceDpi()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    const PdfPageRecord original = recordFor(0);
    const KisImageSP normal = PdfProjectBuilder::buildPageImage(original, backend, 72.0);
    QVERIFY(normal);

    PdfPageRecord scaledRecord = original;
    scaledRecord.extraScale = 2.0;
    const KisImageSP scaled = PdfProjectBuilder::buildPageImage(scaledRecord, backend, 72.0);
    QVERIFY(scaled);

    QVERIFY2(qAbs(scaled->width() - normal->width() * 2) <= 2
                 && qAbs(scaled->height() - normal->height() * 2) <= 2,
             qPrintable(QStringLiteral("the 2x open page was %1x%2; expected about %3x%4")
                            .arg(scaled->width()).arg(scaled->height())
                            .arg(normal->width() * 2).arg(normal->height() * 2)));
    QCOMPARE(scaled->xRes(), normal->xRes());
}

/**
 * A blank page is white paper of its own size, at the size the reader asks for -- and it is a page
 * like any other: the same layer stack, the same budget, the same turn and box arithmetic.
 */
void PdfProjectBuilderTest::testABlankPageIsWhitePaperOfItsOwnSize()
{
    PdfPageRecord blank;
    blank.source = -1;
    blank.index = -1;
    blank.sizePt = QSizeF(300, 400);
    blank.kraFile = QStringLiteral("pages/p0001.kra");

    QString why;
    const KisImageSP image = PdfProjectBuilder::buildBlankPageImage(blank, 72.0, &why);
    QVERIFY2(image, qPrintable(why));

    /// A point IS a pixel at 72 dpi, which is what makes this the one size comparison that needs no
    /// rounding argument.
    QCOMPARE(image->width(), 300);
    QCOMPARE(image->height(), 400);
    QCOMPARE(image->xRes(), 72.0);
    QCOMPARE(image->root()->childCount(), 2u);

    /// The paper is locked and the Ink group above it is not.
    KisNodeSP paper = image->root()->at(0);
    QCOMPARE(paper->name(), PdfProjectBuilder::backgroundLayerName());
    QVERIFY(paper->userLocked());
    QVERIFY(!image->root()->at(1)->userLocked());

    /// The pixels themselves come from the raster the image was built out of, and that is asserted on
    /// the QImage rather than through the paint device: reading a device back needs the application's
    /// threads, which this test binary has neither the reason nor the machinery to start (the same
    /// call hangs on the unmodified tree -- see the note in PDFIO-KNOWN-ISSUES.md). The end-to-end
    /// test in PdfNavigatorIntegrationTest does read the layer back, with the application up.
    const QImage sheet = PdfSourceRenderers::blankPage(blank, 72.0);
    QVERIFY(!sheet.isNull());
    QCOMPARE(sheet.size(), QSize(300, 400));
    QCOMPARE(sheet.pixelColor(0, 0), QColor(Qt::white));
    QCOMPARE(sheet.pixelColor(299, 399), QColor(Qt::white));
    QCOMPARE(sheet.pixelColor(150, 200), QColor(Qt::white));
    QCOMPARE(sheet.pixelColor(0, 0).alpha(), 255);

    /// A box crops it and a turn turns it, exactly as they do a rendered page: a blank page set
    /// down at an angle is the rectangle the sheet fits in, at the box's pixels.
    PdfPageRecord boxed = blank;
    boxed.boxPt = QRectF(50, 40, 120, 90);
    const KisImageSP cropped = PdfProjectBuilder::buildBlankPageImage(boxed, 72.0, &why);
    QVERIFY2(cropped, qPrintable(why));
    QCOMPARE(cropped->width(), 120);
    QCOMPARE(cropped->height(), 90);
    QCOMPARE(PdfSourceRenderers::blankPage(boxed, 72.0).size(), QSize(120, 90));

    PdfPageRecord turned = blank;
    turned.extraRotation = 90;
    const KisImageSP quarter = PdfProjectBuilder::buildBlankPageImage(turned, 72.0, &why);
    QVERIFY2(quarter, qPrintable(why));
    QCOMPARE(quarter->width(), 400);
    QCOMPARE(quarter->height(), 300);
    /// A right angle is exact: the same sheet, the sides swapped.
    QCOMPARE(PdfSourceRenderers::blankPage(turned, 72.0).size(), QSize(400, 300));

    /// The dpi follows the reader's, and a scaled blank page is built at the scale's pixels rather
    /// than blown up afterwards.
    const KisImageSP atDouble = PdfProjectBuilder::buildBlankPageImage(blank, 144.0, &why);
    QVERIFY2(atDouble, qPrintable(why));
    QCOMPARE(atDouble->width(), 600);
    QCOMPARE(atDouble->height(), 800);
    QCOMPARE(atDouble->xRes(), 144.0);

    PdfPageRecord scaled = blank;
    scaled.extraScale = 2.0;
    const KisImageSP scaledImage = PdfProjectBuilder::buildBlankPageImage(scaled, 72.0, &why);
    QVERIFY2(scaledImage, qPrintable(why));
    QCOMPARE(scaledImage->width(), 600);
    QCOMPARE(scaledImage->height(), 800);
    QCOMPARE(PdfSourceRenderers::blankPage(scaled, 72.0).size(), QSize(600, 800));

    /// A margin -- a box that reaches past the sheet -- is paper too, not a hole: the part outside the
    /// sheet is white, which is what a blank page has instead of source pixels.
    PdfPageRecord margin = blank;
    margin.boxPt = QRectF(-50, -50, 400, 500);
    const QImage wider = PdfSourceRenderers::blankPage(margin, 72.0);
    QCOMPARE(wider.size(), QSize(400, 500));
    QCOMPARE(wider.pixelColor(2, 2), QColor(Qt::white));

    /// And it refuses a page that HAS a source: rendering white paper over a PDF page would hide the
    /// paper the user expects under a blank one.
    PdfPageRecord sourced = blank;
    sourced.source = 0;
    sourced.index = 0;
    QVERIFY(!PdfProjectBuilder::buildBlankPageImage(sourced, 72.0, &why));
    QVERIFY(!why.isEmpty());
}


/**
 * A picture as a source: one page, as many points across as the picture has pixels, rendered at the
 * resolution the reader asks for -- and refused by name when the file is not the kind it claims.
 *
 * This is what makes an image page a page like any other: everything above the source (the layout,
 * the strip, the box, the turn, the ink, the previews) only ever sees a page whose paper comes from
 * a renderer, and a picture arrives through the same interface as a PDF page.
 */
void PdfProjectBuilderTest::testAnImageSourceIsOnePageSizedByItsPixels()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    /// A picture with something recognisable in it, so a render cannot pass by being blank.
    QImage scan(64, 40, QImage::Format_ARGB32_Premultiplied);
    scan.fill(Qt::white);
    scan.setPixelColor(3, 3, QColor(Qt::black));
    const QString picture = dir.filePath(QStringLiteral("scan.png"));
    QVERIFY(scan.save(picture, "PNG"));

    PdfSessionManifest manifest;
    manifest.sourceFile = QStringLiteral("scan.png");
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(picture);
    manifest.sourceByteSize = QFileInfo(picture).size();
    PdfSourceRecord source;
    source.file = QStringLiteral("scan.png");
    source.sha256 = manifest.sourceSha256;
    source.byteSize = manifest.sourceByteSize;
    source.kind = QStringLiteral("image");
    manifest.sources.append(source);

    PdfPageRecord page;
    page.index = 0;
    page.source = 0;
    /// One pixel is one point, which is the rule the image backend and the ops screen share.
    page.sizePt = QSizeF(64, 40);
    page.kraFile = QStringLiteral("pages/p0001.kra");
    manifest.pages.append(page);

    QString why;
    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    QVERIFY(manifest.sources.at(0).isImage());

    /// The kind survives being written and read: it is what says which reader opens the file
    /// tomorrow, and a PDF source with no kind has to keep meaning what it always did.
    const PdfSessionManifest readBack = PdfSessionManifest::fromJson(manifest.toJson(), &why);
    QVERIFY2(readBack.isValid(&why), qPrintable(why));
    QVERIFY(readBack.sources.at(0).isImage());
    QCOMPARE(readBack.pages.at(0).sizePt, QSizeF(64, 40));
    /// A plain PDF source writes no kind at all, so an ordinary notebook's manifest is unchanged.
    PdfSessionManifest plain = manifest;
    plain.sources[0].kind.clear();
    QVERIFY(!plain.toJson().value(QStringLiteral("sources")).toArray().at(0).toObject()
                 .contains(QStringLiteral("kind")));

    /// A kind this build does not know is refused BY NAME rather than opened as a PDF.
    PdfSessionManifest unknown = manifest;
    unknown.sources[0].kind = QStringLiteral("spreadsheet");
    QVERIFY(!unknown.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("spreadsheet")), qPrintable(why));

    /// No factory at all: a picture needs no platform renderer, which is the whole point of it
    /// being a source of its own rather than a second kind of PDF.
    PdfSourceRenderers renderers;
    const PdfPageInfo info = renderers.pageInfo(manifest, dir.path(), 0, &why);
    QVERIFY2(info.isValid(), qPrintable(why));
    QCOMPARE(info.sizePt, QSizeF(64, 40));
    QCOMPARE(info.rotation, 0);

    /// At 72 dpi the page is the picture, pixel for pixel.
    const QImage atOne = renderers.renderPage(manifest, dir.path(), 0, 72.0, &why);
    QVERIFY2(!atOne.isNull(), qPrintable(why));
    QCOMPARE(atOne.size(), QSize(64, 40));
    QCOMPARE(atOne.pixelColor(3, 3), QColor(Qt::black));
    QCOMPARE(atOne.pixelColor(30, 20), QColor(Qt::white));

    /// At twice the resolution it is the same picture with twice the pixels in each direction.
    const QImage atTwo = renderers.renderPage(manifest, dir.path(), 0, 144.0, &why);
    QVERIFY2(!atTwo.isNull(), qPrintable(why));
    QCOMPARE(atTwo.size(), QSize(128, 80));
    /// The mark is still dark and still in the same place: a resample of a one-pixel mark is a grey
    /// smudge by nature, which is why this asks where it is rather than what exact colour it is. The
    /// scale is smooth on purpose -- a scan blown up with nearest-neighbour is a page of stairs.
    QVERIFY2(qGray(atTwo.pixelColor(6, 6).rgb()) < 128, "the mark did not survive the resample");
    QVERIFY2(qGray(atTwo.pixelColor(60, 40).rgb()) > 200, "the paper is not white any more");

    /// A page the notebook scaled renders larger, exactly as a PDF page does.
    PdfSessionManifest scaled = manifest;
    scaled.pages[0].extraScale = 2.0;
    const QImage atScale = renderers.renderPage(scaled, dir.path(), 0, 72.0, &why);
    QVERIFY2(!atScale.isNull(), qPrintable(why));
    QCOMPARE(atScale.size(), QSize(128, 80));

    /// A file that is not a picture is refused rather than shown as a blank page: the user's scan
    /// must never come back as white paper with nothing to say about it.
    /// Not a PDF: some builds carry a Qt image plugin that renders a PDF's first page, which would
    /// make this test depend on the machine rather than on the reader. A file nothing can decode is
    /// what the case is about.
    const QString notAPicture = dir.filePath(QStringLiteral("notes.png"));
    {
        QFile bogus(notAPicture);
        QVERIFY(bogus.open(QIODevice::WriteOnly));
        bogus.write("this file is not a picture, and no image reader can make it one");
    }
    PdfSessionManifest wrong = manifest;
    wrong.sourceFile = QStringLiteral("notes.png");
    wrong.sources[0].file = QStringLiteral("notes.png");
    wrong.sources[0].sha256 = PdfSessionManifest::sha256OfFile(notAPicture);
    wrong.sources[0].byteSize = QFileInfo(notAPicture).size();
    /// The three legacy fields are the same source, and isValid() makes them agree.
    wrong.sourceSha256 = wrong.sources[0].sha256;
    wrong.sourceByteSize = wrong.sources[0].byteSize;
    QVERIFY2(wrong.isValid(&why), qPrintable(why));
    why.clear();
    QVERIFY(renderers.renderPage(wrong, dir.path(), 0, 72.0, &why).isNull());
    QVERIFY2(why.contains(QStringLiteral("cannot be read")), qPrintable(why));

    /// And a page of a picture is still a page the notebook can box and turn: the box is applied by
    /// the same code that applies it to a PDF page.
    PdfSessionManifest boxed = manifest;
    boxed.pages[0].boxPt = QRectF(8, 4, 16, 12);
    const QImage boxedRender = renderers.renderPage(boxed, dir.path(), 0, 72.0, &why);
    QVERIFY2(!boxedRender.isNull(), qPrintable(why));
    QCOMPARE(boxedRender.size(), QSize(16, 12));

    PdfSessionManifest turned = manifest;
    turned.pages[0].extraRotation = 90;
    const QImage turnedRender = renderers.renderPage(turned, dir.path(), 0, 72.0, &why);
    QVERIFY2(!turnedRender.isNull(), qPrintable(why));
    QCOMPARE(turnedRender.size(), QSize(40, 64));
}


/**
 * The size an imported picture becomes: the paper it was scanned from, or a share of its natural size.
 *
 * A picture has no size a person can see until something says how big its pixels are -- the same scan
 * is A4 at 300 dpi and a poster at 72 -- so the import asks, and this is the arithmetic behind the
 * question. Getting it wrong is not a wrong label: the page's size IS what the renderer is asked for,
 * so a picture imported at the wrong resolution is a page of the wrong size on screen and in the PDF.
 */
void PdfProjectBuilderTest::testAnImportedPictureIsRescaledAtImport()
{
    /// A 300 dpi scan of A4: 2480 x 3508 pixels is 595 x 842 points, which is the sheet a person
    /// recognises, and the number the dialog shows.
    const QSize a4Pixels(2480, 3508);
    const QSizeF a4 = ImageRenderBackend::pageSizeForScannedPixels(a4Pixels, 300);
    QVERIFY2(qAbs(a4.width() - 595.28) <= 0.6 && qAbs(a4.height() - 841.89) <= 0.6,
             qPrintable(QStringLiteral("a 300 dpi A4 scan came out %1 x %2 points")
                            .arg(a4.width()).arg(a4.height())));

    /// The same pixels at 72 dpi are the natural size -- one pixel to one point -- and at half the
    /// resolution the page is twice as big.
    QCOMPARE(ImageRenderBackend::pageSizeForScannedPixels(a4Pixels, 72), QSizeF(2480, 3508));
    const QSizeF at150 = ImageRenderBackend::pageSizeForScannedPixels(a4Pixels, 150);
    QVERIFY(qAbs(at150.width() - 1190.4) <= 0.6 && qAbs(at150.height() - 1683.8) <= 0.6);

    /// A share of natural size, which is the other answer the import offers.
    QCOMPARE(ImageRenderBackend::pageSizeForNaturalShare(QSize(800, 600), 100), QSizeF(800, 600));
    QCOMPARE(ImageRenderBackend::pageSizeForNaturalShare(QSize(800, 600), 50), QSizeF(400, 300));
    QCOMPARE(ImageRenderBackend::pageSizeForNaturalShare(QSize(800, 600), 150), QSizeF(1200, 900));

    /// What the notebook RECORDS is the SCALE, and the two agree: 300 dpi is 0.24 of the sheet the
    /// pixels would make on their own, and 0.24 of 2480 x 3508 is A4.
    QCOMPARE(ImageRenderBackend::scaleForScannedDpi(72), 1.0);
    QVERIFY(qAbs(ImageRenderBackend::scaleForScannedDpi(300) - 0.24) < 1e-9);
    QCOMPARE(ImageRenderBackend::scaleForNaturalShare(100), 1.0);
    QCOMPARE(ImageRenderBackend::scaleForNaturalShare(25), 0.25);

    /// Nothing usable in, nothing out: the caller refuses rather than making a page of no size.
    QVERIFY(!ImageRenderBackend::pageSizeForScannedPixels(QSize(), 300).isValid());
    QVERIFY(!ImageRenderBackend::pageSizeForScannedPixels(a4Pixels, 0).isValid());
    QVERIFY(!ImageRenderBackend::pageSizeForNaturalShare(QSize(800, 600), 0).isValid());
    QCOMPARE(ImageRenderBackend::scaleForScannedDpi(0), 0.0);
    QCOMPARE(ImageRenderBackend::scaleForNaturalShare(0), 0.0);

    /// And the size is what the page really is: the renderer is asked for exactly that page, so a
    /// scan imported at the resolution it was scanned at comes back as its own pixels.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage scan(248, 351, QImage::Format_ARGB32_Premultiplied);
    scan.fill(Qt::white);
    scan.setPixelColor(5, 5, QColor(Qt::black));
    const QString picture = dir.filePath(QStringLiteral("scan.png"));
    QVERIFY(scan.save(picture, "PNG"));

    PdfSessionManifest manifest;
    manifest.sourceFile = QStringLiteral("scan.png");
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(picture);
    manifest.sourceByteSize = QFileInfo(picture).size();
    PdfSourceRecord source;
    source.file = QStringLiteral("scan.png");
    source.sha256 = manifest.sourceSha256;
    source.byteSize = manifest.sourceByteSize;
    source.kind = QStringLiteral("image");
    manifest.sources.append(source);

    PdfPageRecord page;
    page.index = 0;
    page.source = 0;
    /// The picture's own size, one pixel to one point, and the import's answer as the page's scale.
    page.sizePt = QSizeF(248, 351);
    page.extraScale = ImageRenderBackend::scaleForScannedDpi(300);
    page.kraFile = QStringLiteral("pages/p0001.kra");
    manifest.pages.append(page);

    QString why;
    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    PdfSourceRenderers renderers;

    /// The page a person measures on paper is the sheet the scan is: 248 x 351 pixels at 300 dpi is
    /// 60 x 84 points, which is what the notebook shows and what the dialog promised.
    const QSizeF shown = manifest.pages.at(0).displaySizePt();
    QVERIFY2(qAbs(shown.width() - 59.52) <= 0.1 && qAbs(shown.height() - 84.24) <= 0.1,
             qPrintable(QStringLiteral("the imported page shows as %1 x %2 points")
                            .arg(shown.width()).arg(shown.height())));

    /// And at the resolution it was scanned at, the page IS the scan, pixel for pixel: the picture was
    /// never resampled to get there, which is the whole reason the import records a scale rather than
    /// a smaller size.
    const QImage atScanDpi = renderers.renderPage(manifest, dir.path(), 0, 300.0, &why);
    QVERIFY2(!atScanDpi.isNull(), qPrintable(why));
    QCOMPARE(atScanDpi.size(), QSize(248, 351));
    QCOMPARE(atScanDpi.pixelColor(5, 5), QColor(Qt::black));

    /// At 72 dpi -- the screen's own resolution -- it is the page as it is shown: about 60 x 84.
    const QImage atSeventyTwo = renderers.renderPage(manifest, dir.path(), 0, 72.0, &why);
    QVERIFY2(!atSeventyTwo.isNull(), qPrintable(why));
    QCOMPARE(atSeventyTwo.size(), QSize(60, 84));
}

void PdfProjectBuilderTest::testFailsWithoutGeometry()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QString why;
    PdfPageRecord broken;
    broken.index = 0;
    const KisImageSP image = PdfProjectBuilder::buildPageImage(broken, backend, 72.0, &why);
    QVERIFY(!image);
    QVERIFY(!why.isEmpty());
}

void PdfProjectBuilderTest::testSourceRendererCacheUsesProjectAndContentIdentity()
{
    QTemporaryDir projects;
    QVERIFY(projects.isValid());

    const QString projectA = projects.filePath(QStringLiteral("a"));
    const QString projectB = projects.filePath(QStringLiteral("b"));
    QVERIFY(QDir().mkpath(projectA));
    QVERIFY(QDir().mkpath(projectB));

    const QString sharedRelativeName = QStringLiteral("same-name.pdf");
    const QString sourceA = QDir(projectA).filePath(sharedRelativeName);
    const QString sourceB = QDir(projectB).filePath(sharedRelativeName);
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("text-fixture.pdf")), sourceA));
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("ex-manypage-50.pdf")), sourceB));

    const auto manifestFor = [&sharedRelativeName](const QString &sourcePath, int sourcePage) {
        PdfSessionManifest manifest;
        PdfSourceRecord source;
        source.file = sharedRelativeName;
        source.sha256 = PdfSessionManifest::sha256OfFile(sourcePath);
        source.byteSize = QFileInfo(sourcePath).size();
        manifest.sources.append(source);

        PdfPageRecord page;
        page.index = sourcePage;
        page.sizePt = QSizeF(612.0, 792.0);
        page.source = 0;
        manifest.pages.append(page);
        return manifest;
    };

    const PdfSessionManifest manifestA = manifestFor(sourceA, 0);
    const PdfSessionManifest manifestB = manifestFor(sourceB, 4);
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    QString why;

    const QImage pageA = renderers.renderPage(manifestA, projectA, 0, 72.0, &why);
    QVERIFY2(!pageA.isNull(), qPrintable(why));

    /// This notebook uses the same relative name but a different source PDF. Page 5 is outside the
    /// first source's three pages, so reusing its open backend would silently return an empty image.
    why.clear();
    const QImage pageB = renderers.renderPage(manifestB, projectB, 0, 72.0, &why);
    QVERIFY2(!pageB.isNull(), qPrintable(why));
    QCOMPARE(renderers.openSourceCount(), 2);
}

/**
 * The corners of a turned page are empty, not black.
 *
 * A PDF page is rendered opaque -- there is no alpha in the source -- and QImage::transformed()
 * fills whatever a turn exposes with zero. In an opaque format zero is BLACK, so a page set down at
 * an angle would have come back with black triangles around the sheet. Given an alpha channel first,
 * the corners are the nothing that is really there, which is also what the artifact's own turned
 * pixels have.
 *
 * The right angle is asserted here too, because it is the case that must NOT change: it exposes
 * nothing, so it stays in the format it came in and is never resampled.
 */
void PdfProjectBuilderTest::testAnAngledPageHasEmptyCorners()
{
    QImage source(200, 400, QImage::Format_RGB32);
    source.fill(Qt::white);

    /// 200x400 set down at 37 degrees is 400x440 to the nearest pixel: the box, not the sheet.
    const QImage turned = PdfSourceRenderers::turnedForDisplay(source, 37);
    QVERIFY(!turned.isNull());
    QCOMPARE(turned.size(), QSize(400, 440));
    QVERIFY2(turned.hasAlphaChannel(), "an angled page has to be able to be empty at its corners");

    /// Two corners the sheet no longer covers, and the middle of the sheet, which it still does.
    QCOMPARE(qAlpha(turned.pixel(turned.width() - 1, 0)), 0);
    QCOMPARE(qAlpha(turned.pixel(0, turned.height() - 1)), 0);
    QCOMPARE(qAlpha(turned.pixel(turned.width() / 2, turned.height() / 2)), 255);

    /// A right angle exposes nothing: the same transpose as before, in the format it came in.
    const QImage rightAngle = PdfSourceRenderers::turnedForDisplay(source, 90);
    QCOMPARE(rightAngle.size(), QSize(400, 200));
    QCOMPARE(rightAngle.format(), source.format());
    QCOMPARE(qAlpha(rightAngle.pixel(200, 100)), 255);
}

QTEST_MAIN(PdfProjectBuilderTest)
#include "PdfProjectBuilderTest.moc"
