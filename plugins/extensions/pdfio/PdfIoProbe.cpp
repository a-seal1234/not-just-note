/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoProbe.h"

#include <cstdarg>
#include <cstdio>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>

#include "backend/PdfRenderBackend.h"
#include "session/PdfExporter.h"
#include "session/PdfInkLoader.h"
#include "session/PdfPageSaver.h"
#include "session/PdfProjectBuilder.h"
#include "session/PdfSession.h"

#include <KisDocument.h>
#include <KisPart.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_layer.h>

#include <KoColorSpaceConstants.h>

namespace {

void note(const char *format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    /// Both sinks on purpose. On desktop Krita's message handler swallows plugin output during
    /// startup, where stderr still shows; on Android stderr goes nowhere and the handler is what
    /// reaches logcat.
    fprintf(stderr, "[probe] %s\n", buffer);
    fflush(stderr);
    qWarning("[probe] %s", buffer);
}

/// The same three pages the desktop tests use: plain A4, A5 rotated ninety degrees, and a page
/// whose MediaBox does not start at the origin. Written here rather than shipped so the device
/// needs no fixture and no storage permission.
QByteArray buildFixture()
{
    struct Page {
        int x0, y0, x1, y1, rotate, tx, ty;
    };
    const Page pages[] = {
        { 0, 0, 595, 842, 0, 72, 760 },
        { 0, 0, 595, 420, 90, 72, 360 },
        { 50, 30, 350, 330, 0, 80, 280 },
    };
    const int count = 3;

    QList<QByteArray> objects;
    QByteArray kids;
    for (int i = 0; i < count; ++i) {
        kids += QByteArray::number(3 + 2 * i) + " 0 R ";
    }
    objects.append("<< /Type /Catalog /Pages 2 0 R >>");
    objects.append("<< /Type /Pages /Kids [" + kids.trimmed() + "] /Count " + QByteArray::number(count) + " >>");

    for (int i = 0; i < count; ++i) {
        const Page &p = pages[i];
        QByteArray content = "BT /F1 24 Tf " + QByteArray::number(p.tx) + " "
                             + QByteArray::number(p.ty) + " Td (Page " + QByteArray::number(i + 1)
                             + " heading) Tj ET\n";
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [" + QByteArray::number(p.x0) + " "
                       + QByteArray::number(p.y0) + " " + QByteArray::number(p.x1) + " "
                       + QByteArray::number(p.y1) + "]"
                       + (p.rotate ? " /Rotate " + QByteArray::number(p.rotate) : QByteArray())
                       + " /Contents " + QByteArray::number(4 + 2 * i) + " 0 R"
                       + " /Resources << /Font << /F1 9 0 R >> >> >>");
        objects.append("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n"
                       + content + "endstream");
    }
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    QByteArray out = "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n";
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(out.size());
        out += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const int xrefAt = out.size();
    out += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (int offset : offsets) {
        out += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1)
           + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xrefAt) + "\n%%EOF\n";
    return out;
}

} // namespace

namespace PdfIoProbe {

void runIfRequested()
{
    const QString requested = qEnvironmentVariable("PDFIO_PROBE");

    QString fixture = requested;
    if (fixture.isEmpty() || !QFileInfo::exists(fixture)) {
        /// No fixture given, or not readable (the application's private storage on Android is not
        /// reachable from a desktop path): write the built in one instead.
        fixture = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                      .filePath(QStringLiteral("pdfio-fixture.pdf"));
        QFile file(fixture);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            note("cannot write %s", qPrintable(fixture));
            return;
        }
        file.write(buildFixture());
        file.close();
    }

    note("fixture %s (%lld bytes)", qPrintable(fixture), qint64(QFileInfo(fixture).size()));

    QScopedPointer<PdfRenderBackend> backend(PdfRenderBackend::create());
    if (!backend || !backend->open(fixture)) {
        note("no usable render backend for this platform");
        return;
    }

    note("backend: %d pages", backend->pageCount());
    for (int i = 0; i < backend->pageCount(); ++i) {
        const PdfPageInfo info = backend->pageInfo(i);
        const QImage rendered = backend->renderPage(i, 200.0);
        note("page %d sizePt %.0fx%.0f rotate %d render %dx%d",
             i + 1, info.sizePt.width(), info.sizePt.height(), info.rotation,
             rendered.width(), rendered.height());
    }

    const QString projectDir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QStringLiteral("pdfio-probe-project"));
    QDir(projectDir).removeRecursively();

    QString why;
    const PdfSessionManifest manifest = PdfSession::createProject(projectDir, fixture, *backend, &why);
    if (!manifest.isValid(&why)) {
        note("session failed: %s", qPrintable(why));
        return;
    }
    note("session: pages %d sha %s", manifest.pages.size(),
         qPrintable(QString::fromLatin1(manifest.sourceSha256.left(12))));

    const PdfSessionManifest reopened = PdfSession::openProject(projectDir, &why);
    note("reopen: %s", reopened.isValid() ? "ok" : qPrintable(why));

    /// The rotated page, the case that used to be interesting.
    KisImageSP image = PdfProjectBuilder::buildPageImage(manifest.pages.at(1), *backend, 200.0, &why);
    if (!image) {
        note("builder failed: %s", qPrintable(why));
        return;
    }
    note("image %dx%d at %.0f dpi | layers %d | bg \"%s\" locked %d | ink \"%s\" children %d",
         image->width(), image->height(), image->xRes(), int(image->root()->childCount()),
         qPrintable(image->root()->at(0)->name()), int(image->root()->at(0)->userLocked()),
         qPrintable(image->root()->at(1)->name()), int(image->root()->at(1)->childCount()));

    /// Something recognisable to draw, so the export can be checked by rendering it: a bar a
    /// hundred pixels in from the top left of the page as it is displayed, at 200 dpi.
    if (KisPaintLayer *stroke = qobject_cast<KisPaintLayer *>(PdfProjectBuilder::inkStrokeLayer(image).data())) {
        stroke->paintDevice()->fill(QRect(100, 100, 200, 40),
                                    KoColor(Qt::black, image->colorSpace()));
    }

    /// The file size bound: a document holding the page's own layers, never the page's render.
    KisDocument *pageDocument = PdfPageSaver::createPageLayersDocument(image, &why);
    if (!pageDocument) {
        note("page layers document failed: %s", qPrintable(why));
        return;
    }

    const QString path = QDir(projectDir).filePath(PdfSession::pageFileName(manifest.pages.at(1).index));
    QDir().mkpath(QFileInfo(path).absolutePath());

    QEventLoop loop;
    bool finished = false;
    QObject::connect(pageDocument, &KisDocument::sigSavingFinished, &loop,
                     [&loop, &finished](const QString &) { finished = true; loop.quit(); });
    QTimer::singleShot(60000, &loop, [&loop]() { loop.quit(); });

    if (!PdfPageSaver::saveDocument(pageDocument, path, &why)) {
        note("save failed: %s", qPrintable(why));
        return;
    }
    loop.exec();

    QFile saved(path);
    QByteArray raw;
    if (saved.open(QIODevice::ReadOnly)) {
        raw = saved.readAll();
    }
    note("page layers saved: finished %d, %lld bytes, mergedimage %d",
         int(finished), qint64(raw.size()), int(raw.contains("mergedimage.png")));

    KisPart::instance()->removeDocument(pageDocument, true);

    /// The whole circle, and the only check that matters for an export: the ink that was just
    /// written is read back out of the artifact, composited over the source, and the result is
    /// rendered to see where it landed.
    const QImage loaded = PdfInkLoader::loadInk(path, &why);
    note("ink loader: %dx%d from the artifact", loaded.width(), loaded.height());

    QHash<int, QImage> ink;
    if (!loaded.isNull()) {
        ink.insert(manifest.pages.at(1).index, loaded);
    }

    const QString exportedPath = QDir(projectDir).filePath(QStringLiteral("exported.pdf"));
    note("export: %d (%s)",
         int(PdfExporter::exportWithInk(fixture, manifest, ink, exportedPath, &why)), qPrintable(why));

    QScopedPointer<PdfRenderBackend> checker(PdfRenderBackend::create());
    if (checker && checker->open(exportedPath)) {
        const QImage rendered = checker->renderPage(manifest.pages.at(1).index, 72.0);

        int x0 = rendered.width();
        int y0 = rendered.height();
        int x1 = -1;
        int y1 = -1;
        for (int y = 0; y < rendered.height(); ++y) {
            for (int x = 0; x < rendered.width(); ++x) {
                const QRgb pixel = rendered.pixel(x, y);
                if (qAlpha(pixel) > 0 && qGray(pixel) < 200) {
                    x0 = qMin(x0, x);
                    y0 = qMin(y0, y);
                    x1 = qMax(x1, x);
                    y1 = qMax(y1, y);
                }
            }
        }

        /// The mark was drawn a hundred pixels in at 200 dpi, which is thirty six points, so it
        /// has to come back around thirty six pixels in at 72 dpi.
        note("exported page render %dx%d, dark bounds %d,%d-%d,%d (expected about 36,36)",
             rendered.width(), rendered.height(), x0, y0, x1, y1);
    }
}

} // namespace PdfIoProbe
