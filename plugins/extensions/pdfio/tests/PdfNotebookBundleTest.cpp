/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfNotebookBundle.h"
#include "session/PdfSession.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <KArchive>
#include <KArchiveDirectory>
#include <KArchiveEntry>
#include <KArchiveFile>
#include <KZip>

namespace {

/// Deterministic, and the same bytes on every run, so a measured bundle size means something.
QByteArray filler(const QString &seed, int bytes)
{
    quint32 state = 2166136261u;
    for (const QChar &character : seed) {
        state = (state ^ quint32(character.unicode())) * 16777619u;
    }

    QByteArray data;
    data.reserve(bytes);
    while (data.size() < bytes) {
        state = state * 1664525u + 1013904223u;
        data.append(char(quint8(state >> 24)));
    }
    return data;
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "the test cannot write" << path;
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

QString pageName(int index)
{
    return QStringLiteral("pages/p%1.kra").arg(index + 1, 4, 10, QLatin1Char('0'));
}

QString thumbName(int index)
{
    return QStringLiteral("thumbs/p%1.png").arg(index + 1, 4, 10, QLatin1Char('0'));
}

/// A notebook on disk, without Krita: the bundle moves bytes and never opens a .kra, so the
/// artifacts only have to exist and have a size.
PdfSessionManifest makeProject(const QString &dir,
                               int pages,
                               int pagesWithInk,
                               int pagesWithThumbs,
                               int inkBytes = 4096,
                               int thumbBytes = 512)
{
    QDir().mkpath(dir);
    const QByteArray source = filler(QStringLiteral("source-%1").arg(pages), 8192);
    writeBytes(QDir(dir).filePath(QStringLiteral("source.pdf")), source);

    PdfSessionManifest manifest;
    manifest.schema = PdfSessionManifest::CurrentSchema;
    manifest.sourceFile = QStringLiteral("source.pdf");
    manifest.sourceByteSize = source.size();

    for (int i = 0; i < pages; ++i) {
        PdfPageRecord page;
        page.index = i;
        page.sizePt = QSizeF(595, 842);
        page.rotation = 0;
        page.kraFile = pageName(i);
        page.thumbFile = thumbName(i);
        page.generation = 0;
        manifest.pages.append(page);

        if (i < pagesWithInk) {
            writeBytes(QDir(dir).filePath(page.kraFile),
                       filler(QStringLiteral("ink-%1-%2").arg(pages).arg(i), inkBytes));
        }
        if (i < pagesWithThumbs) {
            writeBytes(QDir(dir).filePath(page.thumbFile),
                       filler(QStringLiteral("thumb-%1-%2").arg(pages).arg(i), thumbBytes));
        }
    }

    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(QDir(dir).filePath(QStringLiteral("source.pdf")));
    manifest.writeTo(QDir(dir).filePath(QStringLiteral("manifest.json")));
    return manifest;
}

void collectArchiveFiles(const KArchiveDirectory *directory,
                         const QString &prefix,
                         QList<QPair<QString, const KArchiveFile *>> *out)
{
    const QStringList names = directory->entries();
    for (const QString &name : names) {
        const KArchiveEntry *child = directory->entry(name);
        if (!child) {
            continue;
        }
        const QString path = prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
        if (const KArchiveDirectory *sub = dynamic_cast<const KArchiveDirectory *>(child)) {
            collectArchiveFiles(sub, path, out);
            continue;
        }
        if (const KArchiveFile *file = dynamic_cast<const KArchiveFile *>(child)) {
            out->append(qMakePair(path, file));
        }
    }
}

QList<QPair<QString, const KArchiveFile *>> archiveFiles(const KZip &zip)
{
    QList<QPair<QString, const KArchiveFile *>> files;
    collectArchiveFiles(zip.directory(), QString(), &files);
    return files;
}

QStringList archivePaths(const KZip &zip)
{
    QStringList paths;
    for (const auto &entry : archiveFiles(zip)) {
        paths.append(entry.first);
    }
    paths.sort();
    return paths;
}

QByteArray archiveBytes(const KZip &zip, const QString &path)
{
    for (const auto &entry : archiveFiles(zip)) {
        if (entry.first != path) {
            continue;
        }
        QIODevice *device = entry.second->createDevice();
        const QByteArray data = device ? device->readAll() : QByteArray();
        delete device;
        return data;
    }
    return QByteArray();
}

/// A zip written straight, entry by entry: how the hostile and hand-made bundles are built.
bool writeRawBundle(const QString &path, const QList<QPair<QString, QByteArray>> &entries)
{
    QFile::remove(path);
    KZip zip(path);
    if (!zip.open(QIODevice::WriteOnly)) {
        return false;
    }
    for (const QPair<QString, QByteArray> &entry : entries) {
        if (!zip.prepareWriting(entry.first, QString(), QString(), entry.second.size())) {
            return false;
        }
        /// The two argument form, for the same reason PdfNotebookBundle uses it: KF5's KZip has no
        /// QByteArray overload and KF6 keeps this one.
        if (!zip.writeData(entry.second.constData(), entry.second.size())) {
            return false;
        }
        if (!zip.finishWriting(entry.second.size())) {
            return false;
        }
    }
    return zip.close();
}

/// The project's manifest as a mutable JSON object, so a test can change one field of it.
QJsonObject manifestObjectOf(const QString &project)
{
    return QJsonDocument::fromJson(readBytes(QDir(project).filePath(QStringLiteral("manifest.json")))).object();
}

/**
 * A bundle whose archive is a project's own files but whose manifest has been rewritten.
 *
 * The archive is not the attacker's only lever: source.file, pages[].kraFile and
 * thumbs[].thumbFile are joined onto the destination and written to, so a bundle whose entries are
 * all ordinary can still have a write of its choosing in it. This is the shape the verifier's C1
 * and C2 used, and the shape the refusals have to survive.
 */
bool writeBundleWithEditedManifest(const QString &bundlePath,
                                   const QString &project,
                                   const QJsonObject &editedManifest)
{
    const PdfSessionManifest original =
        PdfSessionManifest::readFrom(QDir(project).filePath(QStringLiteral("manifest.json")));

    QList<QPair<QString, QByteArray>> entries;
    entries.append(qMakePair(QStringLiteral("manifest.json"), QJsonDocument(editedManifest).toJson()));
    entries.append(qMakePair(QStringLiteral("source.pdf"),
                             readBytes(QDir(project).filePath(QStringLiteral("source.pdf")))));
    for (const PdfPageRecord &page : original.pages) {
        const QString references[] = { page.kraFile, page.thumbFile };
        for (const QString &relative : references) {
            const QString local = QDir(project).filePath(relative);
            if (QFileInfo::exists(local)) {
                entries.append(qMakePair(relative, readBytes(local)));
            }
        }
    }
    return writeRawBundle(bundlePath, entries);
}

} // namespace

/**
 * The bundle is one file in and one file out, and the half that reads a file a stranger sent is
 * the half that has to be right. Both are pure logic over a zip, so they run here rather than only
 * inside a running Krita: a round trip that loses a byte, an extract that writes outside its
 * destination, and a refusal that does not fire are all found by ctest.
 */
class PdfNotebookBundleTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFormatIsAPlainZipAtTheProjectPaths();
    void testInspectReadsTheManifestWithoutWriting();
    void testRoundTripIsByteIdentical();
    void testExtractWorksOnAnotherRootWithTheNotebookGone();
    void testWithheldPagesAreDeclaredAndReported();
    void testUnknownEntriesAreIgnoredAndReported();
    void testExtractRefusesToOverwriteWithoutTheFlag();
    void testRefusesANewerSchema();
    void testRefusesAnOlderSchema();
    void testReadsABundleWrittenBeforeSourcesExisted();
    void testSaveRefusesANotebookWithTwoSources();
    void testRefusesZipSlip();
    void testRefusesAChangedSource();
    void testRefusesAReferencedArtifactTheArchiveLacks();
    void testRefusesACorruptOrTruncatedArchive();
    void testRefusesAnEditedBundle();
    void testRefusesAnEscapingManifestSource();
    void testRefusesAnEscapingManifestPage();
    void testSaveRefusesAnEscapingManifest();
    void testAbsoluteEntryIsNormalisedAndNeverEscapes();
    void testMeasuresSizeAndTimeOnThreeAndFiftyPages();

private:
    QString fixturePath(const QString &name) const
    {
        return QStringLiteral(FILES_DATA_DIR) + name;
    }
};

void PdfNotebookBundleTest::testFormatIsAPlainZipAtTheProjectPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest = makeProject(project, 3, 3, 3);
    QVERIFY(manifest.isValid());

    const QString bundle = dir.filePath(QStringLiteral("notebook.pnb"));
    QString why;
    QVERIFY2(PdfNotebookBundle::save(project, bundle, &why), qPrintable(why));
    QVERIFY(QFileInfo::exists(bundle));

    /// A plain zip, so no tool of ours is needed to look inside one.
    QFile raw(bundle);
    QVERIFY(raw.open(QIODevice::ReadOnly));
    QCOMPARE(raw.read(2), QByteArrayLiteral("PK"));
    raw.close();

    KZip zip(bundle);
    QVERIFY(zip.open(QIODevice::ReadOnly));

    QStringList expected;
    expected << QStringLiteral("bundle.json") << QStringLiteral("manifest.json")
             << QStringLiteral("pages/p0001.kra") << QStringLiteral("pages/p0002.kra")
             << QStringLiteral("pages/p0003.kra") << QStringLiteral("source.pdf")
             << QStringLiteral("thumbs/p0001.png") << QStringLiteral("thumbs/p0002.png")
             << QStringLiteral("thumbs/p0003.png");
    expected.sort();
    QCOMPARE(archivePaths(zip), expected);

    /// The manifest travels byte for byte: the project's own file, not a re-serialisation of it.
    QCOMPARE(archiveBytes(zip, QStringLiteral("manifest.json")),
             readBytes(QDir(project).filePath(QStringLiteral("manifest.json"))));
    QCOMPARE(archiveBytes(zip, QStringLiteral("source.pdf")),
             readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))));
    QCOMPARE(archiveBytes(zip, QStringLiteral("pages/p0002.kra")),
             readBytes(QDir(project).filePath(QStringLiteral("pages/p0002.kra"))));
}

void PdfNotebookBundleTest::testInspectReadsTheManifestWithoutWriting()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest = makeProject(project, 3, 3, 3);

    const QString bundle = dir.filePath(QStringLiteral("notebook.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, bundle));

    QString why;
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(bundle, &why);
    QVERIFY2(info.isValid(), qPrintable(why));
    QCOMPARE(info.manifest.toJson(), manifest.toJson());
    QCOMPARE(info.sourceEntry, QStringLiteral("source.pdf"));
    QVERIFY(info.hasIndex);
    QVERIFY(info.withheld.isEmpty());
    QVERIFY(info.unknown.isEmpty());
    QCOMPARE(info.entries.size(), 8);
    QCOMPARE(info.bundleBytes, QFileInfo(bundle).size());

    /// Nothing was unpacked next to the bundle.
    QCOMPARE(QDir(dir.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
}

void PdfNotebookBundleTest::testRoundTripIsByteIdentical()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    /// Root A is where the notebook was made; root B is somewhere else entirely, which is what
    /// another device is.
    const QString project = dir.filePath(QStringLiteral("root-a/project"));
    const PdfSessionManifest manifest = makeProject(project, 3, 3, 3);

    const QString bundle = dir.filePath(QStringLiteral("root-a/carried.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, bundle));

    const QString dest = dir.filePath(QStringLiteral("root-b/unpacked"));
    QString why;
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why), qPrintable(why));

    const PdfSessionManifest back = PdfSession::openProject(dest, &why);
    QVERIFY2(back.isValid(&why), qPrintable(why));
    QCOMPARE(back.toJson(), manifest.toJson());

    QCOMPARE(readBytes(QDir(dest).filePath(QStringLiteral("source.pdf"))),
             readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))));
    for (int i = 0; i < manifest.pages.size(); ++i) {
        QCOMPARE(readBytes(QDir(dest).filePath(pageName(i))),
                 readBytes(QDir(project).filePath(pageName(i))));
        QCOMPARE(readBytes(QDir(dest).filePath(thumbName(i))),
                 readBytes(QDir(project).filePath(thumbName(i))));
    }

    /// Relative, every one of them: an absolute path in the manifest is the one thing that would
    /// make the notebook openable on this device and nowhere else.
    QVERIFY(!QDir::isAbsolutePath(back.sourceFile));
    for (const PdfPageRecord &page : back.pages) {
        QVERIFY(!QDir::isAbsolutePath(page.kraFile));
        QVERIFY(!QDir::isAbsolutePath(page.thumbFile));
    }
    QVERIFY(!readBytes(QDir(dest).filePath(QStringLiteral("manifest.json"))).contains(dir.path().toUtf8()));
}

void PdfNotebookBundleTest::testExtractWorksOnAnotherRootWithTheNotebookGone()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("root-a/project"));
    const PdfSessionManifest manifest = makeProject(project, 3, 3, 3);

    const QString bundle = dir.filePath(QStringLiteral("root-a/carried.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, bundle));

    /// The machine the bundle arrived on has never had the notebook: the only thing that crosses
    /// is the one file, so the source PDF is gone with the project directory it lived in.
    QVERIFY(QDir(project).removeRecursively());
    QVERIFY(!QFileInfo::exists(project));

    const QString dest = dir.filePath(QStringLiteral("root-b/unpacked"));
    QString why;
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why), qPrintable(why));

    /// openProject checks the source exists and hashes to what the manifest recorded, so this is
    /// the whole cross-device claim in one call.
    const PdfSessionManifest back = PdfSession::openProject(dest, &why);
    QVERIFY2(back.isValid(&why), qPrintable(why));
    QCOMPARE(back.sourceSha256, manifest.sourceSha256);
    QCOMPARE(QFileInfo(QDir(dest).filePath(QStringLiteral("source.pdf"))).size(), manifest.sourceByteSize);

    /// And the name it was unpacked under is the one the navigator gives the same notebook, so a
    /// second empty copy is not made beside it.
    QCOMPARE(PdfNotebookBundle::extractDirName(manifest),
             QStringLiteral("source-") + QString::fromLatin1(manifest.sourceSha256.left(8)));

    /// The root the plugin unpacks into is PdfSession's policy; an empty one would unpack into the
    /// working directory, which is the failure this asserts against.
    QVERIFY(!PdfNotebookBundle::defaultProjectRoot().isEmpty());
    QVERIFY(QDir::isAbsolutePath(PdfNotebookBundle::defaultProjectRoot()));
}

void PdfNotebookBundleTest::testWithheldPagesAreDeclaredAndReported()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    /// One page drawn on and no thumbnails: the ordinary shape of a young notebook, and the reason
    /// the completeness rule cannot simply be "everything the manifest names must be inside".
    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 3, 1, 0).isValid());

    const QString bundle = dir.filePath(QStringLiteral("notebook.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, bundle));

    QString why;
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(bundle, &why);
    QVERIFY2(info.isValid(), qPrintable(why));
    QCOMPARE(info.entries.size(), 3);   ///< manifest, source, one page of ink
    QStringList expected;
    expected << QStringLiteral("pages/p0002.kra") << QStringLiteral("pages/p0003.kra")
             << QStringLiteral("thumbs/p0001.png") << QStringLiteral("thumbs/p0002.png")
             << QStringLiteral("thumbs/p0003.png");
    expected.sort();
    QStringList reported = info.withheld;
    reported.sort();
    QCOMPARE(reported, expected);

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why), qPrintable(why));
    QVERIFY(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("pages/p0001.kra"))));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("pages/p0002.kra"))));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("thumbs"))));
    QVERIFY2(PdfSession::openProject(dest, &why).isValid(&why), qPrintable(why));
}

void PdfNotebookBundleTest::testUnknownEntriesAreIgnoredAndReported()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 2, 2, 2).isValid());

    const QList<QPair<QString, QByteArray>> entries = {
        { QStringLiteral("manifest.json"), readBytes(QDir(project).filePath(QStringLiteral("manifest.json"))) },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
        { pageName(0), readBytes(QDir(project).filePath(pageName(0))) },
        { pageName(1), readBytes(QDir(project).filePath(pageName(1))) },
        { thumbName(0), readBytes(QDir(project).filePath(thumbName(0))) },
        { thumbName(1), readBytes(QDir(project).filePath(thumbName(1))) },
        { QStringLiteral("notes.txt"), QByteArrayLiteral("something else entirely") },
        { QStringLiteral("pages/private.bin"), filler(QStringLiteral("extra"), 64) },
    };
    const QString bundle = dir.filePath(QStringLiteral("handmade.pnb"));
    QVERIFY(writeRawBundle(bundle, entries));

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QString why;
    QStringList ignored;
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why, &ignored), qPrintable(why));

    ignored.sort();
    QCOMPARE(ignored, QStringList({ QStringLiteral("notes.txt"), QStringLiteral("pages/private.bin") }));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("notes.txt"))));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("pages/private.bin"))));
    QVERIFY2(PdfSession::openProject(dest, &why).isValid(&why), qPrintable(why));
}

void PdfNotebookBundleTest::testExtractRefusesToOverwriteWithoutTheFlag()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 2, 2, 2).isValid());
    const QString bundle = dir.filePath(QStringLiteral("notebook.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, bundle));

    const QString dest = dir.filePath(QStringLiteral("already-there"));
    writeBytes(QDir(dest).filePath(QStringLiteral("kept.txt")), QByteArrayLiteral("do not lose me"));

    QString why;
    QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
    QVERIFY2(why.contains(QStringLiteral("already there")), qPrintable(why));

    /// Refused means untouched, not half-written.
    QCOMPARE(readBytes(QDir(dest).filePath(QStringLiteral("kept.txt"))), QByteArrayLiteral("do not lose me"));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("manifest.json"))));

    PdfNotebookBundle::ExtractOptions options;
    options.replaceExisting = true;
    why.clear();
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, options, &why), qPrintable(why));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("kept.txt"))));
    QVERIFY(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("manifest.json"))));
}

void PdfNotebookBundleTest::testRefusesANewerSchema()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest = makeProject(project, 2, 2, 2);

    QJsonObject object = manifest.toJson();
    object.insert(QStringLiteral("schema"), PdfSessionManifest::CurrentSchema + 1);

    const QString bundle = dir.filePath(QStringLiteral("newer.pnb"));
    QVERIFY(writeRawBundle(bundle, {
        { QStringLiteral("manifest.json"), QJsonDocument(object).toJson() },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
    }));

    QString why;
    QVERIFY(!PdfNotebookBundle::inspect(bundle, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("newer")), qPrintable(why));

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
    QVERIFY2(why.contains(QStringLiteral("newer")), qPrintable(why));
    QVERIFY(!QFileInfo::exists(dest));
}

void PdfNotebookBundleTest::testRefusesAnOlderSchema()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest = makeProject(project, 2, 2, 2);

    QJsonObject object = manifest.toJson();
    object.insert(QStringLiteral("schema"), 0);

    const QString bundle = dir.filePath(QStringLiteral("older.pnb"));
    QVERIFY(writeRawBundle(bundle, {
        { QStringLiteral("manifest.json"), QJsonDocument(object).toJson() },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
    }));

    QString why;
    QVERIFY(!PdfNotebookBundle::inspect(bundle, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("schema 0")), qPrintable(why));
}

/**
 * A bundle written before sources[] existed still opens.
 *
 * The one-file form is how a notebook leaves the device, so a build that refused a schema 1 bundle
 * would strand every notebook already carried in one. The manifest is upgraded in memory exactly
 * as it is when a notebook is opened from its own directory, and what lands on extraction is this
 * build's schema.
 */
void PdfNotebookBundleTest::testReadsABundleWrittenBeforeSourcesExisted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest = makeProject(project, 2, 2, 2);

    /// The shape a schema 1 build wrote: the schema number, no sources[] list or counter, and no
    /// per-page source or extra rotation.
    QJsonObject object = manifest.toJson();
    object.insert(QStringLiteral("schema"), 1);
    object.remove(QStringLiteral("sources"));
    object.remove(QStringLiteral("nextPageNumber"));
    QJsonArray pages = object.value(QStringLiteral("pages")).toArray();
    for (int i = 0; i < pages.size(); ++i) {
        QJsonObject page = pages.at(i).toObject();
        page.remove(QStringLiteral("source"));
        page.remove(QStringLiteral("extraRotation"));
        pages.replace(i, page);
    }
    object.insert(QStringLiteral("pages"), pages);

    const QString bundle = dir.filePath(QStringLiteral("legacy.pnb"));
    QVERIFY(writeRawBundle(bundle, {
        { QStringLiteral("manifest.json"), QJsonDocument(object).toJson() },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
        { pageName(0), readBytes(QDir(project).filePath(pageName(0))) },
        { pageName(1), readBytes(QDir(project).filePath(pageName(1))) },
        { thumbName(0), readBytes(QDir(project).filePath(thumbName(0))) },
        { thumbName(1), readBytes(QDir(project).filePath(thumbName(1))) },
    }));

    QString why;
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(bundle, &why);
    QVERIFY2(info.isValid(), qPrintable(why));
    QCOMPARE(info.manifest.sourceCount(), 1);
    QCOMPARE(info.manifest.pages.size(), 2);
    QCOMPARE(info.manifest.pages.at(0).source, 0);

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why), qPrintable(why));

    const PdfSessionManifest read = PdfSession::openProject(dest, &why);
    QVERIFY2(read.isValid(&why), qPrintable(why));
    QCOMPARE(read.schema, PdfSessionManifest::CurrentSchema);
    QCOMPARE(read.sourceCount(), 1);
    QCOMPARE(read.pages.at(0).source, 0);
}

/**
 * A notebook whose pages come from two PDFs is refused by the one-file form, rather than written
 * without its second source.
 *
 * The archive carries exactly one PDF and verifies that one. Writing this notebook out would
 * produce a file that unpacks into a notebook whose pages from the other PDF render blank, with
 * nothing said at either end -- so it is refused until the format carries sources[].
 */
void PdfNotebookBundleTest::testSaveRefusesANotebookWithTwoSources()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    PdfSessionManifest manifest = makeProject(project, 2, 2, 2);

    /// A second source inside the project, and a page that draws from it.
    QVERIFY(QDir().mkpath(QDir(project).filePath(QStringLiteral("sources"))));
    const QString extra = QStringLiteral("sources/9f3a1c02-handout.pdf");
    QVERIFY(QFile::copy(QDir(project).filePath(QStringLiteral("source.pdf")),
                        QDir(project).filePath(extra)));

    PdfSourceRecord first;
    first.file = manifest.sourceFile;
    first.sha256 = manifest.sourceSha256;
    first.byteSize = manifest.sourceByteSize;
    PdfSourceRecord second;
    second.file = extra;
    second.sha256 = PdfSessionManifest::sha256OfFile(QDir(project).filePath(extra));
    second.byteSize = QFileInfo(QDir(project).filePath(extra)).size();
    manifest.sources.clear();
    manifest.sources << first << second;
    manifest.pages[1].source = 1;

    QString why;
    QVERIFY2(manifest.writeTo(QDir(project).filePath(QStringLiteral("manifest.json")), &why),
             qPrintable(why));

    const QString bundle = dir.filePath(QStringLiteral("two-sources.pnb"));
    why.clear();
    QVERIFY2(!PdfNotebookBundle::save(project, bundle, &why),
             "the notebook was written out without its second source");
    QVERIFY2(why.contains(QStringLiteral("draws pages from 2 PDFs")), qPrintable(why));
    QVERIFY(!QFileInfo::exists(bundle));
}

void PdfNotebookBundleTest::testRefusesZipSlip()
{
    /// The rule on its own, which is where the interesting shapes are.
    QString why;
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QString(), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("../escape.txt"), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("/etc/passwd"), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("pages/../../escape"), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("pages//p0001.kra"), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("pages/./p0001.kra"), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("C:/escape"), &why));
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("pages\\..\\escape"), &why));
    QVERIFY(PdfNotebookBundle::isSafeEntryPath(QStringLiteral("manifest.json"), &why));
    QVERIFY(PdfNotebookBundle::isSafeEntryPath(QStringLiteral("pages/p0001.kra"), &why));

    /// And end to end, because the rule only matters if it is actually consulted. KZip writes the
    /// hostile name as given, and unzip would list it too.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString project = dir.filePath(QStringLiteral("project"));
    const PdfSessionManifest manifest = makeProject(project, 1, 1, 1);

    const QString bundle = dir.filePath(QStringLiteral("slip.pnb"));
    QVERIFY(writeRawBundle(bundle, {
        { QStringLiteral("manifest.json"), readBytes(QDir(project).filePath(QStringLiteral("manifest.json"))) },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
        { pageName(0), readBytes(QDir(project).filePath(pageName(0))) },
        { thumbName(0), readBytes(QDir(project).filePath(thumbName(0))) },
        { QStringLiteral("../escape.txt"), QByteArrayLiteral("gotcha") },
    }));
    QVERIFY(!manifest.sourceSha256.isEmpty());

    /// No absolute entry here on purpose: KZip strips a leading "/" when it writes, so a KZip-made
    /// archive cannot carry one and the assertion would be vacuous. The absolute case is covered by
    /// testAbsoluteEntryIsNormalisedAndNeverEscapes(), which uses a fixture written by
    /// tests/data/nb-make-fixtures.py, and the rule itself is exercised for "/etc/passwd" above.
    const QString dest = dir.filePath(QStringLiteral("outside/unpacked"));
    why.clear();
    QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
    QVERIFY2(why.contains(QStringLiteral("escape")), qPrintable(why));
    QVERIFY(!QFileInfo::exists(dest));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("outside/escape.txt"))));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("escape.txt"))));
}

void PdfNotebookBundleTest::testRefusesAChangedSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 2, 2, 2).isValid());

    /// Every path is present and the archive is intact: the only thing wrong is that the bytes
    /// under source.pdf are not the bytes the manifest hashed.
    const QString bundle = dir.filePath(QStringLiteral("swapped.pnb"));
    QVERIFY(writeRawBundle(bundle, {
        { QStringLiteral("manifest.json"), readBytes(QDir(project).filePath(QStringLiteral("manifest.json"))) },
        { QStringLiteral("source.pdf"), QByteArrayLiteral("a different PDF entirely, same name") },
        { pageName(0), readBytes(QDir(project).filePath(pageName(0))) },
        { pageName(1), readBytes(QDir(project).filePath(pageName(1))) },
        { thumbName(0), readBytes(QDir(project).filePath(thumbName(0))) },
        { thumbName(1), readBytes(QDir(project).filePath(thumbName(1))) },
    }));

    QString why;
    QVERIFY(!PdfNotebookBundle::inspect(bundle, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("checksum")), qPrintable(why));

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
    QVERIFY(!QFileInfo::exists(dest));
}

void PdfNotebookBundleTest::testRefusesAReferencedArtifactTheArchiveLacks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 3, 3, 3).isValid());

    /// pages/p0002.kra is named by the manifest and simply not in the file, and nothing declares
    /// it withheld, which is the difference between a young notebook and a damaged one.
    const QString bundle = dir.filePath(QStringLiteral("hole.pnb"));
    QVERIFY(writeRawBundle(bundle, {
        { QStringLiteral("manifest.json"), readBytes(QDir(project).filePath(QStringLiteral("manifest.json"))) },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
        { pageName(0), readBytes(QDir(project).filePath(pageName(0))) },
        { pageName(2), readBytes(QDir(project).filePath(pageName(2))) },
        { thumbName(0), readBytes(QDir(project).filePath(thumbName(0))) },
        { thumbName(1), readBytes(QDir(project).filePath(thumbName(1))) },
        { thumbName(2), readBytes(QDir(project).filePath(thumbName(2))) },
    }));

    QString why;
    QVERIFY(!PdfNotebookBundle::inspect(bundle, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("pages/p0002.kra")), qPrintable(why));
    QVERIFY2(why.contains(QStringLiteral("does not carry")), qPrintable(why));

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
    QVERIFY(!QFileInfo::exists(dest));
}

void PdfNotebookBundleTest::testRefusesACorruptOrTruncatedArchive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 3, 2, 2).isValid());
    const QString good = dir.filePath(QStringLiteral("good.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, good));

    const QByteArray bytes = readBytes(good);
    QVERIFY(bytes.size() > 100);

    /// A copy that stopped half way: the central directory is not there at all.
    const QString cut = dir.filePath(QStringLiteral("cut.pnb"));
    writeBytes(cut, bytes.left(bytes.size() / 2));
    QString why;
    QVERIFY(!PdfNotebookBundle::inspect(cut, &why).isValid());
    QVERIFY(!why.isEmpty());

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY(!PdfNotebookBundle::extract(cut, dest, &why));
    QVERIFY(!QFileInfo::exists(dest));

    /// And a file that is not an archive at all.
    const QString garbage = dir.filePath(QStringLiteral("garbage.pnb"));
    writeBytes(garbage, filler(QStringLiteral("garbage"), 4096));
    QVERIFY(!PdfNotebookBundle::inspect(garbage, &why).isValid());
    QVERIFY(!PdfNotebookBundle::extract(garbage, dest, &why));

    /// A file that is not there at all is not a crash either.
    QVERIFY(!PdfNotebookBundle::inspect(dir.filePath(QStringLiteral("absent.pnb")), &why).isValid());
}

void PdfNotebookBundleTest::testRefusesAnEditedBundle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 2, 2, 2).isValid());
    const QString good = dir.filePath(QStringLiteral("good.pnb"));
    QVERIFY(PdfNotebookBundle::save(project, good));

    KZip zip(good);
    QVERIFY(zip.open(QIODevice::ReadOnly));
    const QByteArray index = archiveBytes(zip, QStringLiteral("bundle.json"));
    QVERIFY(!index.isEmpty());
    zip.close();

    /// Every path is there, the archive is readable, and one ink file has been swapped for other
    /// bytes of the same length: only the checksums in the index can see that.
    const QByteArray ink = readBytes(QDir(project).filePath(pageName(0)));
    QByteArray swapped = ink;
    swapped[0] = char(quint8(swapped.at(0)) ^ 0xff);

    const QString edited = dir.filePath(QStringLiteral("edited.pnb"));
    QVERIFY(writeRawBundle(edited, {
        { QStringLiteral("manifest.json"), readBytes(QDir(project).filePath(QStringLiteral("manifest.json"))) },
        { QStringLiteral("source.pdf"), readBytes(QDir(project).filePath(QStringLiteral("source.pdf"))) },
        { pageName(0), swapped },
        { pageName(1), readBytes(QDir(project).filePath(pageName(1))) },
        { thumbName(0), readBytes(QDir(project).filePath(thumbName(0))) },
        { thumbName(1), readBytes(QDir(project).filePath(thumbName(1))) },
        { QStringLiteral("bundle.json"), index },
    }));

    QString why;
    QVERIFY(!PdfNotebookBundle::inspect(edited, &why).isValid());
    QVERIFY2(why.contains(QStringLiteral("bundle index")), qPrintable(why));

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QVERIFY(!PdfNotebookBundle::extract(edited, dest, &why));
    QVERIFY(!QFileInfo::exists(dest));
}

/**
 * Size and time for the two shapes that matter: the short notebook someone sends and the long one
 * that shows whether anything is quadratic.
 *
 * The artifacts are made here rather than drawn, so this measures the bundle and not Krita's save:
 * each page carries 37,254 bytes of already-compressed-looking ink, which is the size an ink-only
 * .kra was measured at on the three page fixture, and 4,096 bytes of thumbnail.
 */
void PdfNotebookBundleTest::testMeasuresSizeAndTimeOnThreeAndFiftyPages()
{
    const QList<QPair<QString, int>> fixtures = {
        { QStringLiteral("ex-browser-3p.pdf"), 3 },
        { QStringLiteral("ex-manypage-50.pdf"), 50 },
    };

    for (const QPair<QString, int> &fixture : fixtures) {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        PopplerRenderBackend backend;
        const QString project = dir.filePath(QStringLiteral("project"));
        QString why;
        const PdfSessionManifest manifest =
            PdfSession::createProject(project, fixturePath(fixture.first), backend, &why);
        QVERIFY2(manifest.isValid(&why), qPrintable(why));
        QCOMPARE(manifest.pages.size(), fixture.second);

        for (int i = 0; i < manifest.pages.size(); ++i) {
            writeBytes(QDir(project).filePath(pageName(i)), filler(QStringLiteral("ink-%1").arg(i), 37254));
            writeBytes(QDir(project).filePath(thumbName(i)), filler(QStringLiteral("thumb-%1").arg(i), 4096));
        }

        const QString bundle = dir.filePath(QStringLiteral("measured.pnb"));

        QElapsedTimer timer;
        timer.start();
        QVERIFY2(PdfNotebookBundle::save(project, bundle, &why), qPrintable(why));
        const qint64 saveMs = timer.elapsed();

        const QString dest = dir.filePath(QStringLiteral("second-root/unpacked"));
        timer.restart();
        QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why), qPrintable(why));
        const qint64 extractMs = timer.elapsed();

        const qint64 artifacts = qint64(manifest.pages.size()) * (37254 + 4096);
        const qint64 projectBytes =
            QFileInfo(QDir(project).filePath(QStringLiteral("source.pdf"))).size()
            + QFileInfo(QDir(project).filePath(QStringLiteral("manifest.json"))).size()
            + artifacts;

        qInfo().noquote()
            << QStringLiteral("MEASURE %1: %2 pages, source+artifacts %3 bytes, bundle %4 bytes, "
                              "save %5 ms, extract %6 ms")
                   .arg(fixture.first)
                   .arg(manifest.pages.size())
                   .arg(projectBytes)
                   .arg(QFileInfo(bundle).size())
                   .arg(saveMs)
                   .arg(extractMs);

        QVERIFY2(PdfSession::openProject(dest, &why).isValid(&why), qPrintable(why));
        QVERIFY(QFileInfo(bundle).size() > 0);
    }
}

void PdfNotebookBundleTest::testRefusesAnEscapingManifestSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 1, 1, 1).isValid());

    /// The archive is entirely ordinary here: a plain source.pdf entry and every page artifact.
    /// Only source.file differs. That is C1 of docs/verify/BUNDLE-VERIFY.md (absolute; the verifier
    /// used /tmp and any absolute path is the same shape) and C2 (relative, two levels up).
    const QString absolute = dir.filePath(QStringLiteral("outside/nb-absolute-escape.pdf"));
    const QList<QPair<QString, QString>> escapes = {
        { QStringLiteral("absolute"), absolute },
        { QStringLiteral("relative"), QStringLiteral("../../nb-relative-escape.pdf") },
        { QStringLiteral("joined"), QStringLiteral("pages/../../nb-joined-escape.pdf") },
    };

    for (const QPair<QString, QString> &escape : escapes) {
        QJsonObject manifest = manifestObjectOf(project);
        QJsonObject source = manifest.value(QStringLiteral("source")).toObject();
        source.insert(QStringLiteral("file"), escape.second);
        manifest.insert(QStringLiteral("source"), source);

        const QString bundle = dir.filePath(QStringLiteral("source-%1.pnb").arg(escape.first));
        QVERIFY2(writeBundleWithEditedManifest(bundle, project, manifest), qPrintable(escape.first));

        /// inspect() has to refuse it too: a pre-flight that says yes to what extract() refuses
        /// would be the same hole one step earlier.
        QString why;
        QVERIFY2(!PdfNotebookBundle::inspect(bundle, &why).isValid(), qPrintable(escape.first));
        QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(escape.first + ": " + why));
        QVERIFY2(why.contains(QStringLiteral("escape")) || why.contains(QStringLiteral("absolute")),
                 qPrintable(escape.first + ": " + why));

        const QString dest = dir.filePath(QStringLiteral("unpacked-%1").arg(escape.first));
        why.clear();
        QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
        QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(why));

        /// Refused means nothing was written anywhere: no destination, and nothing at the path the
        /// manifest named -- which is the write C1 and C2 performed.
        QVERIFY(!QFileInfo::exists(dest));
        QVERIFY(!QFileInfo::exists(escape.second));
        QVERIFY(!QFileInfo::exists(QDir(dest).filePath(escape.second)));
    }

    /// Control: the same archive with the project's own manifest extracts, so what is refused is the
    /// manifest's path and nothing else about the file.
    const QString control = dir.filePath(QStringLiteral("control.pnb"));
    QVERIFY(writeBundleWithEditedManifest(control, project, manifestObjectOf(project)));
    const QString controlDest = dir.filePath(QStringLiteral("control-unpacked"));
    QString why;
    QVERIFY2(PdfNotebookBundle::extract(control, controlDest, &why), qPrintable(why));
    QVERIFY2(PdfSession::openProject(controlDest, &why).isValid(&why), qPrintable(why));
}

void PdfNotebookBundleTest::testRefusesAnEscapingManifestPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 2, 2, 2).isValid());

    /// The page fields are joined onto the destination exactly like the source is. The verifier
    /// could not reach a write through them, because a reference that is not also an entry was
    /// caught as "does not carry"; with the rule applied to the manifest they are refused for what
    /// they are, which is the difference between an accidental refusal and a check.
    const QList<QPair<QString, QString>> cases = {
        { QStringLiteral("kra"), QStringLiteral("/tmp/nb-page-escape.kra") },
        { QStringLiteral("thumb"), QStringLiteral("../../nb-thumb-escape.png") },
    };

    for (const QPair<QString, QString> &testCase : cases) {
        QJsonObject manifest = manifestObjectOf(project);
        QJsonArray pages = manifest.value(QStringLiteral("pages")).toArray();
        QJsonObject page = pages.at(0).toObject();
        page.insert(testCase.first, testCase.second);
        pages.replace(0, page);
        manifest.insert(QStringLiteral("pages"), pages);

        const QString bundle = dir.filePath(QStringLiteral("page-%1.pnb").arg(testCase.first));
        QVERIFY2(writeBundleWithEditedManifest(bundle, project, manifest), qPrintable(testCase.first));

        QString why;
        QVERIFY(!PdfNotebookBundle::inspect(bundle, &why).isValid());
        QVERIFY2(why.contains(QStringLiteral("manifest's page 1")), qPrintable(why));
        QVERIFY2(why.contains(QStringLiteral("escape")) || why.contains(QStringLiteral("absolute")),
                 qPrintable(why));
        QVERIFY2(why.contains(testCase.first == QStringLiteral("kra") ? QStringLiteral("ink file")
                                                                     : QStringLiteral("thumbnail")),
                 qPrintable(why));

        const QString dest = dir.filePath(QStringLiteral("unpacked-%1").arg(testCase.first));
        why.clear();
        QVERIFY(!PdfNotebookBundle::extract(bundle, dest, &why));
        QVERIFY(!QFileInfo::exists(dest));
    }
}

void PdfNotebookBundleTest::testSaveRefusesAnEscapingManifest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString project = dir.filePath(QStringLiteral("project"));
    QVERIFY(makeProject(project, 1, 1, 1).isValid());

    /// The same hole in the other direction: a project whose own manifest names a file outside it
    /// would have that file read into the bundle.
    const QString outside = dir.filePath(QStringLiteral("outside.pdf"));
    writeBytes(outside, QByteArrayLiteral("a file the notebook does not own"));

    QJsonObject manifest = manifestObjectOf(project);
    QJsonObject source = manifest.value(QStringLiteral("source")).toObject();
    source.insert(QStringLiteral("file"), outside);
    manifest.insert(QStringLiteral("source"), source);
    writeBytes(QDir(project).filePath(QStringLiteral("manifest.json")), QJsonDocument(manifest).toJson());

    const QString bundle = dir.filePath(QStringLiteral("should-not-exist.pnb"));
    QString why;
    QVERIFY(!PdfNotebookBundle::save(project, bundle, &why));
    QVERIFY2(why.contains(QStringLiteral("manifest's source file")), qPrintable(why));
    QVERIFY(!QFileInfo::exists(bundle));
}

void PdfNotebookBundleTest::testAbsoluteEntryIsNormalisedAndNeverEscapes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bundle = fixturePath(QStringLiteral("nb-absolute-entry.pnb"));
    QVERIFY(QFileInfo::exists(bundle));

    /// The rule refuses an absolute path on its own...
    QString why;
    QVERIFY(!PdfNotebookBundle::isSafeEntryPath(QStringLiteral("/tmp/nb-absolute-escape.txt"), &why));
    QVERIFY2(why.contains(QStringLiteral("absolute")), qPrintable(why));

    /// ...but KArchive's zip reader strips the leading "/" before the name reaches this code, so by
    /// the time there is a name to judge it is the ordinary relative one below and there is nothing
    /// left to refuse. The fixture is written by tests/data/nb-make-fixtures.py with Python's
    /// zipfile for exactly that reason -- KZip strips the slash on the way in, so it cannot produce
    /// the file -- and "unzip -l" shows the stored name really is "/tmp/nb-absolute-escape.txt".
    const PdfNotebookBundle::Info info = PdfNotebookBundle::inspect(bundle, &why);
    QVERIFY2(info.isValid(), qPrintable(why));
    QCOMPARE(info.unknown, QStringList({ QStringLiteral("tmp/nb-absolute-escape.txt") }));

    const QString dest = dir.filePath(QStringLiteral("unpacked"));
    QStringList ignored;
    QVERIFY2(PdfNotebookBundle::extract(bundle, dest, &why, &ignored), qPrintable(why));
    QCOMPARE(ignored, QStringList({ QStringLiteral("tmp/nb-absolute-escape.txt") }));

    /// The guarantee, and the only one KZip leaves room for: it never escapes.
    QVERIFY(!QFileInfo::exists(QStringLiteral("/tmp/nb-absolute-escape.txt")));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("tmp/nb-absolute-escape.txt"))));
    QVERIFY(!QFileInfo::exists(QDir(dest).filePath(QStringLiteral("tmp"))));

    /// And the notebook around it is still whole: the entry is reported and ignored, not a reason
    /// to refuse the notebook.
    QVERIFY(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("source.pdf"))));
    QVERIFY(QFileInfo::exists(QDir(dest).filePath(QStringLiteral("pages/p0001.kra"))));
    QVERIFY2(PdfSession::openProject(dest, &why).isValid(&why), qPrintable(why));
}

QTEST_MAIN(PdfNotebookBundleTest)
#include "PdfNotebookBundleTest.moc"
