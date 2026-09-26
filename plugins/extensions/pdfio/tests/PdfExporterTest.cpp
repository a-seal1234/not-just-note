/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfAssembler.h"
#include "session/PdfExporter.h"
#include "session/PdfSession.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTransform>
#include <QtMath>
#include <QtTest>

/**
 * The export contract, checked by rendering the result rather than trusting the bytes.
 *
 * Three families are covered:
 *   - a classic PDF 1.4 file with plain page objects (text-fixture.pdf, ex-rotations.pdf),
 *   - a PDF 1.5 file whose pages live in an object stream behind a cross reference stream
 *     (ex-objstm-predictor.pdf, hand built to use a PNG predictor, and ex-objstm-*.pdf,
 *     produced by Ghostscript, a real third party writer),
 *   - a 50 page mixed size notebook, where untouched pages have to stay byte identical.
 *
 * The ink has to land where it was drawn on plain, rotated and offset MediaBox pages, the source
 * text has to survive all of it, and what the exporter cannot handle has to be refused with a
 * precise message instead of producing a file that only looks right.
 */
class PdfExporterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPageObjects();
    void testObjectStreamPageObjects();
    void testPredictorObjectStreamPageObjects();
    void testRefusesEncrypted();
    void testRefusesUnsupportedFilter();
    void testInkLandsWhereItWasDrawn();
    void testSourceTextSurvives();
    void testRotationsAndOffsetMediaBox();
    void testObjectStreamRotations();
    void testPredictorObjectStreamExport();
    void testBrowserPdfExport();
    void testManyPageNotebook();
    void testManyPagePeakMemory();
    void testIndirectContentsArrayExport();
    void testPageWithoutReadableMediaBoxStillExports();
    void testRefusesInkWithoutAlpha();
    void testMediaBoxCases();

    /// The notebook's own page turn, on top of whatever /Rotate the source declares: the paper has
    /// to come out turned and the ink has to stay on it.
    void testNotebookQuarterTurnTurnsPaperAndInk();
    void testNotebookHalfTurnTurnsPaperAndInk();
    void testNotebookFreeAngleTurnsPaperAndInk();

    /// The notebook's own resize, on top of whatever the source declares: Scale writes the scaled
    /// box and the transform that agrees with it, and Box writes the cropped box and clips the page
    /// to it instead of squeezing the whole page into the smaller rectangle.
    void testAScaledPageExportsItsScaledBoxAndTransform();
    void testACroppedPageExportsOnlyItsBox();

    /// The guard that makes a notebook whose pages are no longer the PDF's own order refuse to
    /// export, rather than write a file whose ink is on the wrong pages.
    void testAMovedNotebookExportsInNotebookOrder();
    void testADuplicatedPageExportsTwiceWithItsOwnInk();
    void testAMultiSourceNotebookIsRefusedWithTheReason();

    /// Assembling a notebook that draws on several PDFs into ONE file (task-34). The first test is
    /// the one that matters: the fixture has the shape every page of the user's own manual has --
    /// no /Resources and no /MediaBox on the page object, both inherited from the /Pages node --
    /// and a copier that passes its own fixtures without resolving that writes blank paper.
    void testAssemblingTwoSourcesWritesOneFileInNotebookOrder();
    void testAnAssembledPageKeepsItsInheritedPaperAndResources();
    void testTheAssemblerNamesASourceItCannotRead();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }

    QByteArray readFile(const QString &path) const
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return QByteArray();
        }
        return file.readAll();
    }

    /// A transparent ink plane with one opaque square near the top left of the *displayed* page.
    QImage inkWithMark(const QSize &displaySize) const
    {
        QImage ink(displaySize, QImage::Format_ARGB32);
        ink.fill(Qt::transparent);
        for (int y = 10; y < 60; ++y) {
            for (int x = 10; x < 60; ++x) {
                ink.setPixel(x, y, qRgba(0, 0, 0, 255));
            }
        }
        return ink;
    }

    /// The same mark in red: the page's own text is dark, so a colour that appears nowhere in the
    /// source lets a test measure where the ink landed instead of where anything dark is.
    QImage inkWithRedMark(const QSize &displaySize) const
    {
        QImage ink(displaySize, QImage::Format_ARGB32);
        ink.fill(Qt::transparent);
        for (int y = 10; y < 60; ++y) {
            for (int x = 10; x < 60; ++x) {
                ink.setPixel(x, y, qRgba(255, 0, 0, 255));
            }
        }
        return ink;
    }

    /// Where the red mark came back, in the exported page's own pixels at 72 dpi. Invalid when the
    /// mark is nowhere on the page, which is itself a result: with the fallback box taken as it
    /// stood, the /Rotate 270 page's mark was drawn right off the page.
    QRect redMarkBounds(const QImage &image) const
    {
        const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
        int x0 = rgb.width();
        int y0 = rgb.height();
        int x1 = -1;
        int y1 = -1;
        for (int y = 0; y < rgb.height(); ++y) {
            for (int x = 0; x < rgb.width(); ++x) {
                const QRgb pixel = rgb.pixel(x, y);
                if (qRed(pixel) > 150 && qGreen(pixel) < 100 && qBlue(pixel) < 100) {
                    x0 = qMin(x0, x);
                    y0 = qMin(y0, y);
                    x1 = qMax(x1, x);
                    y1 = qMax(y1, y);
                }
            }
        }
        return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
    }

    /// The manifest is what maps a page index to a record, so the test builds a real one rather
    /// than exercising a path the application never takes.
    PdfSessionManifest manifestFor(PdfRenderBackend &backend) const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;
        for (int i = 0; i < backend.pageCount(); ++i) {
            const PdfPageInfo info = backend.pageInfo(i);
            manifest.pages.append(PdfPageRecord{i, info.sizePt, info.rotation,
                                                PdfSession::pageFileName(i), QString(), 0});
        }
        return manifest;
    }

    /// A manifest for a file Poppler cannot open (an encrypted one), so the refusal can be
    /// checked where it happens rather than through the renderer.
    PdfSessionManifest syntheticManifest(int pageCount) const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;
        for (int i = 0; i < pageCount; ++i) {
            manifest.pages.append(PdfPageRecord{i, QSizeF(595, 842), 0,
                                                PdfSession::pageFileName(i), QString(), 0});
        }
        return manifest;
    }

    /// Pixels a reader would show as content: the same measure the independent renderers were
    /// checked with, so a test can say "the page's own content plus the mark, not the mark alone".
    int darkPixels(const QImage &image) const
    {
        int dark = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) > 0 && qGray(image.pixel(x, y)) < 200) {
                    ++dark;
                }
            }
        }
        return dark;
    }

    QRect darkBounds(const QImage &image) const
    {
        int x0 = image.width();
        int y0 = image.height();
        int x1 = -1;
        int y1 = -1;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) > 0 && qGray(image.pixel(x, y)) < 200) {
                    x0 = qMin(x0, x);
                    y0 = qMin(y0, y);
                    x1 = qMax(x1, x);
                    y1 = qMax(y1, y);
                }
            }
        }
        return x1 < 0 ? QRect() : QRect(QPoint(x0, y0), QPoint(x1, y1));
    }

    /// Every exported page has to carry the mark near the top left of the page as displayed.
    void verifyMarkTopLeft(PdfRenderBackend &exported, int page) const
    {
        const QImage rendered = exported.renderPage(page, 72.0);
        QVERIFY2(!rendered.isNull(), qPrintable(QStringLiteral("page %1 did not render").arg(page)));
        const QRect bounds = darkBounds(rendered);
        const QString where = QStringLiteral("page %1: ink at (%2,%3)-(%4,%5), expected about (10,10)")
                                  .arg(page)
                                  .arg(bounds.left()).arg(bounds.top())
                                  .arg(bounds.right()).arg(bounds.bottom());
        QVERIFY2(bounds.isValid() && qAbs(bounds.left() - 10) <= 4 && qAbs(bounds.top() - 10) <= 4,
                 qPrintable(where));
    }

    /// Whether two rectangles line up within \a tolerance pixels on every side. Used to compare
    /// where the turned paper's own content came back without depending on a vector render being
    /// pixel identical to a raster that was resampled rather than drawn by the renderer.
    bool rectsClose(const QRect &a, const QRect &b, int tolerance) const
    {
        return a.isValid() && b.isValid() && qAbs(a.left() - b.left()) <= tolerance
            && qAbs(a.top() - b.top()) <= tolerance && qAbs(a.right() - b.right()) <= tolerance
            && qAbs(a.bottom() - b.bottom()) <= tolerance;
    }

    /// The paper as the notebook shows it: the page's render turned by the notebook's own rotation.
    ///
    /// PdfSourceRenderers::turnedForDisplay() is exactly this call, but that .cpp is not linked
    /// into this test target, so the two lines are repeated here. The test measures the exporter
    /// against the same QTransform turn the reader is built on, which is the point: the exported
    /// page has to show what the notebook showed.
    QImage turnedForDisplay(const QImage &rendered, int extraRotation) const
    {
        if (rendered.isNull() || extraRotation == 0) {
            return rendered;
        }
        const int turn = ((extraRotation % 360) + 360) % 360;
        const bool rightAngle = turn % 90 == 0;
        /// An ARGB32 copy first, so that a free-angle turn's exposed corners come back transparent
        /// instead of the black an opaque raster is filled with there. A corner of the bounding box
        /// is not page content, and darkBounds() must not report the whole page because of it.
        const QImage source = rendered.convertToFormat(QImage::Format_ARGB32);
        return source.transformed(QTransform().rotate(extraRotation),
                                  rightAngle ? Qt::FastTransformation : Qt::SmoothTransformation);
    }

    /// Peak resident set of this process, from /proc. Zero where the kernel does not report it.
    qint64 peakRssKb() const
    {
        const QByteArray status = readFile(QStringLiteral("/proc/self/status"));
        const int at = status.indexOf("VmHWM:");
        if (at < 0) {
            return 0;
        }
        const QByteArray line = status.mid(at, 40);
        bool ok = false;
        const qint64 value = line.split(':').value(1).trimmed().split(' ').value(0).toLongLong(&ok);
        return ok ? value : 0;
    }
};

void PdfExporterTest::testPageObjects()
{
    QFile file(fixturePath(QStringLiteral("text-fixture.pdf")));
    QVERIFY(file.open(QIODevice::ReadOnly));

    QString why;
    const QList<int> pages = PdfExporter::pageObjectNumbers(file.readAll(), &why);
    QVERIFY2(pages.size() == 3, qPrintable(why));
    QCOMPARE(pages, QList<int>({3, 5, 7}));
}

void PdfExporterTest::testObjectStreamPageObjects()
{
    /// Ghostscript wrote this one: the page dictionaries sit in an object stream that a cross
    /// reference stream points at, which used to be the refusal case.
    const QByteArray streamed = readFile(fixturePath(QStringLiteral("ex-objstm-rotations.pdf")));
    QVERIFY(!streamed.isEmpty());
    QVERIFY(streamed.contains("/Type /ObjStm") || streamed.contains("/Type/ObjStm"));
    QVERIFY(streamed.contains("/Type /XRef") || streamed.contains("/Type/XRef"));

    QString why;
    const QList<int> pages = PdfExporter::pageObjectNumbers(streamed, &why);
    QVERIFY2(pages.size() == 5, qPrintable(why));

    const QByteArray many = readFile(fixturePath(QStringLiteral("ex-objstm-50p.pdf")));
    QVERIFY(!many.isEmpty());
    const QList<int> manyPages = PdfExporter::pageObjectNumbers(many, &why);
    QVERIFY2(manyPages.size() == 50, qPrintable(why));
}

void PdfExporterTest::testPredictorObjectStreamPageObjects()
{
    const QByteArray bytes = readFile(fixturePath(QStringLiteral("ex-objstm-predictor.pdf")));
    QVERIFY(!bytes.isEmpty());

    QString why;
    const QList<int> pages = PdfExporter::pageObjectNumbers(bytes, &why);
    QVERIFY2(pages.size() == 2, qPrintable(why));
    QCOMPARE(pages, QList<int>({3, 4}));
}

void PdfExporterTest::testRefusesEncrypted()
{
    const QByteArray encrypted = readFile(fixturePath(QStringLiteral("ex-encrypted.pdf")));
    QVERIFY(!encrypted.isEmpty());

    QString why;
    QVERIFY(PdfExporter::pageObjectNumbers(encrypted, &why).isEmpty());
    QVERIFY2(why.contains(QStringLiteral("encrypted")), qPrintable(why));

    /// And the writer refuses too, rather than leaving a file that only looks right.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));
    why.clear();
    QVERIFY(!PdfExporter::exportWithInk(fixturePath(QStringLiteral("ex-encrypted.pdf")),
                                        syntheticManifest(5), {}, out, &why));
    QVERIFY2(why.contains(QStringLiteral("encrypted")), qPrintable(why));
    QVERIFY(!QFile::exists(out));
}

void PdfExporterTest::testRefusesUnsupportedFilter()
{
    /// A stream filter the reader does not implement has to be named, not guessed at. The fixture
    /// is a separate file because editing a filter name in place would move every xref offset.
    const QByteArray bytes = readFile(fixturePath(QStringLiteral("ex-objstm-badfilter.pdf")));
    QVERIFY(!bytes.isEmpty());
    QVERIFY(bytes.contains("/LZWDecode"));

    QString why;
    QVERIFY(PdfExporter::pageObjectNumbers(bytes, &why).isEmpty());
    QVERIFY2(why.contains(QStringLiteral("unsupported stream filter")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("LZWDecode")), qPrintable(why));
}

void PdfExporterTest::testInkLandsWhereItWasDrawn()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QHash<int, QImage> ink;
    for (int i = 0; i < backend.pageCount(); ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("text-fixture.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 3);

    for (int i = 0; i < exported.pageCount(); ++i) {
        verifyMarkTopLeft(exported, i);
    }
}

void PdfExporterTest::testSourceTextSurvives()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QHash<int, QImage> ink;
    ink.insert(0, inkWithMark(QSize(595, 842)));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("text-fixture.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));

    /// No original object is rewritten, so the text has to still be selectable.
    const QString text = exported.pageText(0);
    QVERIFY2(text.contains(QStringLiteral("Page one heading")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Plain A4 page.")), qPrintable(text));
}

void PdfExporterTest::testRotationsAndOffsetMediaBox()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("ex-rotations.pdf"))));
    QCOMPARE(backend.pageCount(), 5);

    /// 0, 90, 180 and 270 degrees, the last page with a MediaBox that does not start at (0,0).
    QCOMPARE(backend.pageInfo(0).sizePt, QSizeF(595, 842));
    QCOMPARE(backend.pageInfo(1).sizePt, QSizeF(842, 595));
    QCOMPARE(backend.pageInfo(2).sizePt, QSizeF(595, 842));
    QCOMPARE(backend.pageInfo(3).sizePt, QSizeF(842, 595));
    QCOMPARE(backend.pageInfo(4).sizePt, QSizeF(300, 300));

    QHash<int, QImage> ink;
    for (int i = 0; i < backend.pageCount(); ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("ex-rotations.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 5);

    for (int i = 0; i < exported.pageCount(); ++i) {
        verifyMarkTopLeft(exported, i);
    }

    const QString last = exported.pageText(4);
    QVERIFY2(last.contains(QStringLiteral("Offset media box")), qPrintable(last));
}

void PdfExporterTest::testObjectStreamRotations()
{
    /// The same geometry, but produced by Ghostscript: the page dictionaries are compressed
    /// inside an object stream and every page redefinition has to override a compressed object.
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("ex-objstm-rotations.pdf"))));
    QCOMPARE(backend.pageCount(), 5);
    QCOMPARE(backend.pageInfo(4).sizePt, QSizeF(300, 300));

    QHash<int, QImage> ink;
    for (int i = 0; i < backend.pageCount(); ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("ex-objstm-rotations.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 5);
    for (int i = 0; i < exported.pageCount(); ++i) {
        verifyMarkTopLeft(exported, i);
    }

    QVERIFY2(exported.pageText(0).contains(QStringLiteral("Rotation zero")),
             qPrintable(exported.pageText(0)));
}

void PdfExporterTest::testPredictorObjectStreamExport()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("ex-objstm-predictor.pdf"))));
    QCOMPARE(backend.pageCount(), 2);

    QHash<int, QImage> ink;
    ink.insert(0, inkWithMark(QSize(595, 842)));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("ex-objstm-predictor.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 2);
    verifyMarkTopLeft(exported, 0);

    QVERIFY2(exported.pageText(0).contains(QStringLiteral("Predictor page one")),
             qPrintable(exported.pageText(0)));
    QVERIFY2(exported.pageText(1).contains(QStringLiteral("Predictor page two")),
             qPrintable(exported.pageText(1)));

    /// Page two had no ink, so every byte of it has to be exactly where it was.
    const QByteArray source = readFile(fixturePath(QStringLiteral("ex-objstm-predictor.pdf")));
    const QByteArray result = readFile(out);
    QVERIFY(result.startsWith(source));
}

void PdfExporterTest::testBrowserPdfExport()
{
    /// A real browser print (Skia/PDF): its own object layout, compact dictionaries and a
    /// trailer with /Info, which is what the attribute-preserving update has to survive.
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("ex-browser-3p.pdf"))));
    QCOMPARE(backend.pageCount(), 3);

    QHash<int, QImage> ink;
    for (int i = 0; i < backend.pageCount(); ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixturePath(QStringLiteral("ex-browser-3p.pdf")),
                                        manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 3);
    for (int i = 0; i < 3; ++i) {
        verifyMarkTopLeft(exported, i);
        QVERIFY2(exported.pageText(i).contains(QStringLiteral("Browser page")),
                 qPrintable(exported.pageText(i)));
    }
}

void PdfExporterTest::testManyPageNotebook()
{
    const QString fixture = fixturePath(QStringLiteral("ex-objstm-50p.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixture));
    QCOMPARE(backend.pageCount(), 50);

    /// Ink on every other page, so the untouched half can be compared byte for byte.
    QHash<int, QImage> ink;
    for (int i = 0; i < 50; i += 2) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(PdfExporter::exportWithInk(fixture, manifestFor(backend), ink, out, &why),
             qPrintable(why));
    const qint64 elapsedMs = timer.elapsed();

    const QByteArray source = readFile(fixture);
    const QByteArray result = readFile(out);
    QVERIFY(!result.isEmpty());

    /// The source is never rewritten: every original byte is still in front of the update.
    QVERIFY(result.startsWith(source));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 50);

    for (int i = 0; i < 50; ++i) {
        const QString text = exported.pageText(i);
        QVERIFY2(text.contains(QStringLiteral("Page %1 heading").arg(i + 1)), qPrintable(text));

        if (i % 2 == 0) {
            verifyMarkTopLeft(exported, i);
        } else {
            /// An untouched page renders exactly as it did in the source.
            QVERIFY2(source.at(0) == '%', "fixture is not a PDF");
            QCOMPARE(exported.renderPage(i, 72.0), backend.renderPage(i, 72.0));
        }
    }

    qInfo("50-page notebook: exported %d bytes from %d bytes of source in %lld ms",
          int(result.size()), int(source.size()), static_cast<long long>(elapsedMs));
}

void PdfExporterTest::testManyPagePeakMemory()
{
    const QString fixture = fixturePath(QStringLiteral("ex-objstm-50p.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixture));
    QCOMPARE(backend.pageCount(), 50);

    const qint64 rssBeforeInk = peakRssKb();

    /// The worst case an application can hand the exporter: a full size plane for all 50 pages.
    QHash<int, QImage> ink;
    for (int i = 0; i < 50; ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }
    const qint64 rssWithInk = peakRssKb();

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(PdfExporter::exportWithInk(fixture, manifestFor(backend), ink, out, &why),
             qPrintable(why));
    const qint64 elapsedMs = timer.elapsed();
    const qint64 rssAfterExport = peakRssKb();

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 50);
    verifyMarkTopLeft(exported, 0);
    verifyMarkTopLeft(exported, 49);

    const QByteArray result = readFile(out);
    qInfo("50-page full ink export: %lld ms, output %d bytes, VmHWM before ink %lld kB, "
          "with ink %lld kB, after export %lld kB",
          static_cast<long long>(elapsedMs), int(result.size()),
          static_cast<long long>(rssBeforeInk), static_cast<long long>(rssWithInk),
          static_cast<long long>(rssAfterExport));

    /// Keep a copy for the independent viewer cross-check when asked to.
    const QByteArray keep = qgetenv("PDFIO_EXPORT_KEEP");
    if (!keep.isEmpty()) {
        QFile::remove(QString::fromLocal8Bit(keep));
        QVERIFY2(QFile::copy(out, QString::fromLocal8Bit(keep)),
                 qPrintable(QString::fromLocal8Bit(keep)));
    }
}

void PdfExporterTest::testIndirectContentsArrayExport()
{
    /// A page's /Contents may be an indirect reference to an array -- this fixture writes exactly
    /// that on page one (object 9 is "[4 0 R 5 0 R]"). The update used to wrap the reference in an
    /// array of its own, and a nested array is not a content list: measured before the fix, the
    /// exported page carried the mark's 2500 dark pixels and none of the source's text (pdfium),
    /// while Poppler answered "Syntax Error: Weird page contents" and rendered the page blank. That
    /// is the Mi Pad 8 report -- ink floating on blank paper -- so the array's entries are spliced
    /// into the new list flat instead.
    const QString fixture = fixturePath(QStringLiteral("ex-indirect-contents.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixture));
    QCOMPARE(backend.pageCount(), 3);

    /// What the source renders, so the loss is measured rather than assumed.
    const QImage sourcePage = backend.renderPage(0, 72.0);
    QVERIFY(!sourcePage.isNull());
    const int sourceDark = darkPixels(sourcePage);
    QVERIFY2(sourceDark > 100, qPrintable(QStringLiteral("the fixture has no visible text: %1 dark "
                                                         "pixels").arg(sourceDark)));

    QHash<int, QImage> ink;
    ink.insert(0, inkWithMark(QSize(595, 842)));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixture, manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 3);

    /// The text is the sharpest witness: with the nested array in the file it is not there at all.
    const QString first = exported.pageText(0);
    QVERIFY2(first.contains(QStringLiteral("Indirect contents page one")), qPrintable(first));
    QVERIFY2(first.contains(QStringLiteral("Second half of page one.")), qPrintable(first));

    /// And in pixels the page has to carry its own content *plus* the mark, not the mark alone.
    const QImage exportedPage = exported.renderPage(0, 72.0);
    QVERIFY(!exportedPage.isNull());
    const int exportedDark = darkPixels(exportedPage);
    QVERIFY2(exportedDark > sourceDark + 1000,
             qPrintable(QStringLiteral("the exported page has %1 dark pixels against the source's "
                                       "%2: the text plus the mark is expected, the mark alone is "
                                       "not").arg(exportedDark).arg(sourceDark)));
    verifyMarkTopLeft(exported, 0);

    /// A page that was not inked is untouched, and its own text is still there.
    QVERIFY2(exported.pageText(1).contains(QStringLiteral("Direct page two")),
             qPrintable(exported.pageText(1)));
}

void PdfExporterTest::testPageWithoutReadableMediaBoxStillExports()
{
    /// Page three of the fixture puts its /MediaBox on another object (/MediaBox 10 0 R), which the
    /// exporter's own reader refuses. The size the notebook recorded is the fallback, and the page's
    /// content has to survive that instead of the whole export being abandoned -- the failure that
    /// made pressing Export do nothing at all on the tablet.
    const QString fixture = fixturePath(QStringLiteral("ex-indirect-contents.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixture));
    QCOMPARE(backend.pageCount(), 3);

    /// Poppler resolves the reference, so the recorded size is the real one.
    QCOMPARE(backend.pageInfo(2).sizePt, QSizeF(595, 842));

    QHash<int, QImage> ink;
    ink.insert(2, inkWithMark(QSize(595, 842)));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixture, manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 3);

    QVERIFY2(exported.pageText(2).contains(QStringLiteral("Indirect media box page three")),
             qPrintable(exported.pageText(2)));
    QVERIFY2(exported.pageText(2).contains(QStringLiteral("Box on another object.")),
             qPrintable(exported.pageText(2)));
    verifyMarkTopLeft(exported, 2);
}

void PdfExporterTest::testRefusesInkWithoutAlpha()
{
    /// An ink plane with no alpha channel is not an overlay at all: /pdfioInk would draw it as an
    /// opaque sheet and the page underneath would be gone. Measured with the old behaviour on
    /// text-fixture.pdf, the page's 1758 dark pixels came back as exactly the 2500 of the mark. The
    /// export refuses and names the page instead of writing that file, because the menu shows the
    /// reason and a file whose background is gone looks right to nobody.
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));

    QImage opaque(QSize(595, 842), QImage::Format_RGB32);
    QVERIFY(!opaque.hasAlphaChannel());
    opaque.fill(Qt::white);
    for (int y = 10; y < 60; ++y) {
        for (int x = 10; x < 60; ++x) {
            opaque.setPixel(x, y, qRgb(0, 0, 0));
        }
    }

    QHash<int, QImage> ink;
    ink.insert(0, opaque);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY(!PdfExporter::exportWithInk(fixturePath(QStringLiteral("text-fixture.pdf")),
                                        manifestFor(backend), ink, out, &why));
    QVERIFY2(why.contains(QStringLiteral("alpha")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("page 1")), qPrintable(why));
    QVERIFY(!QFile::exists(out));
}

/**
 * A notebook whose page list is not the PDF's own order exports in the NOTEBOOK's order.
 *
 * The page written at position i is the source page the i-th RECORD names, and the ink at key i goes
 * with it. That is what makes reorder, delete, duplicate and insert exportable at all: the old code
 * wrote the overlay onto the PDF page at the same position, which is correct only while the notebook
 * happens to be the source's own pages in their own order.
 */
void PdfExporterTest::testAMovedNotebookExportsInNotebookOrder()
{
    PopplerRenderBackend backend;
    const QString source = fixturePath(QStringLiteral("ex-mediabox-cases.pdf"));
    QVERIFY(backend.open(source));
    QCOMPARE(backend.pageCount(), 5);

    /// The notebook's order is [5, 1, 2, 3, 4] (one-based): the PDF's last page first.
    PdfSessionManifest moved = manifestFor(backend);
    moved.pages.move(4, 0);

    /// Ink on notebook pages 1 and 3, which are PDF pages 5 and 2 -- and would be PDF pages 1 and 3
    /// if the export still worked by position.
    QHash<int, QImage> ink;
    ink.insert(0, inkWithMark(backend.pageInfo(4).sizePt.toSize()));
    ink.insert(2, inkWithMark(backend.pageInfo(1).sizePt.toSize()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("moved.pdf"));
    const QString withoutInk = dir.filePath(QStringLiteral("moved-clean.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, moved, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, moved, QHash<int, QImage>(), withoutInk, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    QCOMPARE(exported.pageCount(), 5);

    /// Each exported page is the source page its record names, in the notebook's own order.
    const QStringList headings = { QStringLiteral("Missing media box"),
                                   QStringLiteral("Direct media box"),
                                   QStringLiteral("Indirect media box"),
                                   QStringLiteral("Rotated direct box"),
                                   QStringLiteral("Rotated indirect box") };
    for (int i = 0; i < headings.size(); ++i) {
        QVERIFY2(exported.pageText(i).contains(headings.at(i)),
                 qPrintable(QStringLiteral("exported page %1: expected \"%2\", got \"%3\"")
                                .arg(i + 1).arg(headings.at(i)).arg(exported.pageText(i))));
    }

    /// And the ink is where the notebook says: on the pages that have ink, and not on the pages the
    /// positional export would have marked instead. Compared against the same notebook exported with
    /// no ink at all, so "not marked" is a statement about the pixels.
    PopplerRenderBackend clean;
    QVERIFY(clean.open(withoutInk));
    for (int i = 0; i < exported.pageCount(); ++i) {
        const QImage marked = exported.renderPage(i, 72.0);
        const QImage plain = clean.renderPage(i, 72.0);
        QVERIFY2(!marked.isNull() && !plain.isNull(), "a page of the export did not render");
        const bool inked = i == 0 || i == 2;
        QVERIFY2((marked == plain) != inked,
                 qPrintable(QStringLiteral("exported page %1: ink present=%2, expected=%3")
                                .arg(i + 1).arg(marked == plain ? "no" : "yes")
                                .arg(inked ? "yes" : "no")));
    }
}

/**
 * A duplicated page exports twice, and each copy carries its own ink.
 *
 * Two notebook pages naming one source page used to be a refusal: the overlay was attached to the
 * page OBJECT, so the two would have shared one. A new page object per notebook page is what makes
 * the duplicate a page of its own in the file, exactly as it is in the notebook.
 */
void PdfExporterTest::testADuplicatedPageExportsTwiceWithItsOwnInk()
{
    PopplerRenderBackend backend;
    const QString source = fixturePath(QStringLiteral("ex-mediabox-cases.pdf"));
    QVERIFY(backend.open(source));

    /// The first source page twice in a row: six pages, two of them the same PDF page.
    PdfSessionManifest manifest = manifestFor(backend);
    manifest.pages.insert(1, manifest.pages.at(0));
    QCOMPARE(manifest.pages.size(), 6);

    /// Ink on the COPY only, so the two exported pages of that one source page have to differ.
    QHash<int, QImage> ink;
    ink.insert(1, inkWithMark(backend.pageInfo(0).sizePt.toSize()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("duplicated.pdf"));
    const QString withoutInk = dir.filePath(QStringLiteral("duplicated-clean.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), withoutInk, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    QCOMPARE(exported.pageCount(), 6);
    QVERIFY2(exported.pageText(0).contains(QStringLiteral("Direct media box")),
             qPrintable(exported.pageText(0)));
    QVERIFY2(exported.pageText(1).contains(QStringLiteral("Direct media box")),
             qPrintable(exported.pageText(1)));

    PopplerRenderBackend clean;
    QVERIFY(clean.open(withoutInk));
    QVERIFY2(exported.renderPage(0, 72.0) == clean.renderPage(0, 72.0),
             "the page that was not duplicated with ink came back marked");
    QVERIFY2(exported.renderPage(1, 72.0) != clean.renderPage(1, 72.0),
             "the duplicate did not carry its own ink");
}

/**
 * A notebook that draws its pages from more than one PDF is still refused, with the reason.
 *
 * Rebuilding the page tree of one PDF is what this export does. Merging the objects of a second one
 * into the output means renumbering every reference inside them, which is a writer of its own; until
 * that exists, saying so is better than writing a file whose pages from the other PDF have lost
 * their backgrounds.
 */
void PdfExporterTest::testAMultiSourceNotebookIsRefusedWithTheReason()
{
    PopplerRenderBackend backend;
    const QString source = fixturePath(QStringLiteral("text-fixture.pdf"));
    QVERIFY(backend.open(source));

    PdfSessionManifest manifest = manifestFor(backend);
    PdfSourceRecord first;
    first.file = manifest.sourceFile;
    first.sha256 = manifest.sourceSha256;
    first.byteSize = manifest.sourceByteSize;
    PdfSourceRecord second;
    second.file = QStringLiteral("sources/other.pdf");
    second.sha256 = QByteArrayLiteral("cafebabe");
    second.byteSize = 1;
    manifest.sources.clear();
    manifest.sources << first << second;
    manifest.pages[1].source = 1;

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("multi.pdf"));
    QString why;
    QVERIFY2(!PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), out, &why),
             "a notebook that draws on two PDFs was exported as if it drew on one");
    QVERIFY2(why.contains(QStringLiteral("PDFs")), qPrintable(why));
    QVERIFY(!QFileInfo::exists(out));
}

void PdfExporterTest::testMediaBoxCases()
{
    /// One page per /MediaBox shape: 1 a direct array, 2 an indirect reference, 3 a direct array
    /// with /Rotate 90, 4 an indirect reference with /Rotate 90, 5 no box anywhere.
    ///
    /// The two indirect pages fall back to the size the notebook recorded, and that size is the
    /// DISPLAYED one, because both renderers report the page the way the reader shows it. For a
    /// page turned a quarter turn the displayed size is the transpose of the page's own user space,
    /// so using it as it stood drew a 595x842 ink plane into an 842x595 box: measured before the
    /// fix, page 4's 50x50 mark came back 35x71 at (7,14) and page 5's mark was not on the page at
    /// all (0 red pixels).
    const QString fixture = fixturePath(QStringLiteral("ex-mediabox-cases.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixture));
    QCOMPARE(backend.pageCount(), 5);

    QCOMPARE(backend.pageInfo(0).rotation, 0);
    QCOMPARE(backend.pageInfo(1).rotation, 0);
    QCOMPARE(backend.pageInfo(2).rotation, 90);
    QCOMPARE(backend.pageInfo(3).rotation, 90);
    QCOMPARE(backend.pageInfo(4).rotation, 270);
    /// The quarter-turned pages are recorded as their displayed size, which is the transpose of
    /// their user space: 842x595 for the A4 page, 792x612 for the page Poppler gives a Letter
    /// default because it has no /MediaBox at all. Both are what the fallback has to turn back.
    QCOMPARE(backend.pageInfo(3).sizePt, QSizeF(842, 595));
    QCOMPARE(backend.pageInfo(4).sizePt, QSizeF(792, 612));

    QHash<int, QImage> ink;
    for (int i = 0; i < backend.pageCount(); ++i) {
        const QSizeF sizePt = backend.pageInfo(i).sizePt;
        ink.insert(i, inkWithRedMark(QSize(qRound(sizePt.width()), qRound(sizePt.height()))));
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("exported.pdf"));

    QString why;
    QVERIFY2(PdfExporter::exportWithInk(fixture, manifestFor(backend), ink, out, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(out));
    QCOMPARE(exported.pageCount(), 5);

    /// Every page, whatever its box shape, has to show the mark where it was drawn: a 50x50 square
    /// at (10,10) of the page as displayed.
    for (int i = 0; i < 5; ++i) {
        const QImage rendered = exported.renderPage(i, 72.0);
        QVERIFY(!rendered.isNull());
        const QRect mark = redMarkBounds(rendered);
        const QString where = QStringLiteral("page %1 (%2x%3 as displayed): the mark came back at "
                                             "(%4,%5) measuring %6x%7, expected 50x50 at (10,10)")
                                  .arg(i + 1)
                                  .arg(rendered.width()).arg(rendered.height())
                                  .arg(mark.left()).arg(mark.top())
                                  .arg(mark.width()).arg(mark.height());
        QVERIFY2(mark.isValid(), qPrintable(QStringLiteral("page %1: the mark is not on the page")
                                                    .arg(i + 1)));
        QVERIFY2(qAbs(mark.width() - 50) <= 3 && qAbs(mark.height() - 50) <= 3, qPrintable(where));
        QVERIFY2(qAbs(mark.left() - 10) <= 3 && qAbs(mark.top() - 10) <= 3, qPrintable(where));
    }

    /// The page's own content survives every one of those paths, including the two that fall back.
    const QStringList headings = {
        QStringLiteral("Direct media box"),
        QStringLiteral("Indirect media box"),
        QStringLiteral("Rotated direct box"),
        QStringLiteral("Rotated indirect box"),
        QStringLiteral("Missing media box"),
    };
    for (int i = 0; i < headings.size(); ++i) {
        QVERIFY2(exported.pageText(i).contains(headings.at(i)),
                 qPrintable(QStringLiteral("page %1: expected \"%2\", got \"%3\"")
                                .arg(i + 1).arg(headings.at(i)).arg(exported.pageText(i))));
    }
}

/**
 * A notebook page the user turned a quarter turn exports with the paper AND the ink turned.
 *
 * PDF /Rotate can name 90 degrees, so the source page's own box is untouched and /Rotate becomes
 * the source's own rotation plus the notebook's. Before this fix extraRotation was not read at all:
 * /Rotate stayed 0, the paper came out upright at 595x842, and the display-space ink was squeezed
 * into the source box -- a 50x50 mark at (10,10) came back 35x71 at (7,14). The mark and the paper
 * are measured separately here so "both turned" is a statement about pixels and not about intent.
 */
void PdfExporterTest::testNotebookQuarterTurnTurnsPaperAndInk()
{
    const QString source = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(source));
    QCOMPARE(backend.pageInfo(0).rotation, 0);
    QCOMPARE(backend.pageInfo(0).sizePt, QSizeF(595, 842));

    PdfSessionManifest manifest = manifestFor(backend);
    manifest.pages[0].extraRotation = 90;
    const QSizeF display = manifest.pages[0].displaySizePt();
    QCOMPARE(display, QSizeF(842, 595));

    const QImage sourceRender = backend.renderPage(0, 72.0);
    QVERIFY(!sourceRender.isNull());

    /// The ink the user drew on the turned page, in display space: a red square at (10,10).
    QHash<int, QImage> ink;
    ink.insert(0, inkWithRedMark(QSize(qRound(display.width()), qRound(display.height()))));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("quarter-turn.pdf"));
    const QString paperOnly = dir.filePath(QStringLiteral("quarter-turn-paper.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), paperOnly, &why),
             qPrintable(why));

    /// The paper, on an export with no ink: the page measures what displaySizePt() says, /Rotate
    /// carries the whole quarter turn, and the page's own content came out turned with it.
    PopplerRenderBackend clean;
    QVERIFY(clean.open(paperOnly));
    QCOMPARE(clean.pageInfo(0).sizePt, display);
    QCOMPARE(clean.pageInfo(0).rotation, 90);
    const QImage cleanPage = clean.renderPage(0, 72.0);
    QVERIFY(!cleanPage.isNull());
    QCOMPARE(cleanPage.size(), sourceRender.size().transposed());

    const QImage expectedPaper = turnedForDisplay(sourceRender, 90);
    const QRect paperWhere = darkBounds(cleanPage);
    const QRect paperExpected = darkBounds(expectedPaper);
    QVERIFY2(rectsClose(paperWhere, paperExpected, 4),
             qPrintable(QStringLiteral("the paper came back at (%1,%2)-(%3,%4), where the turned "
                                       "page puts it at (%5,%6)-(%7,%8)")
                            .arg(paperWhere.left()).arg(paperWhere.top())
                            .arg(paperWhere.right()).arg(paperWhere.bottom())
                            .arg(paperExpected.left()).arg(paperExpected.top())
                            .arg(paperExpected.right()).arg(paperExpected.bottom())));

    /// The ink, on the export with it: where it was drawn in the turned page's own space, at its
    /// drawn size.
    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    QCOMPARE(exported.pageInfo(0).sizePt, display);
    const QImage rendered = exported.renderPage(0, 72.0);
    QVERIFY(!rendered.isNull());
    const QRect mark = redMarkBounds(rendered);
    QVERIFY2(mark.isValid(), "the mark is not on the exported page");
    const QString where = QStringLiteral("the mark came back at (%1,%2) measuring %3x%4, expected "
                                         "50x50 at (10,10)")
                              .arg(mark.left()).arg(mark.top()).arg(mark.width()).arg(mark.height());
    QVERIFY2(qAbs(mark.left() - 10) <= 3 && qAbs(mark.top() - 10) <= 3
                 && qAbs(mark.width() - 50) <= 3 && qAbs(mark.height() - 50) <= 3,
             qPrintable(where));

    /// The page's own text is still there, and still selectable.
    QVERIFY2(exported.pageText(0).contains(QStringLiteral("Rotation zero")),
             qPrintable(exported.pageText(0)));
}

/**
 * A notebook page turned half way round exports with the paper and the ink both upside down.
 *
 * The page used here already declares /Rotate 90, so the notebook's half turn has to be ADDED to
 * it: the new page object must be written with /Rotate 270, replacing the source's own value.
 * Writing only a missing /Rotate -- what an insert would do -- would leave the source's 90 in place
 * and export the page a quarter turn out. A half turn keeps the page's displayed size, so only the
 * paper's own content can say which way up it is: the ink at (10,10) comes back at (10,10) either
 * way, because a raster turned 180 and then looked at 180 degrees later looks the same.
 */
void PdfExporterTest::testNotebookHalfTurnTurnsPaperAndInk()
{
    const QString source = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(source));
    QCOMPARE(backend.pageInfo(1).rotation, 90);
    QCOMPARE(backend.pageInfo(1).sizePt, QSizeF(842, 595));

    PdfSessionManifest manifest = manifestFor(backend);
    manifest.pages[1].extraRotation = 180;
    const QSizeF display = manifest.pages[1].displaySizePt();
    QCOMPARE(display, QSizeF(842, 595));

    const QImage sourceRender = backend.renderPage(1, 72.0);
    QVERIFY(!sourceRender.isNull());

    QHash<int, QImage> ink;
    ink.insert(1, inkWithRedMark(QSize(qRound(display.width()), qRound(display.height()))));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("half-turn.pdf"));
    const QString paperOnly = dir.filePath(QStringLiteral("half-turn-paper.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), paperOnly, &why),
             qPrintable(why));

    PopplerRenderBackend clean;
    QVERIFY(clean.open(paperOnly));
    QCOMPARE(clean.pageInfo(1).sizePt, display);
    QCOMPARE(clean.pageInfo(1).rotation, 270);
    const QImage cleanPage = clean.renderPage(1, 72.0);
    QVERIFY(!cleanPage.isNull());
    QCOMPARE(cleanPage.size(), sourceRender.size());

    const QImage expectedPaper = turnedForDisplay(sourceRender, 180);
    const QRect paperWhere = darkBounds(cleanPage);
    const QRect paperExpected = darkBounds(expectedPaper);
    QVERIFY2(rectsClose(paperWhere, paperExpected, 4),
             qPrintable(QStringLiteral("the paper came back at (%1,%2)-(%3,%4), where the turned "
                                       "page puts it at (%5,%6)-(%7,%8)")
                            .arg(paperWhere.left()).arg(paperWhere.top())
                            .arg(paperWhere.right()).arg(paperWhere.bottom())
                            .arg(paperExpected.left()).arg(paperExpected.top())
                            .arg(paperExpected.right()).arg(paperExpected.bottom())));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    QCOMPARE(exported.pageInfo(1).sizePt, display);
    QCOMPARE(exported.pageInfo(1).rotation, 270);
    const QImage rendered = exported.renderPage(1, 72.0);
    QVERIFY(!rendered.isNull());
    const QRect mark = redMarkBounds(rendered);
    QVERIFY2(mark.isValid(), "the mark is not on the exported page");
    const QString where = QStringLiteral("the mark came back at (%1,%2) measuring %3x%4, expected "
                                         "50x50 at (10,10)")
                              .arg(mark.left()).arg(mark.top()).arg(mark.width()).arg(mark.height());
    QVERIFY2(qAbs(mark.left() - 10) <= 3 && qAbs(mark.top() - 10) <= 3
                 && qAbs(mark.width() - 50) <= 3 && qAbs(mark.height() - 50) <= 3,
             qPrintable(where));

    QVERIFY2(exported.pageText(1).contains(QStringLiteral("Rotation ninety")),
             qPrintable(exported.pageText(1)));
}

/**
 * A notebook page set down at a free angle -- 37 degrees -- exports with the paper and the ink
 * turned by that angle, and its box is the bounding box displaySizePt() reports.
 *
 * /Rotate cannot name 37 degrees, so the turn is baked into the page: the page's own content is
 * drawn under a rotation matrix and the MediaBox becomes the rectangle the turned sheet fits in,
 * while the source's own /Rotate 90 stays in charge of the quarter turn the file already declared.
 * A page set down at an angle is bigger than its sizePt; that is the accepted cost.
 */
void PdfExporterTest::testNotebookFreeAngleTurnsPaperAndInk()
{
    const QString source = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(source));
    QCOMPARE(backend.pageInfo(1).rotation, 90);
    QCOMPARE(backend.pageInfo(1).sizePt, QSizeF(842, 595));

    PdfSessionManifest manifest = manifestFor(backend);
    manifest.pages[1].extraRotation = 37;
    const QSizeF display = manifest.pages[1].displaySizePt();
    const qreal radians = qDegreesToRadians(37.0);
    QCOMPARE(display.width(), 842 * qAbs(qCos(radians)) + 595 * qAbs(qSin(radians)));
    QCOMPARE(display.height(), 842 * qAbs(qSin(radians)) + 595 * qAbs(qCos(radians)));
    QVERIFY2(display.width() > 842 && display.height() > 595,
             "a sheet set down at an angle does not fit in its unturned box");

    const QImage sourceRender = backend.renderPage(1, 72.0);
    QVERIFY(!sourceRender.isNull());
    QVERIFY(qAbs(sourceRender.width() - 842) <= 1 && qAbs(sourceRender.height() - 595) <= 1);

    QHash<int, QImage> ink;
    ink.insert(1, inkWithRedMark(QSize(qRound(display.width()), qRound(display.height()))));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("free-angle.pdf"));
    const QString paperOnly = dir.filePath(QStringLiteral("free-angle-paper.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), paperOnly, &why),
             qPrintable(why));

    /// The box is the bounding box of the turned sheet, and the source's own quarter turn is still
    /// what /Rotate says.
    PopplerRenderBackend clean;
    QVERIFY(clean.open(paperOnly));
    const QSizeF box = clean.pageInfo(1).sizePt;
    QVERIFY2(qAbs(box.width() - display.width()) <= 0.05
                 && qAbs(box.height() - display.height()) <= 0.05,
             qPrintable(QStringLiteral("the page box is %1x%2, where displaySizePt() says %3x%4")
                            .arg(box.width()).arg(box.height())
                            .arg(display.width()).arg(display.height())));
    QCOMPARE(clean.pageInfo(1).rotation, 90);

    const QImage cleanPage = clean.renderPage(1, 72.0);
    QVERIFY(!cleanPage.isNull());
    QVERIFY2(qAbs(cleanPage.width() - qRound(display.width())) <= 2
                 && qAbs(cleanPage.height() - qRound(display.height())) <= 2,
             qPrintable(QStringLiteral("the rendered page is %1x%2, expected about %3x%4")
                            .arg(cleanPage.width()).arg(cleanPage.height())
                            .arg(qRound(display.width())).arg(qRound(display.height()))));

    /// The paper is turned by 37 degrees, not merely a bigger box: its own content came back where
    /// the notebook's own render puts it.
    const QImage expectedPaper = turnedForDisplay(sourceRender, 37);
    const QRect paperWhere = darkBounds(cleanPage);
    const QRect paperExpected = darkBounds(expectedPaper);
    QVERIFY2(rectsClose(paperWhere, paperExpected, 8),
             qPrintable(QStringLiteral("the paper came back at (%1,%2)-(%3,%4), where the turned "
                                       "page puts it at (%5,%6)-(%7,%8)")
                            .arg(paperWhere.left()).arg(paperWhere.top())
                            .arg(paperWhere.right()).arg(paperWhere.bottom())
                            .arg(paperExpected.left()).arg(paperExpected.top())
                            .arg(paperExpected.right()).arg(paperExpected.bottom())));

    /// And the ink is where it was drawn in the turned page's space.
    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    const QImage rendered = exported.renderPage(1, 72.0);
    QVERIFY(!rendered.isNull());
    const QRect mark = redMarkBounds(rendered);
    QVERIFY2(mark.isValid(), "the mark is not on the exported page");
    const QString where = QStringLiteral("the mark came back at (%1,%2) measuring %3x%4, expected "
                                         "50x50 at (10,10)")
                              .arg(mark.left()).arg(mark.top()).arg(mark.width()).arg(mark.height());
    QVERIFY2(qAbs(mark.left() - 10) <= 4 && qAbs(mark.top() - 10) <= 4
                 && qAbs(mark.width() - 50) <= 4 && qAbs(mark.height() - 50) <= 4,
             qPrintable(where));

    /// The page's own text survived the bake. A free-angle turn reorders what an extractor sees --
    /// the runs are no longer on one horizontal baseline -- so this checks that the text is still
    /// extractable rather than that it spells the source's line back in the same order.
    QVERIFY2(!exported.pageText(1).trimmed().isEmpty(),
             "the baked page's own text was not extractable");
    /// And the page is not blank: the paper's content came through and not only the ink.
    QVERIFY2(darkPixels(cleanPage) > 0, "the baked page came out blank");
}


/**
 * A page the notebook scaled exports at the scaled size, with its content scaled to match.
 *
 * The MediaBox and the content transform have to AGREE: a box twice the size with the content left
 * at 1x is a small page in the corner of a big sheet, and a content transform twice the size inside
 * the old box is a page two thirds off the paper. The mark the user drew is what tells the two
 * apart -- it is drawn in the page's own points, so it stays 50x50 at (10,10) whatever the factor,
 * and a squeeze would halve it.
 */
void PdfExporterTest::testAScaledPageExportsItsScaledBoxAndTransform()
{
    const QString source = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(source));
    QCOMPARE(backend.pageInfo(0).rotation, 0);
    QCOMPARE(backend.pageInfo(0).sizePt, QSizeF(595, 842));

    PdfSessionManifest manifest = manifestFor(backend);
    manifest.pages[0].extraScale = 2.0;
    const QSizeF display = manifest.pages[0].displaySizePt();
    QCOMPARE(display, QSizeF(1190, 1684));

    /// The ink as the page holds it after a resize: the page in its own frame. A resize never
    /// re-renders the artifact, so the plane is what it was and the page is what got bigger.
    QHash<int, QImage> ink;
    ink.insert(0, inkWithRedMark(QSize(qRound(display.width()), qRound(display.height()))));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("scaled.pdf"));
    const QString paperOnly = dir.filePath(QStringLiteral("scaled-paper.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), paperOnly, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    QCOMPARE(exported.pageInfo(0).sizePt, display);
    const QImage rendered = exported.renderPage(0, 72.0);
    QVERIFY(!rendered.isNull());
    QVERIFY2(qAbs(rendered.width() - 1190) <= 2 && qAbs(rendered.height() - 1684) <= 2,
             qPrintable(QStringLiteral("the exported page rendered %1x%2, expected 1190x1684")
                            .arg(rendered.width()).arg(rendered.height())));

    const QRect mark = redMarkBounds(rendered);
    QVERIFY2(mark.isValid(), "the mark is not on the exported page");
    QVERIFY2(qAbs(mark.left() - 10) <= 4 && qAbs(mark.top() - 10) <= 4
                 && qAbs(mark.width() - 50) <= 4 && qAbs(mark.height() - 50) <= 4,
             qPrintable(QStringLiteral("the mark came back at (%1,%2) measuring %3x%4, expected "
                                       "50x50 at (10,10) in the scaled page")
                            .arg(mark.left()).arg(mark.top()).arg(mark.width()).arg(mark.height())));

    /// And the page's own content is scaled with the box, not left in its corner: at 72 dpi the
    /// exported paper has to measure the same as the SOURCE rendered at 144 dpi, which is the same
    /// page at twice the pixels.
    PopplerRenderBackend clean;
    QVERIFY(clean.open(paperOnly));
    const QImage paper = clean.renderPage(0, 72.0);
    QVERIFY(!paper.isNull());
    const QImage twiceTheDpi = backend.renderPage(0, 144.0);
    QVERIFY(!twiceTheDpi.isNull());
    QVERIFY2(qAbs(paper.width() - twiceTheDpi.width()) <= 2
                 && qAbs(paper.height() - twiceTheDpi.height()) <= 2,
             qPrintable(QStringLiteral("the exported paper is %1x%2 where the source at 144 dpi is "
                                       "%3x%4")
                            .arg(paper.width()).arg(paper.height())
                            .arg(twiceTheDpi.width()).arg(twiceTheDpi.height())));
    const QRect paperWhere = darkBounds(paper);
    const QRect expectedWhere = darkBounds(twiceTheDpi);
    QVERIFY2(rectsClose(paperWhere, expectedWhere, 8),
             qPrintable(QStringLiteral("the scaled paper came back at (%1,%2)-(%3,%4), where the "
                                       "source at 144 dpi puts it at (%5,%6)-(%7,%8)")
                            .arg(paperWhere.left()).arg(paperWhere.top())
                            .arg(paperWhere.right()).arg(paperWhere.bottom())
                            .arg(expectedWhere.left()).arg(expectedWhere.top())
                            .arg(expectedWhere.right()).arg(expectedWhere.bottom())));

    /// The page's own text is still there, still selectable, and not turned into a picture.
    QVERIFY2(exported.pageText(0).contains(QStringLiteral("Rotation zero")),
             qPrintable(exported.pageText(0)));
}

/**
 * A page the notebook cropped exports as the box and nothing else.
 *
 * "Nothing else" is the whole result: the page's own content is under a transform that puts the
 * box's corner at the origin and a /MediaBox that is the box's own size, so what was outside the
 * box falls outside the page and is clipped by the reader. Measured against the source's own
 * top-left corner -- the same pixels, not the whole page squeezed into a smaller rectangle, which
 * would put every mark somewhere else on the paper.
 */
void PdfExporterTest::testACroppedPageExportsOnlyItsBox()
{
    const QString source = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend backend;
    QVERIFY(backend.open(source));
    QCOMPARE(backend.pageInfo(0).rotation, 0);
    QCOMPARE(backend.pageInfo(0).sizePt, QSizeF(595, 842));

    PdfSessionManifest manifest = manifestFor(backend);

    const QImage sourceRender = backend.renderPage(0, 72.0);
    QVERIFY(!sourceRender.isNull());
    QVERIFY(qAbs(sourceRender.width() - 595) <= 1 && qAbs(sourceRender.height() - 842) <= 1);

    /// The box is put WHERE THE PAGE'S OWN CONTENT IS, measured off the source rather than assumed:
    /// a crop of a blank corner would come out blank and prove nothing about clipping. It is in the
    /// reader's frame, whose origin is the page's top left, so the raster's pixels are read directly.
    const QRect content = darkBounds(sourceRender);
    QVERIFY2(content.isValid(), "the source page has no content to crop around");
    const QRectF box = QRectF(content.adjusted(-4, -4, 4, 4));
    manifest.pages[0].boxPt = box;
    QCOMPARE(manifest.pages[0].displaySizePt(), box.size());

    /// The ink is the page's own frame -- the crop has already clipped the artifact in the notebook
    /// (that check is in PdfSessionTest), so the plane here is the box, mark included.
    QHash<int, QImage> ink;
    ink.insert(0, inkWithRedMark(box.size().toSize()));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString withInk = dir.filePath(QStringLiteral("cropped.pdf"));
    const QString paperOnly = dir.filePath(QStringLiteral("cropped-paper.pdf"));
    QString why;
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, ink, withInk, &why), qPrintable(why));
    QVERIFY2(PdfExporter::exportWithInk(source, manifest, QHash<int, QImage>(), paperOnly, &why),
             qPrintable(why));

    PopplerRenderBackend exported;
    QVERIFY(exported.open(withInk));
    QCOMPARE(exported.pageInfo(0).sizePt, box.size());
    const QImage rendered = exported.renderPage(0, 72.0);
    QVERIFY(!rendered.isNull());
    QVERIFY2(qAbs(rendered.width() - qRound(box.width())) <= 2
                 && qAbs(rendered.height() - qRound(box.height())) <= 2,
             qPrintable(QStringLiteral("the cropped page rendered %1x%2, expected %3x%4")
                            .arg(rendered.width()).arg(rendered.height())
                            .arg(qRound(box.width())).arg(qRound(box.height()))));

    /// The mark that was inside the box is still there, at its own size -- the page was cut, not
    /// scaled.
    const QRect mark = redMarkBounds(rendered);
    QVERIFY2(mark.isValid(), "the mark inside the crop is not on the exported page");
    QVERIFY2(qAbs(mark.left() - 10) <= 4 && qAbs(mark.top() - 10) <= 4
                 && qAbs(mark.width() - 50) <= 4 && qAbs(mark.height() - 50) <= 4,
             qPrintable(QStringLiteral("the mark came back at (%1,%2) measuring %3x%4")
                            .arg(mark.left()).arg(mark.top()).arg(mark.width()).arg(mark.height())));

    /// And the page's own content is the source's top-left corner, pixel for pixel, not the whole
    /// sheet squeezed into 300x300.
    PopplerRenderBackend clean;
    QVERIFY(clean.open(paperOnly));
    const QImage paper = clean.renderPage(0, 72.0);
    QVERIFY(!paper.isNull());
    const QImage expected = sourceRender.copy(box.toRect());
    QVERIFY2(darkPixels(paper) > 0, "the cropped page came out blank");
    const QRect paperWhere = darkBounds(paper);
    const QRect expectedWhere = darkBounds(expected);
    QVERIFY2(rectsClose(paperWhere, expectedWhere, 6),
             qPrintable(QStringLiteral("the cropped paper came back at (%1,%2)-(%3,%4), where the "
                                       "source's own top-left corner puts it at (%5,%6)-(%7,%8)")
                            .arg(paperWhere.left()).arg(paperWhere.top())
                            .arg(paperWhere.right()).arg(paperWhere.bottom())
                            .arg(expectedWhere.left()).arg(expectedWhere.top())
                            .arg(expectedWhere.right()).arg(expectedWhere.bottom())));
}


/// A notebook whose pages come from two PDFs assembles into ONE file, in notebook order.
///
/// Page one is the fixture that carries nothing of its own (the manual's shape), page two comes
/// from text-fixture.pdf. The assembled file has to hold both, each rendering as the source page
/// its record names, and the plan has to say what it inherited -- that flag is the difference
/// between a page with paper and a page with nothing.
void PdfExporterTest::testAssemblingTwoSourcesWritesOneFileInNotebookOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("inherited.pdf"));
    const QString second = dir.filePath(QStringLiteral("second.pdf"));
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("inherited-attrs.pdf")), first));
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("text-fixture.pdf")), second));

    PdfSessionManifest manifest;
    manifest.name = QStringLiteral("Two sources");
    PdfSourceRecord recordA;
    recordA.file = QStringLiteral("inherited.pdf");
    recordA.sha256 = PdfSessionManifest::sha256OfFile(first);
    recordA.byteSize = QFileInfo(first).size();
    PdfSourceRecord recordB;
    recordB.file = QStringLiteral("second.pdf");
    recordB.sha256 = PdfSessionManifest::sha256OfFile(second);
    recordB.byteSize = QFileInfo(second).size();
    manifest.sourceFile = recordA.file;
    manifest.sourceSha256 = recordA.sha256;
    manifest.sourceByteSize = recordA.byteSize;
    manifest.sources << recordA << recordB;

    /// Notebook order: the inherited fixture first, then text-fixture's first page. The notebook
    /// page NUMBER (kraFile) is what an artifact is called; the export walks pages, not sources.
    manifest.pages.append(PdfPageRecord{ 0, QSizeF(595, 842), 0,
                                         PdfSession::pageFileName(0), QString(), 0, 0 });
    manifest.pages.append(PdfPageRecord{ 0, QSizeF(595, 842), 0,
                                         PdfSession::pageFileName(1), QString(), 0, 1 });

    const QString out = dir.filePath(QStringLiteral("assembled.pdf"));
    QString why;
    QList<PdfAssembler::PagePlan> written;
    QVERIFY2(PdfAssembler::assemble(dir.path(), manifest, out, &written, &why), qPrintable(why));
    QCOMPARE(written.size(), 2);

    QVERIFY2(written.at(0).inheritsResources,
             "the fixture's page carries no /Resources of its own; the plan has to know");
    QVERIFY2(written.at(0).inheritsMediaBox,
             "the fixture's page carries no /MediaBox of its own; the plan has to know");
    QCOMPARE(written.at(0).sizePt, QSizeF(595, 842));

    PopplerRenderBackend assembled;
    QVERIFY2(assembled.open(out), qPrintable(why));
    QCOMPARE(assembled.pageCount(), 2);
    QCOMPARE(assembled.pageInfo(0).sizePt, QSizeF(595, 842));
    QVERIFY2(assembled.pageText(0).contains(QStringLiteral("Inherited page one")),
             qPrintable(assembled.pageText(0)));
    QVERIFY2(assembled.pageText(1).contains(QStringLiteral("Page one heading")),
             qPrintable(assembled.pageText(1)));
}

/// THE SHAPE THE USER'S OWN FILE HAS, and the reason this module exists.
///
/// Every page of the real manual has no /Resources and no /MediaBox of its own: /Pages carries
/// them. A copier that reads a page dictionary standalone writes a page with no paper size and no
/// font, which renders as blank paper -- and a page whose PAPER is missing renders "successfully"
/// in every naive check. So the assertions are the rendered size AND pixels: the text has to be
/// there, drawn with the font the /Pages node carried.
void PdfExporterTest::testAnAssembledPageKeepsItsInheritedPaperAndResources()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("inherited.pdf"));
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("inherited-attrs.pdf")), source));

    /// The source on its own, to compare against: the same page, rendered by the same backend.
    PopplerRenderBackend original;
    QVERIFY(original.open(source));
    const QImage expected = original.renderPage(0, 72.0);
    QVERIFY2(!expected.isNull(), "the fixture itself did not render");
    QVERIFY2(darkPixels(expected) > 0, "the fixture itself has no text to preserve");

    PdfSessionManifest manifest;
    manifest.name = QStringLiteral("Inherited");
    PdfSourceRecord record;
    record.file = QStringLiteral("inherited.pdf");
    record.sha256 = PdfSessionManifest::sha256OfFile(source);
    record.byteSize = QFileInfo(source).size();
    manifest.sourceFile = record.file;
    manifest.sourceSha256 = record.sha256;
    manifest.sourceByteSize = record.byteSize;
    manifest.sources << record;
    manifest.pages.append(PdfPageRecord{ 0, QSizeF(595, 842), 0,
                                         PdfSession::pageFileName(0), QString(), 0, 0 });

    const QString out = dir.filePath(QStringLiteral("assembled.pdf"));
    QString why;
    QVERIFY2(PdfAssembler::assemble(dir.path(), manifest, out, nullptr, &why), qPrintable(why));

    PopplerRenderBackend assembled;
    QVERIFY2(assembled.open(out), qPrintable(why));
    QCOMPARE(assembled.pageCount(), 1);

    /// The paper came from the ancestor...
    QCOMPARE(assembled.pageInfo(0).sizePt, QSizeF(595, 842));
    const QImage rendered = assembled.renderPage(0, 72.0);
    QVERIFY(!rendered.isNull());
    /// ...and so did the font: without the inherited /Resources this page is blank, and that is
    /// exactly the failure this test exists to catch.
    QVERIFY2(darkPixels(rendered) > 0,
             "the assembled page came out blank: the inherited /Resources did not travel with it");
    QVERIFY2(rendered.size() == expected.size(),
             qPrintable(QStringLiteral("the assembled page is %1x%2, the source's is %3x%4")
                            .arg(rendered.width()).arg(rendered.height())
                            .arg(expected.width()).arg(expected.height())));
}

/// A source the assembler cannot read is NAMED, and nothing is written.
///
/// ex-objstm-50p.pdf is PDF 1.5 with a cross-reference stream -- the modern shape the user's own
/// files do not have and this reader deliberately does not guess at. The per-source export stays
/// the fallback for it; a half-read source written out as a file that only looks right is the one
/// outcome worse than a refusal, so the refusal also has to leave no file behind.
void PdfExporterTest::testTheAssemblerNamesASourceItCannotRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString modern = dir.filePath(QStringLiteral("modern.pdf"));
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("ex-objstm-50p.pdf")), modern));

    PdfSessionManifest manifest;
    manifest.name = QStringLiteral("Modern");
    PdfSourceRecord record;
    record.file = QStringLiteral("modern.pdf");
    record.sha256 = PdfSessionManifest::sha256OfFile(modern);
    record.byteSize = QFileInfo(modern).size();
    manifest.sourceFile = record.file;
    manifest.sourceSha256 = record.sha256;
    manifest.sourceByteSize = record.byteSize;
    manifest.sources << record;
    manifest.pages.append(PdfPageRecord{ 0, QSizeF(595, 842), 0,
                                         PdfSession::pageFileName(0), QString(), 0, 0 });

    const QString out = dir.filePath(QStringLiteral("assembled.pdf"));
    QString why;
    QVERIFY2(!PdfAssembler::assemble(dir.path(), manifest, out, nullptr, &why),
             "a source with a cross-reference stream was assembled as if it had been read");
    QVERIFY2(why.contains(QStringLiteral("modern.pdf")),
             qPrintable(QStringLiteral("the refusal does not name the source: %1").arg(why)));
    QVERIFY2(!QFileInfo::exists(out), "a refusal left a half-written file behind");
}

QTEST_MAIN(PdfExporterTest)
#include "PdfExporterTest.moc"
