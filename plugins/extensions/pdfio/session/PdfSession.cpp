/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfSession.h"

#include "backend/PdfRenderBackend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

/// The one folder, under Documents, that the notebook directories live in.
///
/// Part of the contract with the user rather than an implementation detail: the working format is
/// meant to be found, so the folder says what it holds and there is exactly one of it.
const char *NotebookFolderName = "PerfectNote Notebooks";

/// Where a test has pointed the Documents location, if anywhere. See
/// PdfSession::setDocumentsLocationForTests().
QString &documentsLocationOverride()
{
    static QString path;
    return path;
}

QString documentsLocation()
{
    return documentsLocationOverride().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
        : documentsLocationOverride();
}

/**
 * Whether a directory holds a readable manifest and the source that manifest names.
 *
 * Deliberately not openProject(): that also checksums the source, and a migration should not read
 * a notebook twice. What is being guarded against is a directory that is not a notebook at all.
 */
bool notebookLooksComplete(const QString &dir, QString *why)
{
    QString localWhy;
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(PdfSession::manifestPath(dir), &localWhy);
    if (!manifest.isValid(&localWhy)) {
        if (why) {
            *why = QStringLiteral("%1 does not hold a readable notebook (%2)").arg(dir, localWhy);
        }
        return false;
    }

    const QString source = PdfSession::sourcePath(dir, manifest.sourceFile);
    if (!QFileInfo::exists(source)) {
        if (why) {
            *why = QStringLiteral("%1 names a source that is not there (%2)").arg(dir, manifest.sourceFile);
        }
        return false;
    }

    return true;
}

QString numbered(const QString &directory, const QString &prefix, int index, const QString &suffix)
{
    return QStringLiteral("%1/p%2%3").arg(directory,
                                          QString::number(index + 1).rightJustified(4, QLatin1Char('0')),
                                          suffix);
}

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

QString PdfSession::manifestPath(const QString &projectDir)
{
    return QDir(projectDir).filePath(QStringLiteral("manifest.json"));
}

QString PdfSession::sourcePath(const QString &projectDir, const QString &sourceFile)
{
    /// Deliberately a plain join: this is the path builder every consumer uses, and the names it is
    /// handed have already been checked by PdfSessionManifest::isValid(). openProject() checks the
    /// join itself below; createProject() builds the name from QFileInfo::fileName().
    return QDir(projectDir).filePath(sourceFile);
}

bool PdfSession::isPathInsideProject(const QString &projectDir, const QString &relative, QString *why)
{
    const QString root = QDir::cleanPath(QDir(projectDir).absolutePath());
    const QString joined = QDir::cleanPath(QDir(projectDir).filePath(relative));
    if (joined == root || !joined.startsWith(root + QLatin1Char('/'))) {
        fail(why, QStringLiteral("\"%1\" does not stay inside %2").arg(relative, root));
        return false;
    }
    return true;
}

QString PdfSession::pageFileName(int index)
{
    return pageFileNameForNumber(index + 1);
}

QString PdfSession::pageFileNameForNumber(int number)
{
    /// The same shape PdfSessionManifest::refreshNextPageNumber() reads back, and the one the
    /// manifest's pageFileName() has always produced: "pages/pNNNN.kra".
    return QStringLiteral("pages/p%1.kra")
        .arg(QString::number(number).rightJustified(4, QLatin1Char('0')));
}

QString PdfSession::thumbFileName(int index)
{
    return numbered(QStringLiteral("thumbs"), QStringLiteral("p"), index, QStringLiteral(".png"));
}

PdfSessionManifest PdfSession::createProject(const QString &projectDir,
                                             const QString &sourcePdf,
                                             PdfRenderBackend &backend,
                                             QString *why)
{
    if (!QFileInfo::exists(sourcePdf)) {
        fail(why, QStringLiteral("no such source: %1").arg(sourcePdf));
        return PdfSessionManifest();
    }

    const QDir dir(projectDir);
    if (dir.exists() && !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        fail(why, QStringLiteral("%1 already exists and is not empty").arg(projectDir));
        return PdfSessionManifest();
    }

    if (!backend.open(sourcePdf)) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(sourcePdf));
        return PdfSessionManifest();
    }

    if (!QDir().mkpath(dir.filePath(QStringLiteral("pages"))) ||
        !QDir().mkpath(dir.filePath(QStringLiteral("thumbs")))) {
        fail(why, QStringLiteral("cannot create the project directories under %1").arg(projectDir));
        return PdfSessionManifest();
    }

    const QString sourceFile = QFileInfo(sourcePdf).fileName();
    const QString copied = sourcePath(projectDir, sourceFile);
    if (!QFile::copy(sourcePdf, copied)) {
        fail(why, QStringLiteral("cannot copy the source into %1").arg(projectDir));
        return PdfSessionManifest();
    }

    PdfSessionManifest manifest;
    manifest.sourceFile = sourceFile;
    manifest.sourceSha256 = PdfSessionManifest::sha256OfFile(copied);
    manifest.sourceByteSize = QFileInfo(copied).size();

    /// The list those three fields describe, so a page can name the file its background comes from
    /// from the very first page: one source now, more once pages arrive from another PDF.
    PdfSourceRecord source;
    source.file = sourceFile;
    source.sha256 = manifest.sourceSha256;
    source.byteSize = manifest.sourceByteSize;
    manifest.sources.append(source);

    for (int i = 0; i < backend.pageCount(); ++i) {
        const PdfPageInfo info = backend.pageInfo(i);
        if (!info.isValid()) {
            fail(why, QStringLiteral("page %1 has no usable geometry").arg(i + 1));
            return PdfSessionManifest();
        }

        PdfPageRecord page;
        page.index = info.index;
        page.sizePt = info.sizePt;
        page.rotation = info.rotation;
        page.kraFile = pageFileName(i);
        page.thumbFile = thumbFileName(i);
        page.generation = 0;
        manifest.pages.append(page);
    }

    /// One page per number, and the counter past them: a number handed out later can never be one
    /// of these, whatever happens to the page list in between.
    manifest.refreshNextPageNumber();

    if (!manifest.writeTo(manifestPath(projectDir), why)) {
        return PdfSessionManifest();
    }

    return manifest;
}

PdfSessionManifest PdfSession::openProject(const QString &projectDir, QString *why)
{
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(manifestPath(projectDir), why);
    if (!manifest.isValid(why)) {
        return PdfSessionManifest();
    }

    /// The rule in the manifest already refuses the names that escape, and the join is checked
    /// again here: this is the one function every consumer goes through before it reads or writes
    /// anything, and a check made where the path is built cannot be outflanked by a rule that was
    /// applied somewhere else.
    if (!isPathInsideProject(projectDir, manifest.sourceFile, why)) {
        return PdfSessionManifest();
    }

    /// Every source, not only the first. A notebook whose pages arrived from another PDF draws
    /// those pages' backgrounds from that file too, and one that is missing or has been edited
    /// would otherwise render blank paper -- or the wrong page -- with nothing said.
    for (int i = 0; i < manifest.sourceCount(); ++i) {
        const PdfSourceRecord record = manifest.sourceAt(i);
        const QString source = sourcePath(projectDir, record.file);
        if (!QFileInfo::exists(source)) {
            fail(why, i == 0
                          ? QStringLiteral("the source file %1 is missing").arg(record.file)
                          : QStringLiteral("the source file %1 that the notebook's page backgrounds "
                                           "are drawn from is missing").arg(record.file));
            return PdfSessionManifest();
        }

        /// A source is never written back to, so a checksum change means someone else edited or
        /// replaced it and the recorded page geometry can no longer be trusted.
        if (PdfSessionManifest::sha256OfFile(source) != record.sha256) {
            fail(why, QStringLiteral("the source file %1 changed since the project was created")
                          .arg(record.file));
            return PdfSessionManifest();
        }

        /// And every one of them has to stay inside the project, the same rule the first has
        /// always had: a source's name is joined onto the project directory to be opened.
        if (!isPathInsideProject(projectDir, record.file, why)) {
            return PdfSessionManifest();
        }
    }

    return manifest;
}

QString PdfSession::legacyProjectRoot()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("pdfio-projects"));
}

QString PdfSession::projectRoot()
{
#ifdef Q_OS_ANDROID
    /// Not a preference. The manifest targets SDK 36 and asks for neither
    /// requestLegacyExternalStorage nor MANAGE_EXTERNAL_STORAGE, so scoped storage refuses a
    /// direct-path write into a shared Documents tree -- and this format writes on every page
    /// save, so a notebook there would fail to save rather than merely be untidy. The path a
    /// notebook takes into Documents on Android is the SAF bundle, not this function.
    return legacyProjectRoot();
#else
    const QString documents = documentsLocation();
    const QFileInfo location(documents);
    if (documents.isEmpty() || !location.isDir() || !location.isWritable()) {
        /// No Documents folder, or one nothing may be written to. Falling back is the whole point:
        /// a notebook must open on a machine that has no Documents directory.
        return legacyProjectRoot();
    }

    return QDir(documents).filePath(QString::fromLatin1(NotebookFolderName));
#endif
}

void PdfSession::setDocumentsLocationForTests(const QString &path)
{
    documentsLocationOverride() = path;
}

QString PdfSession::migrateFromLegacy(const QString &name, QString *why)
{
    const QString currentDir = QDir(projectRoot()).filePath(name);
    const QString legacyDir = QDir(legacyProjectRoot()).filePath(name);

    /// Already where notebooks live now. This also covers the case where both roots are the same
    /// directory -- Android, and the Documents fallback -- because then currentDir is legacyDir.
    if (QFileInfo::exists(manifestPath(currentDir))) {
        return currentDir;
    }

    /// Nothing to move: a notebook that has never been opened before, and is about to be created
    /// under the current root.
    if (!QFileInfo::exists(manifestPath(legacyDir))) {
        return currentDir;
    }

    /// One look before the move. A directory that is not a notebook is not moved, and a failure
    /// here leaves the legacy copy exactly as it was.
    if (!notebookLooksComplete(legacyDir, why)) {
        return legacyDir;
    }

    if (!QDir().mkpath(projectRoot())) {
        if (why) {
            *why = QStringLiteral("%1 cannot be created").arg(projectRoot());
        }
        return legacyDir;
    }

    if (!QDir().rename(legacyDir, currentDir)) {
        if (why) {
            *why = QStringLiteral("%1 cannot be moved to %2").arg(legacyDir, currentDir);
        }
        return legacyDir;
    }

    if (!notebookLooksComplete(currentDir, why)) {
        /// The rename itself is atomic, so this is the manifest or the source being unreadable
        /// rather than a half-moved directory. Put it back anyway: one complete copy at the old
        /// root is worth more than a moved one nobody has checked.
        const QString failure = why ? *why : QString();
        if (QDir().rename(currentDir, legacyDir) && why) {
            *why = QStringLiteral("%1; the notebook was put back at %2").arg(failure, legacyDir);
        }
        return legacyDir;
    }

    return currentDir;
}
