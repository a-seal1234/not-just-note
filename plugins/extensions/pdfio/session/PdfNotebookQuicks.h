/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFNOTEBOOKQUICKS_H
#define PDFNOTEBOOKQUICKS_H

#include "session/PdfNotebookOps.h"

#include <QList>
#include <QString>

/**
 * The page operations a page selector offers on the page that is open.
 *
 * The Notebook ops page is the place a whole change is built and applied in one step, and the place
 * its page LIST is changed. These are the other half of that design: the one-click cases, where the
 * change is a single operation on the page the reader is already on and there is nothing to review
 * before it runs. The submenu offers all six; the notebook panel offers the two that turn a page,
 * because those are the ones that belong under the reader's hand while reading. This is where the
 * callers agree about which ones exist, when each one is possible, and what it does.
 *
 * That agreement is the whole reason this is a class rather than six lambdas beside six buttons. The
 * menu entries and the panel buttons enabling themselves from different code is how "move down" ends
 * up possible in one place and impossible in the other on the last page; the rules live here once,
 * and the two callers ask.
 *
 * What this does NOT own is the gate around an operation: the open pages have to be written and the
 * notebook reloaded afterwards, which needs the reader and the application. See
 * PdfPageNavigator::prepareForNotebookChange() and PdfPageNavigator::reloadNotebook(). Everything
 * here is a rule and a call into PdfNotebookOps.
 */
class PdfNotebookQuicks
{
public:
    /**
     * The operations, named for what they do rather than for where they are offered.
     *
     * Delete is here and confirm-free on purpose: whether to ask before it happens is a question for
     * the widget that has a user in front of it, not for the rule.
     */
    enum class Action {
        MoveUp,
        MoveDown,
        Duplicate,
        Delete,
        TurnLeft,
        TurnRight,
    };

    /// Every action, in the order a panel lays them out: turns first, then the list moves, then the
    /// two that change how many pages there are.
    static QList<Action> all();

    /**
     * The turn \a action applies, in degrees, or 0 for an action that is not a turn.
     *
     * Left is negative so that the sign means the same thing here as it does in PdfNotebookOps.
     */
    static int degrees(Action action);

    /// Whether \a action turns the page at all.
    static bool isTurn(Action action);

    /**
     * Whether \a action can run on the page at \a currentPage of a notebook with \a pageCount
     * pages, and why not when it cannot.
     *
     * \a why is what a caller puts in a tooltip or a hint line; it is empty when the action is
     * possible. A notebook that is not open is \a pageCount 0, and nothing is possible.
     */
    static bool available(Action action, int pageCount, int currentPage, QString *why = nullptr);

    /**
     * Runs \a action on the notebook at \a projectDir, on the page at \a currentPage.
     *
     * \a rotator turns an artifact for the two turning actions and is unused by the others -- the
     * plugin passes PdfPageRotator::rotateInto, and a test passes a stub, for the same reason
     * PdfNotebookOps takes it as a parameter: the engine decides what is turned and the rotator only
     * knows how.
     *
     * The caller has to have made the notebook quiescent first, and has to reload it afterwards:
     * this is the operation, not the transaction around it. An action that \a available() refuses
     * is refused here too, with the same reason, so a caller that forgot to ask cannot apply it
     * anyway.
     */
    static PdfNotebookOps::Outcome run(const QString &projectDir, Action action, int currentPage,
                                       const PdfNotebookOps::ArtifactRotator &rotator);

private:
    PdfNotebookQuicks() = delete;
};

#endif // PDFNOTEBOOKQUICKS_H
