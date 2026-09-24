/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfStripBuilder.h"

#include "backend/PdfRenderBackend.h"
#include "session/PdfInkLoader.h"

#include <QColor>
#include <QDebug>
#include <QDir>

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

QString PdfStripBuilder::inkGroupName(int page)
{
    return QStringLiteral("Ink %1").arg(page + 1);
}

QString PdfStripBuilder::backgroundLayerName(int page)
{
    return QStringLiteral("PDF page %1").arg(page + 1);
}

bool PdfStripBuilder::isPageRenderLayerName(const QString &name)
{
    /// backgroundLayerName(page) is "PDF page %1", and the desk is the room around the pages. Both
    /// are the strip's own, and both are written by the builders with those names.
    return name == QStringLiteral("Desk")
        || name.startsWith(QStringLiteral("PDF page "));
}

QString PdfStripBuilder::inkLayerName(int page)
{
    /// Named after its page. Every page having a layer called "Layer 1" is legal -- they live in
    /// different groups -- and it made every log line and every layer panel entry ambiguous.
    return QStringLiteral("Ink strokes %1").arg(page + 1);
}

PdfStripBuilder::Strip PdfStripBuilder::build(const PdfSessionManifest &manifest,
                                              int activePage,
                                              int scope,
                                              qreal dpi,
                                              PdfRenderBackend &backend,
                                              const QString &projectDir,
                                              QString *why)
{
    Strip strip;

    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest, activePage, scope, dpi);
    if (!layout.isValid()) {
        fail(why, QStringLiteral("the strip has no valid layout"));
        return strip;
    }
    if (!backend.isOpen()) {
        fail(why, QStringLiteral("the renderer is not open"));
        return strip;
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return strip;
    }

    /// The image is the layout's size and stays that size, which is what lets the window roll
    /// without rebuilding anything.
    strip.image = new KisImage(0,
                               layout.imageSize().width(),
                               layout.imageSize().height(),
                               colorSpace,
                               QStringLiteral("notebook"));
    strip.image->setResolution(dpi, dpi);
    strip.layout = layout;

    /// A base under everything, so the strip reads as pages on a desk. Left transparent, the room
    /// around a page smaller than the largest one shows Krita's transparency checkerboard, which
    /// reads as a mistake rather than as room -- which is exactly how it was reported.
    KisPaintLayerSP desk = new KisPaintLayer(strip.image, QStringLiteral("Desk"), OPACITY_OPAQUE_U8);
    desk->paintDevice()->fill(QRect(QPoint(0, 0), layout.imageSize()),
                              KoColor(QColor(96, 96, 96), colorSpace));
    desk->setUserLocked(true);
    strip.image->addNode(desk, strip.image->root());

    const QDir project(projectDir);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    /// Paper first, all of it, and only then the ink, all of it. Adding a slot at a time puts the
    /// next page's paper above this page's ink, so a stroke that strays outside its own page
    /// disappears behind the page below it -- which is how "the active page did not change" was
    /// reported: the stroke was there, and hidden.
    for (const PdfStripLayout::Slot &slot : slots) {
        if (slot.page < 0) {
            continue;
        }

        const QImage rendered = backend.renderPage(slot.page, dpi);
        KisPaintLayerSP background =
            new KisPaintLayer(strip.image, backgroundLayerName(slot.page), OPACITY_OPAQUE_U8);
        if (!rendered.isNull()) {
            /// Said out loud when it happens: the slot was sized from the page's own geometry, and
            /// if the renderer disagrees the page is drawn in the wrong place. Deriving a raster
            /// size instead of asking the renderer is a mistake this project has already made.
            if (rendered.size() != slot.rect.size()) {
                qWarning() << "[pdfio] page" << (slot.page + 1) << "rendered at" << rendered.size()
                           << "but the layout made room for" << slot.rect.size();
            }
            background->paintDevice()->convertFromQImage(rendered, nullptr,
                                                         slot.rect.x(), slot.rect.y());
        }
        background->setUserLocked(true);
        strip.image->addNode(background, strip.image->root());
        strip.paperLayers.append(background);
    }

    /// One ink layer for the whole strip, above all of the paper.
    ///
    /// It was one group per page, so that a page could be locked and its neighbour not. That needed
    /// Krita to be told which layer was active every time a page was turned, and it turned out that
    /// activating a node from a plugin does not take in the running application -- every stroke
    /// went on landing on the first page whatever was done. A single layer cannot go wrong that
    /// way: whatever is drawn lands in it, and the page it belongs to is decided when the page is
    /// saved, by cropping the region that page occupies.
    KisPaintLayerSP ink = new KisPaintLayer(strip.image, QStringLiteral("Ink"), OPACITY_OPAQUE_U8);

    /// Every page's saved ink goes back into that one layer, at its own place in the strip.
    for (const PdfStripLayout::Slot &slot : slots) {
        if (slot.page < 0) {
            continue;
        }

        const QImage savedInk =
            PdfInkLoader::loadInk(project.filePath(manifest.pages.at(slot.page).kraFile), nullptr);
        if (!savedInk.isNull()) {
            ink->paintDevice()->convertFromQImage(savedInk, nullptr, slot.rect.x(), slot.rect.y());
        }
    }

    strip.image->addNode(ink, strip.image->root());
    strip.activeInkLayer = ink;

    if (!strip.activeInkLayer) {
        fail(why, QStringLiteral("the strip has no paintable slot"));
    }

    return strip;
}
