/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfInkLoader.h"

#include <QDir>
#include <QScopedPointer>
#include <QTextStream>

#include <QDebug>
#include <QFileInfo>

/// <KArchive> first: the KF5 headers for the individual classes lean on it for KArchive itself
/// and do not include it, which fails on the Android build where the umbrella include is the only
/// thing that brings the type in.
#include <KArchive>
#include <KArchiveDirectory>
#include <KArchiveEntry>
#include <KArchiveFile>
#include <KZip>

#include <KisDocument.h>
#include <KisImportExportErrorCode.h>
#include <KisPart.h>
#include <kra_converter.h>

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

/// The merged image of an artifact whose document holds only the page's own layers *is* the page's
/// content.
///
/// The obvious approach, reading the paint layer, does not work: Krita stores a layer in its own
/// tile format, not as a PNG, so the layer entry begins with "VERSION 2" and no image loader will
/// touch it. The merged image is a PNG, and because page artifacts deliberately contain no page
/// background it is the flatten of everything the user made on that page and nothing else. It also
/// gets several layers right for free -- grouped ones included -- which reading one layer would
/// not, and it is what makes "every layer except the background is saved" need no change here.
bool isMergedImage(const QString &name)
{
    return name.endsWith(QLatin1String("mergedimage.png"));
}

void collectMergedImage(const KArchiveDirectory *directory, const QString &prefix, QList<QByteArray> *found)
{
    const QStringList entries = directory->entries();
    for (const QString &entry : entries) {
        const KArchiveEntry *child = directory->entry(entry);
        if (!child) {
            continue;
        }

        const QString path = prefix + QLatin1Char('/') + entry;
        if (const KArchiveDirectory *sub = dynamic_cast<const KArchiveDirectory *>(child)) {
            collectMergedImage(sub, path, found);
            continue;
        }

        if (const KArchiveFile *file = dynamic_cast<const KArchiveFile *>(child)) {
            if (isMergedImage(path)) {
                found->append(file->data());
            }
        }
    }
}

/// The same walk, handing back the entries themselves: a caller that wants a header out of one does
/// not have to inflate the whole PNG to read it, which data() above does.
void collectMergedImageFiles(const KArchiveDirectory *directory, const QString &prefix,
                             QList<const KArchiveFile *> *found)
{
    const QStringList entries = directory->entries();
    for (const QString &entry : entries) {
        const KArchiveEntry *child = directory->entry(entry);
        if (!child) {
            continue;
        }

        const QString path = prefix + QLatin1Char('/') + entry;
        if (const KArchiveDirectory *sub = dynamic_cast<const KArchiveDirectory *>(child)) {
            collectMergedImageFiles(sub, path, found);
            continue;
        }

        if (const KArchiveFile *file = dynamic_cast<const KArchiveFile *>(child)) {
            if (isMergedImage(path)) {
                found->append(file);
            }
        }
    }
}

/// The size a PNG announces in its own header, without decoding a pixel of it: eight bytes of
/// signature, then the IHDR chunk -- four bytes of length, the type, then width and height as two
/// big-endian 32-bit words.
QSize pngHeaderSize(const QByteArray &bytes)
{
    constexpr int WidthOffset = 16;
    if (bytes.size() < WidthOffset + 8
        || !bytes.startsWith(QByteArrayLiteral("\x89PNG\r\n\x1a\n"))
        || bytes.mid(12, 4) != QByteArrayLiteral("IHDR")) {
        return QSize();
    }

    const auto word = [&bytes](int at) {
        return (quint32(quint8(bytes.at(at))) << 24) | (quint32(quint8(bytes.at(at + 1))) << 16)
            | (quint32(quint8(bytes.at(at + 2))) << 8) | quint32(quint8(bytes.at(at + 3)));
    };
    return QSize(int(word(WidthOffset)), int(word(WidthOffset + 4)));
}

} // namespace

namespace {

/// Copies \a layers into \a parent, keeping names, order and opacity, and keeping groups as
/// groups. Nodes cannot be moved between images: each one is rebuilt in the target image and its
/// pixels cloned, which is what PdfPageSaver does in the other direction.
/// \a target by value: KisSharedPtr hands back a const KisImage through a const smart pointer,
/// and addNode() is not a const method.
void copyRestoredLayers(KisImageSP target, const QList<KisNodeSP> &layers, KisNodeSP parent)
{
    for (KisNodeSP child : layers) {
        if (KisGroupLayer *group = qobject_cast<KisGroupLayer *>(child.data())) {
            KisGroupLayerSP copy = new KisGroupLayer(target, group->name(), group->opacity(),
                                                     target->colorSpace());
            target->addNode(copy, parent);

            QList<KisNodeSP> children;
            for (quint32 i = 0; i < group->childCount(); ++i) {
                children.append(group->at(i));
            }
            copyRestoredLayers(target, children, copy);
            continue;
        }

        KisPaintLayer *paint = qobject_cast<KisPaintLayer *>(child.data());
        if (!paint) {
            continue;
        }

        KisPaintLayerSP copy = new KisPaintLayer(target, paint->name(), paint->opacity());
        copy->paintDevice()->makeCloneFrom(paint->paintDevice(), paint->paintDevice()->extent());
        copy->setX(paint->x());
        copy->setY(paint->y());
        target->addNode(copy, parent);
    }
}

/// Hands a document back the way Krita wants it: removeDocument(document, true) deletes it, and a
/// document made with KisPart::createDocument() is not ours to delete directly.
///
/// The delete stays QUEUED, which is Krita's own arrangement and not ours to hurry: KisDocument's
/// destructor waits on the image's scheduler, and running it in the middle of a page rewrite put a
/// busy-wait dialog on screen from inside the rewrite -- and a Krita sanity assert fired because the
/// image was still referenced while its document went. So the memory is bought by making FEWER
/// documents (one read per rewrite, no packing copy, no read-back decode), not by deleting them
/// earlier. On the tablet this was an OOM (2026-09-28).
void releaseDocument(KisDocument *document)
{
    if (!document) {
        return;
    }
    KisPart::instance()->removeDocument(document, true);
}

} // namespace

bool PdfInkLoader::loadInkLayersInto(const QString &kraPath, const KisImageSP &target,
                                     KisNodeSP parent, QString *why)
{
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error: a page that was never drawn on has no artifact.
        return false;
    }

    if (!target || !parent) {
        fail(why, QStringLiteral("there is no page to put %1 into").arg(kraPath));
        return false;
    }

    /// A copy of the shared pointer, because removeNode() and copyRestoredLayers() are not const.
    KisImageSP page = target;

    KisDocument *document = KisPart::instance()->createDocument();
    if (!document) {
        fail(why, QStringLiteral("no document could be made for %1").arg(kraPath));
        return false;
    }

    KraConverter *converter = new KraConverter(document);
    QFile file(kraPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        delete converter;
        converter = nullptr;
        releaseDocument(document);
        return false;
    }

    const KisImportExportErrorCode code = converter->buildImage(&file);
    file.close();
    if (!code.isOk()) {
        fail(why, QStringLiteral("%1 is not a readable document").arg(kraPath));
        delete converter;
        converter = nullptr;
        releaseDocument(document);
        return false;
    }

    const KisImageSP loaded = converter->image();
    QList<KisNodeSP> layers;
    if (loaded && loaded->root()) {
        for (quint32 i = 0; i < loaded->root()->childCount(); ++i) {
            layers.append(loaded->root()->at(i));
        }
    }

    if (layers.isEmpty()) {
        fail(why, QStringLiteral("%1 holds no layers").arg(kraPath));
        delete converter;
        converter = nullptr;
        releaseDocument(document);
        return false;
    }

    /// The Ink group ships with an empty paint layer for the user to draw on
    /// (PdfProjectBuilder::inkStrokeLayerName). The layers coming back take its place, or the page
    /// would show both an empty stroke layer and the layers that hold the strokes.
    QList<KisNodeSP> placeholders;
    for (quint32 i = 0; i < parent->childCount(); ++i) {
        placeholders.append(parent->at(i));
    }
    for (KisNodeSP placeholder : placeholders) {
        page->removeNode(placeholder);
    }

    copyRestoredLayers(page, layers, parent);
    delete converter;
    converter = nullptr;
    releaseDocument(document);
    return true;
}

QList<PdfInkLoader::ArtifactLayer> PdfInkLoader::loadLayersForRewrite(const QString &kraPath,
                                                                    QSize *artifactSize,
                                                                    QString *why)
{
    QList<ArtifactLayer> layers;
    if (artifactSize) {
        *artifactSize = QSize();
    }
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error: a page that was never drawn on has no artifact, and a caller with nothing
        /// to rewrite says so by the empty list it gets back.
        return layers;
    }

    /// The sidecar first: a PNG per layer, one QImage to read and nothing else. No document, no
    /// layer graph and no update scheduler, which is what a crop on the tablet ran out of both time
    /// and memory in.
    const QString index = kraPath + QStringLiteral(".layers.txt");
    QFile list(index);
    if (list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString dir = kraPath + QStringLiteral(".layers");
        QTextStream in(&list);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
                continue;
            }

            /// index, name, file, opacity, x, y
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 4) {
                continue;
            }

            QImage pixels;
            if (!pixels.load(QDir(dir).filePath(parts.at(2)), "PNG") || pixels.isNull()) {
                continue;
            }

            ArtifactLayer layer;
            layer.name = parts.at(1);
            layer.pixels = pixels;
            layer.opacity = parts.at(3).toDouble();
            layer.x = parts.size() > 4 ? parts.at(4).toInt() : 0;
            layer.y = parts.size() > 5 ? parts.at(5).toInt() : 0;
            layers.append(layer);

            /// The page is the picture: every layer of an artifact is written over the page's own
            /// area, so the first readable one answers the question for all of them.
            if (artifactSize && artifactSize->isEmpty()) {
                *artifactSize = pixels.size();
            }
        }
    }

    if (!layers.isEmpty()) {
        return layers;
    }

    /// No sidecar, or nothing in it that reads: the artifact is then read as the document it is.
    /// This is the slow way, kept for the artifacts written before the sidecar existed.
    const QSize size = PdfInkLoader::artifactSize(kraPath, why);
    const QList<QPair<QString, QImage>> read = loadInkLayers(kraPath, why);
    for (const QPair<QString, QImage> &entry : read) {
        ArtifactLayer layer;
        layer.name = entry.first;
        layer.pixels = entry.second;
        layers.append(layer);
    }
    if (layers.isEmpty()) {
        /// Nothing to crop with, and the caller is told why rather than handed an empty page.
        return layers;
    }
    if (artifactSize) {
        /// The document answers the size when it can; the pictures are the fallback answer.
        *artifactSize = size.isEmpty() ? layers.first().pixels.size() : size;
    }
    return layers;
}

QList<QPair<QString, QImage>> PdfInkLoader::loadInkLayersFromSidecar(const QString &kraPath,
                                                                        QString *why)
{
    QList<QPair<QString, QImage>> layers;
    const QString index = kraPath + QStringLiteral(".layers.txt");
    QFile list(index);
    if (!list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        /// No sidecar: an artifact written before this was added, or one whose write was skipped.
        return loadInkLayers(kraPath, why);
    }

    const QString dir = kraPath + QStringLiteral(".layers");
    QTextStream in(&list);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        /// index, name, file, opacity, x, y
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 4) {
            continue;
        }

        QImage pixels;
        if (!pixels.load(QDir(dir).filePath(parts.at(2)), "PNG") || pixels.isNull()) {
            continue;
        }

        layers.append(QPair<QString, QImage>(parts.at(1), pixels));
    }

    if (layers.isEmpty() && why) {
        *why = QStringLiteral("%1 carries no readable layer images").arg(index);
    }
    return layers;
}

KisImageSP PdfInkLoader::loadArtifactInto(KisDocument *document, const QString &kraPath, QString *why)
{
    if (!document) {
        fail(why, QStringLiteral("no document to read %1 into").arg(kraPath));
        return KisImageSP();
    }
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error in itself: a page that was never drawn on has no artifact. A caller that
        /// has nothing to rewrite says so by the null image it gets back.
        return KisImageSP();
    }

    KraConverter *converter = new KraConverter(document);
    QFile file(kraPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        delete converter;
        return KisImageSP();
    }

    const KisImportExportErrorCode code = converter->buildImage(&file);
    file.close();
    if (!code.isOk()) {
        fail(why, QStringLiteral("%1 is not a readable document").arg(kraPath));
        delete converter;
        return KisImageSP();
    }

    /// The converter's image is the document's own -- buildImage() attached it there -- so it goes
    /// back as it is, layers and all. Nothing is copied into a second image: that copy is a whole
    /// page per layer, and it is what a crop was paying for and OOMing on.
    const KisImageSP image = converter->image();
    delete converter;
    return image;
}

KisImageSP PdfInkLoader::loadArtifactAsPage(KisDocument *document, const QString &kraPath, QString *why)
{
    if (!document) {
        fail(why, QStringLiteral("no document to read %1 into").arg(kraPath));
        return KisImageSP();
    }
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error in itself: a page that was never drawn on has no artifact.
        return KisImageSP();
    }

    KisDocument *loader = KisPart::instance()->createDocument();
    if (!loader) {
        fail(why, QStringLiteral("no document could be made for %1").arg(kraPath));
        return KisImageSP();
    }

    /// The file is read ONCE, into a document that exists only for this. Both the frame and the
    /// layers come out of it: a page rewritten in place has to know its size before it can crop, and
    /// the old answer -- artifactSize() first, the layers after -- decoded the whole artifact twice.
    KisImageSP page;
    {
        const KisImageSP source = loadArtifactInto(loader, kraPath, why);
        if (source) {
            const QSize size = source->bounds().size();
            if (size.isEmpty()) {
                fail(why, QStringLiteral("the artifact %1 has no page in it").arg(kraPath));
            } else {
                page = new KisImage(document->createUndoStore(), size.width(), size.height(),
                                    source->colorSpace(), QStringLiteral("page"));
                page->setResolution(source->xRes(), source->yRes());

                /// The layers come across as layers: names, order and opacity kept, which reading
                /// the file as pictures would lose. Cloned rather than moved -- a node cannot change
                /// image -- which is also the copy that makes the file's own graph redundant from
                /// here on.
                QList<KisNodeSP> layers;
                for (quint32 i = 0; i < source->root()->childCount(); ++i) {
                    layers.append(source->root()->at(i));
                }
                copyRestoredLayers(page, layers, page->root());
            }
        }
    }

    /// The file's own copy goes now, before the caller crops anything: it is a whole page per layer,
    /// and holding it through the rewrite is exactly the memory a crop on the tablet ran out of.
    /// Nothing here references it any more -- the layers above are clones in the image this hands
    /// back, which belongs to \a document.
    releaseDocument(loader);

    if (!page) {
        return KisImageSP();
    }
    document->setCurrentImage(page, false);
    return page;
}

QSize PdfInkLoader::artifactSize(const QString &kraPath, QString *why)
{
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error: a page that was never drawn on has no artifact.
        return QSize();
    }

    KisDocument *document = KisPart::instance()->createDocument();
    if (!document) {
        fail(why, QStringLiteral("no document could be made for %1").arg(kraPath));
        return QSize();
    }

    /// Through the one read of an artifact there is: the size is a property of the image the file
    /// holds, so asking for it any other way is a second decode of the same page. The image is
    /// released inside the block -- an image that outlives its document has no update scheduler and
    /// waits for jobs that will never run (see PdfPageRotator).
    QSize size;
    {
        const KisImageSP image = loadArtifactInto(document, kraPath, why);
        if (image) {
            size = image->bounds().size();
        }
    }
    releaseDocument(document);
    return size;
}

QSize PdfInkLoader::artifactSizeFromArchive(const QString &kraPath)
{
    if (!QFileInfo::exists(kraPath)) {
        return QSize();
    }

    KZip zip(kraPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        return QSize();
    }
    const KArchiveDirectory *root = zip.directory();
    if (!root) {
        return QSize();
    }

    QList<const KArchiveFile *> candidates;
    collectMergedImageFiles(root, QString(), &candidates);
    const KArchiveFile *best = nullptr;
    for (const KArchiveFile *candidate : candidates) {
        if (!best || candidate->size() > best->size()) {
            best = candidate;
        }
    }
    if (!best) {
        /// No merged image: an artifact that carries layers only. Nothing to read a size out of, and
        /// the caller asks the full read instead.
        return QSize();
    }

    /// Through a device, and only the header: data() would inflate the whole PNG -- a page, several
    /// megabytes of it -- to answer what the first 24 bytes say.
    QScopedPointer<QIODevice> device(best->createDevice());
    if (!device || !device->open(QIODevice::ReadOnly)) {
        return QSize();
    }
    return pngHeaderSize(device->read(24));
}

QList<QPair<QString, QImage>> PdfInkLoader::loadInkLayers(const QString &kraPath, QString *why)
{
    QList<QPair<QString, QImage>> layers;
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error: a page that was never drawn on has no artifact.
        return layers;
    }

    KisDocument *document = KisPart::instance()->createDocument();
    if (!document) {
        fail(why, QStringLiteral("no document could be made for %1").arg(kraPath));
        return layers;
    }

    KraConverter *converter = new KraConverter(document);
    QFile file(kraPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        delete converter;
        converter = nullptr;
        releaseDocument(document);
        return layers;
    }

    const KisImportExportErrorCode code = converter->buildImage(&file);
    file.close();
    if (!code.isOk()) {
        fail(why, QStringLiteral("%1 is not a readable document").arg(kraPath));
        delete converter;
        converter = nullptr;
        releaseDocument(document);
        return layers;
    }

    const KisImageSP loaded = converter->image();
    if (loaded && loaded->root()) {
        const QRect area = loaded->bounds();
        for (quint32 i = 0; i < loaded->root()->childCount(); ++i) {
            KisPaintLayer *paint = qobject_cast<KisPaintLayer *>(loaded->root()->at(i).data());
            if (!paint) {
                continue;
            }
            const QImage pixels = paint->paintDevice()->convertToQImage(nullptr, area);
            if (!pixels.isNull()) {
                layers.append(QPair<QString, QImage>(paint->name(), pixels));
            }
        }
    }

    delete converter;
    converter = nullptr;
    releaseDocument(document);
    return layers;
}

QImage PdfInkLoader::loadInk(const QString &kraPath, QString *why)
{
    if (!QFileInfo::exists(kraPath)) {
        /// Not an error: a page that was never drawn on has no artifact.
        return QImage();
    }

    KZip zip(kraPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        return QImage();
    }

    const KArchiveDirectory *root = zip.directory();
    if (!root) {
        fail(why, QStringLiteral("%1 has no directory").arg(kraPath));
        return QImage();
    }

    QList<QByteArray> candidates;
    collectMergedImage(root, QString(), &candidates);

    QByteArray best;
    for (const QByteArray &candidate : candidates) {
        if (candidate.size() > best.size()) {
            best = candidate;
        }
    }

    if (best.isEmpty()) {
        return QImage();
    }

    QImage ink;
    if (!ink.loadFromData(best, "PNG")) {
        fail(why, QStringLiteral("the merged image of %1 is not readable").arg(kraPath));
        return QImage();
    }

    return ink;
}
