/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfNotebookBackup.h"
#include "session/PdfNotebookBundle.h"
#include "session/PdfSession.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QByteArray bytesFor(const QString &seed, int size = 256)
{
    QByteArray result;
    result.reserve(size);
    while (result.size() < size) {
        result.append(seed.toUtf8());
    }
    result.truncate(size);
    return result;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool makeNotebook(const QString &projectDir, PdfSessionManifest *out)
{
    const QByteArray source = bytesFor(QStringLiteral("source-pdf"), 1024);
    if (!writeBytes(QDir(projectDir).filePath(QStringLiteral("source.pdf")), source)
        || !writeBytes(QDir(projectDir).filePath(QStringLiteral("pages/p0001.kra")),
                       bytesFor(QStringLiteral("page-1")))
        || !writeBytes(QDir(projectDir).filePath(QStringLiteral("thumbs/p0001.png")),
                       bytesFor(QStringLiteral("thumb-1"), 64))) {
        return false;
    }

    PdfSessionManifest manifest;
    manifest.schema = PdfSessionManifest::CurrentSchema;
    manifest.sourceFile = QStringLiteral("source.pdf");
    manifest.sourceByteSize = source.size();
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(
        QDir(projectDir).filePath(QStringLiteral("source.pdf")));

    PdfPageRecord page;
    page.index = 0;
    page.sizePt = QSizeF(595, 842);
    page.kraFile = QStringLiteral("pages/p0001.kra");
    page.thumbFile = QStringLiteral("thumbs/p0001.png");
    manifest.pages.append(page);

    if (!manifest.writeTo(PdfSession::manifestPath(projectDir))) {
        return false;
    }
    if (out) {
        *out = manifest;
    }
    return true;
}

bool archiveHas(const PdfNotebookBundle::Info &info, const QString &path)
{
    for (const PdfNotebookBundle::Entry &entry : info.entries) {
        if (entry.path == path) {
            return true;
        }
    }
    return false;
}

bool addUndoJournal(const QString &journalDir,
                    const PdfSessionManifest &before,
                    const PdfSessionManifest &after,
                    const QStringList &added)
{
    if (!QDir().mkpath(journalDir)
        || !before.writeTo(QDir(journalDir).filePath(QStringLiteral("before.json")))
        || !after.writeTo(QDir(journalDir).filePath(QStringLiteral("after.json")))
        || !writeBytes(QDir(journalDir).filePath(QStringLiteral("op.json")), QByteArrayLiteral("{}"))) {
        return false;
    }

    QByteArray lines;
    for (const QString &path : added) {
        lines += path.toUtf8();
        lines += '\n';
    }
    return writeBytes(QDir(journalDir).filePath(QStringLiteral("added.txt")), lines);
}

class DocumentsOverrideReset
{
public:
    ~DocumentsOverrideReset()
    {
        PdfSession::setDocumentsLocationForTests(QString());
    }
};

} // namespace

class PdfNotebookBackupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void cleanupTestCase();
    void testDailyBackupIsOncePerDayAndCarriesAllPageArtifacts();
    void testSafetyBackupsRetainTheNewestThree();
    void testFreshSafetyBackupIsReusedUntilSavedStateChanges();
    void testCollectorPreviewsAndProtectsManifestUndoAndBackups();
    void testCollectorRequiresAVerifiedBackup();
    void testCollectorStopsWhenUndoDataIsUnknown();
    void testRestoreAlwaysCreatesANewNotebook();
};

void PdfNotebookBackupTest::cleanupTestCase()
{
    PdfSession::setDocumentsLocationForTests(QString());
    const QString backups = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("pdfio-backups"));
    QDir(backups).removeRecursively();
}

void PdfNotebookBackupTest::testDailyBackupIsOncePerDayAndCarriesAllPageArtifacts()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString project = temp.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));

    const QByteArray sidecarIndex = QByteArrayLiteral("layer 0 visible\n");
    const QByteArray sidecarImage = bytesFor(QStringLiteral("sidecar-layer"));
    const QByteArray asset = bytesFor(QStringLiteral("inserted-image"));
    QVERIFY(writeBytes(QDir(project).filePath(QStringLiteral("pages/p0001.kra.layers.txt")), sidecarIndex));
    QVERIFY(writeBytes(QDir(project).filePath(QStringLiteral("pages/p0001.kra.layers/0.png")), sidecarImage));
    QVERIFY(writeBytes(QDir(project).filePath(QStringLiteral("assets/scan.png")), asset));

    const QByteArray firstPage = readBytes(QDir(project).filePath(manifest.pages.first().kraFile));
    QString backupPath;
    QString why;
    bool created = false;
    QVERIFY2(PdfNotebookBackup::createDailyBackup(project, &backupPath, &created, &why), qPrintable(why));
    QVERIFY(created);
    QVERIFY(QFileInfo::exists(backupPath));
    QCOMPARE(QFileInfo(backupPath).fileName(),
             QDate::currentDate().toString(Qt::ISODate) + QStringLiteral("-daily.pnb"));

    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(backupPath, &why);
    QVERIFY2(info.isValid(), qPrintable(why));
    QVERIFY(archiveHas(info, QStringLiteral("pages/p0001.kra.layers.txt")));
    QVERIFY(archiveHas(info, QStringLiteral("pages/p0001.kra.layers/0.png")));
    QVERIFY(archiveHas(info, QStringLiteral("assets/scan.png")));

    const QString extracted = temp.filePath(QStringLiteral("roundtrip"));
    QVERIFY2(PdfNotebookBundle::extract(backupPath, extracted, &why), qPrintable(why));
    QCOMPARE(readBytes(QDir(extracted).filePath(QStringLiteral("pages/p0001.kra.layers.txt"))), sidecarIndex);
    QCOMPARE(readBytes(QDir(extracted).filePath(QStringLiteral("pages/p0001.kra.layers/0.png"))), sidecarImage);
    QCOMPARE(readBytes(QDir(extracted).filePath(QStringLiteral("assets/scan.png"))), asset);

    QVERIFY(writeBytes(QDir(project).filePath(manifest.pages.first().kraFile),
                       bytesFor(QStringLiteral("page-changed-later"))));
    QString sameDayPath;
    created = true;
    QVERIFY2(PdfNotebookBackup::createDailyBackup(project, &sameDayPath, &created, &why), qPrintable(why));
    QVERIFY(!created);
    QCOMPARE(sameDayPath, backupPath);

    const QString secondExtract = temp.filePath(QStringLiteral("second-roundtrip"));
    QVERIFY2(PdfNotebookBundle::extract(sameDayPath, secondExtract, &why), qPrintable(why));
    QCOMPARE(readBytes(QDir(secondExtract).filePath(manifest.pages.first().kraFile)), firstPage);
    QCOMPARE(PdfNotebookBackup::availableBackups(project).size(), 1);
}

void PdfNotebookBackupTest::testSafetyBackupsRetainTheNewestThree()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString project = temp.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));

    QStringList made;
    QString why;
    for (int i = 0; i < 5; ++i) {
        QVERIFY(writeBytes(QDir(project).filePath(manifest.pages.first().kraFile),
                           bytesFor(QStringLiteral("state-%1").arg(i))));
        QString backupPath;
        QVERIFY2(PdfNotebookBackup::createSafetyBackup(project, &backupPath, &why), qPrintable(why));
        made.append(backupPath);
        QTest::qWait(15);
    }

    const QList<PdfNotebookBackup::Snapshot> retained = PdfNotebookBackup::availableBackups(project);
    QCOMPARE(retained.size(), 3);
    QCOMPARE(retained.first().path, made.last());
    QVERIFY(!QFileInfo::exists(made.at(0)));
    QVERIFY(!QFileInfo::exists(made.at(1)));
    QVERIFY(QFileInfo::exists(made.at(2)));
    QVERIFY(QFileInfo::exists(made.at(3)));
    QVERIFY(QFileInfo::exists(made.at(4)));

    const QString restored = temp.filePath(QStringLiteral("newest"));
    QVERIFY2(PdfNotebookBundle::extract(retained.first().path, restored, &why), qPrintable(why));
    QCOMPARE(readBytes(QDir(restored).filePath(manifest.pages.first().kraFile)),
             bytesFor(QStringLiteral("state-4")));
}

void PdfNotebookBackupTest::testFreshSafetyBackupIsReusedUntilSavedStateChanges()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString project = temp.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));

    QString firstBackup;
    QString why;
    QVERIFY2(PdfNotebookBackup::createSafetyBackup(project, &firstBackup, &why), qPrintable(why));
    QVERIFY(writeBytes(QDir(project).filePath(QStringLiteral("pages/p0099.kra")),
                       bytesFor(QStringLiteral("unreferenced-file"))));

    QString reusableBackup;
    QVERIFY2(PdfNotebookBackup::ensureFreshSafetyBackup(project, &reusableBackup, &why), qPrintable(why));
    QCOMPARE(reusableBackup, firstBackup);
    QCOMPARE(PdfNotebookBackup::availableBackups(project).size(), 1);

    const QByteArray changedState = bytesFor(QStringLiteral("saved-state-changed"));
    QVERIFY(writeBytes(QDir(project).filePath(manifest.pages.first().kraFile), changedState));
    QString freshBackup;
    QVERIFY2(PdfNotebookBackup::ensureFreshSafetyBackup(project, &freshBackup, &why), qPrintable(why));
    QVERIFY(freshBackup != firstBackup);
    QCOMPARE(PdfNotebookBackup::availableBackups(project).size(), 2);

    const QString restored = temp.filePath(QStringLiteral("fresh-state"));
    QVERIFY2(PdfNotebookBundle::extract(freshBackup, restored, &why), qPrintable(why));
    QCOMPARE(readBytes(QDir(restored).filePath(manifest.pages.first().kraFile)), changedState);
}

void PdfNotebookBackupTest::testCollectorPreviewsAndProtectsManifestUndoAndBackups()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString project = temp.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));

    const QStringList orphanFiles = {
        QStringLiteral("pages/p0099.kra"),
        QStringLiteral("pages/p0099.kra.layers.txt"),
        QStringLiteral("pages/p0099.kra.layers/0.png"),
        QStringLiteral("thumbs/p0099.png"),
        QStringLiteral("sources/orphan.pdf")
    };
    for (const QString &relative : orphanFiles) {
        QVERIFY(writeBytes(QDir(project).filePath(relative), bytesFor(relative)));
    }

    const QStringList undoFiles = {
        QStringLiteral("pages/p0088.kra"),
        QStringLiteral("thumbs/p0088.png"),
        QStringLiteral("pages/p0089.kra"),
        QStringLiteral("pages/p0089.kra.layers/ink.png")
    };
    for (const QString &relative : undoFiles) {
        QVERIFY(writeBytes(QDir(project).filePath(relative), bytesFor(relative)));
    }
    const QString userFile = QStringLiteral("pages/my-notes.kra");
    const QString asset = QStringLiteral("assets/keep.png");
    QVERIFY(writeBytes(QDir(project).filePath(userFile), bytesFor(QStringLiteral("user-file"))));
    QVERIFY(writeBytes(QDir(project).filePath(asset), bytesFor(QStringLiteral("user-asset"))));

    QString backupPath;
    QString why;
    QVERIFY2(PdfNotebookBackup::createSafetyBackup(project, &backupPath, &why), qPrintable(why));

    PdfSessionManifest before = manifest;
    PdfPageRecord oldPage = manifest.pages.first();
    oldPage.index = 1;
    oldPage.kraFile = undoFiles.at(0);
    oldPage.thumbFile = undoFiles.at(1);
    before.pages.append(oldPage);
    const QString journal = QDir(project).filePath(QStringLiteral(".ops/last"));
    QVERIFY(addUndoJournal(journal, before, manifest,
                           {undoFiles.at(2), QStringLiteral("pages/p0089.kra.layers")}));

    QStringList preview;
    QVERIFY2(PdfNotebookBackup::findOrphanedArtifacts(project, &preview, &why), qPrintable(why));
    QString currentSafetyBackup;
    QVERIFY2(PdfNotebookBackup::ensureFreshSafetyBackup(project, &currentSafetyBackup, &why), qPrintable(why));
    QCOMPARE(currentSafetyBackup, backupPath);
    QCOMPARE(PdfNotebookBackup::availableBackups(project).size(), 1);
    QStringList expected = orphanFiles;
    expected.sort(Qt::CaseSensitive);
    QCOMPARE(preview, expected);
    for (const QString &relative : preview) {
        QVERIFY(QFileInfo::exists(QDir(project).filePath(relative)));
    }
    for (const QString &relative : undoFiles) {
        QVERIFY(!preview.contains(relative));
    }
    QVERIFY(!preview.contains(userFile));
    QVERIFY(!preview.contains(asset));

    const QString changedAfterPreview = QStringLiteral("thumbs/p0098.png");
    QVERIFY(writeBytes(QDir(project).filePath(changedAfterPreview), bytesFor(changedAfterPreview)));
    QStringList removed;
    QVERIFY(!PdfNotebookBackup::removeOrphanedArtifacts(project, preview, &removed, &why));
    QVERIFY(removed.isEmpty());
    QVERIFY(QFileInfo::exists(QDir(project).filePath(orphanFiles.first())));

    preview.append(changedAfterPreview);
    preview.sort(Qt::CaseSensitive);
    QVERIFY2(PdfNotebookBackup::removeOrphanedArtifacts(project, preview, &removed, &why), qPrintable(why));
    QCOMPARE(removed, preview);
    for (const QString &relative : preview) {
        QVERIFY(!QFileInfo::exists(QDir(project).filePath(relative)));
    }
    for (const QString &relative : undoFiles) {
        QVERIFY(QFileInfo::exists(QDir(project).filePath(relative)));
    }
    QVERIFY(QFileInfo::exists(QDir(project).filePath(manifest.pages.first().kraFile)));
    QVERIFY(QFileInfo::exists(QDir(project).filePath(userFile)));
    QVERIFY(QFileInfo::exists(QDir(project).filePath(asset)));
    QVERIFY(QFileInfo::exists(backupPath));
}

void PdfNotebookBackupTest::testCollectorStopsWhenUndoDataIsUnknown()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString project = temp.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));
    const QString orphan = QStringLiteral("pages/p0099.kra");
    QVERIFY(writeBytes(QDir(project).filePath(orphan), bytesFor(orphan)));

    QString backupPath;
    QString why;
    QVERIFY2(PdfNotebookBackup::createSafetyBackup(project, &backupPath, &why), qPrintable(why));
    const QString journal = QDir(project).filePath(QStringLiteral(".ops/last"));
    QVERIFY(addUndoJournal(journal, manifest, manifest, {}));
    QVERIFY(writeBytes(QDir(journal).filePath(QStringLiteral("future-format.bin")), QByteArrayLiteral("keep")));

    QStringList candidates;
    QVERIFY(!PdfNotebookBackup::findOrphanedArtifacts(project, &candidates, &why));
    QVERIFY(candidates.isEmpty());
    QVERIFY(why.contains(QStringLiteral("unrecognized entry")));
    QVERIFY(QFileInfo::exists(QDir(project).filePath(orphan)));
    QVERIFY(QFileInfo::exists(backupPath));
}

void PdfNotebookBackupTest::testCollectorRequiresAVerifiedBackup()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString project = temp.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));
    const QString orphan = QStringLiteral("pages/p0099.kra");
    QVERIFY(writeBytes(QDir(project).filePath(orphan), bytesFor(orphan)));

    QStringList candidates;
    QString why;
    QVERIFY(!PdfNotebookBackup::findOrphanedArtifacts(project, &candidates, &why));
    QVERIFY(candidates.isEmpty());
    QVERIFY(why.contains(QStringLiteral("verified backup")));
    QVERIFY(QFileInfo::exists(QDir(project).filePath(orphan)));
}

void PdfNotebookBackupTest::testRestoreAlwaysCreatesANewNotebook()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString documents = temp.filePath(QStringLiteral("documents"));
    QVERIFY(QDir().mkpath(documents));
    PdfSession::setDocumentsLocationForTests(documents);
    DocumentsOverrideReset resetDocumentsOverride;

    const QString project = temp.filePath(QStringLiteral("original"));
    PdfSessionManifest manifest;
    QVERIFY(makeNotebook(project, &manifest));
    const QString sidecar = QStringLiteral("pages/p0001.kra.layers/0.png");
    const QString asset = QStringLiteral("assets/scan.png");
    QVERIFY(writeBytes(QDir(project).filePath(sidecar), bytesFor(sidecar)));
    QVERIFY(writeBytes(QDir(project).filePath(asset), bytesFor(asset)));
    const QByteArray originalManifest = readBytes(PdfSession::manifestPath(project));
    const QByteArray originalPage = readBytes(QDir(project).filePath(manifest.pages.first().kraFile));

    QString backupPath;
    QString why;
    QVERIFY2(PdfNotebookBackup::createSafetyBackup(project, &backupPath, &why), qPrintable(why));

    QString firstRestore;
    QVERIFY2(PdfNotebookBackup::restoreAsNewNotebook(backupPath, &firstRestore, &why), qPrintable(why));
    QVERIFY(firstRestore != project);
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(firstRestore)));
    QCOMPARE(readBytes(PdfSession::manifestPath(firstRestore)), originalManifest);
    QCOMPARE(readBytes(QDir(firstRestore).filePath(manifest.pages.first().kraFile)), originalPage);
    QCOMPARE(readBytes(QDir(firstRestore).filePath(sidecar)), bytesFor(sidecar));
    QCOMPARE(readBytes(QDir(firstRestore).filePath(asset)), bytesFor(asset));

    QString secondRestore;
    QVERIFY2(PdfNotebookBackup::restoreAsNewNotebook(backupPath, &secondRestore, &why), qPrintable(why));
    QVERIFY(secondRestore != firstRestore);
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(firstRestore)));
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(secondRestore)));
    QCOMPARE(readBytes(PdfSession::manifestPath(project)), originalManifest);
    QCOMPARE(readBytes(QDir(project).filePath(manifest.pages.first().kraFile)), originalPage);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("PerfectNote"));
    QCoreApplication::setApplicationName(QStringLiteral("PdfNotebookBackupTest"));
    QStandardPaths::setTestModeEnabled(true);

    PdfNotebookBackupTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PdfNotebookBackupTest.moc"
