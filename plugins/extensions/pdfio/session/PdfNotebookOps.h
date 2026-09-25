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
     * Deletes \a count pages starting at \a first.
     *
     * The last page is never deleted: a notebook with no pages is not a notebook, and the manifest
     * refuses it. The files of the deleted pages are moved into the journal rather than removed, so
     * the change can be undone and a crash cannot take them.
     */
    static Outcome deletePages(const QString &projectDir, int first, int count, int currentPage = -1);

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
