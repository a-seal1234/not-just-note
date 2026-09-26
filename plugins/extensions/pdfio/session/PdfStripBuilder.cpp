/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfStripBuilder.h"

#include "backend/PdfRenderBackend.h"
#include "session/PdfInkLoader.h"
#include "session/PdfSourceRenderers.h"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QHash>

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
                                              PdfSourceRenderers &renderers,
                                              const QString &projectDir,
                                              QString *why)
{
    Strip strip;

    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest, activePage, scope, dpi);
    if (!layout.isValid()) {
        fail(why, QStringLiteral("the strip has no valid layout"));
        return strip;
    }
    /// The active page's source, opened once. A slot whose own source cannot be opened renders
    /// nothing (below) rather than taking the whole strip down with it; the page the user is on is
    /// the one that has to be there.
    if (!renderers.forPage(manifest, projectDir, activePage, why)) {
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

        const QImage rendered = renderers.renderPage(manifest, projectDir, slot.page, dpi, nullptr);
        KisPaintLayerSP background =
            new KisPaintLayer(strip.image, backgroundLayerName(manifest.pages.at(slot.page).index),
                              OPACITY_OPAQUE_U8);
        if (!rendered.isNull()) {
            /// Said out loud when it happens: the slot was sized from the page's own geometry, and
            /// if the renderer disagrees the page is drawn in the wrong place. Deriving a raster
            /// size instead of asking the renderer is a mistake this project has already made.
            ///
            /// A page set down at an angle is the one case where the two are allowed to differ, and
            /// by a pixel: the renderer turns the page's own PIXELS and rounds the box out of them
            /// (PdfSourceRenderers::turnedForDisplay, through PdfPageRecord::turnedSize), while the
            /// layout turns the page's size in POINTS and only then converts to pixels -- two
            /// roundings of one box, not two boxes. Measured over the fixture geometries, angles and
            /// dpis the gap is never more than a pixel, and a right angle is exact. Two is the
            /// tolerance: wide enough not to cry wolf over half a pixel either way, narrow enough
            /// that a slot filled with the wrong page -- whose size differs by far more -- still
            /// speaks up.
            if (qAbs(rendered.width() - slot.rect.width()) > 2
                || qAbs(rendered.height() - slot.rect.height()) > 2) {
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
    /// The layer now sits inside a group called Ink, which is the shape a single page has and the
    /// one that was asked for: everything the notebook draws lives in the Ink group, and a layer
    /// that ends up outside it is moved in rather than left floating beside the notebook.
    ///
    /// The layer keeps the name "Ink" as well, so every search for the paint layer called Ink still
    /// finds it -- what changed is its parent, not its name.
    KisGroupLayerSP inkGroup = new KisGroupLayer(strip.image, QStringLiteral("Ink"),
                                                 OPACITY_OPAQUE_U8, strip.image->colorSpace());
    KisPaintLayerSP ink = new KisPaintLayer(strip.image, QStringLiteral("Ink"), OPACITY_OPAQUE_U8);

    /// Every page's saved layers go back in, each into a layer of its own: the artifact's stroke
    /// entry (the one named like this layer) into the strip-wide stroke layer that keeps being what
    /// a stroke lands in, and every other name the artifacts carry into one strip-wide layer of
    /// that name, made the first time the name is seen and put inside the same Ink group.
    ///
    /// Reading each page's MERGED image instead is what merged the inserted picture into Ink: the
    /// merged image is the flatten of every layer the page holds, so pouring it into the one layer
    /// the strip had turned a picture that was its own layer into strokes. Measured on the desktop:
    /// after an insert and a notebook reopened, the Ink group came back with one child where there
    /// had been two, and "Ink" carried the picture's pixels.
    ///
    /// The order the names are first seen in decides the stacking, and each page's part is written
    /// at that page's own rectangle: an artifact's pixels are page-local, so the slot's top left is
    /// where they belong. A page whose artifact cannot be read at all changes nothing here.
    QList<QPair<QString, KisPaintLayerSP>> restored;
    QHash<QString, int> restoredByName;
    for (const PdfStripLayout::Slot &slot : slots) {
        if (slot.page < 0) {
            continue;
        }

        const QString kraPath = project.filePath(manifest.pages.at(slot.page).kraFile);
        const QList<QPair<QString, QImage>> saved =
            PdfInkLoader::loadInkLayersFromSidecar(kraPath, nullptr);

        if (saved.isEmpty()) {
            /// No sidecar: an artifact written before the layer PNGs existed, whose only readable
            /// form is the merged image. Its layers cannot be told apart any more, so the pixels go
            /// into the stroke layer -- which is what this always did.
            const QImage flattened = PdfInkLoader::loadInk(kraPath, nullptr);
            if (!flattened.isNull()) {
                ink->paintDevice()->convertFromQImage(flattened, nullptr,
                                                      slot.rect.x(), slot.rect.y());
            }
            continue;
        }

        for (const QPair<QString, QImage> &entry : saved) {
            if (entry.second.isNull()) {
                continue;
            }

            KisPaintLayerSP layer = ink;
            if (entry.first != ink->name()) {
                const int known = restoredByName.value(entry.first, -1);
                if (known >= 0) {
                    layer = restored.at(known).second;
                } else {
                    layer = new KisPaintLayer(strip.image, entry.first, OPACITY_OPAQUE_U8);
                    restoredByName.insert(entry.first, int(restored.size()));
                    restored.append(QPair<QString, KisPaintLayerSP>(entry.first, layer));
                }
            }

            layer->paintDevice()->convertFromQImage(entry.second, nullptr,
                                                    slot.rect.x(), slot.rect.y());
            qWarning() << "[pdfio] strip: page" << (slot.page + 1) << "restored layer"
                       << entry.first << entry.second.size() << "at" << slot.rect.topLeft();
        }
    }

    /// The group goes into the image and the stroke layer inside it, and the restored content
    /// layers after it. The two lines that used to be here made the group and then added the layer
    /// to the ROOT: the group never entered the graph, and the strip was still a bare layer beside
    /// the notebook -- the shape the user saw in the Layer panel, and the one commit 476ec00147
    /// meant to replace but did not.
    strip.image->addNode(inkGroup, strip.image->root());
    strip.image->addNode(ink, inkGroup);
    for (const QPair<QString, KisPaintLayerSP> &extra : restored) {
        strip.image->addNode(extra.second, inkGroup);
    }
    strip.activeInkLayer = ink;

    if (!strip.activeInkLayer) {
        fail(why, QStringLiteral("the strip has no paintable slot"));
    }

    return strip;
}
