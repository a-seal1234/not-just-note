/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfNotebookBundle.h"

#include "PdfSession.h"

#include <cstdio>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QScopedPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

/// <KArchive> first, for the same reason PdfInkLoader puts it first: the KF5 headers for the
/// individual classes lean on it for KArchive itself and do not include it, which fails on the
/// Android build where that umbrella include is the only thing that brings the type in.
#include <KArchive>
#include <KArchiveDirectory>
#include <KArchiveEntry>
#include <KArchiveFile>
#include <KZip>

namespace {

const char *const ManifestName = "manifest.json";
const char *const BundleIndexName = "bundle.json";

/// Bumped when the index changes shape. Separate from the manifest schema on purpose: the index is
/// ours and describes this file, while the manifest is the project's and describes the notebook.
const int BundleIndexSchema = 1;

const int ChunkSize = 64 * 1024;

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// One file in the archive, with the path it was stored under.
struct ArchiveEntry {
    QString path;
    const KArchiveFile *file = nullptr;
};

/**
 * The archive as a flat list of files.
 *
 * Both shapes KArchive can produce are handled: a nested tree, where the name at each level has no
 * slash, and a flat one, where a whole relative path arrives as a single name. In both cases the
 * joined path is the path inside the archive, which is the only thing the checks care about.
 */
void collectEntries(const KArchiveDirectory *directory, const QString &prefix, QList<ArchiveEntry> *out)
{
    const QStringList names = directory->entries();
    for (const QString &name : names) {
        const KArchiveEntry *child = directory->entry(name);
        if (!child) {
            continue;
        }

        const QString path = prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
        if (const KArchiveDirectory *sub = dynamic_cast<const KArchiveDirectory *>(child)) {
            collectEntries(sub, path, out);
            continue;
        }
        if (const KArchiveFile *file = dynamic_cast<const KArchiveFile *>(child)) {
            ArchiveEntry entry;
            entry.path = path;
            entry.file = file;
            out->append(entry);
        }
    }
}

const KArchiveFile *findEntry(const QList<ArchiveEntry> &files, const QString &path)
{
    for (const ArchiveEntry &entry : files) {
        if (entry.path == path) {
            return entry.file;
        }
    }
    return nullptr;
}

/// Everything an archive entry offers as one QByteArray. Small files only: the manifest and the
/// index. Notebook artifacts are streamed.
bool readEntry(const KArchiveFile *file, QByteArray *out, QString *why)
{
    QScopedPointer<QIODevice> device(file->createDevice());
    if (!device) {
        fail(why, QStringLiteral("the archive will not read out %1").arg(file->name()));
        return false;
    }
    if (!device->isOpen() && !device->open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("the archive will not read out %1").arg(file->name()));
        return false;
    }
    *out = device->readAll();
    return true;
}

/**
 * The contents of one entry, streamed.
 *
 * The size is compared with what the directory says the entry holds, which is what turns a file
 * that was cut in half by a failed copy into a refusal instead of a notebook with a truncated
 * source in it.
 */
bool hashEntry(const KArchiveFile *file, QByteArray *sha256, qint64 *bytes, QString *why)
{
    QScopedPointer<QIODevice> device(file->createDevice());
    if (!device) {
        fail(why, QStringLiteral("the archive will not read out %1").arg(file->name()));
        return false;
    }
    if (!device->isOpen() && !device->open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("the archive will not read out %1").arg(file->name()));
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (true) {
        const QByteArray chunk = device->read(ChunkSize);
        if (chunk.isEmpty()) {
            break;
        }
        total += chunk.size();
        hash.addData(chunk);
    }

    if (total != file->size()) {
        fail(why, QStringLiteral("%1 is truncated inside the bundle: %2 of %3 bytes")
                      .arg(file->name()).arg(total).arg(file->size()));
        return false;
    }

    *sha256 = hash.result().toHex();
    if (bytes) {
        *bytes = total;
    }
    return true;
}

/// Writes one entry out. The parent directories are made as needed, because a nnnn.kra always
/// arrives as "pages/p0005.kra" and there is no directory entry for "pages" in a hand-made zip.
bool copyEntryTo(const KArchiveFile *file, const QString &destPath, QString *why)
{
    QScopedPointer<QIODevice> device(file->createDevice());
    if (!device) {
        fail(why, QStringLiteral("the archive will not read out %1").arg(file->name()));
        return false;
    }
    if (!device->isOpen() && !device->open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("the archive will not read out %1").arg(file->name()));
        return false;
    }

    if (!QDir().mkpath(QFileInfo(destPath).absolutePath())) {
        fail(why, QStringLiteral("cannot create %1").arg(QFileInfo(destPath).absolutePath()));
        return false;
    }

    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(why, QStringLiteral("cannot write %1").arg(destPath));
        return false;
    }

    qint64 total = 0;
    while (true) {
        const QByteArray chunk = device->read(ChunkSize);
        if (chunk.isEmpty()) {
            break;
        }
        if (out.write(chunk) != chunk.size()) {
            fail(why, QStringLiteral("cannot write %1").arg(destPath));
            return false;
        }
        total += chunk.size();
    }

    if (total != file->size()) {
        fail(why, QStringLiteral("%1 is truncated inside the bundle: %2 of %3 bytes")
                      .arg(file->name()).arg(total).arg(file->size()));
        return false;
    }

    out.close();
    if (out.error() != QFileDevice::NoError) {
        fail(why, QStringLiteral("cannot write %1").arg(destPath));
        return false;
    }
    return true;
}

/// ::rename rather than QFile::rename: only the former replaces the destination atomically on
/// POSIX, and the whole point of writing the bundle beside its destination is that the old file is
/// still there, or the new one is, and never a mixture.
bool replaceFile(const QString &from, const QString &to, QString *why)
{
#if defined(Q_OS_UNIX)
    if (::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0) {
        return true;
    }
    fail(why, QStringLiteral("cannot move %1 into place at %2").arg(from, to));
    return false;
#else
    QFile::remove(to);
    if (!QFile::rename(from, to)) {
        fail(why, QStringLiteral("cannot move %1 into place at %2").arg(from, to));
        return false;
    }
    return true;
#endif
}

bool removeTree(const QString &path)
{
    return QDir(path).removeRecursively();
}

/**
 * The archive-entry rule applied to the paths the manifest itself declares.
 *
 * An archive entry is not the only lever a hostile bundle has. source.file, pages[].kraFile and
 * thumbs[].thumbFile are all joined onto the staging directory and written to, so a manifest that
 * names "/tmp/x.pdf" or "../../x.pdf" gets a write of its own choosing while every entry in the
 * archive is a perfectly ordinary name. The same rule therefore has to be applied to them, and it
 * has to be applied here -- in analyze(), which inspect() also runs -- so that the pre-flight
 * refuses exactly what extract() would.
 *
 * An empty thumbFile is not an error: a page whose thumbnail has not been made yet records none,
 * and every other piece of code skips it.
 */
bool validateManifestPaths(const PdfSessionManifest &manifest, QString *why)
{
    const auto check = [why](const QString &path, const QString &what) {
        QString reason;
        if (path.isEmpty()) {
            fail(why, QStringLiteral("the manifest names no %1").arg(what));
            return false;
        }
        if (!PdfNotebookBundle::isSafeEntryPath(path, &reason)) {
            fail(why, QStringLiteral("the manifest's %1 \"%2\" is not a name inside the notebook: %3")
                          .arg(what, path, reason));
            return false;
        }
        return true;
    };

    if (!check(manifest.sourceFile, QStringLiteral("source file"))) {
        return false;
    }
    for (const PdfPageRecord &page : manifest.pages) {
        if (!check(page.kraFile, QStringLiteral("page %1 ink file").arg(page.index + 1))) {
            return false;
        }
        if (!page.thumbFile.isEmpty()
            && !check(page.thumbFile, QStringLiteral("page %1 thumbnail").arg(page.index + 1))) {
            return false;
        }
    }
    return true;
}

/**
 * Where \a relative lands under \a root, refusing anything that does not stay inside it.
 *
 * validateManifestPaths() already refuses the shapes that escape; this is the other half of the
 * same check and it is what makes "absolute only after joining" impossible rather than unlikely:
 * the joined, cleaned path still has to be under the root. \a root is the staging directory, so
 * the answer is also false for the root itself -- a file has to be a file.
 */
bool destinationInside(const QString &root, const QString &relative, QString *destination, QString *why)
{
    const QString cleanRoot = QDir::cleanPath(QDir(root).absolutePath());
    const QString joined = QDir::cleanPath(QDir(root).filePath(relative));
    if (joined == cleanRoot || !joined.startsWith(cleanRoot + QLatin1Char('/'))) {
        fail(why, QStringLiteral("refusing %1: it would be written outside %2").arg(relative, cleanRoot));
        return false;
    }
    if (destination) {
        *destination = joined;
    }
    return true;
}

struct Analysis {
    PdfNotebookBundle::Info info;
    QList<ArchiveEntry> files;
};

/// Reads the bundle index. Only its own shape is checked here; whether it agrees with the archive
/// is decided once the archive's own contents have been hashed.
bool readIndex(const KArchiveFile *file, Analysis *analysis, QString *why)
{
    QByteArray bytes;
    if (!readEntry(file, &bytes, why)) {
        return false;
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        fail(why, QStringLiteral("bundle.json is not valid JSON: %1").arg(error.errorString()));
        return false;
    }

    const QJsonObject root = document.object();
    const int schema = root.value(QStringLiteral("bundle")).toInt(0);
    if (schema != BundleIndexSchema) {
        fail(why, schema > BundleIndexSchema
                      ? QStringLiteral("this bundle was written by a newer version (index %1; this build understands %2)")
                            .arg(schema).arg(BundleIndexSchema)
                      : QStringLiteral("unsupported bundle index %1 (this build understands %2)")
                            .arg(schema).arg(BundleIndexSchema));
        return false;
    }

    const QString source = root.value(QStringLiteral("source")).toString();
    if (!source.isEmpty() && source != analysis->info.sourceEntry) {
        fail(why, QStringLiteral("the bundle index names %1 as the source but the manifest names %2")
                      .arg(source, analysis->info.sourceEntry));
        return false;
    }

    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject object = value.toObject();
        PdfNotebookBundle::Entry entry;
        entry.path = object.value(QStringLiteral("path")).toString();
        entry.bytes = qint64(object.value(QStringLiteral("bytes")).toDouble());
        entry.sha256 = object.value(QStringLiteral("sha256")).toString().toLatin1();
        if (!PdfNotebookBundle::isSafeEntryPath(entry.path, why)) {
            return false;
        }
        if (!findEntry(analysis->files, entry.path)) {
            fail(why, QStringLiteral("the bundle index lists %1, which the archive does not carry: "
                                     "the file is truncated or was edited")
                          .arg(entry.path));
            return false;
        }
        analysis->info.entries.append(entry);
    }

    const QJsonArray withheld = root.value(QStringLiteral("withheld")).toArray();
    for (const QJsonValue &value : withheld) {
        const QString path = value.toString();
        if (!PdfNotebookBundle::isSafeEntryPath(path, why)) {
            return false;
        }
        if (findEntry(analysis->files, path)) {
            fail(why, QStringLiteral("the bundle index calls %1 withheld but the archive carries it")
                          .arg(path));
            return false;
        }
        analysis->info.withheld.append(path);
    }

    analysis->info.hasIndex = true;
    return true;
}

/**
 * Everything that has to be true before a byte is written anywhere.
 *
 * Ordered so the cheapest and most fundamental refusal comes first: a path that escapes, then a
 * manifest this build cannot read, then the source's checksum, then agreement between the index
 * and the archive, then the pages the manifest names. Nothing here touches the filesystem outside
 * the archive.
 */
bool analyze(const KZip &zip, const QString &bundlePath, Analysis *analysis, QString *why)
{
    const KArchiveDirectory *root = zip.directory();
    if (!root) {
        fail(why, QStringLiteral("%1 has no archive directory").arg(bundlePath));
        return false;
    }

    collectEntries(root, QString(), &analysis->files);
    if (analysis->files.isEmpty()) {
        fail(why, QStringLiteral("%1 is empty").arg(bundlePath));
        return false;
    }

    /// Every entry, before anything is looked up: an archive is attacker-controlled input, and an
    /// unknown entry with a hostile name has to be refused just as loudly as a known one.
    for (const ArchiveEntry &entry : analysis->files) {
        if (!PdfNotebookBundle::isSafeEntryPath(entry.path, why)) {
            return false;
        }
    }

    const KArchiveFile *manifestFile = findEntry(analysis->files, QLatin1String(ManifestName));
    if (!manifestFile) {
        fail(why, QStringLiteral("%1 is not a notebook bundle: it carries no manifest.json")
                      .arg(bundlePath));
        return false;
    }

    QByteArray manifestBytes;
    if (!readEntry(manifestFile, &manifestBytes, why)) {
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !manifestDocument.isObject()) {
        fail(why, QStringLiteral("manifest.json is not valid JSON: %1").arg(parseError.errorString()));
        return false;
    }

    /// The schema is read before fromJson, which collapses every reason a manifest can be invalid
    /// into one verdict, and "written by a newer Krita" deserves to be said out loud.
    ///
    /// A schema 1 manifest is accepted and upgraded by fromJson, exactly as it is when a notebook
    /// is opened from its directory: every bundle made before sources[] existed has to keep
    /// opening. What is refused is a schema this build does not know, in either direction.
    const int schema = manifestDocument.object().value(QStringLiteral("schema")).toInt(0);
    if (schema < 1 || schema > PdfSessionManifest::CurrentSchema) {
        fail(why, schema > PdfSessionManifest::CurrentSchema
                      ? QStringLiteral("this notebook was written by a newer version (manifest schema %1; "
                                       "this build understands %2)")
                            .arg(schema).arg(PdfSessionManifest::CurrentSchema)
                      : QStringLiteral("unsupported notebook manifest schema %1 (this build understands %2)")
                            .arg(schema).arg(PdfSessionManifest::CurrentSchema));
        return false;
    }

    analysis->info.manifest = PdfSessionManifest::fromJson(manifestDocument.object(), why);
    if (!analysis->info.manifest.isValid(why)) {
        return false;
    }
    const PdfSessionManifest &manifest = analysis->info.manifest;

    /// The same rule on the way in: a manifest naming more than one source describes a notebook
    /// this format cannot rebuild, because the archive carries exactly one PDF. Refused here rather
    /// than extracted into a notebook whose other pages would render blank.
    if (manifest.sourceCount() > 1) {
        fail(why, QStringLiteral("the bundle's notebook draws pages from %1 PDFs; this format "
                                 "carries one").arg(manifest.sourceCount()));
        return false;
    }

    /// Every file the manifest names, before one of them is joined onto a destination. The archive
    /// is not the only place a path comes from, and this is the check that keeps a hostile
    /// manifest from choosing where the source is written.
    if (!validateManifestPaths(manifest, why)) {
        return false;
    }

    /// The source. Normally at the name the manifest records; a bundle written by hand may simply
    /// have called it source.pdf, which is the same thing under the name the format documents.
    QString sourceEntry = manifest.sourceFile;
    if (!findEntry(analysis->files, sourceEntry)) {
        if (findEntry(analysis->files, QStringLiteral("source.pdf"))) {
            sourceEntry = QStringLiteral("source.pdf");
        } else {
            fail(why, QStringLiteral("the bundle does not carry the source %1").arg(manifest.sourceFile));
            return false;
        }
    }
    analysis->info.sourceEntry = sourceEntry;

    QByteArray sourceSha;
    qint64 sourceBytes = 0;
    if (!hashEntry(findEntry(analysis->files, sourceEntry), &sourceSha, &sourceBytes, why)) {
        return false;
    }
    if (sourceSha != manifest.sourceSha256
        || (manifest.sourceByteSize > 0 && sourceBytes != manifest.sourceByteSize)) {
        fail(why, QStringLiteral("the source inside the bundle does not match the checksum the manifest "
                                 "records for it: it was changed or the bundle was edited"));
        return false;
    }

    if (const KArchiveFile *indexFile = findEntry(analysis->files, QLatin1String(BundleIndexName))) {
        if (!readIndex(indexFile, analysis, why)) {
            return false;
        }
    }

    /// What the archive actually carries, hashed. The manifest is first so the list has one order.
    QList<PdfNotebookBundle::Entry> carried;
    const auto carry = [&](const QString &path, const KArchiveFile *file) {
        PdfNotebookBundle::Entry entry;
        entry.path = path;
        if (path == sourceEntry) {
            entry.bytes = sourceBytes;
            entry.sha256 = sourceSha;
        } else if (!hashEntry(file, &entry.sha256, &entry.bytes, why)) {
            return false;
        }
        carried.append(entry);
        return true;
    };

    if (!carry(QLatin1String(ManifestName), manifestFile)) {
        return false;
    }
    if (!carry(sourceEntry, findEntry(analysis->files, sourceEntry))) {
        return false;
    }

    for (const PdfPageRecord &page : manifest.pages) {
        const QString references[] = { page.kraFile, page.thumbFile };
        for (const QString &reference : references) {
            if (reference.isEmpty()) {
                continue;
            }
            const KArchiveFile *file = findEntry(analysis->files, reference);
            if (!file) {
                /// A page that was never drawn on has no ink file and a page never shown has no
                /// thumbnail, so a bundle is allowed not to carry them -- but only when its own
                /// index says so. Without that declaration the manifest is referring to something
                /// that is not there, which is a truncated or edited file.
                if (!analysis->info.withheld.contains(reference)) {
                    fail(why, QStringLiteral("the manifest references %1, which the bundle does not carry")
                                  .arg(reference));
                    return false;
                }
                continue;
            }
            if (!carry(reference, file)) {
                return false;
            }
        }
    }

    /// The index has to agree with the archive exactly. A file that was edited, or a copy that
    /// stopped half way, shows up here even when every individual path is present.
    if (analysis->info.hasIndex) {
        if (analysis->info.entries.size() != carried.size()) {
            fail(why, QStringLiteral("the bundle index lists %1 files but the archive carries %2: "
                                     "the bundle was edited or truncated")
                          .arg(analysis->info.entries.size()).arg(carried.size()));
            return false;
        }
        for (const PdfNotebookBundle::Entry &expected : analysis->info.entries) {
            bool matched = false;
            for (const PdfNotebookBundle::Entry &actual : carried) {
                if (actual.path != expected.path) {
                    continue;
                }
                matched = actual.bytes == expected.bytes && actual.sha256 == expected.sha256;
                break;
            }
            if (!matched) {
                fail(why, QStringLiteral("the contents of %1 do not match the bundle index: the bundle "
                                         "was edited or truncated")
                              .arg(expected.path));
                return false;
            }
        }
    }

    analysis->info.entries = carried;

    QSet<QString> known;
    known.insert(QLatin1String(ManifestName));
    known.insert(QLatin1String(BundleIndexName));
    known.insert(sourceEntry);
    for (const PdfNotebookBundle::Entry &entry : carried) {
        known.insert(entry.path);
    }
    for (const ArchiveEntry &entry : analysis->files) {
        if (!known.contains(entry.path)) {
            analysis->info.unknown.append(entry.path);
        }
    }

    return true;
}

/// Writes the archive at \a path: the carried files first, then the index that describes them.
bool writeBundleFile(const QString &path,
                     const QList<QPair<QString, QString>> &carried,
                     const QString &sourceEntry,
                     const QStringList &withheld,
                     QString *why)
{
    KZip zip(path);
    if (!zip.open(QIODevice::WriteOnly)) {
        fail(why, QStringLiteral("cannot write %1").arg(path));
        return false;
    }

    QList<PdfNotebookBundle::Entry> entries;
    for (const QPair<QString, QString> &pair : carried) {
        QFile source(pair.second);
        if (!source.open(QIODevice::ReadOnly)) {
            fail(why, QStringLiteral("cannot read %1").arg(pair.second));
            return false;
        }

        const qint64 size = source.size();
        if (!zip.prepareWriting(pair.first, QString(), QString(), size)) {
            fail(why, QStringLiteral("cannot add %1 to the bundle").arg(pair.first));
            return false;
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!source.atEnd()) {
            const QByteArray chunk = source.read(ChunkSize);
            if (chunk.isEmpty()) {
                break;
            }
            hash.addData(chunk);
            /// The two argument form: KF5's KZip has no QByteArray overload and KF6 keeps this
            /// one, so it is the only spelling that compiles on both the desktop and Android.
            if (!zip.writeData(chunk.constData(), chunk.size())) {
                fail(why, QStringLiteral("cannot write %1 into the bundle").arg(pair.first));
                return false;
            }
        }
        if (!zip.finishWriting(size)) {
            fail(why, QStringLiteral("cannot finish %1 in the bundle").arg(pair.first));
            return false;
        }

        PdfNotebookBundle::Entry entry;
        entry.path = pair.first;
        entry.bytes = size;
        entry.sha256 = hash.result().toHex();
        entries.append(entry);
    }

    QJsonArray entryArray;
    for (const PdfNotebookBundle::Entry &entry : entries) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), entry.path);
        object.insert(QStringLiteral("bytes"), double(entry.bytes));
        object.insert(QStringLiteral("sha256"), QString::fromLatin1(entry.sha256));
        entryArray.append(object);
    }

    QJsonObject index;
    index.insert(QStringLiteral("bundle"), BundleIndexSchema);
    index.insert(QStringLiteral("source"), sourceEntry);
    index.insert(QStringLiteral("entries"), entryArray);
    if (!withheld.isEmpty()) {
        index.insert(QStringLiteral("withheld"), QJsonArray::fromStringList(withheld));
    }

    /// The index is written last and describes everything above it; a reader that finds it never
    /// has to guess what the archive was supposed to contain.
    const QByteArray bytes = QJsonDocument(index).toJson(QJsonDocument::Indented);
    if (!zip.prepareWriting(QLatin1String(BundleIndexName), QString(), QString(), bytes.size())
        || !zip.writeData(bytes.constData(), bytes.size()) || !zip.finishWriting(bytes.size())) {
        fail(why, QStringLiteral("cannot write the bundle index into %1").arg(path));
        return false;
    }

    if (!zip.close()) {
        fail(why, QStringLiteral("cannot finish writing %1").arg(path));
        return false;
    }
    return true;
}

} // namespace

QString PdfNotebookBundle::extension()
{
    return QStringLiteral("pnb");
}

QString PdfNotebookBundle::fileFilter()
{
    /// Both spellings: the bundle is a zip, and on a device where .pnb was claimed by something
    /// else the same file still has to open. See the class comment.
    return QStringLiteral("PDF note bundles (*.pnb);;Zip archives (*.zip);;All files (*)");
}

bool PdfNotebookBundle::isSafeEntryPath(const QString &path, QString *why)
{
    if (path.isEmpty()) {
        fail(why, QStringLiteral("the archive carries an entry with an empty name"));
        return false;
    }
    if (QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('/'))) {
        fail(why, QStringLiteral("refusing the entry %1: it is an absolute path").arg(path));
        return false;
    }
    if (path.contains(QLatin1Char('\\'))) {
        fail(why, QStringLiteral("refusing the entry %1: it uses a backslash as a separator").arg(path));
        return false;
    }
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':')) {
        fail(why, QStringLiteral("refusing the entry %1: it names a drive").arg(path));
        return false;
    }

    const QStringList components = path.split(QLatin1Char('/'));
    for (const QString &component : components) {
        if (component.isEmpty()) {
            fail(why, QStringLiteral("refusing the entry %1: it has an empty path component").arg(path));
            return false;
        }
        if (component == QLatin1String(".") || component == QLatin1String("..")) {
            fail(why, QStringLiteral("refusing the entry %1: it escapes the destination").arg(path));
            return false;
        }
    }
    return true;
}

QString PdfNotebookBundle::extractDirName(const PdfSessionManifest &manifest)
{
    const QString base = QFileInfo(manifest.sourceFile).completeBaseName();
    const QString key = QString::fromLatin1(manifest.sourceSha256.left(8));
    const QString name = base + QLatin1Char('-') + key;
    return name == QLatin1String("-") || name.isEmpty() ? QStringLiteral("notebook") : name;
}

QString PdfNotebookBundle::defaultProjectRoot()
{
    /// The policy -- Documents on the desktop, the app-private location on Android, and what to do
    /// when Documents is not there -- belongs to PdfSession, which is also where the navigator
    /// reads it. Going through it rather than repeating the rule is what makes opening the
    /// extracted source find the project that was just written instead of making a second one.
    return PdfSession::projectRoot();
}

bool PdfNotebookBundle::save(const QString &projectDir, const QString &outPath, QString *why)
{
    if (projectDir.isEmpty() || outPath.isEmpty()) {
        fail(why, QStringLiteral("a project directory and a destination are both needed"));
        return false;
    }

    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(PdfSession::manifestPath(projectDir), why);
    if (!manifest.isValid(why)) {
        return false;
    }

    /// One source is what this format carries, and it verifies that one. A notebook that was given
    /// pages from another PDF has more than one, and writing it out here would silently leave those
    /// pages' backgrounds behind: the bundle would unpack into a notebook that renders blank paper
    /// where those pages are, with nothing said at either end.
    if (manifest.sourceCount() > 1) {
        fail(why, QStringLiteral("this notebook draws pages from %1 PDFs and the one-file form "
                                 "carries only one, so the pages that come from %2 would lose "
                                 "their background")
                      .arg(manifest.sourceCount())
                      .arg(manifest.sourceAt(1).file));
        return false;
    }

    /// A project whose manifest names a file outside itself would have that file read into the
    /// bundle: the same rule as on the way in, for the same reason.
    if (!validateManifestPaths(manifest, why)) {
        return false;
    }

    const QString source = PdfSession::sourcePath(projectDir, manifest.sourceFile);
    if (!QFileInfo::exists(source)) {
        fail(why, QStringLiteral("the notebook has no source at %1").arg(source));
        return false;
    }
    /// The bundle is only worth as much as the identity it carries: a source that no longer
    /// matches the manifest would produce a file that its own reader refuses.
    if (PdfSessionManifest::sha256OfFile(source) != manifest.sourceSha256) {
        fail(why, QStringLiteral("the source %1 changed since the notebook was created").arg(manifest.sourceFile));
        return false;
    }

    QList<QPair<QString, QString>> carried;
    QSet<QString> seen;
    const auto carry = [&carried, &seen](const QString &relative, const QString &local) {
        if (relative.isEmpty() || seen.contains(relative)) {
            return;
        }
        seen.insert(relative);
        carried.append(qMakePair(relative, local));
    };

    carry(QLatin1String(ManifestName), PdfSession::manifestPath(projectDir));
    carry(manifest.sourceFile, source);

    /// A page that was never drawn on has no ink file and most pages have no thumbnail, so
    /// whatever is not there is declared withheld rather than left to be discovered as a hole.
    QStringList withheld;
    const auto offer = [&](const QString &relative) {
        if (relative.isEmpty()) {
            return;
        }
        const QString local = QDir(projectDir).filePath(relative);
        if (QFileInfo::exists(local)) {
            carry(relative, local);
        } else {
            withheld.append(relative);
        }
    };
    for (const PdfPageRecord &page : manifest.pages) {
        offer(page.kraFile);
        offer(page.thumbFile);

        /// The layer sidecar travels with the page. Without it, a notebook that is exported and
        /// opened somewhere else reads its layers by opening a document per page again -- the read
        /// that ends in a tombstone on a tablet during a window move. It is small: one PNG per
        /// layer and a line of text each.
        const QString sidecar = page.kraFile + QStringLiteral(".layers");
        const QString sidecarIndex = sidecar + QStringLiteral(".txt");

        /// Only what is actually there. Offering a file that was never written would put it on the
        /// bundle's list of things it withheld, which is a statement about the notebook -- and it
        /// changed how many entries a bundle of a notebook with no sidecars reports.
        if (QFileInfo(QDir(projectDir).filePath(sidecarIndex)).exists()) {
            offer(sidecarIndex);
        }
        const QDir sidecarDir(QDir(projectDir).filePath(sidecar));
        const QStringList images = sidecarDir.entryList(QDir::Files, QDir::Name);
        for (const QString &image : images) {
            offer(sidecar + QLatin1Char('/') + image);
        }
    }

    /// The notebook's own assets/ -- the images an "Insert image..." went into -- travel beside
    /// pages/ and thumbs/. They are referenced by a page's content layer rather than by the
    /// manifest, so nothing above would offer them, and a bundle without them restores with the
    /// pictures missing while the original notebook still shows them.
    const QDir assetsDir(QDir(projectDir).filePath(QStringLiteral("assets")));
    const QStringList assetNames = assetsDir.entryList(QDir::Files, QDir::Name);
    for (const QString &asset : assetNames) {
        offer(QStringLiteral("assets/") + asset);
    }

    /// Beside the destination, then renamed onto it: an interrupted save leaves the file that was
    /// already there, not a half-written archive that looks like a notebook.
    const QString temporary = QStringLiteral("%1.part-%2").arg(outPath).arg(QCoreApplication::applicationPid());
    QFile::remove(temporary);

    if (!writeBundleFile(temporary, carried, manifest.sourceFile, withheld, why)) {
        QFile::remove(temporary);
        return false;
    }

    if (!replaceFile(temporary, outPath, why)) {
        QFile::remove(temporary);
        return false;
    }
    return true;
}

PdfNotebookBundle::Info PdfNotebookBundle::inspect(const QString &bundlePath, QString *why)
{
    const QFileInfo info(bundlePath);
    if (!info.exists() || !info.isFile()) {
        fail(why, QStringLiteral("no such notebook file: %1").arg(bundlePath));
        return Info();
    }

    KZip zip(bundlePath);
    if (!zip.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("%1 is not a readable zip archive: it is corrupt or truncated")
                      .arg(bundlePath));
        return Info();
    }

    Analysis analysis;
    if (!analyze(zip, bundlePath, &analysis, why)) {
        return Info();
    }
    analysis.info.bundleBytes = info.size();
    return analysis.info;
}

bool PdfNotebookBundle::extract(const QString &bundlePath,
                                const QString &destProjectDir,
                                QString *why,
                                QStringList *ignoredEntries)
{
    return extract(bundlePath, destProjectDir, ExtractOptions(), why, ignoredEntries);
}

bool PdfNotebookBundle::extract(const QString &bundlePath,
                                const QString &destProjectDir,
                                const ExtractOptions &options,
                                QString *why,
                                QStringList *ignoredEntries)
{
    if (destProjectDir.isEmpty()) {
        fail(why, QStringLiteral("no destination was given"));
        return false;
    }

    KZip zip(bundlePath);
    if (!zip.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("%1 is not a readable zip archive: it is corrupt or truncated")
                      .arg(bundlePath));
        return false;
    }

    Analysis analysis;
    if (!analyze(zip, bundlePath, &analysis, why)) {
        return false;
    }
    const PdfSessionManifest &manifest = analysis.info.manifest;

    const QString destination = QFileInfo(destProjectDir).absoluteFilePath();
    if (QFileInfo::exists(destination) && !options.replaceExisting) {
        fail(why, QStringLiteral("%1 is already there; the notebook in it was not touched")
                      .arg(destination));
        return false;
    }

    const QString parent = QFileInfo(destination).absolutePath();
    if (!QDir().mkpath(parent)) {
        fail(why, QStringLiteral("cannot create %1").arg(parent));
        return false;
    }

    /// The notebook is rebuilt whole, in a directory beside the one it will become. It is renamed
    /// into place only once everything below has passed, so a refusal leaves nothing behind.
    QTemporaryDir staging(QDir(parent).filePath(
        QStringLiteral(".%1-extracting-XXXXXX").arg(QFileInfo(destination).fileName())));
    if (!staging.isValid()) {
        fail(why, QStringLiteral("cannot create a temporary directory beside %1").arg(destination));
        return false;
    }
    const QString stagingPath = staging.path();

    const KArchiveFile *manifestFile = findEntry(analysis.files, QLatin1String(ManifestName));
    if (!copyEntryTo(manifestFile, QDir(stagingPath).filePath(QLatin1String(ManifestName)), why)) {
        return false;
    }

    /// The source goes to the name the manifest records, which is not necessarily the name the
    /// entry was stored under: a bundle may call it source.pdf, and the project cannot. That name
    /// was checked by validateManifestPaths() and is checked again here against the staging root,
    /// because it is the manifest's string that decides where this write goes.
    QString stagedSource;
    if (!destinationInside(stagingPath, manifest.sourceFile, &stagedSource, why)) {
        return false;
    }
    if (!copyEntryTo(findEntry(analysis.files, analysis.info.sourceEntry), stagedSource, why)) {
        return false;
    }

    for (const PdfPageRecord &page : manifest.pages) {
        const QString references[] = { page.kraFile, page.thumbFile };
        for (const QString &reference : references) {
            const KArchiveFile *file = findEntry(analysis.files, reference);
            if (!file) {
                continue;
            }

            QString stagedArtifact;
            if (!destinationInside(stagingPath, reference, &stagedArtifact, why)) {
                return false;
            }
            if (!copyEntryTo(file, stagedArtifact, why)) {
                return false;
            }
        }
    }

    /// And every asset the bundle carries, written to the same place it was packed from. These
    /// are not named by the manifest: a page's content layer points at them, so they are copied by
    /// their archive path and nothing else decides whether they are wanted.
    for (const ArchiveEntry &entry : analysis.files) {
        if (!entry.path.startsWith(QStringLiteral("assets/"))) {
            continue;
        }
        QString stagedAsset;
        if (!destinationInside(stagingPath, entry.path, &stagedAsset, why)) {
            return false;
        }
        if (!copyEntryTo(entry.file, stagedAsset, why)) {
            return false;
        }
    }

    /// Read back what was written, before it is given a name that says it is a notebook. The
    /// manifest has to come out identical and the source has to hash to what the manifest says.
    const PdfSessionManifest staged =
        PdfSessionManifest::readFrom(QDir(stagingPath).filePath(QLatin1String(ManifestName)), why);
    if (!staged.isValid(why)) {
        return false;
    }
    if (staged.toJson() != manifest.toJson()) {
        fail(why, QStringLiteral("the manifest written out of the bundle does not match the one it carried"));
        return false;
    }
    if (PdfSessionManifest::sha256OfFile(stagedSource) != manifest.sourceSha256) {
        fail(why, QStringLiteral("the source written out of the bundle does not match the manifest"));
        return false;
    }

    if (ignoredEntries) {
        *ignoredEntries = analysis.info.unknown;
    }

    /// Renamed, not copied: from here the directory is either the notebook or removed by hand, and
    /// QTemporaryDir must not delete it on the way out.
    staging.setAutoRemove(false);

    QString movedAside;
    if (QFileInfo::exists(destination)) {
        movedAside = destination + QStringLiteral(".replaced");
        removeTree(movedAside);
        if (!QDir().rename(destination, movedAside)) {
            removeTree(stagingPath);
            fail(why, QStringLiteral("cannot move the notebook already at %1 out of the way").arg(destination));
            return false;
        }
    }

    if (!QDir().rename(stagingPath, destination)) {
        if (!movedAside.isEmpty()) {
            QDir().rename(movedAside, destination);
        }
        removeTree(stagingPath);
        fail(why, QStringLiteral("cannot move the extracted notebook into %1").arg(destination));
        return false;
    }

    if (!movedAside.isEmpty()) {
        removeTree(movedAside);
    }
    return true;
}
