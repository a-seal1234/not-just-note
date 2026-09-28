/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFIONOTEBOOKACTIONS_H
#define PDFIONOTEBOOKACTIONS_H

#include "session/PdfNotebookQuicks.h"

#include <QString>

/**
 * The application half of the quick page operations, defined in PdfIoPlugin.cpp.
 *
 * The Notebook ops submenu offers six one-click page operations and the notebook panel offers two
 * of them -- the turns -- and PdfNotebookQuicks is where they agree about what those are and when
 * each one is possible. Neither caller can RUN one, though: writing the open pages before it,
 * applying it, reloading the notebook after it, and the confirmation an operation that removes a
 * page should ask for all need the application. These are those entry points, in one place, so a
 * panel button and a menu entry cannot end up doing different things.
 */

/// The one wording both the submenu and the panel give \a action.
QString pdfIoQuickTitle(PdfNotebookQuicks::Action action);

/**
 * Runs \a action on the page that is open: the open pages are written first, the operation is
 * applied as one change, and the notebook is reloaded onto the page the operation answers with.
 *
 * A refusal -- no notebook, ink that could not be written, an operation the page cannot take -- is
 * reported and changes nothing. Delete asks for confirmation first; nothing else does.
 */
void runPdfIoQuickAction(PdfNotebookQuicks::Action action);

/// Opens the Notebook ops screen for the notebook that is open, and says so when there is none.
void openPdfIoNotebookOpsScreen();

/**
 * Whether there is a notebook change to undo right now.
 *
 * Each notebook operation is one undo step, and up to twenty steps are retained. Asked here so the
 * panel's button and the submenu entry are enabled by the same question.
 */
bool pdfIoCanUndoNotebookChange();

/// Undoes the last notebook change, through the same gate as every other operation: the open pages
/// are written first, and the notebook is reloaded onto the page the undo answers with.
void pdfIoUndoNotebookChange();

#endif // PDFIONOTEBOOKACTIONS_H
