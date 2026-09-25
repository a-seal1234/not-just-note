/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFNOTEBOOKOPS_H
#define PDFNOTEBOOKOPS_H

#include "session/PdfSessionManifest.h"

#include <QString>
#include <QStringList>

#include <functional>

class PdfRenderBackend;

/**
 * The notebook-level operations, as changes to the files on disk.
 *
 * A notebook is a manifest plus a set of artifacts, and an operation is a change to both. Three
 * rules hold every one of them together, and they are the reason this is one class rather than
 * logic spread through the menu handlers:
 *
 *  1. **The manifest is committed last and atomically.** Everything an operation creates is on disk
 *     before the manifest that names it, and everything it displaces is moved aside only after the
 *     manifest is in place. A failure at any point therefore leaves the notebook byte-identical to
 *     what it was, and files that were written but never referenced are harmless.
 *  2. **Nothing is deleted, only moved into the journal.** The operation that removed it can be
 *     undone one step, and a crash leaves the artifacts in the project rather than gone.
 *  3. **The reader's page is kept.** Each operation is told which page is open and answers with the
 *     position of that same page afterwards, so a move does not silently show a different sheet.
 *
 * What this class does NOT own: whether the notebook may be changed at all right now. The open
 * pages hold ink that lives in a document until it is written, so the caller has to make sure the
 * notebook is quiescent -- PdfPageNavigator::saveStripPages() has to have succeeded -- before an
 * operation runs. Applying one on top of ink that is still in the air is how a stroke is lost.
 *
 * The manifest it works from is read with PdfSession::openProject(), so a notebook whose source has
 * changed under it is refused rather than edited.
 */
class PdfNotebookOps
{
public:
    /**
     * Turns the artifact at the first path into the second, by the given quarter turn.
     *
     * PdfPageRotator::rotateInto is what the plugin passes. It is a parameter rather than a call so
     * that the operations here stay free of Krita: the engine decides WHAT is turned, when and in
     * what order, and the rotator only knows how. A test passes a stub for the same reason.
     */
    using ArtifactRotator = std::function<bool(const QString &source, const QString &destination,
                                               int degrees, QString *why)>;

    /**
     * What an operation did, in the terms a caller needs to report it and to reload afterwards.
     */
    struct Outcome {
        bool ok = false;
        /// Why it was refused, or why it could not be completed. Empty on success.
        QString why;
        /// The notebook page the reader was on, in the positions the notebook has AFTER the change.
        int anchorPage = 0;
        /// One line for the log and the user: what changed, in page numbers a person counts.
        QString summary;
        /// True when files were written but the manifest could not be committed, or the other way
        /// round. The notebook is still consistent; this says work was left behind.
        bool partial = false;
    };

    /**
     * Moves the page at \a from to \a to (both are positions in the notebook). \a currentPage is
     * the page the reader is on, -1 for "the page being moved".
     *
     * Nothing on disk moves: the records travel and carry their file names, which is why a reorder
     * is one atomic manifest write and why the artifact a page names never has to be renamed.
     */
    static Outcome movePage(const QString &projectDir, int from, int to, int currentPage = -1);

    /**
     * Copies \a page and inserts the copy after it, under fresh artifact and thumbnail numbers.
     *
     * The copy is a page of its own: its own files, its own ink, and its own entry in the page
     * list. Nothing is shared with the original, because a later edit of one must not appear on
     * the other.
     */
    static Outcome duplicatePage(const QString &projectDir, int page, int currentPage = -1);

    /**
     * Inserts \a count pages of \a pdfPath, starting at its page \a firstPage, at the notebook
     * position \a at (0 is before every page, the page count is after all of them).
     *
     * The PDF becomes one of the notebook's sources. When a source with the same content is already
     * one of them -- the notebook's own PDF, or a PDF inserted before -- its pages are drawn from
     * that entry and nothing is copied. Otherwise the file is copied into the project under
     * sources/<sha8>-<name>.pdf, and every page that comes from it names that entry. The name is
     * relative and inside the project, like every other name the manifest carries.
     *
     * Each inserted page takes its own artifact number from the allocator, so the same PDF page can
     * be inserted twice and the two copies stay independent. Nothing is written for a page until it
     * is drawn on: a page with no ink has no artifact, which is what keeps a notebook proportional
     * to what was written on it rather than to how many pages it has.
     *
     * \a backend has to be open on \a pdfPath: the page geometry comes from the renderer, and what
     * is recorded is what puts the background back under the ink. \a count of -1 means "to the end
     * of the file".
     */
    static Outcome insertPages(const QString &projectDir, int at, const QString &pdfPath,
                               PdfRenderBackend &backend, int firstPage = 0, int count = -1,
                               int currentPage = -1);

    /**
     * Where a copy of \a pdfPath would live inside a project: sources/<sha8>-<name>.pdf. Exposed
     * for the log, and for a caller that wants to say where the pages are about to come from.
     */
    static QString sourceFileNameFor(const QString &pdfPath);

    /**
     * Turns \a count pages starting at \a first by \a degrees, which has to be a quarter turn
     * (90, 180 or 270; a negative value is the same turn the other way, normalized).
     *
     * The paper is turned by the manifest -- the record's extraRotation, and the size the reader
     * sees follows from displaySizePt() -- and the ink is turned by the artifact: \a rotator turns
     * each artifact into a temporary name, the operation journals the original and only then swaps
     * the turned file in, all of it before the manifest is committed. A page that was never drawn on
     * has no artifact, and its turn is recorded in the manifest alone.
     *
     * The pages' previews are dropped rather than left showing the page as it was: the docker makes
     * a new one when it asks for it.
     */
    static Outcome rotatePages(const QString &projectDir, int first, int count, int degrees,
                               const ArtifactRotator &rotator, int currentPage = -1);

    /**
     * Deletes \a count pages starting at \a first.
     *
     * The last page is never deleted: a notebook with no pages is not a notebook, and the manifest
     * refuses it. The files of the deleted pages are moved into the journal rather than removed, so
     * the change can be undone and a crash cannot take them.
     */
    static Outcome deletePages(const QString &projectDir, int first, int count, int currentPage = -1);

    /// What extractRange() may do about a directory that is already there.
    struct ExtractOptions {
        /**
         * Write over a notebook that is already at the destination.
         *
         * Off by default: two notebooks made from the same pages is a decision for a person, and the
         * one already there may be the one they are working in.
         */
        bool replaceExisting = false;
    };

    /**
     * Writes \a count pages of \a projectDir, starting at \a first, as a notebook of their own at
     * \a destinationDir.
     *
     * The new notebook is the range and nothing else: the sources those pages draw from are copied
     * into it (in the order they are first needed, one of them becoming its own source), the pages'
     * artifacts and previews travel with them, and \a projectDir is not touched at all. A range that
     * spans two PDFs therefore produces a notebook that carries both.
     *
     * It is built whole in a directory beside the destination and renamed into place only once it
     * has been read back and opened: a failure leaves nothing behind, and never takes the notebook
     * that is already there with it. The new notebook's name is the destination directory's own.
     *
     * The open notebook afterwards is the caller's business -- see
     * PdfPageNavigator::openNotebookDir(), because two notebooks can share one source PDF and the
     * source path cannot tell them apart.
     */
    static Outcome extractRange(const QString &projectDir, int first, int count,
                                const QString &destinationDir, const ExtractOptions &options);

    /// The same, leaving a notebook that is already at the destination alone.
    static Outcome extractRange(const QString &projectDir, int first, int count,
                                const QString &destinationDir);

    /// Whether there is a change that can be undone.
    static bool canUndo(const QString &projectDir);

    /**
     * Puts the notebook back the way the last operation found it: the manifest as it was, the files
     * the operation moved aside back where they were, and the files it created gone.
     *
     * The files are restored first and the manifest committed last, the same order an operation
     * uses: a failure part way through leaves files nothing references rather than a manifest
     * naming files that are not there.
     */
    static Outcome undoLast(const QString &projectDir);

    /**
     * Whether the notebook's page list is still the source PDF's own pages, in their own order.
     *
     * The PDF export overlays ink on the source's existing pages, and it finds the page to write on
     * by the notebook's list position. So it is only correct while the notebook's page N is the
     * PDF's page N. A move, a delete, a duplicate, an insert or a page from another PDF breaks
     * that, and the export then has exactly one safe answer: refuse, with the reason, rather than
     * write a PDF whose ink is on the wrong pages. This is the guard until the writer can rebuild
     * the page tree in notebook order (stage G of docs/PDFIO-NOTEBOOK-OPS.md).
     */
    static bool exportIsOrderPreserving(const PdfSessionManifest &manifest, QString *why = nullptr);

    /// The directory the journal of the last operation lives in, inside the project.
    static QString journalDir(const QString &projectDir);

private:
    PdfNotebookOps() = delete;
};

#endif // PDFNOTEBOOKOPS_H
