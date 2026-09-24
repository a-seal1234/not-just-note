/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfInkLoader.h"

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
