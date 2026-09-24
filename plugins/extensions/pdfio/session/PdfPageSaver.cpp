/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageSaver.h"

#include "session/PdfProjectBuilder.h"
#include "session/PdfStripBuilder.h"

#include <KisDocument.h>
#include <KisPart.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/**
 * Copies the page's own layers into \a target, keeping groups as groups.
 *
 * Layer name, opacity and pixels are carried over; masks, effects and blend modes are not, which
 * is a deliberate first cut: the ink a note is made of is paint, and anything Krita-specific that
 * cannot be reproduced is better rejected loudly later than flattened silently now.
 *
 * Groups ARE carried, and recursively. The flat copy that used to live here asked its caller for
 * every child of a group and skipped anything that was not a paint layer, so a group nested inside
 * the page's layers lost everything in it without a word -- the same silence that dropped a layer
 * the user made outside the Ink group.
 */
/// \a target by value, not by const reference: KisSharedPtr hands back a const KisImage through a
/// const smart pointer, and addNode() is not a const method.
void copyPageLayers(KisImageSP target, const QList<KisNodeSP> &layers, const QRect &area,
                    KisNodeSP parent)
{
    /// By value, not by const reference: qobject_cast refuses to cast away constness, and a shared
    /// pointer copy costs nothing.
    for (KisNodeSP child : layers) {
        if (PdfPageSaver::isPageBackground(child)) {
            continue;
        }

        if (KisGroupLayer *group = qobject_cast<KisGroupLayer *>(child.data())) {
            /// Descended into, not copied as a group. A group layer written into the artifact
            /// document made its projection come out empty -- every pixel of the merged image was
            /// transparent, including the mark a sibling paint layer carried, measured on the
            /// desktop. Nothing needs the group there either: the artifact is read back through
            /// PdfInkLoader, which takes mergedimage.png, so what the file has to carry is the
            /// pixels and their order in the stack. The leaves go to the same parent, which keeps
            /// the order the groups gave them.
            QList<KisNodeSP> children;
            for (quint32 i = 0; i < group->childCount(); ++i) {
                children.append(group->at(i));
            }
            copyPageLayers(target, children, area, parent);
            continue;
        }

        KisPaintLayer *paint = qobject_cast<KisPaintLayer *>(child.data());
        if (!paint) {
            continue;
        }

        KisPaintLayerSP copy = new KisPaintLayer(target, paint->name(), paint->opacity());
        copy->paintDevice()->makeCloneFrom(paint->paintDevice(), paint->paintDevice()->extent());

        /// Shifted so the region's top left becomes the origin, which is what makes an artifact of
        /// a page inside a strip identical to one of a page on its own.
        copy->setX(-area.x());
        copy->setY(-area.y());

        target->addNode(copy, parent);
    }
}

} // namespace

bool PdfPageSaver::isPageBackground(const KisNodeSP &node)
{
    if (!node) {
        return false;
    }

    const QString name = node->name();
    return PdfProjectBuilder::isPageRenderLayerName(name)
        || PdfStripBuilder::isPageRenderLayerName(name);
}

QList<KisNodeSP> PdfPageSaver::pageLayers(const KisImageSP &source)
{
    QList<KisNodeSP> layers;
    if (!source || !source->root()) {
        return layers;
    }

    for (quint32 i = 0; i < source->root()->childCount(); ++i) {
        KisNodeSP child = source->root()->at(i);
        if (!isPageBackground(child)) {
            layers.append(child);
        }
    }

    return layers;
}

KisDocument *PdfPageSaver::createPageLayersDocument(const KisImageSP &source, QString *why)
{
    if (!source) {
        fail(why, QStringLiteral("no page image to save"));
        return nullptr;
    }

    return createPageLayersDocument(source, QRect(0, 0, source->width(), source->height()), why);
}

KisDocument *PdfPageSaver::createPageLayersDocument(const KisImageSP &source,
                                                    const QRect &area,
                                                    QString *why)
{
    if (!source) {
        fail(why, QStringLiteral("no page image to save"));
        return nullptr;
    }

    if (area.isEmpty()) {
        fail(why, QStringLiteral("the page has no area to save"));
        return nullptr;
    }

    const QList<KisNodeSP> layers = pageLayers(source);
    if (layers.isEmpty()) {
        fail(why, QStringLiteral("the page holds nothing but its background"));
        return nullptr;
    }

    KisDocument *document = KisPart::instance()->createDocument();

    /// The artifact is the size of the page, not of whatever document the page happened to live in.
    KisImageSP page = new KisImage(document->createUndoStore(),
                                   area.width(), area.height(), source->colorSpace(),
                                   QStringLiteral("page"));
    page->setResolution(source->xRes(), source->yRes());

    copyPageLayers(page, layers, area, page->rootLayer());
    document->setCurrentImage(page, false);

    /// The merged image the save writes comes from the image's projection, and a document built and
    /// saved in the same breath has none yet: Krita computes it in an update job, and without this
    /// the artifact's mergedimage.png comes out transparent at every pixel -- measured on the
    /// desktop, a page whose extra layer carried a mark came back rgba(0,0,0,0) everywhere, and a
    /// page artifact that reads back as nothing is ink the notebook has silently lost.
    /// PdfStripCursorTest found the same thing for the reload path and documents the pair.
    page->refreshGraphAsync(page->root(), { page->bounds() }, page->bounds());
    page->waitForDone();

    return document;
}

bool PdfPageSaver::saveDocument(KisDocument *document, const QString &path, QString *why)
{
    if (!document) {
        fail(why, QStringLiteral("no document to save"));
        return false;
    }

    if (!document->saveAs(path, QByteArrayLiteral("application/x-krita"), false)) {
        fail(why, QStringLiteral("could not start saving %1").arg(path));
        return false;
    }

    return true;
}
