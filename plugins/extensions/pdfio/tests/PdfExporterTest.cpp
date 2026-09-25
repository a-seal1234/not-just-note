/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfExporter.h"
#include "session/PdfSession.h"

#include <QElapsedTimer>
#include <QFile>
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

    /// The guard that makes a notebook whose pages are no longer the PDF's own order refuse to
    /// export, rather than write a file whose ink is on the wrong pages.
    void testAMovedPageIsRefusedRatherThanExportedWrongly();

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
 * A notebook whose pages are not the PDF's own pages, in their own order, is refused.
 *
 * The export finds the page to overlay by the notebook's LIST POSITION: the ink is keyed by
 * position and the page object written is the one at the same index. So a moved page would have its
 * ink attached to a different sheet -- silently, in a file that opens perfectly, which is exactly
 * the failure mode this project keeps having to design against. The guard refuses with the reason
 * until the writer can rebuild the page tree in notebook order (stage G of the Notebook ops plan).
 *
 * It lives in this function rather than in the menu because this is what writes the file: a menu
 * check a future caller forgets cannot then produce a wrong PDF.
 */
void PdfExporterTest::testAMovedPageIsRefusedRatherThanExportedWrongly()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath(QStringLiteral("text-fixture.pdf"))));
    QCOMPARE(backend.pageCount(), 3);

    QHash<int, QImage> ink;
    ink.insert(0, inkWithMark(QSize(595, 842)));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = fixturePath(QStringLiteral("text-fixture.pdf"));

    /// The notebook as it was made: the source's own pages in their own order, and it exports.
    QString why;
    const QString good = dir.filePath(QStringLiteral("good.pdf"));
    QVERIFY2(PdfExporter::exportWithInk(source, manifestFor(backend), ink, good, &why), qPrintable(why));

    /// The same notebook with page 1 moved to the end: the ink that was on PDF page 1 would be
    /// written onto PDF page 3, and PDF page 1 would keep the original art with no ink at all.
    PdfSessionManifest moved = manifestFor(backend);
    moved.pages.move(0, 2);

    const QString wrong = dir.filePath(QStringLiteral("wrong.pdf"));
    why.clear();
    QVERIFY2(!PdfExporter::exportWithInk(source, moved, ink, wrong, &why),
             "a notebook whose pages are not the PDF's own order was exported anyway");
    QVERIFY2(why.contains(QStringLiteral("wrong page")), qPrintable(why));
    /// [P2, P3, P1] after the move, so notebook page 1 now holds PDF page 2: the ink that was on
    /// PDF page 1 would be written onto PDF page 2, and the message names both.
    QVERIFY2(why.contains(QStringLiteral("notebook page 1")) && why.contains(QStringLiteral("page 2 of the PDF")),
             qPrintable(why));

    /// Nothing was written: a refusal that left a file behind would be worse than useless, and the
    /// destination is the thing a user would hand on.
    QVERIFY(!QFileInfo::exists(wrong));
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

QTEST_MAIN(PdfExporterTest)
#include "PdfExporterTest.moc"
