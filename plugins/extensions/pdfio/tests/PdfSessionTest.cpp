/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfNotebookOps.h"
#include "session/PdfSession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest>

namespace {

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(bytes);
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll();
}

/**
 * A rotator that turns nothing: it writes a file the test can recognise at the destination, plus the
 * sidecar a page save writes, and remembers what it was asked for.
 *
 * What is under test here is the OPERATION -- the journal, the order of the swaps and atomicity --
 * so a stub is enough. That the rotation itself puts the ink where the paper went is checked
 * against a real artifact in PdfNavigatorIntegrationTest, with PdfPageRotator doing the turning.
 */
struct StubRotator {
    struct Call {
        QString source;
        QString destination;
        int degrees = 0;
    };

    QList<Call> calls;
    bool fail = false;

    PdfNotebookOps::ArtifactRotator fn()
    {
        return [this](const QString &source, const QString &destination, int degrees, QString *why) {
            calls.append(Call{source, destination, degrees});
            if (fail) {
                /// Written and THEN failed, so the operation has something to clean up: a rotator
                /// that leaves nothing behind would pass a test of the cleanup by doing nothing.
                writeBytes(destination, QByteArrayLiteral("half a turn"));
                writeBytes(destination + QStringLiteral(".layers.txt"),
                           QByteArrayLiteral("half a sidecar"));
                if (why) {
                    *why = QStringLiteral("the stub was asked to fail");
                }
                return false;
            }
            writeBytes(destination, QByteArrayLiteral("turned"));
            writeBytes(destination + QStringLiteral(".layers.txt"), QByteArrayLiteral("turned sidecar"));
            writeBytes(destination + QStringLiteral(".layers/Ink.png"), QByteArrayLiteral("turned layer"));
            return true;
        };
    }
};

} // namespace

/**
 * The manifest is the one durable description of a note project, and the source checksum is
 * what makes "the file underneath me changed" detectable instead of silently wrong. Both are
 * pure logic and belong in ctest.
 */
class PdfSessionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testCreateProject();
    void testSourceIsCopiedUnchanged();
    void testManifestRoundTrip();
    void testRefusesToClobber();
    void testDetectsChangedSource();
    void testRejectsBadManifest();

    /// The manifest as the one durable description of a notebook: a write that fails must leave the
    /// one that was there, the schema-2 fields must round-trip, a notebook made before them must
    /// still open, and the artifact-number counter must never hand a number out twice.
    void testManifestWriteLeavesTheOldOneWhenItFails();
    void testLegacyManifestWithoutSourcesIsUpgradedOnRead();
    void testSourcesRoundTripAndTheFirstMirrorsTheLegacyFields();
    void testAPageCannotNameASourceTheManifestDoesNotHave();
    void testThePageNumberAllocatorNeverReusesANumber();
    void testDisplaySizeFollowsTheNotebooksOwnRotation();
    void testOpenProjectVerifiesEverySource();

    /// The notebook-level operations, as changes to the files on disk: what they change, what they
    /// keep, what they refuse, and that a change which cannot be committed leaves nothing behind.
    void testMovingAPageChangesOnlyTheManifest();
    void testDeletingAPageJournalsItsFiles();
    void testDeletingTheLastPageIsRefused();
    void testDuplicatingAPageCopiesItsArtifacts();
    void testAnOperationThatCannotCommitChangesNothing();
    void testUndoUndoesTheLastChangeOnly();
    void testTheExportGuardRefusesAPageListThatIsNotTheSourcesOrder();

    /// Inserting pages from a PDF: it becomes a source of the notebook (copied into the project
    /// once, reused after that), every inserted page gets its own artifact number, and nothing is
    /// written for a page until it is drawn on.
    void testInsertingPagesFromAnotherPdf();
    void testUndoingAnInsertTakesTheCopiedPdfWithIt();
    void testInsertingFromTheNotebooksOwnPdfReusesItsSource();
    void testInsertingRefusesAPageRangeThePdfDoesNotHave();

    /// Turning pages: the manifest records the turn, the ink is turned with the paper, a page that
    /// was never drawn on is the manifest's alone, and a turn that cannot be made changes nothing.
    void testRotatingAPageTurnsTheManifestAndTheArtifact();
    void testRotatingPagesWithoutArtifactsIsManifestOnly();
    void testARotationThatFailsChangesNothing();

    /// Extracting a range: a notebook of its own beside the one it came from, carrying the sources
    /// and the files its pages need, with the original untouched.
    void testExtractingARangeMakesANotebook();
    void testExtractingARangeThatSpansTwoSourcesKeepsBoth();
    void testExtractingRefusesADestinationOrARangeThatCannotBeMade();

    // Where a manifest's own file names are checked: once, at the boundary, so that no consumer
    // has to, and so a directory copied onto the machine cannot make one read or write outside it.
    void testRejectsEscapingManifestPaths();
    void testAcceptsLegitimateFileNames();
    void testEmptyThumbnailStaysLegal();
    void testOpenProjectRefusesAnEscapingManifest();
    void testPathInsideProject();

    // Where the notebook folder is, and what happens to an older one when it moves.
    void testProjectRootIsUnderDocuments();
    void testProjectRootFallsBackWhenDocumentsIsUnusable();
    void testLegacyNotebookIsMovedAndStillOpens();
    void testFailedMoveLeavesTheLegacyNotebookIntact();
    void testMigratedManifestKeepsRelativePaths();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }

    /// A manifest that is valid in every way except whatever a case changes: one page, ordinary
    /// names, a checksum that is only ever compared against itself in these cases.
    PdfSessionManifest baseManifest() const
    {
        PdfSessionManifest manifest;
        manifest.schema = PdfSessionManifest::CurrentSchema;
        manifest.sourceFile = QStringLiteral("source.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("deadbeef");
        manifest.sourceByteSize = 4;
        manifest.pages.append(PdfPageRecord{0, QSizeF(595, 842), 0,
                                            QStringLiteral("pages/p0001.kra"),
                                            QStringLiteral("thumbs/p0001.png"), 0});
        return manifest;
    }

    /**
     * The parent a case's Documents directory is made under: beside the legacy root, so both are
     * on one filesystem.
     *
     * The migration is an atomic rename, and rename(2) cannot cross a filesystem boundary. /tmp is
     * usually a different one (tmpfs here), so a QTemporaryDir there would fail every migration
     * case for a reason that has nothing to do with the policy. Production puts Documents and the
     * app data directory under the same home, which is one filesystem.
     */
    QString temporaryDocumentsParent() const
    {
        const QString parent =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                .filePath(QStringLiteral("documents"));
        QDir().mkpath(parent);
        return parent;
    }

    /// A notebook built at the legacy root, which is the state a migration starts from. Returns an
    /// empty string on failure, with the reason in \a why.
    QString makeLegacyNotebook(const QString &name, QString *why)
    {
        const QString dir = QDir(PdfSession::legacyProjectRoot()).filePath(name);
        QDir(dir).removeRecursively();

        PopplerRenderBackend backend;
        const PdfSessionManifest manifest =
            PdfSession::createProject(dir, fixturePath(QStringLiteral("text-fixture.pdf")), backend, why);
        return manifest.isValid(why) ? dir : QString();
    }
};

void PdfSessionTest::initTestCase()
{
    /// The legacy root is AppDataLocation/pdfio-projects. Test mode keeps that under ~/.qttest, so
    /// a migration case cannot touch a real notebook store.
    QStandardPaths::setTestModeEnabled(true);
}

void PdfSessionTest::cleanupTestCase()
{
    PdfSession::setDocumentsLocationForTests(QString());
    QDir(QDir(PdfSession::legacyProjectRoot()).absoluteFilePath(QStringLiteral("../documents")))
        .removeRecursively();
    QDir(PdfSession::legacyProjectRoot()).removeRecursively();
}

void PdfSessionTest::testCreateProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    QString why;
    const PdfSessionManifest manifest =
        PdfSession::createProject(dir.filePath(QStringLiteral("project")), fixturePath(QStringLiteral("text-fixture.pdf")), backend, &why);

    QVERIFY2(manifest.isValid(&why), qPrintable(why));
    QCOMPARE(manifest.sourceFile, QStringLiteral("text-fixture.pdf"));
    QCOMPARE(manifest.pages.size(), 3);

    /// Geometry comes from the backend, so the rotated page is recorded rotated.
    QCOMPARE(manifest.pages.at(0).sizePt, QSizeF(595, 842));
    QCOMPARE(manifest.pages.at(0).rotation, 0);
    QCOMPARE(manifest.pages.at(1).sizePt, QSizeF(420, 595));
    QCOMPARE(manifest.pages.at(1).rotation, 90);
    QCOMPARE(manifest.pages.at(2).sizePt, QSizeF(300, 300));

    QCOMPARE(manifest.pages.at(0).kraFile, QStringLiteral("pages/p0001.kra"));
    QCOMPARE(manifest.pages.at(2).thumbFile, QStringLiteral("thumbs/p0003.png"));
}

void PdfSessionTest::testSourceIsCopiedUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const PdfSessionManifest manifest =
        PdfSession::createProject(dir.filePath(QStringLiteral("project")), fixturePath(QStringLiteral("text-fixture.pdf")), backend);

    const QString copied = PdfSession::sourcePath(dir.filePath(QStringLiteral("project")), manifest.sourceFile);
    QVERIFY(QFileInfo::exists(copied));

    /// The source is immutable: the copy has to be byte for byte the original.
    QCOMPARE(PdfSessionManifest::sha256OfFile(copied),
             PdfSessionManifest::sha256OfFile(fixturePath(QStringLiteral("text-fixture.pdf"))));
    QCOMPARE(QFileInfo(copied).size(), manifest.sourceByteSize);
}

void PdfSessionTest::testManifestRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest written =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);

    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(project)));

    QString why;
    const PdfSessionManifest read = PdfSession::openProject(project, &why);
    QVERIFY2(read.isValid(&why), qPrintable(why));
    QCOMPARE(read.toJson(), written.toJson());
    QCOMPARE(read.sourceSha256, written.sourceSha256);
    QCOMPARE(read.sourceByteSize, written.sourceByteSize);
}

void PdfSessionTest::testRefusesToClobber()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend).isValid());

    QString why;
    const PdfSessionManifest second =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend, &why);
    QVERIFY(!second.isValid());
    QVERIFY(why.contains(QStringLiteral("not empty")));
}

void PdfSessionTest::testDetectsChangedSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    PopplerRenderBackend backend;
    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);

    const QString source = PdfSession::sourcePath(project, manifest.sourceFile);
    {
        QFile file(source);
        QVERIFY(file.open(QIODevice::Append));
        file.write(" ");
    }

    QString why;
    QVERIFY(!PdfSession::openProject(project, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("changed")), qPrintable(why));
}

void PdfSessionTest::testRejectsBadManifest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("manifest.json"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not json");
    }

    QString why;
    QVERIFY(!PdfSessionManifest::readFrom(path, &why).isValid());
    QVERIFY(!why.isEmpty());

    PdfSessionManifest wrongSchema;
    wrongSchema.schema = 999;
    wrongSchema.sourceFile = QStringLiteral("x.pdf");
    wrongSchema.sourceSha256 = QByteArrayLiteral("deadbeef");
    wrongSchema.pages.append(PdfPageRecord{0, QSizeF(10, 10), 0, QStringLiteral("pages/p0001.kra"), QString(), 0});
    QVERIFY(!wrongSchema.isValid(&why));
    QVERIFY(why.contains(QStringLiteral("schema")));
}

/**
 * A manifest write that fails leaves the manifest that was already there, byte for byte.
 *
 * The manifest is the one durable description of a notebook: the page list, the file each page's
 * ink lives in, its geometry. A write that truncates the file and then stops -- a full disk, a
 * crash, a rename that cannot be made -- leaves something that is neither the old manifest nor the
 * new one, and everything the notebook holds is described only by it. The write now goes through a
 * temporary file that is renamed into place; this drives the one moment that matters, after the new
 * content exists and before it is put in place.
 */
void PdfSessionTest::testManifestWriteLeavesTheOldOneWhenItFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("manifest.json"));

    const PdfSessionManifest original = baseManifest();
    QString why;
    QVERIFY2(original.writeTo(path, &why), qPrintable(why));

    QFile before(path);
    QVERIFY(before.open(QIODevice::ReadOnly));
    const QByteArray beforeBytes = before.readAll();
    before.close();
    QVERIFY(!beforeBytes.isEmpty());

    PdfSessionManifest replacement = original;
    replacement.name = QStringLiteral("a name a failed write must not leave behind");
    replacement.pages[0].kraFile = QStringLiteral("pages/p0002.kra");

    PdfSessionManifest::setFailBeforeCommitForTests(true);
    const bool written = replacement.writeTo(path, &why);
    PdfSessionManifest::setFailBeforeCommitForTests(false);

    QVERIFY2(!written, "the write reported success although it was made to fail");
    QVERIFY(!why.isEmpty());

    QFile after(path);
    QVERIFY(after.open(QIODevice::ReadOnly));
    QCOMPARE(after.readAll(), beforeBytes);
    after.close();

    const PdfSessionManifest readBack = PdfSessionManifest::readFrom(path, &why);
    QVERIFY2(readBack.isValid(&why), qPrintable(why));
    QCOMPARE(readBack.name, original.name);
    QCOMPARE(readBack.pages.at(0).kraFile, original.pages.at(0).kraFile);

    /// And a write that is allowed to finish still lands, so the guard above is not simply refusing
    /// everything.
    QVERIFY2(replacement.writeTo(path, &why), qPrintable(why));
    QCOMPARE(PdfSessionManifest::readFrom(path, &why).name, replacement.name);
}

/**
 * A notebook written before sources[] existed still opens, and is upgraded in memory.
 *
 * The file a schema 1 build left has one source object, pages with no source and no extra rotation,
 * and no artifact-number counter. Reading it must fill all of that in without rewriting the file:
 * a notebook that is only opened is not changed on disk, and the next write is what makes it this
 * schema.
 */
void PdfSessionTest::testLegacyManifestWithoutSourcesIsUpgradedOnRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("manifest.json"));

    const QByteArray legacy = R"({
    "schema": 1,
    "name": "lecture notes",
    "source": { "file": "text-fixture.pdf", "sha256": "deadbeef", "bytes": 4 },
    "pages": [
        { "index": 0, "sizePt": [595, 842], "rotation": 0, "kra": "pages/p0001.kra", "thumb": "thumbs/p0001.png", "generation": 1 },
        { "index": 1, "sizePt": [420, 595], "rotation": 90, "kra": "pages/p0002.kra", "thumb": "thumbs/p0002.png", "generation": 0 }
    ]
})";
    writeBytes(path, legacy);

    QString why;
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(path, &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    QCOMPARE(manifest.schema, PdfSessionManifest::CurrentSchema);
    QCOMPARE(manifest.sourceCount(), 1);
    QCOMPARE(manifest.sourceAt(0).file, QStringLiteral("text-fixture.pdf"));
    QCOMPARE(manifest.sourceAt(0).sha256, QByteArrayLiteral("deadbeef"));
    QCOMPARE(manifest.sourceAt(0).byteSize, qint64(4));
    QCOMPARE(manifest.pages.size(), 2);
    for (const PdfPageRecord &page : manifest.pages) {
        QCOMPARE(page.source, 0);
        QCOMPARE(page.extraRotation, 0);
    }
    QCOMPARE(manifest.nextPageNumber, 3);

    /// Reading did not write: the file on disk is still the schema 1 one.
    QFile raw(path);
    QVERIFY(raw.open(QIODevice::ReadOnly));
    const QJsonObject onDisk = QJsonDocument::fromJson(raw.readAll()).object();
    raw.close();
    QCOMPARE(onDisk.value(QStringLiteral("schema")).toInt(), 1);

    /// The next write is what turns it into this schema, and the result is this build's own shape.
    QVERIFY2(manifest.writeTo(path, &why), qPrintable(why));
    const PdfSessionManifest upgraded = PdfSessionManifest::readFrom(path, &why);
    QVERIFY2(upgraded.isValid(&why), qPrintable(why));
    QCOMPARE(upgraded.toJson(), manifest.toJson());
    QCOMPARE(upgraded.toJson().value(QStringLiteral("schema")).toInt(),
             PdfSessionManifest::CurrentSchema);
    QCOMPARE(upgraded.toJson().value(QStringLiteral("sources")).toArray().size(), 1);
}

/**
 * More than one source round-trips, pages say which one they come from, and the first entry keeps
 * agreeing with the three fields every older reader uses.
 */
void PdfSessionTest::testSourcesRoundTripAndTheFirstMirrorsTheLegacyFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("manifest.json"));

    PdfSessionManifest manifest = baseManifest();
    PdfSourceRecord first;
    first.file = manifest.sourceFile;
    first.sha256 = manifest.sourceSha256;
    first.byteSize = manifest.sourceByteSize;
    PdfSourceRecord second;
    second.file = QStringLiteral("sources/9f3a1c02-handout.pdf");
    second.sha256 = QByteArrayLiteral("cafebabe");
    second.byteSize = 99;
    manifest.sources << first << second;
    manifest.pages[0].source = 1;
    manifest.nextPageNumber = 7;
    QVERIFY(manifest.isValid());

    QCOMPARE(manifest.sourceCount(), 2);
    QCOMPARE(manifest.sourceForPage(manifest.pages.at(0)).file, second.file);
    QCOMPARE(manifest.sourceIndexForSha(QByteArrayLiteral("cafebabe")), 1);
    QCOMPARE(manifest.sourceIndexForSha(QByteArrayLiteral("not-a-source")), -1);

    QString why;
    QVERIFY2(manifest.writeTo(path, &why), qPrintable(why));
    const PdfSessionManifest read = PdfSessionManifest::readFrom(path, &why);
    QVERIFY2(read.isValid(&why), qPrintable(why));
    QCOMPARE(read.toJson(), manifest.toJson());
    QCOMPARE(read.sourceCount(), 2);
    QCOMPARE(read.pages.at(0).source, 1);
    QCOMPARE(read.nextPageNumber, 7);

    /// The list and the legacy object describe different PDFs: refused rather than read two ways,
    /// because an older reader would follow the legacy object and a newer one the list.
    PdfSessionManifest mismatched = read;
    mismatched.sourceFile = QStringLiteral("some-other.pdf");
    why.clear();
    QVERIFY(!mismatched.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("first source")), qPrintable(why));

    /// A source that is not a file inside the project is refused exactly like the source itself.
    PdfSessionManifest escaping = read;
    escaping.sources[1].file = QStringLiteral("../outside.pdf");
    why.clear();
    QVERIFY(!escaping.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("inside the project")), qPrintable(why));
}

void PdfSessionTest::testAPageCannotNameASourceTheManifestDoesNotHave()
{
    PdfSessionManifest manifest = baseManifest();
    manifest.pages[0].source = 1;

    QString why;
    QVERIFY(!manifest.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("source")), qPrintable(why));
}

/**
 * The artifact-number counter never hands out a number the page list already names.
 *
 * A number freed by deleting a page must not come back: an artifact left behind by a failed delete
 * would be read back as the ink of a page that never had any, which is a silent resurrection of
 * somebody else's strokes.
 */
void PdfSessionTest::testThePageNumberAllocatorNeverReusesANumber()
{
    PdfSessionManifest manifest = baseManifest();
    manifest.pages[0].kraFile = QStringLiteral("pages/p0007.kra");
    manifest.nextPageNumber = 0;

    QCOMPARE(manifest.allocatePageNumber(), 8);
    QCOMPARE(manifest.nextPageNumber, 9);

    manifest.pages.append(PdfPageRecord{1, QSizeF(10, 10), 0,
                                        PdfSession::pageFileNameForNumber(8), QString(), 0, 0, 0});
    QCOMPARE(manifest.allocatePageNumber(), 9);
    QCOMPARE(manifest.nextPageNumber, 10);

    /// A counter a hand edit lowered, or one that came from an older file, is raised past what the
    /// page list already names instead of being trusted.
    manifest.nextPageNumber = 2;
    QCOMPARE(manifest.allocatePageNumber(), 9);
    QCOMPARE(manifest.nextPageNumber, 10);

    /// A name that is not a numbered artifact is none of this counter's business.
    manifest.pages.append(PdfPageRecord{2, QSizeF(10, 10), 0, QStringLiteral("pages/renamed-ink.kra"),
                                        QString(), 0, 0, 0});
    QCOMPARE(manifest.allocatePageNumber(), 10);
    QCOMPARE(manifest.nextPageNumber, 11);
}

void PdfSessionTest::testDisplaySizeFollowsTheNotebooksOwnRotation()
{
    PdfPageRecord page{0, QSizeF(595, 842), 0, QStringLiteral("pages/p0001.kra"), QString(), 0, 0, 0};
    QCOMPARE(page.displaySizePt(), QSizeF(595, 842));

    page.extraRotation = 90;
    QCOMPARE(page.displaySizePt(), QSizeF(842, 595));
    page.extraRotation = 270;
    QCOMPARE(page.displaySizePt(), QSizeF(842, 595));
    page.extraRotation = 180;
    QCOMPARE(page.displaySizePt(), QSizeF(595, 842));

    /// sizePt is what the file declares and stays that whatever the notebook records, or the two
    /// would drift and the record would stop surviving a renderer change.
    QCOMPARE(page.sizePt, QSizeF(595, 842));

    /// Only quarter turns are legal: anything else is a manifest this code refuses rather than a
    /// size it guesses.
    PdfSessionManifest manifest = baseManifest();
    manifest.pages[0].extraRotation = 45;
    QString why;
    QVERIFY(!manifest.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("quarter turn")), qPrintable(why));
}

/**
 * Opening a notebook checks every source, not only its first.
 *
 * A page whose background can only come from the second PDF would otherwise be rendered blank --
 * or, worse, from whatever the first file happens to hold -- with nothing said. A second source
 * that is missing, or that was replaced under the notebook, is a refusal with a reason.
 */
void PdfSessionTest::testOpenProjectVerifiesEverySource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    PdfSessionManifest manifest =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(manifest.isValid());
    QCOMPARE(manifest.sourceCount(), 1);

    const QString extra = QStringLiteral("sources/9f3a1c02-handout.pdf");
    QVERIFY(QDir(project).mkpath(QStringLiteral("sources")));
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("text-fixture.pdf")),
                        QDir(project).filePath(extra)));

    PdfSourceRecord second;
    second.file = extra;
    second.sha256 = PdfSessionManifest::sha256OfFile(QDir(project).filePath(extra));
    second.byteSize = QFileInfo(QDir(project).filePath(extra)).size();
    manifest.sources << second;

    PdfPageRecord fromSecond = manifest.pages.at(1);
    fromSecond.source = 1;
    fromSecond.kraFile = PdfSession::pageFileNameForNumber(manifest.allocatePageNumber());
    fromSecond.thumbFile.clear();
    manifest.pages.append(fromSecond);

    QString why;
    QVERIFY2(manifest.writeTo(PdfSession::manifestPath(project), &why), qPrintable(why));
    QVERIFY2(PdfSession::openProject(project, &why).isValid(&why), qPrintable(why));

    /// The second source goes away: the notebook has a page that can only be drawn from it.
    QVERIFY(QFile::remove(QDir(project).filePath(extra)));
    why.clear();
    QVERIFY(!PdfSession::openProject(project, &why).isValid());
    QVERIFY2(why.contains(extra), qPrintable(why));

    /// And a second source that was replaced under the notebook is refused for the same reason the
    /// first one would be.
    QVERIFY(QFile::copy(fixturePath(QStringLiteral("text-fixture.pdf")),
                        QDir(project).filePath(extra)));
    {
        QFile file(QDir(project).filePath(extra));
        QVERIFY(file.open(QIODevice::Append));
        file.write(" ");
    }
    why.clear();
    QVERIFY(!PdfSession::openProject(project, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("changed")), qPrintable(why));
}

/**
 * Moving a page changes the manifest and nothing on disk.
 *
 * The records carry their own file names, so a reorder is one atomic manifest write: no artifact is
 * renamed, no sidecar is touched, and the page a record points at keeps its ink. That is also why
 * the operation can be undone by putting one file back.
 */
void PdfSessionTest::testMovingAPageChangesOnlyTheManifest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());
    QCOMPARE(before.pages.size(), 3);

    /// An artifact for the page that moves, so "nothing moved" is a statement about a file.
    const QString artifact = QDir(project).filePath(before.pages.at(0).kraFile);
    const QByteArray ink = QByteArrayLiteral("the first page's ink");
    writeBytes(artifact, ink);

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::movePage(project, 0, 2, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    QCOMPARE(outcome.anchorPage, 2);

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.size(), 3);
    QCOMPARE(after.pages.at(2).kraFile, before.pages.at(0).kraFile);
    QCOMPARE(after.pages.at(0).kraFile, before.pages.at(1).kraFile);
    QCOMPARE(after.pages.at(1).kraFile, before.pages.at(2).kraFile);

    /// The artifact is where it was, with the same bytes.
    QVERIFY(QFileInfo::exists(artifact));
    QCOMPARE(readBytes(artifact), ink);

    QVERIFY(PdfNotebookOps::canUndo(project));
    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));
    /// The reader goes back to the page they were reading, which is where they were when the
    /// change was made.
    QCOMPARE(undo.anchorPage, 0);
    QCOMPARE(PdfSession::openProject(project, &why).toJson(), before.toJson());
    QVERIFY(!PdfNotebookOps::canUndo(project));
}

/**
 * Deleting a page takes its files out of the notebook and keeps them in the journal.
 *
 * Nothing is deleted: the artifact, its sidecar and the thumbnail are moved aside, which is what
 * makes the change reversible and what keeps a crash from taking the ink with it.
 */
void PdfSessionTest::testDeletingAPageJournalsItsFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());

    const PdfPageRecord second = before.pages.at(1);
    const QByteArray ink = QByteArrayLiteral("ink of page 2");
    const QByteArray sidecar = QByteArrayLiteral("a sidecar layer");
    const QByteArray thumb = QByteArrayLiteral("thumb of page 2");
    writeBytes(QDir(project).filePath(second.kraFile), ink);
    writeBytes(QDir(project).filePath(second.kraFile + QStringLiteral(".layers/Inserted image.png")), sidecar);
    writeBytes(QDir(project).filePath(second.thumbFile), thumb);

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::deletePages(project, 1, 1, 1);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    /// The page that took its place is where the reader lands: the deletion does not move them.
    QCOMPARE(outcome.anchorPage, 1);

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.size(), 2);
    QCOMPARE(after.pages.at(1).kraFile, before.pages.at(2).kraFile);

    /// Gone from the notebook...
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(second.kraFile)));
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(second.thumbFile)));

    /// ...and in the journal.
    const QString journal = PdfNotebookOps::journalDir(project);
    const QString removed = QDir(journal).filePath(QStringLiteral("removed"));
    QVERIFY(QFileInfo::exists(QDir(removed).filePath(second.kraFile)));
    QVERIFY(QFileInfo::exists(QDir(removed).filePath(second.thumbFile)));
    QVERIFY(QFileInfo::exists(
        QDir(removed).filePath(second.kraFile + QStringLiteral(".layers/Inserted image.png"))));

    /// The undo puts every byte back where it was.
    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));
    QCOMPARE(undo.anchorPage, 1);
    QCOMPARE(PdfSession::openProject(project, &why).toJson(), before.toJson());
    QCOMPARE(readBytes(QDir(project).filePath(second.kraFile)), ink);
    QCOMPARE(readBytes(QDir(project).filePath(second.thumbFile)), thumb);
    QCOMPARE(readBytes(QDir(project).filePath(second.kraFile + QStringLiteral(".layers/Inserted image.png"))),
             sidecar);
    QVERIFY(!QFileInfo::exists(journal));
}

void PdfSessionTest::testDeletingTheLastPageIsRefused()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());
    const QByteArray manifestBefore = readBytes(PdfSession::manifestPath(project));

    /// Every page: a notebook with none is not a notebook, and the manifest refuses it.
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::deletePages(project, 0, 3, 0);
    QVERIFY(!outcome.ok);
    QVERIFY2(outcome.why.contains(QStringLiteral("last page")), qPrintable(outcome.why));

    QCOMPARE(readBytes(PdfSession::manifestPath(project)), manifestBefore);
    QVERIFY(!PdfNotebookOps::canUndo(project));

    /// One page short of that is allowed, so the refusal is the last page and not "delete".
    QVERIFY2(PdfNotebookOps::deletePages(project, 0, 2, 0).ok, "two of three pages must be deletable");
}

/**
 * A duplicate is a page of its own: its own artifact, its own sidecar, its own preview, copied
 * before the manifest that names them is committed.
 */
void PdfSessionTest::testDuplicatingAPageCopiesItsArtifacts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());

    const PdfPageRecord original = before.pages.at(0);
    writeBytes(QDir(project).filePath(original.kraFile), QByteArrayLiteral("ink of page 1"));
    writeBytes(QDir(project).filePath(original.kraFile + QStringLiteral(".layers/Inserted image.png")),
               QByteArrayLiteral("a sidecar layer"));
    writeBytes(QDir(project).filePath(original.thumbFile), QByteArrayLiteral("thumb of page 1"));

    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::duplicatePage(project, 0, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    /// The reader stays on the page they duplicated.
    QCOMPARE(outcome.anchorPage, 0);

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.size(), 4);
    QCOMPARE(after.pages.at(0).kraFile, original.kraFile);

    const PdfPageRecord copy = after.pages.at(1);
    QVERIFY(copy.kraFile != original.kraFile);
    /// The number comes from the allocator, past every number the page list already names: three
    /// pages name 1..3, so the copy is 4.
    QCOMPARE(copy.kraFile, PdfSession::pageFileNameForNumber(4));
    QCOMPARE(copy.thumbFile, PdfSession::thumbFileNameForNumber(4));

    QCOMPARE(readBytes(QDir(project).filePath(copy.kraFile)),
             readBytes(QDir(project).filePath(original.kraFile)));
    QCOMPARE(readBytes(QDir(project).filePath(copy.thumbFile)),
             readBytes(QDir(project).filePath(original.thumbFile)));
    QCOMPARE(readBytes(QDir(project).filePath(copy.kraFile + QStringLiteral(".layers/Inserted image.png"))),
             QByteArrayLiteral("a sidecar layer"));

    /// The undo takes the copy away and leaves the original exactly as it was.
    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));
    QCOMPARE(PdfSession::openProject(project, &why).toJson(), before.toJson());
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(copy.kraFile)));
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(copy.kraFile + QStringLiteral(".layers"))));
    QCOMPARE(readBytes(QDir(project).filePath(original.kraFile)), QByteArrayLiteral("ink of page 1"));
}

/**
 * An operation that cannot commit its manifest leaves the notebook exactly as it was.
 *
 * The commit is the manifest write, and it comes after every file the change creates -- so the
 * failure here is the interesting one: the copies are already on disk. They are rolled back, and no
 * undo is offered for a change that did not happen.
 */
void PdfSessionTest::testAnOperationThatCannotCommitChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());
    const QString artifact = QDir(project).filePath(before.pages.at(0).kraFile);
    writeBytes(artifact, QByteArrayLiteral("ink of page 1"));
    const QByteArray manifestBefore = readBytes(PdfSession::manifestPath(project));

    PdfSessionManifest::setFailBeforeCommitForTests(true);
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::duplicatePage(project, 0, 0);
    PdfSessionManifest::setFailBeforeCommitForTests(false);

    QVERIFY2(!outcome.ok, "the duplicate was reported as done although the manifest was not committed");
    QVERIFY(!outcome.why.isEmpty());

    /// The manifest is the one that was there, byte for byte.
    QCOMPARE(readBytes(PdfSession::manifestPath(project)), manifestBefore);

    /// The copy the operation had made is gone, and so is its journal: there is no change to undo.
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(PdfSession::pageFileNameForNumber(4))));
    QVERIFY(!QFileInfo::exists(PdfNotebookOps::journalDir(project)));
    QVERIFY(!PdfNotebookOps::canUndo(project));

    /// And the original is untouched.
    QCOMPARE(readBytes(artifact), QByteArrayLiteral("ink of page 1"));
}

/**
 * Undo is one change deep, and it is the LAST change: a second operation replaces what the first
 * one left to undo.
 */
void PdfSessionTest::testUndoUndoesTheLastChangeOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    PdfSessionManifest start =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(start.isValid());

    QVERIFY(PdfNotebookOps::movePage(project, 0, 2, 0).ok);
    QString why;
    const PdfSessionManifest afterMove = PdfSession::openProject(project, &why);
    QVERIFY2(afterMove.isValid(&why), qPrintable(why));

    QVERIFY(PdfNotebookOps::duplicatePage(project, 1, 1).ok);
    const PdfSessionManifest afterDuplicate = PdfSession::openProject(project, &why);
    QCOMPARE(afterDuplicate.pages.size(), 4);

    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));

    /// Back to the state after the move -- not to the notebook before it.
    QCOMPARE(PdfSession::openProject(project, &why).toJson(), afterMove.toJson());
    QVERIFY(!PdfNotebookOps::canUndo(project));
    QCOMPARE(PdfSession::openProject(project, &why).pages.size(), 3);
}

/**
 * The export guard: ink is placed by list position, so the export is only correct while the
 * notebook's page N is the PDF's page N. Everything else is refused, with the reason.
 *
 * This is what keeps "move a page, then export" from writing a PDF whose marks are on the wrong
 * sheets -- a file that looks right and is not. Stage G's page-tree rebuild is what will replace it.
 */
void PdfSessionTest::testTheExportGuardRefusesAPageListThatIsNotTheSourcesOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest untouched =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(untouched.isValid());

    QString why;
    QVERIFY2(PdfNotebookOps::exportIsOrderPreserving(untouched, &why), qPrintable(why));

    /// A move: notebook page 1 now holds PDF page 3.
    PdfSessionManifest moved = untouched;
    moved.pages.move(0, 2);
    why.clear();
    QVERIFY2(!PdfNotebookOps::exportIsOrderPreserving(moved, &why),
             "a moved notebook was allowed to export");
    QVERIFY2(why.contains(QStringLiteral("wrong page")), qPrintable(why));
    /// [P2, P3, P1] after the move, so notebook page 1 now holds PDF page 2 -- and the message says
    /// which page of the PDF it holds, so the user can see what would have happened.
    QVERIFY2(why.contains(QStringLiteral("notebook page 1")) && why.contains(QStringLiteral("page 2 of the PDF")),
             qPrintable(why));

    /// A delete out of the middle: the pages after it are no longer at their own positions.
    PdfSessionManifest deleted = untouched;
    deleted.pages.removeAt(1);
    why.clear();
    QVERIFY(!PdfNotebookOps::exportIsOrderPreserving(deleted, &why));
    QVERIFY2(why.contains(QStringLiteral("notebook page 2")), qPrintable(why));

    /// A duplicate: two notebook pages would name one PDF page.
    PdfSessionManifest duplicated = untouched;
    duplicated.pages.insert(1, duplicated.pages.at(0));
    QVERIFY(!PdfNotebookOps::exportIsOrderPreserving(duplicated, &why));

    /// A page from another PDF.
    PdfSessionManifest twoSources = untouched;
    PdfSourceRecord first;
    first.file = twoSources.sourceFile;
    first.sha256 = twoSources.sourceSha256;
    first.byteSize = twoSources.sourceByteSize;
    PdfSourceRecord second;
    second.file = QStringLiteral("sources/other.pdf");
    second.sha256 = QByteArrayLiteral("cafebabe");
    second.byteSize = 1;
    twoSources.sources.clear();
    twoSources.sources << first << second;
    twoSources.pages[0].source = 1;
    why.clear();
    QVERIFY(!PdfNotebookOps::exportIsOrderPreserving(twoSources, &why));
    QVERIFY2(why.contains(QStringLiteral("PDFs")), qPrintable(why));

    /// A notebook that is the source's first pages, in order, is still exportable: an untouched
    /// page is copied through, which is what the export has always done.
    PdfSessionManifest shorter = untouched;
    shorter.pages.removeLast();
    why.clear();
    QVERIFY2(PdfNotebookOps::exportIsOrderPreserving(shorter, &why), qPrintable(why));
}

/**
 * Inserting pages from another PDF copies that PDF into the notebook once, records where each page
 * came from, and gives every inserted page its own artifact number.
 *
 * This is what makes a notebook more than one PDF's pages: a page names the source its background
 * is rendered from, so the file has to travel with the notebook, and the same PDF inserted a second
 * time must not put a second copy of its bytes in the project.
 */
void PdfSessionTest::testInsertingPagesFromAnotherPdf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend own;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), own);
    QVERIFY(before.isValid());
    QCOMPARE(before.pages.size(), 3);
    QCOMPARE(before.sourceCount(), 1);

    const QString otherPath = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend other;
    QVERIFY(other.open(otherPath));
    QVERIFY(other.pageCount() >= 2);

    /// Two pages of it, at position 1: after the first page of the notebook.
    const PdfNotebookOps::Outcome outcome =
        PdfNotebookOps::insertPages(project, 1, otherPath, other, 0, 2, 1);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    /// The reader was on page 2, two pages arrived before it, so that page is now page 4.
    QCOMPARE(outcome.anchorPage, 3);

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.size(), 5);
    QCOMPARE(after.sourceCount(), 2);

    /// The PDF is inside the project, with the bytes and the checksum the manifest records.
    const PdfSourceRecord source = after.sourceAt(1);
    QVERIFY2(source.file.startsWith(QStringLiteral("sources/")), qPrintable(source.file));
    QVERIFY(QFileInfo::exists(QDir(project).filePath(source.file)));
    QCOMPARE(PdfSessionManifest::sha256OfFile(QDir(project).filePath(source.file)),
             PdfSessionManifest::sha256OfFile(otherPath));

    /// Each inserted page says which source it is from and which page of it it is, with the
    /// geometry the renderer reports -- which is what puts the background back under the ink.
    for (int i = 0; i < 2; ++i) {
        const PdfPageRecord page = after.pages.at(1 + i);
        QCOMPARE(page.source, 1);
        QCOMPARE(page.index, i);
        QCOMPARE(page.sizePt, other.pageInfo(i).sizePt);
        QCOMPARE(page.rotation, other.pageInfo(i).rotation);
    }

    /// Its own number from the allocator, and no artifact yet: a page that was never drawn on has
    /// none, which is what keeps a notebook proportional to what was written on it.
    QCOMPARE(after.pages.at(1).kraFile, PdfSession::pageFileNameForNumber(4));
    QCOMPARE(after.pages.at(2).kraFile, PdfSession::pageFileNameForNumber(5));
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(after.pages.at(1).kraFile)));

    /// The same PDF inserted again: one source, not two, and one copy of its bytes.
    const PdfNotebookOps::Outcome again =
        PdfNotebookOps::insertPages(project, 5, otherPath, other, 0, 1, 0);
    QVERIFY2(again.ok, qPrintable(again.why));
    const PdfSessionManifest twice = PdfSession::openProject(project, &why);
    QCOMPARE(twice.sourceCount(), 2);
    QCOMPARE(twice.pages.size(), 6);
    QCOMPARE(twice.pages.at(5).source, 1);
    QCOMPARE(twice.pages.at(5).index, 0);

    /// Undo is one change deep, so this undoes the SECOND insert and leaves the first: five pages
    /// again, and the copied PDF still there, because the pages that came with it still draw on it.
    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));
    const PdfSessionManifest back = PdfSession::openProject(project, &why);
    QVERIFY2(back.isValid(&why), qPrintable(why));
    QCOMPARE(back.pages.size(), 5);
    QCOMPARE(back.sourceCount(), 2);
    QVERIFY2(QFileInfo::exists(QDir(project).filePath(source.file)),
             "an undo took a copied source away while pages still draw on it");
}

/**
 * Undoing the insert that brought a PDF in takes the copied PDF away with it: the copy was that
 * change's, so nothing else in the notebook references it afterwards.
 */
void PdfSessionTest::testUndoingAnInsertTakesTheCopiedPdfWithIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend own;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), own);
    QVERIFY(before.isValid());

    const QString otherPath = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend other;
    QVERIFY(other.open(otherPath));

    const PdfNotebookOps::Outcome outcome =
        PdfNotebookOps::insertPages(project, 0, otherPath, other, 0, 2, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));

    QString why;
    const PdfSourceRecord source = PdfSession::openProject(project, &why).sourceAt(1);
    QVERIFY(QFileInfo::exists(QDir(project).filePath(source.file)));

    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));

    QCOMPARE(PdfSession::openProject(project, &why).toJson(), before.toJson());
    QVERIFY2(!QFileInfo::exists(QDir(project).filePath(source.file)),
             "the copied PDF stayed behind after the insert that brought it in was undone");
}

/**
 * Inserting pages of the notebook's own PDF adds no source and copies nothing.
 */
void PdfSessionTest::testInsertingFromTheNotebooksOwnPdfReusesItsSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());

    /// The copy inside the project, which is the file whose pages the notebook draws from today.
    const QString copied = PdfSession::sourcePath(project, before.sourceFile);
    PopplerRenderBackend own;
    QVERIFY(own.open(copied));

    const PdfNotebookOps::Outcome outcome =
        PdfNotebookOps::insertPages(project, 3, copied, own, 0, 1, 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.size(), 4);
    QCOMPARE(after.sourceCount(), 1);
    QCOMPARE(after.pages.at(3).source, 0);
    QCOMPARE(after.pages.at(3).index, 0);

    /// Nothing was copied: the same content already has a home in this project.
    QVERIFY2(!QFileInfo::exists(QDir(project).filePath(QStringLiteral("sources"))),
             "a copy was made of a PDF the notebook already has");
}

void PdfSessionTest::testInsertingRefusesAPageRangeThePdfDoesNotHave()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend own;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), own);
    QVERIFY(before.isValid());
    const QByteArray manifestBefore = readBytes(PdfSession::manifestPath(project));

    const QString otherPath = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend other;
    QVERIFY(other.open(otherPath));
    QVERIFY(other.pageCount() >= 2);

    /// More pages than the file has: refused whole, with nothing copied and nothing journalled.
    const PdfNotebookOps::Outcome outcome =
        PdfNotebookOps::insertPages(project, 0, otherPath, other, 1, 100, 0);
    QVERIFY(!outcome.ok);
    QVERIFY2(outcome.why.contains(QStringLiteral("page")), qPrintable(outcome.why));

    QCOMPARE(readBytes(PdfSession::manifestPath(project)), manifestBefore);
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(QStringLiteral("sources"))));
    QVERIFY(!PdfNotebookOps::canUndo(project));
}

/**
 * Turning a page records the turn in the manifest, turns the ink with the paper, and keeps the
 * original in the journal.
 *
 * The paper is turned at render time from extraRotation; the ink cannot be, because it is pixels in
 * the artifact -- so the artifact is turned through the same journalled, committed path as every
 * other operation. What is checked here is the ordering and the books: the record's rotation, the
 * turned file in place, the original in the journal, and an undo that puts all of it back.
 */
void PdfSessionTest::testRotatingAPageTurnsTheManifestAndTheArtifact()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());

    const PdfPageRecord page = before.pages.at(0);
    const QString artifact = QDir(project).filePath(page.kraFile);
    const QByteArray ink = QByteArrayLiteral("the page's ink");
    writeBytes(artifact, ink);
    writeBytes(QDir(project).filePath(page.kraFile + QStringLiteral(".layers.txt")),
               QByteArrayLiteral("the sidecar"));
    writeBytes(QDir(project).filePath(page.thumbFile), QByteArrayLiteral("the preview"));

    StubRotator rotator;
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::rotatePages(project, 0, 1, 90, rotator.fn(), 0);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    /// Turning a page is not a move: the reader stays on it.
    QCOMPARE(outcome.anchorPage, 0);

    /// The rotator was asked for the artifact, into a name beside it, by the turn that was asked for.
    QCOMPARE(rotator.calls.size(), 1);
    QCOMPARE(rotator.calls.at(0).source, artifact);
    QCOMPARE(rotator.calls.at(0).destination, artifact + QStringLiteral(".rotating"));
    QCOMPARE(rotator.calls.at(0).degrees, 90);

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    QCOMPARE(after.pages.at(0).extraRotation, 90);
    /// sizePt is what the file declares and does not change; what the reader sees does.
    QCOMPARE(after.pages.at(0).sizePt, page.sizePt);
    QCOMPARE(after.pages.at(0).displaySizePt(), QSizeF(page.sizePt.height(), page.sizePt.width()));

    /// The turned artifact is in place, its sidecar with it, and the original is in the journal.
    QCOMPARE(readBytes(artifact), QByteArrayLiteral("turned"));
    QCOMPARE(readBytes(QDir(project).filePath(page.kraFile + QStringLiteral(".layers.txt"))),
             QByteArrayLiteral("turned sidecar"));
    QVERIFY(QFileInfo::exists(QDir(project).filePath(page.kraFile + QStringLiteral(".layers/Ink.png"))));

    const QString removed = QDir(PdfNotebookOps::journalDir(project)).filePath(QStringLiteral("removed"));
    QCOMPARE(readBytes(QDir(removed).filePath(page.kraFile)), ink);
    QCOMPARE(readBytes(QDir(removed).filePath(page.kraFile + QStringLiteral(".layers.txt"))),
             QByteArrayLiteral("the sidecar"));

    /// The preview is a picture of the page as it was: dropped rather than left showing the old
    /// orientation.
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(page.thumbFile)));

    /// And the undo puts the ink, its sidecar, the preview and the rotation back.
    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));
    QCOMPARE(PdfSession::openProject(project, &why).toJson(), before.toJson());
    QCOMPARE(readBytes(artifact), ink);
    QCOMPARE(readBytes(QDir(project).filePath(page.kraFile + QStringLiteral(".layers.txt"))),
             QByteArrayLiteral("the sidecar"));
    QCOMPARE(readBytes(QDir(project).filePath(page.thumbFile)), QByteArrayLiteral("the preview"));
    QVERIFY(!QFileInfo::exists(QDir(project).filePath(page.kraFile + QStringLiteral(".rotating"))));
}

/**
 * A page that was never drawn on has no artifact, so its turn is the manifest's alone: the rotator
 * is never asked for it, and nothing is written for it.
 */
void PdfSessionTest::testRotatingPagesWithoutArtifactsIsManifestOnly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());
    QCOMPARE(before.pages.size(), 3);
    for (const PdfPageRecord &page : before.pages) {
        QVERIFY(!QFileInfo::exists(QDir(project).filePath(page.kraFile)));
    }

    /// A range, and a left turn: 270 is what a -90 is recorded as.
    StubRotator rotator;
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::rotatePages(project, 0, 3, -90, rotator.fn(), 1);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));
    QCOMPARE(outcome.anchorPage, 1);
    QVERIFY2(rotator.calls.isEmpty(), "the rotator was asked to turn a page that has no artifact");

    QString why;
    const PdfSessionManifest after = PdfSession::openProject(project, &why);
    QVERIFY2(after.isValid(&why), qPrintable(why));
    for (const PdfPageRecord &page : after.pages) {
        QCOMPARE(page.extraRotation, 270);
    }

    /// Nothing was written for those pages, and nothing is left over.
    const QDir pages(QDir(project).filePath(QStringLiteral("pages")));
    QVERIFY2(pages.entryList(QStringList() << QStringLiteral("*.rotating*")).isEmpty(),
             "a rotation left a temporary file behind");

    /// One undo puts every turn back.
    const PdfNotebookOps::Outcome undo = PdfNotebookOps::undoLast(project);
    QVERIFY2(undo.ok, qPrintable(undo.why));
    QCOMPARE(PdfSession::openProject(project, &why).toJson(), before.toJson());
}

/**
 * A turn that cannot be made refuses the whole operation: the manifest is the one that was there,
 * the artifact is untouched, and the rotator's half-written temporary is cleaned up.
 */
void PdfSessionTest::testARotationThatFailsChangesNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());

    /// The artifact is on the second page, so the range passes over an empty page first.
    const PdfPageRecord page = before.pages.at(1);
    const QString artifact = QDir(project).filePath(page.kraFile);
    writeBytes(artifact, QByteArrayLiteral("the second page's ink"));
    const QByteArray manifestBefore = readBytes(PdfSession::manifestPath(project));

    StubRotator rotator;
    rotator.fail = true;
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::rotatePages(project, 0, 3, 90, rotator.fn(), 0);
    QVERIFY2(!outcome.ok, "a failed rotation was reported as done");
    QVERIFY(!outcome.why.isEmpty());
    QCOMPARE(rotator.calls.size(), 1);

    /// Nothing changed: the manifest, the artifact, and no journal to undo.
    QCOMPARE(readBytes(PdfSession::manifestPath(project)), manifestBefore);
    QCOMPARE(readBytes(artifact), QByteArrayLiteral("the second page's ink"));
    QVERIFY(!QFileInfo::exists(PdfNotebookOps::journalDir(project)));
    QVERIFY(!PdfNotebookOps::canUndo(project));
    /// And the half-written turn is gone.
    QVERIFY(!QFileInfo::exists(artifact + QStringLiteral(".rotating")));
    QVERIFY(!QFileInfo::exists(artifact + QStringLiteral(".rotating.layers.txt")));
}

/**
 * Extracting a range writes a notebook of its own: the pages, the sources they are drawn from and
 * the files they need, and nothing else. The notebook it came from is not touched.
 */
void PdfSessionTest::testExtractingARangeMakesANotebook()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    const PdfSessionManifest before =
        PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend);
    QVERIFY(before.isValid());
    QCOMPARE(before.pages.size(), 3);

    /// Files for the two pages that will be taken -- an artifact with known bytes, a sidecar layer
    /// and a preview -- and for the page that stays behind, so "untouched" is about more than the
    /// manifest.
    const PdfPageRecord first = before.pages.at(0);
    const PdfPageRecord second = before.pages.at(1);
    const PdfPageRecord left = before.pages.at(2);
    writeBytes(QDir(project).filePath(first.kraFile), QByteArrayLiteral("ink of page 1"));
    writeBytes(QDir(project).filePath(first.kraFile + QStringLiteral(".layers/Inserted image.png")),
               QByteArrayLiteral("a layer"));
    writeBytes(QDir(project).filePath(first.thumbFile), QByteArrayLiteral("preview of page 1"));
    /// Page 2 is left WITHOUT an artifact on purpose: a page that was never drawn on has none, and
    /// the extracted notebook has to carry that hole rather than invent a file for it.
    writeBytes(QDir(project).filePath(left.kraFile), QByteArrayLiteral("ink of page 3"));
    const QByteArray manifestBefore = readBytes(PdfSession::manifestPath(project));

    const QString destination = dir.filePath(QStringLiteral("range-notebook"));
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::extractRange(project, 0, 2, destination);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));

    /// A notebook of its own, that opens.
    QString why;
    const PdfSessionManifest extracted = PdfSession::openProject(destination, &why);
    QVERIFY2(extracted.isValid(&why), qPrintable(why));
    QCOMPARE(extracted.pages.size(), 2);
    QCOMPARE(extracted.sourceCount(), 1);
    QCOMPARE(extracted.name, QStringLiteral("range-notebook"));
    QCOMPARE(extracted.pages.at(0).kraFile, first.kraFile);
    QCOMPARE(extracted.pages.at(1).kraFile, second.kraFile);

    /// The files came with it, byte for byte, and so did the source they are drawn against.
    QCOMPARE(readBytes(QDir(destination).filePath(first.kraFile)), QByteArrayLiteral("ink of page 1"));
    QCOMPARE(readBytes(QDir(destination).filePath(first.thumbFile)),
             QByteArrayLiteral("preview of page 1"));
    QCOMPARE(readBytes(QDir(destination).filePath(first.kraFile
                                                  + QStringLiteral(".layers/Inserted image.png"))),
             QByteArrayLiteral("a layer"));
    QVERIFY(QFileInfo::exists(QDir(destination).filePath(extracted.sourceFile)));
    /// And the page that was never drawn on came across as a page with no artifact, not as one
    /// with a file nothing wrote.
    QVERIFY(!QFileInfo::exists(QDir(destination).filePath(second.kraFile)));

    /// The page that was not extracted did not travel, and nothing where it was changed.
    QVERIFY(!QFileInfo::exists(QDir(destination).filePath(left.kraFile)));
    QCOMPARE(readBytes(QDir(project).filePath(left.kraFile)), QByteArrayLiteral("ink of page 3"));
    QCOMPARE(readBytes(PdfSession::manifestPath(project)), manifestBefore);

    /// Nothing was left beside the new notebook: it was built in a directory of its own and moved
    /// into place, not assembled where it now is.
    const QStringList leftovers =
        QDir(dir.path()).entryList(QStringList() << QStringLiteral(".range-notebook-extracting-*"));
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QStringLiteral(", "))));

    /// A page that was never drawn on has no artifact, and a range that holds it is no less valid
    /// for that: the new notebook is exactly the pages that were asked for.
    const QString lone = dir.filePath(QStringLiteral("lone-page"));
    QVERIFY2(PdfNotebookOps::extractRange(project, 1, 1, lone).ok,
             "one page of a three page notebook could not be extracted");
    const PdfSessionManifest single = PdfSession::openProject(lone, &why);
    QVERIFY2(single.isValid(&why), qPrintable(why));
    QCOMPARE(single.pages.size(), 1);
    QVERIFY(!QFileInfo::exists(QDir(lone).filePath(single.pages.at(0).kraFile)));
}

/**
 * A range that spans two PDFs produces a notebook that carries both, and its pages say which one
 * they come from -- the first page of the range decides which source is the new notebook's own.
 */
void PdfSessionTest::testExtractingARangeThatSpansTwoSourcesKeepsBoth()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend own;
    QVERIFY(PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), own).isValid());

    /// A page from another PDF, in the middle, so the range below spans both sources and starts
    /// with the inserted one.
    const QString otherPath = fixturePath(QStringLiteral("ex-rotations.pdf"));
    PopplerRenderBackend other;
    QVERIFY(other.open(otherPath));
    const PdfNotebookOps::Outcome inserted =
        PdfNotebookOps::insertPages(project, 1, otherPath, other, 0, 1, 0);
    QVERIFY2(inserted.ok, qPrintable(inserted.why));

    QString why;
    const PdfSessionManifest withTwo = PdfSession::openProject(project, &why);
    QVERIFY2(withTwo.isValid(&why), qPrintable(why));
    QCOMPARE(withTwo.sourceCount(), 2);
    QCOMPARE(withTwo.pages.size(), 4);

    const QString destination = dir.filePath(QStringLiteral("span"));
    const PdfNotebookOps::Outcome outcome = PdfNotebookOps::extractRange(project, 1, 3, destination);
    QVERIFY2(outcome.ok, qPrintable(outcome.why));

    const PdfSessionManifest extracted = PdfSession::openProject(destination, &why);
    QVERIFY2(extracted.isValid(&why), qPrintable(why));
    QCOMPARE(extracted.pages.size(), 3);
    QCOMPARE(extracted.sourceCount(), 2);

    /// The first page of the range came from the second source, so that one is the new notebook's
    /// own source, and the other page's record was remapped to follow it.
    QCOMPARE(extracted.sourceFile, withTwo.sourceAt(1).file);
    QCOMPARE(extracted.pages.at(0).source, 0);
    QCOMPARE(extracted.pages.at(0).index, withTwo.pages.at(1).index);
    QCOMPARE(extracted.pages.at(1).source, 1);
    QCOMPARE(extracted.pages.at(1).index, withTwo.pages.at(2).index);

    /// Both PDFs are inside the new notebook, at the names the manifest records: opening it checks
    /// every one of them, which is what makes a copy that did not arrive intact impossible to miss.
    QVERIFY(QFileInfo::exists(QDir(destination).filePath(extracted.sourceFile)));
    QVERIFY(QFileInfo::exists(QDir(destination).filePath(extracted.sourceAt(1).file)));
}

/**
 * A range that is not in the notebook, and a destination that is already someone's notebook, are
 * both refused -- and the second one is refused without a byte written into it.
 */
void PdfSessionTest::testExtractingRefusesADestinationOrARangeThatCannotBeMade()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    PopplerRenderBackend backend;
    QVERIFY(PdfSession::createProject(project, fixturePath(QStringLiteral("text-fixture.pdf")), backend).isValid());

    QVERIFY(!PdfNotebookOps::extractRange(project, 2, 5, dir.filePath(QStringLiteral("outside"))).ok);
    QVERIFY(!PdfNotebookOps::extractRange(project, 0, 0, dir.filePath(QStringLiteral("nothing"))).ok);
    QVERIFY(!PdfNotebookOps::extractRange(project, 0, 1, QString()).ok);

    /// A destination that already holds something is a decision for a person: refused, and left
    /// exactly as it was.
    const QString taken = dir.filePath(QStringLiteral("taken"));
    QVERIFY(QDir().mkpath(taken));
    writeBytes(QDir(taken).filePath(QStringLiteral("keep.txt")), QByteArrayLiteral("do not touch"));

    const PdfNotebookOps::Outcome refused = PdfNotebookOps::extractRange(project, 0, 1, taken);
    QVERIFY(!refused.ok);
    QVERIFY2(refused.why.contains(QStringLiteral("already there")), qPrintable(refused.why));
    QCOMPARE(readBytes(QDir(taken).filePath(QStringLiteral("keep.txt"))), QByteArrayLiteral("do not touch"));
    QVERIFY(!QFileInfo::exists(QDir(taken).filePath(QStringLiteral("manifest.json"))));

    /// With the decision made explicit, the same destination is replaced -- and what was there is
    /// gone, which is what "replace" means.
    PdfNotebookOps::ExtractOptions options;
    options.replaceExisting = true;
    const PdfNotebookOps::Outcome replaced =
        PdfNotebookOps::extractRange(project, 0, 1, taken, options);
    QVERIFY2(replaced.ok, qPrintable(replaced.why));
    QVERIFY(QFileInfo::exists(QDir(taken).filePath(QStringLiteral("manifest.json"))));
    QVERIFY(!QFileInfo::exists(QDir(taken).filePath(QStringLiteral("keep.txt"))));
    QString why;
    QVERIFY2(PdfSession::openProject(taken, &why).isValid(&why), qPrintable(why));
}

/**
 * The notebook folder is somewhere the user can see, and older notebooks have to survive the move.
 *
 * The Documents location is pointed at a temporary directory for these cases; the policy reads it
 * through PdfSession::setDocumentsLocationForTests(), which nothing in the plugin calls.
 */
void PdfSessionTest::testProjectRootIsUnderDocuments()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    const QString root = PdfSession::projectRoot();

    /// One folder directly under Documents -- no longer the app-private root, and not a path
    /// spelled out here, because the folder's name is the policy's to choose.
    QCOMPARE(QFileInfo(root).absolutePath(), QDir(documents.path()).absolutePath());
    QVERIFY(!QFileInfo(root).fileName().isEmpty());
    QVERIFY(root != PdfSession::legacyProjectRoot());

    /// And that is where a notebook that exists nowhere yet is about to be made.
    QCOMPARE(PdfSession::migrateFromLegacy(QStringLiteral("brand-new")),
             QDir(root).filePath(QStringLiteral("brand-new")));

    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testProjectRootFallsBackWhenDocumentsIsUnusable()
{
    /// A Documents location that is not there at all.
    const QString missing = QDir(QDir::tempPath()).filePath(QStringLiteral("pdfsession-nothing-here"));
    QDir(missing).removeRecursively();
    QVERIFY(!QFileInfo::exists(missing));
    PdfSession::setDocumentsLocationForTests(missing);
    QCOMPARE(PdfSession::projectRoot(), PdfSession::legacyProjectRoot());

    /// And one that is there and cannot be written to. This case would not hold for a run as root,
    /// where nothing is unwritable.
    QTemporaryDir unwritable;
    QVERIFY(unwritable.isValid());
    QVERIFY(QFile::setPermissions(unwritable.path(),
                                  QFile::ReadOwner | QFile::ReadUser
                                      | QFile::ExeOwner | QFile::ExeUser));
    QVERIFY(!QFileInfo(unwritable.path()).isWritable());
    PdfSession::setDocumentsLocationForTests(unwritable.path());
    QCOMPARE(PdfSession::projectRoot(), PdfSession::legacyProjectRoot());

    QFile::setPermissions(unwritable.path(),
                          QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser
                              | QFile::ExeOwner | QFile::ExeUser);
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testLegacyNotebookIsMovedAndStillOpens()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    QString why;
    const QString legacyDir = makeLegacyNotebook(QStringLiteral("legacy-moved"), &why);
    QVERIFY2(!legacyDir.isEmpty(), qPrintable(why));
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(legacyDir)));

    const QString moved = PdfSession::migrateFromLegacy(QStringLiteral("legacy-moved"), &why);
    QCOMPARE(moved, QDir(PdfSession::projectRoot()).filePath(QStringLiteral("legacy-moved")));

    /// Moved, not copied: the old root no longer holds it, and there is exactly one copy.
    QVERIFY(!QFileInfo::exists(PdfSession::manifestPath(legacyDir)));
    QVERIFY(QFileInfo::exists(PdfSession::manifestPath(moved)));

    /// And it is still a notebook: the source came with it and the manifest still opens.
    QVERIFY2(PdfSession::openProject(moved, &why).isValid(&why), qPrintable(why));

    /// Asking again changes nothing.
    QCOMPARE(PdfSession::migrateFromLegacy(QStringLiteral("legacy-moved"), &why), moved);

    QDir(moved).removeRecursively();
    QDir(legacyDir).removeRecursively();
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testFailedMoveLeavesTheLegacyNotebookIntact()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    QString why;
    const QString legacyDir = makeLegacyNotebook(QStringLiteral("legacy-refused"), &why);
    QVERIFY2(!legacyDir.isEmpty(), qPrintable(why));

    /// A plain file where the moved notebook would have to go, so the rename cannot be made. What
    /// has to happen then is that the notebook stays whole where it is.
    const QString blocked = QDir(PdfSession::projectRoot()).filePath(QStringLiteral("legacy-refused"));
    QVERIFY(QDir().mkpath(PdfSession::projectRoot()));
    {
        QFile file(blocked);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("not a notebook"), qint64(14));
    }

    why.clear();
    QCOMPARE(PdfSession::migrateFromLegacy(QStringLiteral("legacy-refused"), &why), legacyDir);
    QVERIFY2(!why.isEmpty(), "a refused move has to say why");

    /// The legacy copy is untouched, source included.
    const PdfSessionManifest manifest =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(legacyDir));
    QVERIFY(manifest.isValid());
    QVERIFY(QFileInfo::exists(PdfSession::sourcePath(legacyDir, manifest.sourceFile)));

    /// And it is still openable where it is -- that is what returning the legacy directory means.
    why.clear();
    QVERIFY2(PdfSession::openProject(legacyDir, &why).isValid(&why), qPrintable(why));

    QFile::remove(blocked);
    QDir(legacyDir).removeRecursively();
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testMigratedManifestKeepsRelativePaths()
{
    QTemporaryDir documents(temporaryDocumentsParent() + QStringLiteral("/case-XXXXXX"));
    QVERIFY(documents.isValid());
    PdfSession::setDocumentsLocationForTests(documents.path());

    QString why;
    const QString legacyDir = makeLegacyNotebook(QStringLiteral("legacy-paths"), &why);
    QVERIFY2(!legacyDir.isEmpty(), qPrintable(why));

    const QString moved = PdfSession::migrateFromLegacy(QStringLiteral("legacy-paths"), &why);
    QVERIFY2(moved != legacyDir, qPrintable(why));

    const PdfSessionManifest manifest = PdfSession::openProject(moved, &why);
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    /// A notebook is portable only while its manifest holds relative paths, and a migration is a
    /// chance to write an absolute one by accident.
    QVERIFY(!QDir::isAbsolutePath(manifest.sourceFile));
    QVERIFY(!manifest.sourceFile.contains(QLatin1Char('/')));
    for (const PdfPageRecord &page : manifest.pages) {
        QVERIFY2(!QDir::isAbsolutePath(page.kraFile), qPrintable(page.kraFile));
        QVERIFY2(!QDir::isAbsolutePath(page.thumbFile), qPrintable(page.thumbFile));
        QVERIFY2(!page.kraFile.startsWith(QStringLiteral("..")), qPrintable(page.kraFile));
        QVERIFY2(!page.thumbFile.startsWith(QStringLiteral("..")), qPrintable(page.thumbFile));
    }

    /// And the file on disk says the same: neither root left a path behind in it.
    QFile raw(PdfSession::manifestPath(moved));
    QVERIFY(raw.open(QIODevice::ReadOnly));
    const QByteArray json = raw.readAll();
    QVERIFY(!json.contains(moved.toUtf8()));
    QVERIFY(!json.contains(documents.path().toUtf8()));
    QVERIFY(!json.contains(PdfSession::legacyProjectRoot().toUtf8()));

    /// The paths still resolve, relative to the notebook that moved.
    QVERIFY(QFileInfo::exists(PdfSession::sourcePath(moved, manifest.sourceFile)));

    QDir(moved).removeRecursively();
    QDir(legacyDir).removeRecursively();
    PdfSession::setDocumentsLocationForTests(QString());
}

void PdfSessionTest::testRejectsEscapingManifestPaths()
{
    /// One case per escape shape, on the rule itself first: what is being refused is a name that
    /// would leave the project directory once somebody joins it.
    const QList<QPair<QString, QString>> escapes = {
        { QStringLiteral("absolute"), QStringLiteral("/etc/passwd") },
        { QStringLiteral("absolute-in-tmp"), QStringLiteral("/tmp/source.pdf") },
        { QStringLiteral("drive"), QStringLiteral("C:/source.pdf") },
        { QStringLiteral("drive-relative"), QStringLiteral("C:source.pdf") },
        { QStringLiteral("backslash"), QStringLiteral("pages\\..\\source.pdf") },
        { QStringLiteral("parent"), QStringLiteral("../source.pdf") },
        { QStringLiteral("parent-inside"), QStringLiteral("pages/../../source.pdf") },
        { QStringLiteral("empty-component"), QStringLiteral("pages//p0001.kra") },
        { QStringLiteral("dot-component"), QStringLiteral("./source.pdf") },
        { QStringLiteral("empty"), QString() },
    };

    for (const QPair<QString, QString> &escape : escapes) {
        QString why;
        QVERIFY2(!PdfSessionManifest::isSafeRelativePath(escape.second, &why), qPrintable(escape.first));
        QVERIFY2(!why.isEmpty(), qPrintable(escape.first));
    }

    /// And through the manifest, where the field and its value have to be named in the reason.
    for (const QPair<QString, QString> &escape : escapes) {
        if (escape.second.isEmpty()) {
            continue;   ///< empty has its own message, checked below
        }
        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile = escape.second;

        QString why;
        QVERIFY2(!manifest.isValid(&why), qPrintable(escape.first));
        QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(escape.first + ": " + why));
        QVERIFY2(why.contains(escape.second), qPrintable(escape.first + ": " + why));
    }

    /// Empty where a name is required: the source, and a page's ink. Both are refused, and the
    /// page one names the page.
    {
        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile.clear();
        QString why;
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("no source file recorded")), qPrintable(why));

        manifest = baseManifest();
        manifest.pages[0].kraFile.clear();
        why.clear();
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("ink file is not recorded")), qPrintable(why));
    }

    /// The page fields carry the same rule, with the page and the field named.
    {
        PdfSessionManifest manifest = baseManifest();
        manifest.pages[0].kraFile = QStringLiteral("/tmp/ink-escape.kra");
        QString why;
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("manifest's page 1 ink file")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("/tmp/ink-escape.kra")), qPrintable(why));

        manifest = baseManifest();
        manifest.pages[0].thumbFile = QStringLiteral("../../thumb-escape.png");
        why.clear();
        QVERIFY(!manifest.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("manifest's page 1 thumbnail")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("escape")), qPrintable(why));
    }

    /// The entry point: a manifest.json on disk that names an outside source. This is the
    /// hand-placed project directory the whole change is about, and readFrom() is where it enters.
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile = QStringLiteral("../outside.pdf");
        const QString path = dir.filePath(QStringLiteral("manifest.json"));
        writeBytes(path, QJsonDocument(manifest.toJson()).toJson());

        QString why;
        const PdfSessionManifest read = PdfSessionManifest::readFrom(path, &why);
        QVERIFY(!read.isValid(&why));
        QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("../outside.pdf")), qPrintable(why));
    }
}

void PdfSessionTest::testAcceptsLegitimateFileNames()
{
    /// Real names are not ASCII and are not short. A case here has to pass the rule and survive the
    /// JSON round trip, or the validation would be refusing notebooks that work today.
    const QStringList sources = {
        QStringLiteral("source.pdf"),
        QStringLiteral("my notes.pdf"),
        QStringLiteral("source (copy).pdf"),
        QStringLiteral("v1.2.3.pdf"),
        QStringLiteral("2026-09-23T12.30.45+07.00.pdf"),
        QStringLiteral("บันทึกของฉัน.pdf"),
        QStringLiteral("ページ.ノート.2026.pdf"),
        QStringLiteral("sub/dir/source.pdf"),
    };

    for (const QString &source : sources) {
        PdfSessionManifest manifest = baseManifest();
        manifest.sourceFile = source;

        QString why;
        QVERIFY2(manifest.isValid(&why), qPrintable(source + ": " + why));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("manifest.json"));
        QVERIFY2(manifest.writeTo(path, &why), qPrintable(source + ": " + why));
        why.clear();
        const PdfSessionManifest read = PdfSessionManifest::readFrom(path, &why);
        QVERIFY2(read.isValid(&why), qPrintable(source + ": " + why));
        QCOMPARE(read.sourceFile, source);
    }

    const QStringList inkNames = {
        QStringLiteral("pages/p0001.kra"),
        QStringLiteral("pages/my notes 01.kra"),
        QStringLiteral("ink/ページ1.kra"),
    };
    for (const QString &ink : inkNames) {
        PdfSessionManifest manifest = baseManifest();
        manifest.pages[0].kraFile = ink;
        manifest.pages[0].thumbFile = QStringLiteral("thumbs/บันทึก 01.png");
        QString why;
        QVERIFY2(manifest.isValid(&why), qPrintable(ink + ": " + why));
    }
}

void PdfSessionTest::testEmptyThumbnailStaysLegal()
{
    /// The decision: an empty thumbnail means "no preview made yet". Refusing it would stop
    /// notebooks that open today from opening, and the exporter and strip tests build exactly such
    /// manifests. A thumbnail that is there is checked like any other name, because the docker, the
    /// strip decoration and the bundle all join it.
    PdfSessionManifest manifest = baseManifest();
    manifest.pages[0].thumbFile.clear();

    QString why;
    QVERIFY2(manifest.isValid(&why), qPrintable(why));

    manifest.pages[0].thumbFile = QStringLiteral("../thumbs/p0001.png");
    why.clear();
    QVERIFY(!manifest.isValid(&why));
    QVERIFY2(why.contains(QStringLiteral("manifest's page 1 thumbnail")), qPrintable(why));
}

void PdfSessionTest::testOpenProjectRefusesAnEscapingManifest()
{
    /// A project directory copied onto the machine by hand, whose manifest names a source outside
    /// itself. The file exists and hashes to exactly what the manifest records, so nothing but the
    /// boundary check stands between it and being read.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));

    const QByteArray bytes = QByteArrayLiteral("the bytes the manifest hashes");
    const QString outside = dir.filePath(QStringLiteral("outside.pdf"));
    writeBytes(outside, bytes);

    PdfSessionManifest manifest = baseManifest();
    manifest.sourceFile = outside;
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(outside);
    manifest.sourceByteSize = bytes.size();
    writeBytes(QDir(project).filePath(QStringLiteral("manifest.json")),
               QJsonDocument(manifest.toJson()).toJson());

    QString why;
    const PdfSessionManifest opened = PdfSession::openProject(project, &why);
    QVERIFY(!opened.isValid());
    QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("absolute")), qPrintable(why));
}

void PdfSessionTest::testPathInsideProject()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));

    QString why;
    QVERIFY2(PdfSession::isPathInsideProject(project, QStringLiteral("source.pdf"), &why), qPrintable(why));
    QVERIFY(PdfSession::isPathInsideProject(project, QStringLiteral("pages/p0001.kra"), &why));
    QVERIFY(PdfSession::isPathInsideProject(project, QStringLiteral("sub dir/บันทึก 01.kra"), &why));

    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("/etc/passwd"), &why));
    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("../outside.pdf"), &why));
    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("pages/../../outside.pdf"), &why));
    QVERIFY(!PdfSession::isPathInsideProject(project, QString(), &why));
    /// The project directory itself is not a file inside it.
    QVERIFY(!PdfSession::isPathInsideProject(project, QStringLiteral("."), &why));
}

QTEST_MAIN(PdfSessionTest)
#include "PdfSessionTest.moc"
