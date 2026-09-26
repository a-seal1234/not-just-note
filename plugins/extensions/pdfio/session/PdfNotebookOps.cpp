/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfNotebookOps.h"

#include "backend/PdfRenderBackend.h"
#include "session/PdfSession.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

namespace {

const char *const OpsDirName = ".ops";
const char *const LastDirName = "last";
const char *const BeforeName = "before.json";
const char *const OpName = "op.json";
const char *const RemovedDirName = "removed";
const char *const AddedName = "added.txt";

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// The sidecar directory of an artifact: what the strip reads a page's layers out of.
QString sidecarDirOf(const QString &kraFile)
{
    return kraFile + QStringLiteral(".layers");
}

/**
 * Everything an operation will do, worked out entirely before any of it is done.
 *
 * The planning half is separate from the doing half on purpose: whether an operation is possible
 * at all is answered by reading the manifest, not by starting to write files and finding out.
 */
struct Plan {
    PdfSessionManifest after;
    const char *opName = "";
    /// The reader's page before the change, for the journal and for an undo.
    int anchorBefore = 0;
    /// relative source -> relative destination, whole trees. Copied before the manifest.
    QList<QPair<QString, QString>> copyDirs;
    /// relative source -> relative destination, single files. Copied before the manifest.
    QList<QPair<QString, QString>> copyFiles;
    /// absolute source outside the project -> relative destination inside it: a PDF being inserted,
    /// or the files of a notebook being merged in. Copied before the manifest, like everything else
    /// that manifest is about to name.
    QList<QPair<QString, QString>> copyExternal;
    /// The same, for whole trees: the sidecar directory of a page arriving from another notebook.
    QList<QPair<QString, QString>> copyExternalDirs;
    /// relative paths the journal takes over after the manifest is committed.
    QStringList removeAfter;
    /// relative paths this operation creates, for the rollback and for an undo.
    QStringList added;
    /// Artifacts to turn in place. Each is turned into a name beside itself, the original is
    /// journalled, and only then does the turned file take its place -- all before the commit.
    struct Rotation {
        QString kraFile;
        int degrees = 0;
    };
    QList<Rotation> rotations;
    QString summary;
};

bool copyDirectory(const QString &from, const QString &to)
{
    const QDir source(from);
    if (!source.exists()) {
        return true;
    }
    if (!QDir().mkpath(to)) {
        return false;
    }
    const QFileInfoList entries = source.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        const QString target = QDir(to).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!copyDirectory(entry.absoluteFilePath(), target)) {
                return false;
            }
        } else if (!QFile::copy(entry.absoluteFilePath(), target)) {
            return false;
        }
    }
    return true;
}

void removePath(const QString &absolute)
{
    const QFileInfo info(absolute);
    if (info.isDir()) {
        QDir(absolute).removeRecursively();
    } else if (info.exists()) {
        QFile::remove(absolute);
    }
}

bool writeLines(const QString &path, const QStringList &lines)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8() + "\n";
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QStringList readLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringList();
    }
    QStringList lines;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const QString text = QString::fromUtf8(line).trimmed();
        if (!text.isEmpty()) {
            lines.append(text);
        }
    }
    return lines;
}

/// Moves \a relative out of the project and into the journal's removed/. Missing is not a failure:
/// a page that was never drawn on has no artifact, and one without a preview has no thumbnail.
bool moveIntoJournal(const QString &projectDir, const QString &journal, const QString &relative, QString *why)
{
    const QDir project(projectDir);
    const QString source = project.filePath(relative);
    if (!QFileInfo::exists(source)) {
        return true;
    }

    const QString destination =
        QDir(journal).filePath(QLatin1String(RemovedDirName) + QLatin1Char('/') + relative);
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        fail(why, QStringLiteral("cannot make room in the journal for %1").arg(relative));
        return false;
    }
    if (!QDir().rename(source, destination)) {
        fail(why, QStringLiteral("cannot move %1 out of the notebook").arg(relative));
        return false;
    }
    return true;
}

/// Puts everything a plan created back the way it was, and forgets the journal: an operation that
/// did not happen must not leave an undo behind that would "restore" the state it is already in.
void undoCreatedFiles(const QString &projectDir, const Plan &plan)
{
    const QDir project(projectDir);
    for (const QString &relative : plan.added) {
        removePath(project.filePath(relative));
    }
}

/// The journal's before.json is a copy of the manifest file itself, so this does not need the
/// already-parsed manifest: an undo restores the bytes that were there, including anything a hand
/// edit put in them that this code does not understand.
bool applyPlan(const QString &projectDir, const Plan &plan,
                const PdfNotebookOps::ArtifactRotator &rotator, QString *why)
{
    const QDir project(projectDir);
    const QString journal = PdfNotebookOps::journalDir(projectDir);

    /// A new journal replaces the last one. Undo is one change deep by design -- the state a second
    /// undo would need is exactly what this operation is about to destroy -- and the journal is
    /// what makes the change atomic and inspectable.
    QDir(journal).removeRecursively();
    if (!QDir().mkpath(journal)) {
        fail(why, QStringLiteral("cannot write the journal under %1").arg(projectDir));
        return false;
    }

    /// The manifest as it is right now, byte for byte: an undo restores the file, not a
    /// re-serialisation of it, so a hand edit this code does not understand is not lost.
    if (!QFile::copy(PdfSession::manifestPath(projectDir),
                     QDir(journal).filePath(QLatin1String(BeforeName)))) {
        fail(why, QStringLiteral("cannot journal the notebook's manifest"));
        return false;
    }

    QJsonObject op;
    op.insert(QStringLiteral("name"), QString::fromLatin1(plan.opName));
    op.insert(QStringLiteral("anchor"), plan.anchorBefore);
    op.insert(QStringLiteral("summary"), plan.summary);
    {
        QSaveFile file(QDir(journal).filePath(QLatin1String(OpName)));
        if (!file.open(QIODevice::WriteOnly)) {
            fail(why, QStringLiteral("cannot journal the operation"));
            QDir(journal).removeRecursively();
            return false;
        }
        file.write(QJsonDocument(op).toJson(QJsonDocument::Indented));
        if (!file.commit()) {
            fail(why, QStringLiteral("cannot journal the operation"));
            QDir(journal).removeRecursively();
            return false;
        }
    }

    /// What this operation writes at a path, written before it writes any of it: a crash then
    /// leaves a list of the files to clean up rather than an unknown pile of them.
    ///
    /// A turned artifact belongs on that list even though it is not "created": it takes the place
    /// of a file the journal holds, and an undo has to remove it before the original can be renamed
    /// back. It is kept out of plan.added on purpose -- that list is also the rollback's, and a
    /// failure BEFORE the turn must not delete the artifact it was going to replace.
    QStringList written = plan.added;
    for (const Plan::Rotation &rotation : plan.rotations) {
        written << rotation.kraFile << rotation.kraFile + QStringLiteral(".layers")
                << rotation.kraFile + QStringLiteral(".layers.txt");
    }
    if (!writeLines(QDir(journal).filePath(QLatin1String(AddedName)), written)) {
        fail(why, QStringLiteral("cannot journal the files this change creates"));
        QDir(journal).removeRecursively();
        return false;
    }

    /// One: every file the manifest is about to name.
    for (const QPair<QString, QString> &pair : plan.copyDirs) {
        if (!QFileInfo::exists(project.filePath(pair.first))) {
            continue;
        }
        if (!copyDirectory(project.filePath(pair.first), project.filePath(pair.second))) {
            fail(why, QStringLiteral("cannot copy %1").arg(pair.first));
            undoCreatedFiles(projectDir, plan);
            QDir(journal).removeRecursively();
            return false;
        }
    }
    for (const QPair<QString, QString> &pair : plan.copyExternalDirs) {
        if (!copyDirectory(pair.first, project.filePath(pair.second))) {
            fail(why, QStringLiteral("cannot copy %1 into the notebook").arg(pair.first));
            undoCreatedFiles(projectDir, plan);
            QDir(journal).removeRecursively();
            return false;
        }
    }
    for (const QPair<QString, QString> &pair : plan.copyExternal) {
        /// A file that is not there is not a failure: a page that was never drawn on has no
        /// artifact, and a page whose preview was never made has none either. The manifest records
        /// the name; the absence is the notebook's normal shape.
        if (!QFileInfo::exists(pair.first)) {
            continue;
        }
        const QString destination = project.filePath(pair.second);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())
            || !QFile::copy(pair.first, destination)) {
            fail(why, QStringLiteral("cannot copy %1 into the notebook").arg(pair.first));
            undoCreatedFiles(projectDir, plan);
            QDir(journal).removeRecursively();
            return false;
        }
    }
    for (const QPair<QString, QString> &pair : plan.copyFiles) {
        if (!QFileInfo::exists(project.filePath(pair.first))) {
            continue;
        }
        const QString destination = project.filePath(pair.second);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath())
            || !QFile::copy(project.filePath(pair.first), destination)) {
            fail(why, QStringLiteral("cannot copy %1 to %2").arg(pair.first, pair.second));
            undoCreatedFiles(projectDir, plan);
            QDir(journal).removeRecursively();
            return false;
        }
    }

    /// Three: the pages that are turned. Each artifact is turned into a name beside itself and read
    /// back by the rotator, then the original is journalled and the turned file takes its place --
    /// so at every instant there is a complete artifact under one name or the other, and a failure
    /// here puts back everything already turned before the operation gives up.
    QStringList turned;
    const auto removeTemporaryTurn = [](const QString &path) {
        removePath(path);
        removePath(path + QStringLiteral(".layers"));
        removePath(path + QStringLiteral(".layers.txt"));
    };
    const auto putTurnsBack = [&projectDir, &journal, &turned]() {
        const QDir project(projectDir);
        for (const QString &relative : turned) {
            for (const QString &suffix : { QString(), QStringLiteral(".layers"),
                                           QStringLiteral(".layers.txt") }) {
                removePath(project.filePath(relative + suffix));
                const QString held = QDir(journal).filePath(QLatin1String(RemovedDirName)
                                                             + QLatin1Char('/') + relative + suffix);
                if (QFileInfo::exists(held)) {
                    QDir().rename(held, project.filePath(relative + suffix));
                }
            }
        }
    };

    for (const Plan::Rotation &rotation : plan.rotations) {
        const QString original = project.filePath(rotation.kraFile);
        if (!QFileInfo::exists(original)) {
            /// A page that was never drawn on has no artifact: its turn is the manifest's alone.
            continue;
        }

        const QString temporary = original + QStringLiteral(".rotating");
        if (!rotator || !rotator(original, temporary, rotation.degrees, why)) {
            if (!why || why->isEmpty()) {
                fail(why, QStringLiteral("the page %1 could not be turned").arg(rotation.kraFile));
            }
            removeTemporaryTurn(temporary);
            putTurnsBack();
            undoCreatedFiles(projectDir, plan);
            QDir(journal).removeRecursively();
            return false;
        }

        bool swapped = true;
        for (const QString &suffix : { QString(), QStringLiteral(".layers"), QStringLiteral(".layers.txt") }) {
            if (!moveIntoJournal(projectDir, journal, rotation.kraFile + suffix, why)) {
                swapped = false;
                break;
            }
        }
        if (swapped) {
            for (const QString &suffix : { QString(), QStringLiteral(".layers"), QStringLiteral(".layers.txt") }) {
                const QString from = temporary + suffix;
                if (QFileInfo::exists(from) && !QDir().rename(from, original + suffix)) {
                    fail(why, QStringLiteral("the turned page could not take the place of %1")
                                  .arg(rotation.kraFile));
                    swapped = false;
                    break;
                }
            }
        }
        if (!swapped) {
            removeTemporaryTurn(temporary);
            putTurnsBack();
            undoCreatedFiles(projectDir, plan);
            QDir(journal).removeRecursively();
            return false;
        }

        turned.append(rotation.kraFile);
    }

    /// Four: the manifest, atomically. This is the commit -- before it the notebook is exactly what
    /// it was, and after it the change has happened.
    PdfSessionManifest after = plan.after;
    after.refreshNextPageNumber();
    if (!after.writeTo(PdfSession::manifestPath(projectDir), why)) {
        /// The turned pages go back first: the manifest was not committed, so the notebook has to
        /// be the one it was, artifacts included.
        putTurnsBack();
        undoCreatedFiles(projectDir, plan);
        QDir(journal).removeRecursively();
        return false;
    }

    /// Five, and last: the files the change displaced. They are unreferenced now, so a failure
    /// here costs space and an undo that is only partly reversible -- not a broken notebook.
    QString moveFailure;
    for (const QString &relative : plan.removeAfter) {
        if (!moveIntoJournal(projectDir, journal, relative, &moveFailure)) {
            fail(why, moveFailure);
            return false;
        }
    }

    return true;
}

/// The page \a page becomes when \a from is moved to \a to.
int remapAfterMove(int page, int from, int to)
{
    if (page == from) {
        return to;
    }
    if (from < to) {
        return (page > from && page <= to) ? page - 1 : page;
    }
    return (page >= to && page < from) ? page + 1 : page;
}

/// The page \a page becomes when \a count pages are deleted from \a first, or -1 when the page
/// itself is one of them.
int remapAfterDelete(int page, int first, int count)
{
    if (page < first) {
        return page;
    }
    if (page >= first + count) {
        return page - count;
    }
    return -1;
}

/// A file name that is safe to put under sources/: printable, with no separator in it. A backslash
/// is refused because the manifest's own path rule refuses it, and this name is going into the
/// manifest.
QString safeSourceBase(const QString &pdfPath)
{
    QString clean;
    for (const QChar character : QFileInfo(pdfPath).completeBaseName()) {
        if (character.isPrint() && character != QLatin1Char('/') && character != QLatin1Char('\\')) {
            clean.append(character);
        }
    }
    clean = clean.trimmed();
    if (clean.size() > 64) {
        clean.truncate(64);
    }
    return clean.isEmpty() ? QStringLiteral("source") : clean;
}

/// sources/<sha8>-<name>.pdf, with an ordinal when that name is already taken by other content.
/// The checksum prefix is what makes two different PDFs with the same file name two files.
QString sourceRelativeName(const QByteArray &sha256, const QString &base, int ordinal)
{
    const QString key = QString::fromLatin1(sha256.left(8));
    const QString stem = ordinal <= 1 ? QStringLiteral("%1-%2").arg(key, base)
                                      : QStringLiteral("%1-%2-%3").arg(key, base).arg(ordinal);
    return QStringLiteral("sources/%1.pdf").arg(stem);
}

/// A name inside the target for a file arriving from another notebook: the name it already had,
/// with an ordinal when the target has a file of that name holding something else. A merge never
/// writes over a file the target already owns.
QString freeRelativeName(const QString &projectDir, const QString &incoming, const QByteArray &sha256)
{
    const QFileInfo info(incoming);
    const QString directory =
        info.path() == QLatin1String(".") ? QString() : info.path() + QLatin1Char('/');
    QString candidate = incoming;
    int ordinal = 1;
    while (QFileInfo::exists(QDir(projectDir).filePath(candidate))
           && PdfSessionManifest::sha256OfFile(QDir(projectDir).filePath(candidate)) != sha256) {
        candidate = QStringLiteral("%1%2-%3.%4")
                        .arg(directory, info.completeBaseName())
                        .arg(++ordinal)
                        .arg(info.suffix());
    }
    return candidate;
}

/// Places one of an incoming notebook's asset files in the target's assets/: under its own name, an
/// ordinal when the target has that name holding different bytes, and not copied at all when the
/// target already has the same bytes. The rule wherever a notebook's files are brought into another.
void planAssetCopy(const QString &projectDir, const QString &absoluteAsset, Plan *plan)
{
    const QString inTarget = QStringLiteral("assets/") + QFileInfo(absoluteAsset).fileName();
    const QString existing = QDir(projectDir).filePath(inTarget);
    if (QFileInfo::exists(existing)
        && PdfSessionManifest::sha256OfFile(existing)
               == PdfSessionManifest::sha256OfFile(absoluteAsset)) {
        return;
    }

    const QString relative = freeRelativeName(projectDir, inTarget, QByteArray());
    plan->copyExternal.append(qMakePair(absoluteAsset, relative));
    plan->added.append(relative);
}

bool loadManifest(const QString &projectDir, PdfSessionManifest *manifest, QString *why)
{
    *manifest = PdfSession::openProject(projectDir, why);
    if (manifest->isValid()) {
        return true;
    }

    /// openProject() has already said what was wrong -- "the source file X changed since the
    /// project was created" is the answer a caller needs. Asking the empty manifest it returned
    /// again would replace that with "no source file recorded", which says nothing about the
    /// notebook the caller actually has. Only a why that is still empty is filled in here.
    if (why && why->isEmpty()) {
        manifest->isValid(why);
    }
    return false;
}

PdfNotebookOps::Outcome refused(const QString &why)
{
    PdfNotebookOps::Outcome outcome;
    outcome.why = why;
    return outcome;
}

} // namespace

QString PdfNotebookOps::journalDir(const QString &projectDir)
{
    return QDir(projectDir).filePath(QLatin1String(OpsDirName) + QLatin1Char('/')
                                     + QLatin1String(LastDirName));
}

PdfNotebookOps::Outcome PdfNotebookOps::movePage(const QString &projectDir, int from, int to,
                                                 int currentPage)
{
    PdfSessionManifest before;
    QString why;
    if (!loadManifest(projectDir, &before, &why)) {
        return refused(why);
    }

    const int count = before.pages.size();
    if (from < 0 || from >= count || to < 0 || to >= count) {
        return refused(QStringLiteral("page %1 cannot be moved to position %2: the notebook has %3 page(s)")
                           .arg(from + 1).arg(to + 1).arg(count));
    }
    if (from == to) {
        return refused(QStringLiteral("page %1 is already at position %2").arg(from + 1).arg(to + 1));
    }
    if (currentPage < 0 || currentPage >= count) {
        currentPage = from;
    }

    Plan plan;
    plan.after = before;
    plan.after.pages.move(from, to);
    plan.opName = "move";
    plan.anchorBefore = currentPage;
    plan.summary = QStringLiteral("page %1 moved to position %2").arg(from + 1).arg(to + 1);

    Outcome outcome;
    if (!applyPlan(projectDir, plan, ArtifactRotator(), &why)) {
        outcome.why = why;
        return outcome;
    }

    outcome.ok = true;
    outcome.anchorPage = qBound(0, remapAfterMove(currentPage, from, to), count - 1);
    outcome.summary = plan.summary;
    return outcome;
}

PdfNotebookOps::Outcome PdfNotebookOps::duplicatePage(const QString &projectDir, int page,
                                                      int currentPage)
{
    PdfSessionManifest before;
    QString why;
    if (!loadManifest(projectDir, &before, &why)) {
        return refused(why);
    }

    const int count = before.pages.size();
    if (page < 0 || page >= count) {
        return refused(QStringLiteral("page %1 is not part of the notebook: it has %2 page(s)")
                           .arg(page + 1).arg(count));
    }
    if (currentPage < 0 || currentPage >= count) {
        currentPage = page;
    }

    Plan plan;
    plan.after = before;

    const PdfPageRecord original = before.pages.at(page);
    PdfPageRecord copy = original;
    const int number = plan.after.allocatePageNumber();
    copy.kraFile = PdfSession::pageFileNameForNumber(number);
    copy.thumbFile = PdfSession::thumbFileNameForNumber(number);
    copy.generation = 0;
    plan.after.pages.insert(page + 1, copy);

    /// The copy is its own page: its own artifact, its own sidecar, its own preview. Sharing any
    /// of them would make a later edit of one appear on the other.
    plan.copyDirs.append(qMakePair(sidecarDirOf(original.kraFile), sidecarDirOf(copy.kraFile)));
    plan.copyFiles.append(qMakePair(original.kraFile, copy.kraFile));
    if (!original.thumbFile.isEmpty()) {
        plan.copyFiles.append(qMakePair(original.thumbFile, copy.thumbFile));
    }
    plan.added << sidecarDirOf(copy.kraFile) << copy.kraFile;
    if (!original.thumbFile.isEmpty()) {
        plan.added << copy.thumbFile;
    }

    plan.opName = "duplicate";
    plan.anchorBefore = currentPage;
    plan.summary = QStringLiteral("page %1 duplicated as page %2").arg(page + 1).arg(page + 2);

    Outcome outcome;
    if (!applyPlan(projectDir, plan, ArtifactRotator(), &why)) {
        outcome.why = why;
        return outcome;
    }

    outcome.ok = true;
    /// The reader stays on the page they were reading: the copy arrives after it, and staying on
    /// the original is what "duplicate and keep going" means.
    outcome.anchorPage = qBound(0, currentPage >= page + 1 ? currentPage + 1 : currentPage, count);
    outcome.summary = plan.summary;
    return outcome;
}

PdfNotebookOps::Outcome PdfNotebookOps::insertPages(const QString &projectDir, int at,
                                                    const QString &pdfPath,
                                                    PdfRenderBackend &backend,
                                                    int firstPage, int count, int currentPage)
{
    PdfSessionManifest before;
    QString why;
    if (!loadManifest(projectDir, &before, &why)) {
        return refused(why);
    }

    if (!QFileInfo::exists(pdfPath)) {
        return refused(QStringLiteral("there is no PDF at %1").arg(pdfPath));
    }
    if (!backend.isOpen()) {
        return refused(QStringLiteral("the renderer is not open on %1").arg(pdfPath));
    }

    const int sourcePages = backend.pageCount();
    if (count < 0) {
        count = sourcePages - firstPage;
    }
    if (firstPage < 0 || count < 1 || firstPage + count > sourcePages) {
        return refused(QStringLiteral("pages %1..%2 are not in %3: it has %4 page(s)")
                           .arg(firstPage + 1)
                           .arg(firstPage + count)
                           .arg(QFileInfo(pdfPath).fileName())
                           .arg(sourcePages));
    }

    const int pages = before.pages.size();
    if (at < 0 || at > pages) {
        return refused(QStringLiteral("pages cannot be inserted at position %1: the notebook has %2 page(s)")
                           .arg(at + 1)
                           .arg(pages));
    }
    if (currentPage < 0 || currentPage >= pages) {
        currentPage = at > 0 ? at - 1 : 0;
    }

    const QByteArray sha = PdfSessionManifest::sha256OfFile(pdfPath);
    if (sha.isEmpty()) {
        return refused(QStringLiteral("%1 cannot be read").arg(pdfPath));
    }

    Plan plan;
    plan.after = before;
    plan.opName = "insert";

    /// The PDF becomes a source, or its pages are drawn from one that is already there. Inserting a
    /// notebook's own PDF, or the same handout twice, must not leave two copies of the same bytes in
    /// the project -- and an empty sources[] means "one source: the fields beside it", which has to
    /// become the first entry of the list before anything can be appended to it.
    int sourceIndex = plan.after.sourceIndexForSha(sha);
    if (sourceIndex < 0) {
        if (plan.after.sources.isEmpty()) {
            PdfSourceRecord existing;
            existing.file = plan.after.sourceFile;
            existing.sha256 = plan.after.sourceSha256;
            existing.byteSize = plan.after.sourceByteSize;
            plan.after.sources.append(existing);
        }

        const QString base = safeSourceBase(pdfPath);
        QString relative = sourceRelativeName(sha, base, 1);
        /// A name already taken inside the project by different content -- another file whose first
        /// eight hex digits agree, or a file put there by hand. The content is what the manifest
        /// records, so the name gives way, not the source.
        const QDir project(projectDir);
        int ordinal = 1;
        while (QFileInfo::exists(project.filePath(relative))
               && PdfSessionManifest::sha256OfFile(project.filePath(relative)) != sha) {
            relative = sourceRelativeName(sha, base, ++ordinal);
        }

        PdfSourceRecord source;
        source.file = relative;
        source.sha256 = sha;
        source.byteSize = QFileInfo(pdfPath).size();
        sourceIndex = plan.after.sources.size();
        plan.after.sources.append(source);

        /// A file with that name and that content is already there -- a leftover nothing
        /// references, from an operation that was undone. Copying over it would be pointless and
        /// listing it as created would make an undo delete a file the notebook already had.
        if (!QFileInfo::exists(project.filePath(relative))) {
            plan.copyExternal.append(qMakePair(pdfPath, relative));
            plan.added.append(relative);
        }
    }

    for (int i = 0; i < count; ++i) {
        const PdfPageInfo info = backend.pageInfo(firstPage + i);
        if (!info.isValid()) {
            return refused(QStringLiteral("page %1 of %2 has no usable geometry")
                               .arg(firstPage + i + 1)
                               .arg(QFileInfo(pdfPath).fileName()));
        }

        PdfPageRecord page;
        page.index = firstPage + i;
        page.source = sourceIndex;
        page.sizePt = info.sizePt;
        page.rotation = info.rotation;
        /// Its own number from the allocator: the same PDF page can be inserted twice and the two
        /// copies stay independent, and nothing is written until one of them is drawn on.
        const int number = plan.after.allocatePageNumber();
        page.kraFile = PdfSession::pageFileNameForNumber(number);
        page.thumbFile = PdfSession::thumbFileNameForNumber(number);
        page.generation = 0;
        plan.after.pages.insert(at + i, page);
    }

    plan.anchorBefore = currentPage;
    plan.summary = count == 1
        ? QStringLiteral("inserted page %1 of %2 as notebook page %3")
              .arg(firstPage + 1)
              .arg(QFileInfo(pdfPath).fileName())
              .arg(at + 1)
        : QStringLiteral("inserted %1 pages of %2 as notebook pages %3..%4")
              .arg(count)
              .arg(QFileInfo(pdfPath).fileName())
              .arg(at + 1)
              .arg(at + count);

    Outcome outcome;
    if (!applyPlan(projectDir, plan, ArtifactRotator(), &why)) {
        outcome.why = why;
        return outcome;
    }

    /// The reader keeps the page they were reading: pages arriving before it push it down by
    /// exactly the number that arrived.
    outcome.ok = true;
    outcome.anchorPage =
        qBound(0, at <= currentPage ? currentPage + count : currentPage, plan.after.pages.size() - 1);
    outcome.summary = plan.summary;
    return outcome;
}

PdfNotebookOps::Outcome PdfNotebookOps::extractRange(const QString &projectDir, int first, int count,
                                                     const QString &destinationDir)
{
    return extractRange(projectDir, first, count, destinationDir, ExtractOptions());
}

PdfNotebookOps::Outcome PdfNotebookOps::extractRange(const QString &projectDir, int first, int count,
                                                     const QString &destinationDir,
                                                     const ExtractOptions &options)
{
    PdfSessionManifest source;
    QString why;
    if (!loadManifest(projectDir, &source, &why)) {
        return refused(why);
    }

    const int pages = source.pages.size();
    if (count < 1) {
        return refused(QStringLiteral("there is no page to extract"));
    }
    if (first < 0 || first + count > pages) {
        return refused(QStringLiteral("pages %1..%2 are not part of the notebook: it has %3 page(s)")
                           .arg(first + 1).arg(first + count).arg(pages));
    }
    if (destinationDir.isEmpty()) {
        return refused(QStringLiteral("no destination was given for the new notebook"));
    }

    const QString destination = QFileInfo(destinationDir).absoluteFilePath();
    if (QFileInfo::exists(destination) && !options.replaceExisting) {
        return refused(QStringLiteral("%1 is already there; choose another name for the new notebook")
                           .arg(destination));
    }
    const QString parent = QFileInfo(destination).absolutePath();
    if (!QDir().mkpath(parent)) {
        return refused(QStringLiteral("cannot create %1").arg(parent));
    }

    /// Built whole beside where it will be, and renamed into place only once it has been read back
    /// and opened: a failure leaves nothing behind and never takes the notebook that is already
    /// there with it.
    QTemporaryDir staging(QDir(parent).filePath(
        QStringLiteral(".%1-extracting-XXXXXX").arg(QFileInfo(destination).fileName())));
    if (!staging.isValid()) {
        return refused(QStringLiteral("cannot create a temporary directory beside %1").arg(destination));
    }
    const QString stagingPath = staging.path();

    /// The sources the range draws from, in the order they are first needed, each copied under the
    /// name it already had inside the notebook it came from. The first of them becomes the new
    /// notebook's own source, because a manifest records one source beside the list.
    PdfSessionManifest after;
    after.schema = PdfSessionManifest::CurrentSchema;
    after.name = QFileInfo(destination).fileName();
    QList<int> sourceMap;
    for (int i = first; i < first + count; ++i) {
        const PdfPageRecord &page = source.pages.at(i);
        int mapped = sourceMap.indexOf(page.source);
        if (mapped < 0) {
            const PdfSourceRecord record = source.sourceAt(page.source);
            const QString from = QDir(projectDir).filePath(record.file);
            const QString to = QDir(stagingPath).filePath(record.file);
            if (!QFileInfo::exists(from)
                || !QDir().mkpath(QFileInfo(to).absolutePath())
                || !QFile::copy(from, to)) {
                return refused(QStringLiteral("cannot copy the source %1 into the new notebook")
                                   .arg(record.file));
            }
            mapped = sourceMap.size();
            sourceMap.append(page.source);
            after.sources.append(record);
        }

        PdfPageRecord copy = page;
        copy.source = mapped;
        after.pages.append(copy);
    }
    after.sourceFile = after.sources.first().file;
    after.sourceSha256 = after.sources.first().sha256;
    after.sourceByteSize = after.sources.first().byteSize;

    /// The pages' own files: the artifact, its sidecar (what the strip reads a page's layers out of)
    /// and the preview. A page that was never drawn on has no artifact and one never shown has no
    /// preview, and neither is a hole -- exactly as in the notebook they came from.
    const auto copyIfThere = [&projectDir, &stagingPath](const QString &relative) {
        const QString from = QDir(projectDir).filePath(relative);
        if (!QFileInfo::exists(from)) {
            return true;
        }
        const QString to = QDir(stagingPath).filePath(relative);
        return QDir().mkpath(QFileInfo(to).absolutePath()) && QFile::copy(from, to);
    };

    for (const PdfPageRecord &page : after.pages) {
        /// The sidecar is a directory, so it is copied whole; the artifact and the preview are
        /// files.
        if (!copyIfThere(page.kraFile) || !copyIfThere(page.thumbFile)) {
            return refused(QStringLiteral("cannot copy the files of the page %1").arg(page.kraFile));
        }
        if (!copyDirectory(QDir(projectDir).filePath(sidecarDirOf(page.kraFile)),
                           QDir(stagingPath).filePath(sidecarDirOf(page.kraFile)))) {
            return refused(QStringLiteral("cannot copy the sidecar of %1").arg(page.kraFile));
        }
    }

    /// And the notebook's assets/, which a page's content layer points at rather than the manifest:
    /// a notebook carried without them opens with its pictures missing.
    if (!copyDirectory(QDir(projectDir).filePath(QStringLiteral("assets")),
                       QDir(stagingPath).filePath(QStringLiteral("assets")))) {
        return refused(QStringLiteral("cannot copy the notebook's assets"));
    }

    /// One page per number and the counter past them, so the new notebook allocates names that are
    /// free in IT rather than names its own pages already use.
    after.refreshNextPageNumber();
    if (!after.writeTo(QDir(stagingPath).filePath(QStringLiteral("manifest.json")), &why)) {
        return refused(why);
    }

    /// Read back through the same door the notebook will be opened with: it checks that every
    /// source is there and hashes to what the manifest recorded, so a copy that did not arrive
    /// intact is caught here rather than by whoever opens the new notebook.
    if (!PdfSession::openProject(stagingPath, &why).isValid(&why)) {
        return refused(QStringLiteral("the new notebook does not read back: %1").arg(why));
    }

    /// Renamed, not copied: from here the directory is either the notebook or removed by hand, and
    /// QTemporaryDir must not delete it on the way out.
    staging.setAutoRemove(false);

    QString movedAside;
    if (QFileInfo::exists(destination)) {
        movedAside = destination + QStringLiteral(".replaced");
        removePath(movedAside);
        if (!QDir().rename(destination, movedAside)) {
            removePath(stagingPath);
            return refused(QStringLiteral("cannot move the notebook already at %1 out of the way")
                               .arg(destination));
        }
    }

    if (!QDir().rename(stagingPath, destination)) {
        if (!movedAside.isEmpty()) {
            QDir().rename(movedAside, destination);
        }
        removePath(stagingPath);
        return refused(QStringLiteral("cannot move the new notebook into %1").arg(destination));
    }

    if (!movedAside.isEmpty()) {
        removePath(movedAside);
    }

    Outcome outcome;
    outcome.ok = true;
    outcome.anchorPage = 0;
    outcome.summary = count == 1
        ? QStringLiteral("extracted page %1 as \"%2\"").arg(first + 1).arg(after.name)
        : QStringLiteral("extracted pages %1..%2 as \"%3\"")
              .arg(first + 1)
              .arg(first + count)
              .arg(after.name);
    return outcome;
}

QString PdfNotebookOps::sourceFileNameFor(const QString &pdfPath)
{
    return sourceRelativeName(PdfSessionManifest::sha256OfFile(pdfPath), safeSourceBase(pdfPath), 1);
}

PdfNotebookOps::Outcome PdfNotebookOps::rotatePages(const QString &projectDir, int first, int count,
                                                    int degrees, const ArtifactRotator &rotator,
                                                    int currentPage)
{
    PdfSessionManifest before;
    QString why;
    if (!loadManifest(projectDir, &before, &why)) {
        return refused(why);
    }

    const int pages = before.pages.size();
    if (count < 1) {
        return refused(QStringLiteral("there is no page to turn"));
    }
    if (first < 0 || first + count > pages) {
        return refused(QStringLiteral("pages %1..%2 are not part of the notebook: it has %3 page(s)")
                           .arg(first + 1)
                           .arg(first + count)
                           .arg(pages));
    }

    /// One value for the turn, normalized once, and used for both halves: the record's rotation and
    /// the angle the artifact is turned by. A left turn of 90 degrees is a right turn of 270, with
    /// the same result.
    const int turn = ((degrees % 360) + 360) % 360;
    if (turn != 90 && turn != 180 && turn != 270) {
        return refused(QStringLiteral("%1 degrees is not a quarter turn").arg(degrees));
    }
    if (currentPage < 0 || currentPage >= pages) {
        currentPage = first;
    }

    Plan plan;
    plan.after = before;
    plan.opName = "rotate";
    for (int i = first; i < first + count; ++i) {
        PdfPageRecord &page = plan.after.pages[i];
        page.extraRotation = ((page.extraRotation + turn) % 360 + 360) % 360;

        Plan::Rotation rotation;
        rotation.kraFile = page.kraFile;
        rotation.degrees = turn;
        plan.rotations.append(rotation);

        /// The turned artifact is written AT this path, so an undo has to remove it before the
        /// journal's copy can go back -- applyPlan() records it in added.txt for exactly that, and
        /// deliberately NOT in plan.added, which the rollback removes on a failure before the turn.

        /// A preview is a picture of the page as it was. Dropped rather than left showing the old
        /// orientation; the docker makes a new one when it asks for that page.
        if (!page.thumbFile.isEmpty()) {
            plan.removeAfter << page.thumbFile;
        }
    }

    plan.anchorBefore = currentPage;
    plan.summary = count == 1
        ? QStringLiteral("page %1 turned by %2 degrees").arg(first + 1).arg(turn)
        : QStringLiteral("pages %1..%2 turned by %3 degrees")
              .arg(first + 1)
              .arg(first + count)
              .arg(turn);

    Outcome outcome;
    if (!applyPlan(projectDir, plan, rotator, &why)) {
        outcome.why = why;
        return outcome;
    }

    /// Turning a page moves nothing and changes no position: the reader stays where they were.
    outcome.ok = true;
    outcome.anchorPage = currentPage;
    outcome.summary = plan.summary;
    return outcome;
}

PdfNotebookOps::Outcome PdfNotebookOps::mergeNotebook(const QString &projectDir, int at,
                                                       const QString &sourceDir, int currentPage)
{
    PdfSessionManifest target;
    QString why;
    if (!loadManifest(projectDir, &target, &why)) {
        return refused(why);
    }

    const QString source = QFileInfo(sourceDir).absoluteFilePath();
    if (source.isEmpty() || !QFileInfo::exists(PdfSession::manifestPath(source))) {
        return refused(QStringLiteral("%1 does not hold a notebook").arg(sourceDir));
    }
    if (source == QFileInfo(projectDir).absoluteFilePath()) {
        return refused(QStringLiteral("a notebook cannot be merged into itself"));
    }

    PdfSessionManifest incoming;
    if (!loadManifest(source, &incoming, &why)) {
        return refused(why);
    }

    const int pages = target.pages.size();
    if (at < 0 || at > pages) {
        return refused(QStringLiteral("pages cannot be merged in at position %1: the notebook has "
                                      "%2 page(s)")
                           .arg(at + 1).arg(pages));
    }
    if (currentPage < 0 || currentPage >= pages) {
        currentPage = at > 0 ? at - 1 : 0;
    }

    Plan plan;
    plan.after = target;
    plan.opName = "merge";

    /// The list has to be there before anything can be appended to it: an empty one means "one
    /// source: the fields beside it", which is what a manifest built in code looks like.
    if (plan.after.sources.isEmpty()) {
        PdfSourceRecord existing;
        existing.file = plan.after.sourceFile;
        existing.sha256 = plan.after.sourceSha256;
        existing.byteSize = plan.after.sourceByteSize;
        plan.after.sources.append(existing);
    }

    const QDir from(source);
    for (int i = 0; i < incoming.pages.size(); ++i) {
        const PdfPageRecord &page = incoming.pages.at(i);
        const PdfSourceRecord incomingSource = incoming.sourceForPage(page);

        /// The PDF travels once. A target that already has this content draws those pages from the
        /// entry it has; otherwise the file is copied in, under its own name unless that name is
        /// taken in the target by something else.
        int sourceIndex = plan.after.sourceIndexForSha(incomingSource.sha256);
        if (sourceIndex < 0) {
            const QString relative = freeRelativeName(projectDir, incomingSource.file,
                                                      incomingSource.sha256);
            plan.copyExternal.append(qMakePair(from.filePath(incomingSource.file), relative));
            plan.added.append(relative);

            PdfSourceRecord record = incomingSource;
            record.file = relative;
            sourceIndex = plan.after.sources.size();
            plan.after.sources.append(record);
        }

        /// The page keeps its place in its source and its geometry, and gets a name nothing in the
        /// target uses: artifact numbers belong to the notebook's own allocator, so a merge cannot
        /// land on top of a page that is already there.
        PdfPageRecord copy = page;
        copy.source = sourceIndex;
        copy.generation = 0;
        const int number = plan.after.allocatePageNumber();
        copy.kraFile = PdfSession::pageFileNameForNumber(number);
        copy.thumbFile = PdfSession::thumbFileNameForNumber(number);
        plan.after.pages.insert(at + i, copy);

        plan.copyExternal.append(qMakePair(from.filePath(page.kraFile), copy.kraFile));
        plan.added.append(copy.kraFile);
        if (!page.thumbFile.isEmpty()) {
            plan.copyExternal.append(qMakePair(from.filePath(page.thumbFile), copy.thumbFile));
            plan.added.append(copy.thumbFile);
        }
        plan.copyExternalDirs.append(
            qMakePair(from.filePath(sidecarDirOf(page.kraFile)), sidecarDirOf(copy.kraFile)));
        plan.added.append(sidecarDirOf(copy.kraFile));
    }

    /// Whatever the incoming notebook kept in assets/ comes too, because a page's content layer
    /// points at it rather than at the manifest. A file the target already has with the same bytes
    /// is not copied twice; one it has with other bytes gets an ordinal name, so neither is lost.
    const QDir incomingAssets(from.filePath(QStringLiteral("assets")));
    for (const QString &name : incomingAssets.entryList(QDir::Files, QDir::Name)) {
        planAssetCopy(projectDir, incomingAssets.filePath(name), &plan);
    }

    plan.anchorBefore = currentPage;
    plan.summary = QStringLiteral("merged %1 page(s) from \"%2\"")
                       .arg(incoming.pages.size())
                       .arg(incoming.displayName());

    Outcome outcome;
    if (!applyPlan(projectDir, plan, ArtifactRotator(), &why)) {
        outcome.why = why;
        return outcome;
    }

    /// The reader keeps the page they were reading: what arrived was placed before it or after it,
    /// and either way that page moved by the number that arrived before it.
    const int arrived = incoming.pages.size();
    outcome.ok = true;
    outcome.anchorPage =
        qBound(0, at <= currentPage ? currentPage + arrived : currentPage,
               plan.after.pages.size() - 1);
    outcome.summary = plan.summary;
    return outcome;
}

PdfNotebookOps::Outcome PdfNotebookOps::deletePages(const QString &projectDir, int first, int count,
                                                    int currentPage)
{
    PdfSessionManifest before;
    QString why;
    if (!loadManifest(projectDir, &before, &why)) {
        return refused(why);
    }

    const int pages = before.pages.size();
    if (count < 1) {
        return refused(QStringLiteral("there is no page to delete"));
    }
    if (first < 0 || first + count > pages) {
        return refused(QStringLiteral("pages %1..%2 are not part of the notebook: it has %3 page(s)")
                           .arg(first + 1).arg(first + count).arg(pages));
    }
    if (pages - count < 1) {
        /// A notebook with no pages is not a notebook: the manifest refuses it, and there would be
        /// nothing to open. Deleting the last one is refused rather than left half done.
        return refused(QStringLiteral("the notebook's last page cannot be deleted; a notebook needs "
                                      "at least one page"));
    }
    if (currentPage < 0 || currentPage >= pages) {
        currentPage = first;
    }

    Plan plan;
    plan.after = before;
    for (int i = first; i < first + count; ++i) {
        const PdfPageRecord &page = before.pages.at(i);
        plan.removeAfter << page.kraFile << sidecarDirOf(page.kraFile);
        if (!page.thumbFile.isEmpty()) {
            plan.removeAfter << page.thumbFile;
        }
    }
    for (int i = 0; i < count; ++i) {
        plan.after.pages.removeAt(first);
    }

    plan.opName = "delete";
    plan.anchorBefore = currentPage;
    plan.summary = count == 1
        ? QStringLiteral("page %1 deleted").arg(first + 1)
        : QStringLiteral("pages %1..%2 deleted").arg(first + 1).arg(first + count);

    Outcome outcome;
    if (!applyPlan(projectDir, plan, ArtifactRotator(), &why)) {
        outcome.why = why;
        return outcome;
    }

    const int remaining = pages - count;
    const int remapped = remapAfterDelete(currentPage, first, count);
    outcome.ok = true;
    /// The page that took the deleted one's place, or the last one when the end was deleted: the
    /// reader lands on a page that exists, next to where they were.
    outcome.anchorPage = qBound(0, remapped >= 0 ? remapped : qMin(first, remaining - 1), remaining - 1);
    outcome.summary = plan.summary;
    return outcome;
}

int PdfNotebookOps::nextFreePageNumber(const QString &projectDir)
{
    QString why;
    const PdfSessionManifest manifest = PdfSession::openProject(projectDir, &why);
    if (!manifest.isValid()) {
        return -1;
    }
    return manifest.effectiveNextPageNumber();
}

PdfNotebookOps::Outcome PdfNotebookOps::applyPageEdits(const QString &projectDir,
                                                       const PageEdits &edits,
                                                       const ArtifactRotator &rotator)
{
    PdfSessionManifest before;
    QString why;
    if (!loadManifest(projectDir, &before, &why)) {
        return refused(why);
    }

    /// The same rules a notebook always has: at least one page, and a PDF to draw them from.
    if (edits.pages.isEmpty()) {
        return refused(QStringLiteral("a notebook keeps at least one page"));
    }
    if (edits.sources.isEmpty()) {
        return refused(QStringLiteral("the page list has no PDF to be drawn from"));
    }

    Plan plan;
    plan.after = before;
    plan.after.sources = edits.sources;
    plan.after.pages = edits.pages;
    plan.after.sourceFile = edits.sources.first().file;
    plan.after.sourceSha256 = edits.sources.first().sha256;
    plan.after.sourceByteSize = edits.sources.first().byteSize;
    plan.opName = "edits";
    plan.copyExternal = edits.copyExternal;
    plan.copyExternalDirs = edits.copyExternalDirs;
    plan.removeAfter = edits.removeAfter;
    plan.summary = edits.summary;
    /// The turns are read off the records rather than listed separately: whatever extraRotation a
    /// page's record has gained over the record it came from is what its artifact has to be turned
    /// by. For a page that stayed that is its own record before; for a duplicate it is the record it
    /// was copied from, found through the copy's source path. One place the turn lives means a
    /// caller cannot ask for a turned artifact and an unturned record -- which is paper and ink
    /// disagreeing, the defect this project keeps designing against.
    QHash<QString, int> turnBefore;
    for (const PdfPageRecord &page : before.pages) {
        turnBefore.insert(page.kraFile, page.extraRotation);
    }
    QHash<QString, QString> copiedFrom;
    for (const QPair<QString, QString> &pair : plan.copyExternal) {
        copiedFrom.insert(pair.second, pair.first);
    }
    for (const PdfPageRecord &page : plan.after.pages) {
        int from = 0;
        const QHash<QString, int>::const_iterator own = turnBefore.constFind(page.kraFile);
        if (own != turnBefore.constEnd()) {
            from = own.value();
        } else {
            const QString source = copiedFrom.value(page.kraFile);
            const QString sourceName =
                source.isEmpty() ? QString() : QDir(projectDir).relativeFilePath(source);
            const QHash<QString, int>::const_iterator origin = turnBefore.constFind(sourceName);
            if (origin == turnBefore.constEnd()) {
                /// A page arriving from another notebook or PDF: its artifact is copied as it was
                /// stored, so there is nothing to turn.
                continue;
            }
            from = origin.value();
        }

        const int turn = ((page.extraRotation - from) % 360 + 360) % 360;
        if (turn == 0) {
            continue;
        }
        if (turn != 90 && turn != 180 && turn != 270) {
            return refused(QStringLiteral("page %1 would be turned by %2 degrees, which is not a "
                                          "quarter turn")
                               .arg(page.kraFile).arg(turn));
        }
        Plan::Rotation rotation;
        rotation.kraFile = page.kraFile;
        rotation.degrees = turn;
        plan.rotations.append(rotation);
    }

    /// The PDFs this change brings in, copied and named here so that an insert commits together
    /// with everything around it. Where each one lands is remembered: a PDF the notebook already has
    /// is drawn from its existing entry, and the pages that named the addition are re-pointed at it.
    QHash<int, int> additionLandedAt;
    for (int k = 0; k < edits.additions.size(); ++k) {
        const QString &pdfPath = edits.additions.at(k);
        if (!QFileInfo::exists(pdfPath)) {
            return refused(QStringLiteral("there is no PDF at %1").arg(pdfPath));
        }
        const QByteArray sha = PdfSessionManifest::sha256OfFile(pdfPath);
        if (sha.isEmpty()) {
            return refused(QStringLiteral("%1 cannot be read").arg(pdfPath));
        }

        const int existing = plan.after.sourceIndexForSha(sha);
        if (existing >= 0) {
            additionLandedAt.insert(k, existing);
            continue;
        }

        const QString base = safeSourceBase(pdfPath);
        QString relative = sourceRelativeName(sha, base, 1);
        const QDir project(projectDir);
        int ordinal = 1;
        while (QFileInfo::exists(project.filePath(relative))
               && PdfSessionManifest::sha256OfFile(project.filePath(relative)) != sha) {
            relative = sourceRelativeName(sha, base, ++ordinal);
        }

        PdfSourceRecord source;
        source.file = relative;
        source.sha256 = sha;
        source.byteSize = QFileInfo(pdfPath).size();
        additionLandedAt.insert(k, plan.after.sources.size());
        plan.after.sources.append(source);

        /// A file with that name and that content is already there -- a leftover nothing references.
        /// Copying over it would be pointless, and listing it as created would make an undo delete a
        /// file the notebook already had.
        if (!QFileInfo::exists(project.filePath(relative))) {
            plan.copyExternal.append(qMakePair(pdfPath, relative));
            plan.added.append(relative);
        }
    }

    /// The assets an incoming notebook brought with it, by the same rule a merge uses.
    for (const QString &asset : edits.assetsToMerge) {
        if (!QFileInfo::exists(asset)) {
            return refused(QStringLiteral("there is no file at %1 to bring into the notebook")
                               .arg(asset));
        }
        planAssetCopy(projectDir, asset, &plan);
    }

    /// And every page that named an addition is pointed at where that PDF really landed.
    for (PdfPageRecord &page : plan.after.pages) {
        if (page.source < edits.sources.size()) {
            continue;
        }
        const int k = page.source - edits.sources.size();
        if (!additionLandedAt.contains(k)) {
            return refused(QStringLiteral("page %1 of the change names a PDF that is not being "
                                          "brought in")
                               .arg(page.kraFile));
        }
        page.source = additionLandedAt.value(k);
    }

    /// Two pages cannot share a file: a caller that allocated the same number twice would write one
    /// page's ink over the other's.
    QSet<QString> namedPages;
    for (const PdfPageRecord &page : plan.after.pages) {
        if (page.kraFile.isEmpty()) {
            continue;
        }
        if (namedPages.contains(page.kraFile)) {
            return refused(QStringLiteral("two pages of the change would share %1").arg(page.kraFile));
        }
        namedPages.insert(page.kraFile);
    }

    /// Every path this change creates goes into the rollback's and the undo's list -- and none of
    /// them may be a path the notebook already names. A screen that miscounted its numbers would
    /// otherwise write over a page's ink, which is the one thing the allocator exists to prevent.
    QSet<QString> named;
    for (const PdfPageRecord &page : before.pages) {
        named.insert(page.kraFile);
        if (!page.thumbFile.isEmpty()) {
            named.insert(page.thumbFile);
        }
    }
    const auto acceptDestination = [&](const QString &relative) {
        if (named.contains(relative)) {
            why = QStringLiteral("the change would write over %1, which the notebook already has")
                      .arg(relative);
            return false;
        }
        plan.added.append(relative);
        return true;
    };
    for (const QPair<QString, QString> &pair : plan.copyExternal) {
        if (!acceptDestination(pair.second)) {
            return refused(why);
        }
    }
    for (const QPair<QString, QString> &pair : plan.copyExternalDirs) {
        if (!acceptDestination(pair.second)) {
            return refused(why);
        }
    }

    /// A change that changes nothing is not a change: no journal, no manifest write, no reload. The
    /// screen keeps Apply off while its list is the list on disk, and this is the same rule where it
    /// matters most -- nothing is written even if a caller asks for a no-op.
    const auto samePageList = [](const QList<PdfPageRecord> &a, const QList<PdfPageRecord> &b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (int i = 0; i < a.size(); ++i) {
            if (a.at(i).kraFile != b.at(i).kraFile || a.at(i).thumbFile != b.at(i).thumbFile
                || a.at(i).source != b.at(i).source || a.at(i).index != b.at(i).index
                || a.at(i).sizePt != b.at(i).sizePt
                || a.at(i).extraRotation != b.at(i).extraRotation
                || a.at(i).rotation != b.at(i).rotation) {
                return false;
            }
        }
        return true;
    };
    bool sameSources = plan.after.sources.size() == before.sources.size();
    if (sameSources) {
        for (int i = 0; i < plan.after.sources.size(); ++i) {
            if (plan.after.sources.at(i).file != before.sources.at(i).file
                || plan.after.sources.at(i).sha256 != before.sources.at(i).sha256) {
                sameSources = false;
                break;
            }
        }
    }
    if (plan.copyExternal.isEmpty() && plan.copyExternalDirs.isEmpty() && plan.rotations.isEmpty()
        && plan.removeAfter.isEmpty() && sameSources
        && samePageList(plan.after.pages, before.pages)) {
        Outcome nothing;
        nothing.ok = true;
        nothing.anchorPage = 0;
        nothing.summary =
            QStringLiteral("nothing was changed: the page list is the one the notebook has");
        return nothing;
    }

    /// The list the caller built is checked BEFORE anything is written: the commit below would
    /// refuse it anyway, but by then the journal would already hold a copy of the old manifest and
    /// the user would be told about a change that never started.
    if (!plan.after.isValid(&why)) {
        return refused(QStringLiteral("the page list this change builds is not a valid notebook: %1")
                           .arg(why));
    }

    Outcome outcome;
    if (!applyPlan(projectDir, plan, rotator, &why)) {
        outcome.why = why;
        return outcome;
    }

    outcome.ok = true;
    outcome.anchorPage = 0;
    outcome.summary = edits.summary.isEmpty() ? QStringLiteral("the notebook's pages were changed")
                                              : edits.summary;
    return outcome;
}

bool PdfNotebookOps::canUndo(const QString &projectDir)
{
    if (projectDir.isEmpty()) {
        return false;
    }
    QString why;
    return PdfSessionManifest::readFrom(
               QDir(journalDir(projectDir)).filePath(QLatin1String(BeforeName)), &why).isValid(&why);
}

PdfNotebookOps::Outcome PdfNotebookOps::undoLast(const QString &projectDir)
{
    const QDir project(projectDir);
    const QString journal = journalDir(projectDir);

    QString why;
    const PdfSessionManifest before =
        PdfSessionManifest::readFrom(QDir(journal).filePath(QLatin1String(BeforeName)), &why);
    if (!before.isValid(&why)) {
        return refused(QStringLiteral("there is no notebook change to undo: %1").arg(why));
    }

    /// The reader's page when the change was made, so undo puts them back where they were.
    int anchor = 0;
    {
        QFile file(QDir(journal).filePath(QLatin1String(OpName)));
        if (file.open(QIODevice::ReadOnly)) {
            anchor = QJsonDocument::fromJson(file.readAll()).object()
                         .value(QStringLiteral("anchor")).toInt(0);
        }
    }

    /// One: the files the change created go away, and only then do the files it displaced come
    /// back. The order matters because an operation that turned an artifact wrote over a path the
    /// journal also holds -- the turned file has to be gone before the original can be renamed into
    /// its place. For an operation that only created files the two steps are disjoint either way.
    /// Directories are removed whole: an artifact's sidecar is listed as the directory it is.
    for (const QString &relative : readLines(QDir(journal).filePath(QLatin1String(AddedName)))) {
        removePath(project.filePath(relative));
    }

    /// Two: the files the change moved aside go back. Before the manifest, always: a crash between
    /// the two leaves files nothing references rather than a manifest naming files in the journal.
    const QString removedRoot = QDir(journal).filePath(QLatin1String(RemovedDirName));
    const QDir removed(removedRoot);
    if (removed.exists()) {
        QList<QFileInfo> files;
        QDirIterator iterator(removedRoot, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            files.append(QFileInfo(iterator.next()));
        }
        for (const QFileInfo &file : files) {
            const QString relative = removed.relativeFilePath(file.absoluteFilePath());
            const QString destination = project.filePath(relative);
            if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
                return refused(QStringLiteral("cannot put %1 back").arg(relative));
            }
            if (!QDir().rename(file.absoluteFilePath(), destination)) {
                return refused(QStringLiteral("cannot put %1 back").arg(relative));
            }
        }
    }

    /// Three: the manifest, as it was -- byte for byte in meaning, and atomically.
    if (!before.writeTo(PdfSession::manifestPath(projectDir), &why)) {
        return refused(why);
    }

    QDir(journal).removeRecursively();

    Outcome outcome;
    outcome.ok = true;
    outcome.anchorPage = qBound(0, anchor, before.pages.size() - 1);
    outcome.summary = QStringLiteral("the last notebook change was undone");
    return outcome;
}


