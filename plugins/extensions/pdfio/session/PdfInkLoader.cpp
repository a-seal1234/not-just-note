/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfInkLoader.h"

#include <QDir>
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

    KraConverter converter(document);
    QFile file(kraPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        KisPart::instance()->removeDocument(document, true);
        return false;
    }

    const KisImportExportErrorCode code = converter.buildImage(&file);
    file.close();
    if (!code.isOk()) {
        fail(why, QStringLiteral("%1 is not a readable document").arg(kraPath));
        KisPart::instance()->removeDocument(document, true);
        return false;
    }

    const KisImageSP loaded = converter.image();
    QList<KisNodeSP> layers;
    if (loaded && loaded->root()) {
        for (quint32 i = 0; i < loaded->root()->childCount(); ++i) {
            layers.append(loaded->root()->at(i));
        }
    }

    if (layers.isEmpty()) {
        fail(why, QStringLiteral("%1 holds no layers").arg(kraPath));
        KisPart::instance()->removeDocument(document, true);
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
    KisPart::instance()->removeDocument(document, true);
    return true;
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

    KraConverter converter(document);
    QFile file(kraPath);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("cannot read %1").arg(kraPath));
        KisPart::instance()->removeDocument(document, true);
        return layers;
    }

    const KisImportExportErrorCode code = converter.buildImage(&file);
    file.close();
    if (!code.isOk()) {
        fail(why, QStringLiteral("%1 is not a readable document").arg(kraPath));
        KisPart::instance()->removeDocument(document, true);
        return layers;
    }

    const KisImageSP loaded = converter.image();
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

    KisPart::instance()->removeDocument(document, true);
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
