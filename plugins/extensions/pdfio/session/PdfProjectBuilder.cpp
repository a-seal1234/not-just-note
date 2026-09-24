/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfProjectBuilder.h"

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
    const long pages = sysconf(_SC_AVPHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || pageSize <= 0) {
        return 0;
    }
    return qint64(pages) * qint64(pageSize);
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

    const qint64 maxPixels = maxPagePixels();
    const qreal wantedPixels =
        (page.sizePt.width() * dpi / 72.0) * (page.sizePt.height() * dpi / 72.0);
    if (wantedPixels > maxPixels) {
        const qreal requestedDpi = dpi;
        dpi *= std::sqrt(qreal(maxPixels) / wantedPixels);

        /// Said out loud. A page rendered below the resolution that was asked for is a page whose
        /// notes are drawn on a coarser grid than the user expects, and they should be able to
        /// find out why rather than wonder whether the notebook is blurry.
        qWarning() << "[pdfio] page" << (page.index + 1) << "wants" << qint64(wantedPixels)
                   << "pixels but this device allows" << maxPixels
                   << "; rendering at" << dpi << "dpi instead of" << requestedDpi;
    }

    const QImage rendered = backend.renderPage(page.index, dpi);
    if (rendered.isNull()) {
        fail(why, QStringLiteral("the renderer produced nothing for page %1").arg(page.index + 1));
        return KisImageSP();
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return KisImageSP();
    }

    /// The image is measured in pixels of the render, so nothing downstream has to redo the
    /// point-to-pixel conversion that the renderer already made.
    KisImageSP image = new KisImage(0, rendered.width(), rendered.height(), colorSpace,
                                    QStringLiteral("PDF page %1").arg(page.index + 1));
    image->setResolution(dpi, dpi);

    KisPaintLayerSP background = new KisPaintLayer(image, backgroundLayerName(), OPACITY_OPAQUE_U8);
    background->paintDevice()->convertFromQImage(rendered, 0, 0, 0);

    /// The page artwork is not ours to edit; only the Ink group is written by the session.
    background->setUserLocked(true);

    KisGroupLayerSP ink = new KisGroupLayer(image, inkLayerName(), OPACITY_OPAQUE_U8, colorSpace);

    /// Added in order: the background first, so the Ink group ends up above it.
    image->addNode(background, image->root());
    image->addNode(ink, image->root());

    /// The Ink group needs a paint layer of its own. A group is not paintable, so without this
    /// the user selects Ink, draws, and nothing happens at all.
    KisPaintLayerSP stroke = new KisPaintLayer(image, inkStrokeLayerName(), OPACITY_OPAQUE_U8);
    image->addNode(stroke, ink);

    return image;
}
