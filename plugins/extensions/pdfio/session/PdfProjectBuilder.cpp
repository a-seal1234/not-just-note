/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfProjectBuilder.h"

#include "session/PdfSourceRenderers.h"

#include <cmath>
#include <unistd.h>

#include <QDebug>

#if defined(Q_OS_ANDROID)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniObject>
#else
#include <QAndroidJniObject>
#endif
#endif

#include "backend/PdfRenderBackend.h"

#include <QImage>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

#include <KoColorSpaceConstants.h>
#include <KoColorSpaceRegistry.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

QString PdfProjectBuilder::backgroundLayerName()
{
    return QStringLiteral("PDF page");
}

bool PdfProjectBuilder::isPageRenderLayerName(const QString &name)
{
    return name == backgroundLayerName();
}

QString PdfProjectBuilder::inkLayerName()
{
    return QStringLiteral("Ink");
}

QString PdfProjectBuilder::inkStrokeLayerName()
{
    return QStringLiteral("Layer 1");
}

KisNodeSP PdfProjectBuilder::inkStrokeLayer(const KisImageSP &image)
{
    if (!image || !image->root() || image->root()->childCount() < 2) {
        return KisNodeSP();
    }

    KisNodeSP group = image->root()->at(1);
    if (!group || group->childCount() == 0) {
        return KisNodeSP();
    }

    return group->at(0);
}

namespace {

/// Measured, not assumed: a 3.87 megapixel page costs about 41 MB resident, which is 11.1 bytes
/// per pixel across the bitmap the renderer fills, the QImage it is read into and the layer it is
/// converted into.
constexpr qreal BytesPerPagePixel = 11.1;

/// The share of what the device reports it has that a single page may take. The quantity is real;
/// this fraction is a policy, and a conservative one.
constexpr qreal PageMemoryShare = 0.05;

/// On Android a bitmap comes from the Java heap, capped separately from memory in general and
/// usually much smaller. This is the share of that heap one page may take, at four bytes a pixel.
constexpr qreal JavaHeapShare = 0.25;

/// When nothing can be read at all, and never below this, so a device that reports nonsense does
/// not turn every page into a thumbnail.
constexpr qint64 FallbackMaxPagePixels = 8 * 1000 * 1000;
constexpr qint64 MinMaxPagePixels = 1 * 1000 * 1000;

qint64 availableMemoryBytes()
{
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0) {
        return 0;
    }
    return qint64(pages) * qint64(pageSize);
#else
    /// Use the existing conservative page-pixel cap on platforms without POSIX sysconf values.
    return 0;
#endif
}

#if defined(Q_OS_ANDROID)
/// The Java heap a bitmap has to fit in, in bytes, or 0 when the framework will not say.
qint64 javaHeapBytes()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    using JniObject = QJniObject;
#else
    using JniObject = QAndroidJniObject;
#endif

    JniObject activity = JniObject::callStaticObjectMethod("org/qtproject/qt5/android/QtNative",
                                                           "activity",
                                                           "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        return 0;
    }

    JniObject service = activity.callObjectMethod(
        "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
        JniObject::fromString(QStringLiteral("activity")).object<jstring>());
    if (!service.isValid()) {
        return 0;
    }

    const jint megabytes = service.callMethod<jint>("getLargeMemoryClass", "()I");
    return megabytes > 0 ? qint64(megabytes) * 1024 * 1024 : 0;
}
#endif

/// The resolution a page may really be built at, after the device's own page budget.
///
/// One place for both kinds of page: a blank page costs exactly as much memory as a rendered one of
/// the same size, so it answers to the same cap, and the log line names which page was clamped.
/// \a displaySizePt is the size the reader sees, turn and scale included -- charging the budget for
/// the unturned rectangle would let an angled page past the cap by the whole area the angle adds.
qreal clampedDpiForPage(const PdfPageRecord &page, qreal dpi, const QSizeF &displaySizePt)
{
    const qint64 maxPixels = PdfProjectBuilder::maxPagePixels();
    const qreal wantedPixels = (displaySizePt.width() * dpi / 72.0) * (displaySizePt.height() * dpi / 72.0);
    if (wantedPixels <= maxPixels) {
        return dpi;
    }

    /// Said out loud. A page rendered below the resolution that was asked for is a page whose notes
    /// are drawn on a coarser grid than the user expects, and they should be able to find out why
    /// rather than wonder whether the notebook is blurry.
    const qreal requested = dpi;
    const qreal clamped = dpi * std::sqrt(qreal(maxPixels) / wantedPixels);
    qWarning() << "[pdfio] page" << (page.isBlank() ? QStringLiteral("(blank)") : QString::number(page.index + 1))
               << "wants" << qint64(wantedPixels) << "pixels but this device allows" << maxPixels
               << "; rendering at" << clamped << "dpi instead of" << requested;
    return clamped;
}

/// The layer stack every page is, once its raster exists: the paper locked underneath, the Ink group
/// above it with the paint layer the user draws into.
///
/// \a raster is the page as the reader sees it -- scale, box and turn already applied -- and \a dpi
/// is the resolution it was built at, which is what the image records and what the size check below
/// measures against. The check is deliberately shared: a render and a blank sheet reach it the same
/// way, so "this page measures what its record says" is asserted for both kinds of page.
KisImageSP pageImageFromRaster(const PdfPageRecord &page, const QImage &raster, qreal dpi,
                               const QSizeF &displaySizePt, QString *why)
{
    if (raster.isNull()) {
        fail(why, QStringLiteral("the page produced no raster"));
        return KisImageSP();
    }

    /// The image is the box the budget above was measured on, so the two are compared rather than
    /// assumed equal. A renderer's own transform rounds its result outwards, so a pixel or two more
    /// than displaySizePt() is the rounding, not a disagreement; a page that came back any further
    /// off would mean the turned sheet and the box it is measured against had drifted apart, which
    /// is worth saying out loud.
    const QSize boxPixels(qRound(displaySizePt.width() * dpi / 72.0),
                          qRound(displaySizePt.height() * dpi / 72.0));
    if (qAbs(raster.width() - boxPixels.width()) > 2 || qAbs(raster.height() - boxPixels.height()) > 2) {
        qWarning() << "[pdfio] page"
                   << (page.isBlank() ? QStringLiteral("(blank)") : QString::number(page.index + 1))
                   << "turned by" << page.extraRotation << "degrees measures" << raster.size()
                   << "where displaySizePt() puts it at" << boxPixels;
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return KisImageSP();
    }

    /// The image is measured in pixels of the raster, so nothing downstream has to redo the
    /// point-to-pixel conversion that the raster already made.
    KisImageSP image = new KisImage(0, raster.width(), raster.height(), colorSpace,
                                    QStringLiteral("PDF page %1").arg(page.index + 1));
    image->setResolution(dpi, dpi);

    KisPaintLayerSP background = new KisPaintLayer(image, PdfProjectBuilder::backgroundLayerName(),
                                                   OPACITY_OPAQUE_U8);
    background->paintDevice()->convertFromQImage(raster, 0, 0, 0);

    /// The page artwork is not ours to edit; only the Ink group is written by the session.
    background->setUserLocked(true);

    KisGroupLayerSP ink = new KisGroupLayer(image, PdfProjectBuilder::inkLayerName(), OPACITY_OPAQUE_U8,
                                            colorSpace);

    /// Added in order: the background first, so the Ink group ends up above it.
    image->addNode(background, image->root());
    image->addNode(ink, image->root());

    /// The Ink group needs a paint layer of its own. A group is not paintable, so without this
    /// the user selects Ink, draws, and nothing happens at all.
    KisPaintLayerSP stroke = new KisPaintLayer(image, PdfProjectBuilder::inkStrokeLayerName(),
                                               OPACITY_OPAQUE_U8);
    image->addNode(stroke, ink);

    return image;
}

} // namespace

qint64 PdfProjectBuilder::maxPagePixels()
{
    qint64 budget = 0;

    const qint64 freeBytes = availableMemoryBytes();
    if (freeBytes > 0) {
        budget = qint64(freeBytes * PageMemoryShare / BytesPerPagePixel);
    }

#if defined(Q_OS_ANDROID)
    const qint64 heapBytes = javaHeapBytes();
    if (heapBytes > 0) {
        const qint64 fromHeap = qint64(heapBytes * JavaHeapShare / 4.0);
        budget = budget > 0 ? qMin(budget, fromHeap) : fromHeap;
    }
#endif

    if (budget <= 0) {
        return FallbackMaxPagePixels;
    }
    return qMax(MinMaxPagePixels, budget);
}

KisImageSP PdfProjectBuilder::buildPageImage(const PdfPageRecord &page,
                                             PdfRenderBackend &backend,
                                             qreal dpi,
                                             QString *why)
{
    if (!page.sizePt.isValid()) {
        fail(why, QStringLiteral("page %1 has no usable geometry").arg(page.index + 1));
        return KisImageSP();
    }

    const QSizeF displaySizePt = page.displaySizePt();
    dpi = clampedDpiForPage(page, dpi, displaySizePt);

    /// displaySizePt() includes extraScale, so the source PDF must be rendered at the corresponding
    /// higher source DPI. Using the effective canvas DPI directly here leaves an opened scaled page
    /// at its old pixel dimensions, with gray canvas showing where the enlarged page should be.
    const qreal extraScale = page.extraScale > 0.0 ? page.extraScale : 1.0;
    const qreal sourceDpi = dpi * extraScale;
    const QImage rendered = backend.renderPage(page.index, sourceDpi);
    if (rendered.isNull()) {
        fail(why, QStringLiteral("the renderer produced nothing for page %1").arg(page.index + 1));
        return KisImageSP();
    }

    /// And the page's own BOX, before the turn, exactly as the strip applies it: what the box does
    /// not cover is dropped and a margin comes back as paper white. Without this a cropped page
    /// opened as the whole sheet -- the image bigger than the size its record declares, and the
    /// paper under the ink the crop had already cut away. Found by
    /// PdfNavigatorIntegrationTest::testTheRealClipperCropsThePageAndKeepsItsInk.
    const QImage boxed = page.boxPt.isValid()
        ? PdfSourceRenderers::croppedToBox(rendered, page.boxPt, sourceDpi)
        : rendered;

    /// And turned by the notebook's own turn, if it has one: the renderer hands back the source's
    /// orientation, and a page the user has turned -- by a right angle or by any other -- comes back
    /// turned. The ink is turned with it -- the artifact was rotated when the page was -- so a
    /// stroke stays on its line.
    return pageImageFromRaster(page, PdfSourceRenderers::turnedForDisplay(boxed, page.extraRotation),
                               dpi, displaySizePt, why);
}

KisImageSP PdfProjectBuilder::buildBlankPageImage(const PdfPageRecord &page, qreal dpi, QString *why)
{
    if (!page.sizePt.isValid()) {
        fail(why, QStringLiteral("the blank page has no usable geometry"));
        return KisImageSP();
    }
    if (!page.isBlank()) {
        /// Said rather than done: a caller that asks for a blank page and hands over a page with a
        /// source has the two halves of the notebook disagreeing, and rendering a white sheet over a
        /// PDF page would hide the paper the user expects under a blank one.
        fail(why, QStringLiteral("page %1 has a source, so it is not a blank page").arg(page.index + 1));
        return KisImageSP();
    }

    const QSizeF displaySizePt = page.displaySizePt();
    dpi = clampedDpiForPage(page, dpi, displaySizePt);

    /// White paper of the page's own sheet: the same call the strip, the thumbnail and the exporter
    /// make for a page with no source, so the page opens at exactly the size the others measure.
    return pageImageFromRaster(page, PdfSourceRenderers::blankPage(page, dpi), dpi, displaySizePt, why);
}
