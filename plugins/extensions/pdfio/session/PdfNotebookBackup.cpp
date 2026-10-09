/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfNotebookBackup.h"

#include "session/PdfNotebookBundle.h"
#include "session/PdfSession.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>

namespace {

const int RetainedSnapshotCount = 3;

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

QString canonicalOrAbsolute(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString backupDirectory(const QString &projectDir, QString *why)
{
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (appData.isEmpty()) {
        fail(why, QStringLiteral("the application has no writable data directory for notebook backups"));
        return QString();
    }

    const QByteArray key = QCryptographicHash::hash(canonicalOrAbsolute(projectDir).toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    return QDir(appData).filePath(QStringLiteral("pdfio-backups/")
                                  + QString::fromLatin1(key.left(32)));
}

bool ensureBackupDirectory(const QString &directory, QString *why)
{
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString root = QDir(appData).filePath(QStringLiteral("pdfio-backups"));
    if (QFileInfo(root).isSymLink() || QFileInfo(directory).isSymLink()) {
        fail(why, QStringLiteral("the notebook backup folder must not be a symbolic link"));
        return false;
    }
    if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink()
        || !QFileInfo(directory).isDir()) {
        fail(why, QStringLiteral("cannot create a plain notebook backup folder at %1").arg(directory));
        return false;
    }
    return true;
}

bool safeProjectManifest(const QString &projectDir, PdfSessionManifest *manifest, QString *why)
{
    QString readWhy;
    const PdfSessionManifest read = PdfSessionManifest::readFrom(PdfSession::manifestPath(projectDir), &readWhy);
    if (!read.isValid(&readWhy)) {
        fail(why, QStringLiteral("the notebook manifest is not valid: %1").arg(readWhy));
        return false;
    }
    if (manifest) {
        *manifest = read;
    }
    return true;
}

bool createVerifiedSnapshot(const QString &projectDir,
                            const QString &destination,
                            QString *why)
{
    QString staging;
    do {
        staging = destination + QStringLiteral(".pending-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
    } while (QFileInfo::exists(staging) || QFileInfo(staging).isSymLink());

    QString localWhy;
    if (!PdfNotebookBundle::save(projectDir, staging, &localWhy)) {
        QFile::remove(staging);
        fail(why, QStringLiteral("the notebook could not be written as a backup: %1").arg(localWhy));
        return false;
    }

    localWhy.clear();
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(staging, &localWhy);
    if (!info.isValid()) {
        QFile::remove(staging);
        fail(why, QStringLiteral("the new backup did not pass its integrity check: %1").arg(localWhy));
        return false;
    }

    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink()
        || !QFile::rename(staging, destination)) {
        QFile::remove(staging);
        fail(why, QStringLiteral("the verified backup could not be moved into place at %1")
                      .arg(destination));
        return false;
    }
    return true;
}

QList<PdfNotebookBackup::Snapshot> validSnapshots(const QString &projectDir)
{
    QList<PdfNotebookBackup::Snapshot> result;
    QString why;
    const QString directory = backupDirectory(projectDir, &why);
    if (directory.isEmpty() || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()) {
        return result;
    }

    const QDir backups(directory);
    const QFileInfoList files = backups.entryInfoList(QStringList() << QStringLiteral("*.pnb"),
                                                     QDir::Files | QDir::NoSymLinks,
                                                     QDir::Time);
    for (const QFileInfo &file : files) {
        if (!file.isFile() || file.isSymLink()) {
            continue;
        }
        QString inspectWhy;
        const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(file.absoluteFilePath(), &inspectWhy);
        if (!info.isValid()) {
            continue;
        }

        PdfNotebookBackup::Snapshot snapshot;
        snapshot.path = file.absoluteFilePath();
        snapshot.displayName = file.fileName();
        snapshot.modified = file.lastModified();
        snapshot.bytes = file.size();
        result.append(snapshot);
    }

    std::sort(result.begin(), result.end(), [](const PdfNotebookBackup::Snapshot &a,
                                               const PdfNotebookBackup::Snapshot &b) {
        if (a.modified != b.modified) {
            return a.modified > b.modified;
        }
        return a.displayName > b.displayName;
    });
    return result;
}

bool pruneSnapshots(const QString &projectDir, QString *why)
{
    const QList<PdfNotebookBackup::Snapshot> snapshots = validSnapshots(projectDir);
    for (int i = RetainedSnapshotCount; i < snapshots.size(); ++i) {
        if (!QFile::remove(snapshots.at(i).path)) {
            fail(why, QStringLiteral("a backup was saved, but an older snapshot could not be removed: %1")
                          .arg(snapshots.at(i).path));
            return false;
        }
    }
    return true;
}

bool sameBundleContents(PdfNotebookBundle::Info left, PdfNotebookBundle::Info right)
{
    if (left.manifest.toJson() != right.manifest.toJson() || left.entries.size() != right.entries.size()) {
        return false;
    }

    const auto byPath = [](const PdfNotebookBundle::Entry &a, const PdfNotebookBundle::Entry &b) {
        return a.path < b.path;
    };
    std::sort(left.entries.begin(), left.entries.end(), byPath);
    std::sort(right.entries.begin(), right.entries.end(), byPath);
    for (int i = 0; i < left.entries.size(); ++i) {
        const PdfNotebookBundle::Entry &a = left.entries.at(i);
        const PdfNotebookBundle::Entry &b = right.entries.at(i);
        if (a.path != b.path || a.bytes != b.bytes || a.sha256 != b.sha256) {
            return false;
        }
    }
    return true;
}

void protectPath(const QString &path, QSet<QString> *protectedPaths)
{
    if (path.isEmpty()) {
        return;
    }
    protectedPaths->insert(QDir::fromNativeSeparators(path));
    /// A journal's added.txt can name a directory as well as a file. Protecting descendants for
    /// every recorded path is conservative and does not make the scan leave its known directories.
    protectedPaths->insert(QDir::fromNativeSeparators(path) + QLatin1Char('/'));
}

void protectManifest(const PdfSessionManifest &manifest, QSet<QString> *protectedPaths)
{
    protectPath(manifest.sourceFile, protectedPaths);
    for (int i = 0; i < manifest.sourceCount(); ++i) {
        protectPath(manifest.sourceAt(i).file, protectedPaths);
    }
    for (const PdfPageRecord &page : manifest.pages) {
        protectPath(page.kraFile, protectedPaths);
        protectPath(page.thumbFile, protectedPaths);
        protectPath(page.kraFile + QStringLiteral(".layers.txt"), protectedPaths);
        protectedPaths->insert(QDir::fromNativeSeparators(page.kraFile
                               + QStringLiteral(".layers/")));
    }
}

bool isProtected(const QString &path, const QSet<QString> &protectedPaths)
{
    if (protectedPaths.contains(path)) {
        return true;
    }
    for (const QString &reference : protectedPaths) {
        if (reference.endsWith(QLatin1Char('/')) && path.startsWith(reference)) {
            return true;
        }
    }
    return false;
}

bool addJournalReferences(const QString &journalDir,
                          QSet<QString> *protectedPaths,
                          QString *why)
{
    const QFileInfo journalInfo(journalDir);
    if (!journalInfo.exists()) {
        return true;
    }
    if (!journalInfo.isDir() || journalInfo.isSymLink()) {
        fail(why, QStringLiteral("undo data at %1 is not a plain directory; artifact collection was stopped")
                      .arg(journalDir));
        return false;
    }

    const QDir journal(journalDir);
    const QFileInfoList entries = journal.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                            | QDir::Hidden | QDir::System,
                                                        QDir::Name);
    if (entries.isEmpty()) {
        return true;
    }

    const QSet<QString> knownEntries = { QStringLiteral("before.json"), QStringLiteral("after.json"),
                                         QStringLiteral("op.json"), QStringLiteral("added.txt"),
                                         QStringLiteral("removed") };
    for (const QFileInfo &entry : entries) {
        if (!knownEntries.contains(entry.fileName())
            || (entry.fileName() == QLatin1String("removed")
                && (!entry.isDir() || entry.isSymLink()))) {
            fail(why, QStringLiteral("undo data at %1 has an unrecognized entry; artifact collection was stopped")
                          .arg(entry.absoluteFilePath()));
            return false;
        }
    }

    const QString beforePath = journal.filePath(QStringLiteral("before.json"));
    const QString afterPath = journal.filePath(QStringLiteral("after.json"));
    const QString addedPath = journal.filePath(QStringLiteral("added.txt"));
    const QString opPath = journal.filePath(QStringLiteral("op.json"));
    const auto isPlainFile = [](const QString &path) {
        const QFileInfo file(path);
        return file.isFile() && !file.isSymLink();
    };
    if (!QFileInfo::exists(beforePath) || !QFileInfo::exists(addedPath)
        || !QFileInfo::exists(opPath)
        || !isPlainFile(beforePath) || !isPlainFile(addedPath) || !isPlainFile(opPath)) {
        fail(why, QStringLiteral("undo data at %1 is incomplete; artifact collection was stopped")
                      .arg(journalDir));
        return false;
    }

    QString manifestWhy;
    const PdfSessionManifest before = PdfSessionManifest::readFrom(beforePath, &manifestWhy);
    if (!before.isValid(&manifestWhy)) {
        fail(why, QStringLiteral("undo data at %1 has an unreadable before-image: %2")
                      .arg(journalDir, manifestWhy));
        return false;
    }
    protectManifest(before, protectedPaths);

    if (QFileInfo::exists(afterPath)) {
        if (!isPlainFile(afterPath)) {
            fail(why, QStringLiteral("undo data at %1 has an unsafe after-image; artifact collection was stopped")
                          .arg(journalDir));
            return false;
        }
        manifestWhy.clear();
        const PdfSessionManifest after = PdfSessionManifest::readFrom(afterPath, &manifestWhy);
        if (!after.isValid(&manifestWhy)) {
            fail(why, QStringLiteral("undo data at %1 has an unreadable after-image: %2")
                          .arg(journalDir, manifestWhy));
            return false;
        }
        protectManifest(after, protectedPaths);
    }

    QFile added(addedPath);
    if (!added.open(QIODevice::ReadOnly)) {
        fail(why, QStringLiteral("undo data at %1 has an unreadable added-file list")
                      .arg(journalDir));
        return false;
    }
    const QList<QByteArray> lines = added.readAll().split('\n');
    for (const QByteArray &line : lines) {
        const QString path = QString::fromUtf8(line).trimmed();
        if (path.isEmpty()) {
            continue;
        }
        QString pathWhy;
        if (!PdfSessionManifest::isSafeRelativePath(path, &pathWhy)) {
            fail(why, QStringLiteral("undo data at %1 names an unsafe path (%2); artifact collection was stopped")
                          .arg(journalDir, path));
            return false;
        }
        protectPath(path, protectedPaths);
    }

    return true;
}

bool collectUndoReferences(const QString &projectDir, QSet<QString> *protectedPaths, QString *why)
{
    const QString opsPath = QDir(projectDir).filePath(QStringLiteral(".ops"));
    const QFileInfo opsInfo(opsPath);
    if (!opsInfo.exists()) {
        return true;
    }
    if (!opsInfo.isDir() || opsInfo.isSymLink()) {
        fail(why, QStringLiteral("the notebook's undo store is not a plain directory; artifact collection was stopped"));
        return false;
    }

    const QDir ops(opsPath);
    const QFileInfoList opsEntries = ops.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                           | QDir::Hidden | QDir::System,
                                                       QDir::Name);
    for (const QFileInfo &entry : opsEntries) {
        if ((entry.fileName() != QLatin1String("last")
             && entry.fileName() != QLatin1String("history"))
            || entry.isSymLink()) {
            fail(why, QStringLiteral("the notebook's undo store has an unrecognized entry at %1; artifact collection was stopped")
                          .arg(entry.absoluteFilePath()));
            return false;
        }
    }

    const QString active = QDir(opsPath).filePath(QStringLiteral("last"));
    if (!addJournalReferences(active, protectedPaths, why)) {
        return false;
    }

    const QString historyPath = QDir(opsPath).filePath(QStringLiteral("history"));
    const QFileInfo historyInfo(historyPath);
    if (!historyInfo.exists()) {
        return true;
    }
    if (!historyInfo.isDir() || historyInfo.isSymLink()) {
        fail(why, QStringLiteral("the notebook's undo history is not a plain directory; artifact collection was stopped"));
        return false;
    }

    const QDir history(historyPath);
    const QFileInfoList entries = history.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                           | QDir::Hidden | QDir::System,
                                                       QDir::Name);
    for (const QFileInfo &entry : entries) {
        bool numeric = false;
        entry.fileName().toULongLong(&numeric);
        if (!numeric || !entry.isDir() || entry.isSymLink()) {
            fail(why, QStringLiteral("undo history contains an unrecognized entry at %1; artifact collection was stopped")
                          .arg(entry.absoluteFilePath()));
            return false;
        }
        if (!addJournalReferences(entry.absoluteFilePath(), protectedPaths, why)) {
            return false;
        }
    }
    return true;
}

bool validGeneratedArtifactName(const QString &relative)
{
    static const QRegularExpression pageFile(QStringLiteral("^pages/p[0-9]{4,}\\.kra(?:\\.layers\\.txt)?$"));
    static const QRegularExpression sidecarImage(QStringLiteral("^pages/p[0-9]{4,}\\.kra\\.layers/[^/]+\\.png$"));
    static const QRegularExpression thumbnail(QStringLiteral("^thumbs/p[0-9]{4,}\\.png$"));
    static const QRegularExpression source(QStringLiteral("^sources/[^/]+\\.pdf$"),
                                           QRegularExpression::CaseInsensitiveOption);
    return pageFile.match(relative).hasMatch()
        || sidecarImage.match(relative).hasMatch()
        || thumbnail.match(relative).hasMatch()
        || source.match(relative).hasMatch();
}

QString uniqueRestoredDirectory(const QString &root, const QString &base)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString stem = base + QStringLiteral("-restored-") + stamp;
    QString candidate = QDir(root).filePath(stem);
    for (int suffix = 2; QFileInfo::exists(candidate) || QFileInfo(candidate).isSymLink(); ++suffix) {
        candidate = QDir(root).filePath(stem + QLatin1Char('-') + QString::number(suffix));
    }
    return candidate;
}

} // namespace

bool PdfNotebookBackup::createDailyBackup(const QString &projectDir,
                                          QString *backupPath,
                                          bool *created,
                                          QString *why)
{
    if (created) {
        *created = false;
    }
    if (backupPath) {
        backupPath->clear();
    }
    PdfSessionManifest manifest;
    if (!safeProjectManifest(projectDir, &manifest, why)) {
        return false;
    }

    QString localWhy;
    const QString directory = backupDirectory(projectDir, &localWhy);
    if (directory.isEmpty() || !ensureBackupDirectory(directory, &localWhy)) {
        fail(why, localWhy);
        return false;
    }

    const QString today = QDate::currentDate().toString(Qt::ISODate);
    const QString destination = QDir(directory).filePath(today + QStringLiteral("-daily.pnb"));
    if (QFileInfo::exists(destination)) {
        if (QFileInfo(destination).isSymLink()) {
            fail(why, QStringLiteral("today's backup path is a symbolic link: %1").arg(destination));
            return false;
        }
        if (!QFileInfo(destination).isFile()) {
            fail(why, QStringLiteral("today's backup path is not a plain file: %1").arg(destination));
            return false;
        }
        QString inspectWhy;
        const PdfNotebookBundle::Info existing = PdfNotebookBundle::inspect(destination, &inspectWhy);
        if (existing.isValid()) {
            if (backupPath) {
                *backupPath = destination;
            }
            if (!pruneSnapshots(projectDir, why)) {
                return false;
            }
            return true;
        }
        if (!QFile::remove(destination)) {
            fail(why, QStringLiteral("today's incomplete backup could not be replaced: %1")
                          .arg(destination));
            return false;
        }
    }

    if (!createVerifiedSnapshot(projectDir, destination, why)) {
        return false;
    }
    if (backupPath) {
        *backupPath = destination;
    }
    if (created) {
        *created = true;
    }
    return pruneSnapshots(projectDir, why);
}

bool PdfNotebookBackup::createSafetyBackup(const QString &projectDir,
                                           QString *backupPath,
                                           QString *why)
{
    if (backupPath) {
        backupPath->clear();
    }
    PdfSessionManifest manifest;
    if (!safeProjectManifest(projectDir, &manifest, why)) {
        return false;
    }

    QString localWhy;
    const QString directory = backupDirectory(projectDir, &localWhy);
    if (directory.isEmpty() || !ensureBackupDirectory(directory, &localWhy)) {
        fail(why, localWhy);
        return false;
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd-HHmmss-zzz"));
    const QString stem = stamp + QStringLiteral("-safety");
    QString destination = QDir(directory).filePath(stem + QStringLiteral(".pnb"));
    for (int suffix = 2; QFileInfo::exists(destination) || QFileInfo(destination).isSymLink(); ++suffix) {
        destination = QDir(directory).filePath(stem + QLatin1Char('-') + QString::number(suffix)
                                              + QStringLiteral(".pnb"));
    }

    if (!createVerifiedSnapshot(projectDir, destination, why)) {
        return false;
    }
    if (backupPath) {
        *backupPath = destination;
    }
    return pruneSnapshots(projectDir, why);
}

bool PdfNotebookBackup::ensureFreshSafetyBackup(const QString &projectDir,
                                                QString *backupPath,
                                                QString *why)
{
    if (backupPath) {
        backupPath->clear();
    }
    const QList<Snapshot> snapshots = validSnapshots(projectDir);
    if (snapshots.isEmpty()) {
        return createSafetyBackup(projectDir, backupPath, why);
    }

    QString localWhy;
    const QString directory = backupDirectory(projectDir, &localWhy);
    if (directory.isEmpty() || !ensureBackupDirectory(directory, &localWhy)) {
        fail(why, localWhy);
        return false;
    }

    QString probePath;
    do {
        probePath = QDir(directory).filePath(QStringLiteral(".current-check-")
                                             + QUuid::createUuid().toString(QUuid::WithoutBraces));
    } while (QFileInfo::exists(probePath) || QFileInfo(probePath).isSymLink());

    if (!createVerifiedSnapshot(projectDir, probePath, &localWhy)) {
        fail(why, QStringLiteral("the current notebook could not be checked against its latest backup: %1")
                      .arg(localWhy));
        return false;
    }

    QString currentWhy;
    const PdfNotebookBundle::Info current = PdfNotebookBundle::inspect(probePath, &currentWhy);
    QString latestWhy;
    const PdfNotebookBundle::Info latest = PdfNotebookBundle::inspect(snapshots.first().path, &latestWhy);
    QFile::remove(probePath);
    if (!current.isValid() || !latest.isValid()) {
        fail(why, QStringLiteral("the current notebook or its latest backup failed verification: %1 %2")
                      .arg(currentWhy, latestWhy));
        return false;
    }

    if (sameBundleContents(current, latest)) {
        if (!pruneSnapshots(projectDir, why)) {
            return false;
        }
        if (backupPath) {
            *backupPath = snapshots.first().path;
        }
        return true;
    }

    return createSafetyBackup(projectDir, backupPath, why);
}

QList<PdfNotebookBackup::Snapshot> PdfNotebookBackup::availableBackups(const QString &projectDir)
{
    return validSnapshots(projectDir);
}

bool PdfNotebookBackup::findOrphanedArtifacts(const QString &projectDir,
                                              QStringList *candidates,
                                              QString *why)
{
    if (candidates) {
        candidates->clear();
    }
    PdfSessionManifest manifest;
    if (!safeProjectManifest(projectDir, &manifest, why)) {
        return false;
    }

    QSet<QString> protectedPaths;
    protectManifest(manifest, &protectedPaths);
    if (!collectUndoReferences(projectDir, &protectedPaths, why)) {
        return false;
    }

    const QList<Snapshot> snapshots = validSnapshots(projectDir);
    if (snapshots.isEmpty()) {
        fail(why, QStringLiteral("there is no verified backup to protect artifact collection"));
        return false;
    }
    for (const Snapshot &snapshot : snapshots) {
        QString inspectWhy;
        const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(snapshot.path, &inspectWhy);
        if (!info.isValid()) {
            continue;
        }
        for (const PdfNotebookBundle::Entry &entry : info.entries) {
            protectPath(entry.path, &protectedPaths);
        }
    }

    QStringList found;
    QDirIterator it(projectDir, QDir::Files | QDir::NoSymLinks | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo file = it.fileInfo();
        const QString relative = QDir::fromNativeSeparators(QDir(projectDir).relativeFilePath(file.absoluteFilePath()));
        if (!validGeneratedArtifactName(relative) || isProtected(relative, protectedPaths)) {
            continue;
        }
        found.append(relative);
    }
    found.sort(Qt::CaseSensitive);
    if (candidates) {
        *candidates = found;
    }
    return true;
}

bool PdfNotebookBackup::removeOrphanedArtifacts(const QString &projectDir,
                                               const QStringList &confirmedCandidates,
                                               QStringList *removed,
                                               QString *why)
{
    if (removed) {
        removed->clear();
    }

    QStringList current;
    if (!findOrphanedArtifacts(projectDir, &current, why)) {
        return false;
    }
    QStringList confirmed = confirmedCandidates;
    confirmed.sort(Qt::CaseSensitive);
    if (confirmed != current) {
        fail(why, QStringLiteral("the artifact list changed after confirmation; nothing was removed"));
        return false;
    }

    const QString root = canonicalOrAbsolute(projectDir);
    for (const QString &relative : current) {
        QString pathWhy;
        if (!PdfSessionManifest::isSafeRelativePath(relative, &pathWhy)) {
            fail(why, QStringLiteral("refusing unsafe artifact path %1: %2").arg(relative, pathWhy));
            return false;
        }
        const QString absolute = QDir::cleanPath(QDir(projectDir).filePath(relative));
        if (!absolute.startsWith(root + QLatin1Char('/'))) {
            fail(why, QStringLiteral("refusing artifact outside the notebook: %1").arg(relative));
            return false;
        }
        const QFileInfo file(absolute);
        if (!file.isFile() || file.isSymLink() || !file.isWritable()) {
            fail(why, QStringLiteral("artifact %1 changed or cannot be safely removed; nothing was removed")
                          .arg(relative));
            return false;
        }
    }

    QStringList deleted;
    for (const QString &relative : current) {
        const QString absolute = QDir(projectDir).filePath(relative);
        if (!QFile::remove(absolute)) {
            if (removed) {
                *removed = deleted;
            }
            fail(why, QStringLiteral("could not remove %1; %2 earlier orphan(s) were removed")
                          .arg(relative).arg(deleted.size()));
            return false;
        }
        deleted.append(relative);
    }
    if (removed) {
        *removed = deleted;
    }
    return true;
}

bool PdfNotebookBackup::restoreAsNewNotebook(const QString &backupPath,
                                            QString *newProjectDir,
                                            QString *why)
{
    if (newProjectDir) {
        newProjectDir->clear();
    }
    const QFileInfo backup(backupPath);
    if (!backup.isFile() || backup.isSymLink()) {
        fail(why, QStringLiteral("the selected backup is not a plain file: %1").arg(backupPath));
        return false;
    }

    QString inspectWhy;
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(backupPath, &inspectWhy);
    if (!info.isValid()) {
        fail(why, QStringLiteral("the selected backup is not valid: %1").arg(inspectWhy));
        return false;
    }

    const QString root = PdfSession::projectRoot();
    if (root.isEmpty() || !QDir().mkpath(root)) {
        fail(why, QStringLiteral("the notebook store cannot be created at %1").arg(root));
        return false;
    }

    const QString destination = uniqueRestoredDirectory(root, PdfNotebookBundle::extractDirName(info.manifest));
    if (!PdfNotebookBundle::extract(backupPath, destination, why)) {
        return false;
    }
    if (newProjectDir) {
        *newProjectDir = destination;
    }
    return true;
}
