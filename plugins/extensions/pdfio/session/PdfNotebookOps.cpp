/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfNotebookOps.h"

#include "session/PdfSession.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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
    /// relative paths the journal takes over after the manifest is committed.
    QStringList removeAfter;
    /// relative paths this operation creates, for the rollback and for an undo.
    QStringList added;
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
bool applyPlan(const QString &projectDir, const Plan &plan, QString *why)
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

    /// What this operation creates, written before it creates any of it: a crash then leaves a
    /// list of the files to clean up rather than an unknown pile of them.
    if (!writeLines(QDir(journal).filePath(QLatin1String(AddedName)), plan.added)) {
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

    /// Two: the manifest, atomically. This is the commit -- before it the notebook is exactly what
    /// it was, and after it the change has happened.
    PdfSessionManifest after = plan.after;
    after.refreshNextPageNumber();
    if (!after.writeTo(PdfSession::manifestPath(projectDir), why)) {
        undoCreatedFiles(projectDir, plan);
        QDir(journal).removeRecursively();
        return false;
    }

    /// Three, and last: the files the change displaced. They are unreferenced now, so a failure
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

bool loadManifest(const QString &projectDir, PdfSessionManifest *manifest, QString *why)
{
    *manifest = PdfSession::openProject(projectDir, why);
    return manifest->isValid(why);
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
    if (!applyPlan(projectDir, plan, &why)) {
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
    if (!applyPlan(projectDir, plan, &why)) {
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
    if (!applyPlan(projectDir, plan, &why)) {
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

    /// One: the files the change moved aside go back. Before the manifest, always: a crash between
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

    /// Two: the files the change created go away. Directories are removed whole; an artifact's
    /// sidecar is listed as the directory it is.
    for (const QString &relative : readLines(QDir(journal).filePath(QLatin1String(AddedName)))) {
        removePath(project.filePath(relative));
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

bool PdfNotebookOps::exportIsOrderPreserving(const PdfSessionManifest &manifest, QString *why)
{
    if (!manifest.isValid(why)) {
        return false;
    }

    if (manifest.sourceCount() > 1) {
        fail(why, QStringLiteral("this notebook draws its pages from %1 PDFs, and the export overlays "
                                 "each page's ink on that page of one PDF. Export a range, or keep a "
                                 "notebook's pages in one PDF")
                      .arg(manifest.sourceCount()));
        return false;
    }

    for (int i = 0; i < manifest.pages.size(); ++i) {
        const PdfPageRecord &page = manifest.pages.at(i);
        if (page.source != 0 || page.index != i) {
            fail(why, QStringLiteral("notebook page %1 holds page %2 of the PDF, and an export writes "
                                     "each page's ink on the PDF page at the same position; it would "
                                     "put the ink on the wrong page. The notebook's pages are no longer "
                                     "the PDF's own pages, in their own order")
                          .arg(i + 1).arg(page.index + 1));
            return false;
        }
    }

    return true;
}
