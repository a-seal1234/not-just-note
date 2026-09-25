/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageSaver.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

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
            /// Not paint. The case that matters is a file layer -- File > Insert Image... -- and
            /// skipping it silently is why an inserted image never reached the page's .kra nor the
            /// exported PDF. Its projection is rasterised into a plain paint layer with the same
            /// name, opacity and place, so the artifact stays self-contained and does not depend on
            /// the inserted file still being where it was.
            ///
            /// Nothing to read means nothing to write: a file layer whose source has gone is
            /// skipped with a line in the log rather than failing the whole save.
            KisPaintDeviceSP projection = child->projection();
            if (!projection || projection->exactBounds().isEmpty()) {
                qWarning() << "pdfio: skipping the layer" << child->name()
                           << "-- nothing can be read from it (an image layer whose source is gone)";
                continue;
            }

            KisPaintLayerSP copy = new KisPaintLayer(target, child->name(), child->opacity());
            copy->paintDevice()->makeCloneFrom(projection, area);
            copy->setX(-area.x());
            copy->setY(-area.y());
            target->addNode(copy, parent);
            continue;
        }

        /// Only the page's own rectangle is cloned. Cloning the whole device and shifting it
        /// leaves every other page's ink inside the artifact's layer data -- clipped by the
        /// artifact's bounds when it is rendered, and read back as a layer 6438 pixels tall for a
        /// page 1189 tall. Measured with PDFIO_PROBE_RESTORE=1 on the desktop:
        /// "ink bounds 100,100 841x6438 (expected 100,100 200x40)".
        KisPaintLayerSP copy = new KisPaintLayer(target, paint->name(), paint->opacity());
        copy->paintDevice()->makeCloneFrom(paint->paintDevice(), area);

        /// Shifted so the page's top left becomes the origin: the clone keeps the coordinates it
        /// was taken from, and without this the page's pixels sit where the page sits inside the
        /// strip -- outside a page-sized artifact. Measured by the roll test: "the mark did not
        /// reach the artifact of the page that stayed".
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

    /// The source's own graph is refreshed first: a non-paint layer's pixels live only in its
    /// projection -- a file layer has no paint device of its own -- and a graph that has not been
    /// computed reads back empty, so the copy would skip the very layer this exists for.
    KisImageSP sourceImage = source;
    sourceImage->refreshGraphAsync(sourceImage->root(), { area }, area);
    sourceImage->waitForDone();

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

    /// The same layers, also written as plain PNGs beside the artifact, with a one-line-per-layer
    /// index.
    ///
    /// Reading a page's layers back out of the artifact means opening a document, and on the tablet
    /// five of those per window move end in SIGSEGV -- logcat ends at "strip: reading the layers of
    /// page 4" and a tombstone follows two seconds later. A PNG per layer costs one QImage to read
    /// and nothing else, which is what the layer path is meant to read from now on. The artifact
    /// keeps its layers either way; this is the cheap copy, not a replacement.
    const KisImageSP image = document->image();
    if (image && image->root() && !image->root()->childCount() == 0) {
        const QString index = path + QStringLiteral(".layers.txt");
        QFile list(index);
        if (list.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&list);
            out << "# index\tname\tfile\topacity\tx\ty\n";
            const QRect bounds = image->bounds();
            int count = 0;
            for (quint32 i = 0; i < image->root()->childCount(); ++i) {
                KisPaintLayer *layer =
                    qobject_cast<KisPaintLayer *>(image->root()->at(i).data());
                if (!layer || !layer->visible()) {
                    continue;
                }

                const QImage pixels = layer->paintDevice()->convertToQImage(nullptr, bounds);
                const QString file = QStringLiteral("%1.layers/%2.png").arg(path).arg(count);
                QDir().mkpath(QFileInfo(file).absolutePath());
                if (pixels.isNull() || !pixels.save(file, "PNG")) {
                    continue;
                }

                out << count << '\t' << layer->name() << '\t'
                    << QFileInfo(file).fileName() << '\t'
                    << layer->opacity() << '\t' << layer->x() << '\t' << layer->y() << '\n';
                ++count;
            }
        }
    }

    return true;
}
