/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfNotebookQuicks.h"

#include "session/PdfSession.h"

namespace {

/// An outcome that says no, so that a refusal reads the same whether it came from the rule here or
/// from PdfNotebookOps an operation later.
PdfNotebookOps::Outcome refused(const QString &why)
{
    PdfNotebookOps::Outcome outcome;
    outcome.ok = false;
    outcome.why = why;
    return outcome;
}

/// Whether \a currentPage names a page of a notebook with \a pageCount pages.
bool pageIsOpen(int pageCount, int currentPage)
{
    return currentPage >= 0 && currentPage < pageCount;
}

} // namespace

QList<PdfNotebookQuicks::Action> PdfNotebookQuicks::all()
{
    return {
        Action::TurnLeft,
        Action::TurnRight,
        Action::MoveUp,
        Action::MoveDown,
        Action::Duplicate,
        Action::Delete,
    };
}

int PdfNotebookQuicks::degrees(Action action)
{
    switch (action) {
    case Action::TurnLeft:
        return -90;
    case Action::TurnRight:
        return 90;
    default:
        return 0;
    }
}

bool PdfNotebookQuicks::isTurn(Action action)
{
    return degrees(action) != 0;
}

bool PdfNotebookQuicks::available(Action action, int pageCount, int currentPage, QString *why)
{
    /// Cleared first, so a caller that reuses one string across a row of questions cannot read the
    /// reason for the last one it asked: "possible" and "a reason is waiting" must never both be
    /// true, and a panel that asked twice would otherwise show the first answer's tooltip forever.
    if (why) {
        why->clear();
    }

    const auto no = [why](const QString &reason) {
        if (why) {
            *why = reason;
        }
        return false;
    };

    if (!pageIsOpen(pageCount, currentPage)) {
        return no(QStringLiteral("No page is open."));
    }

    switch (action) {
    case Action::MoveUp:
        /// The first page has nowhere above it, and the entry says so rather than leaving a greyed
        /// button standing there mute.
        if (currentPage == 0) {
            return no(QStringLiteral("This is the first page, so it cannot move up."));
        }
        return true;
    case Action::MoveDown:
        if (currentPage == pageCount - 1) {
            return no(QStringLiteral("This is the last page, so it cannot move down."));
        }
        return true;
    case Action::Delete:
        return true;
    case Action::Duplicate:
    case Action::TurnLeft:
    case Action::TurnRight:
        return true;
    }

    return no(QStringLiteral("That operation is not one this page offers."));
}

PdfNotebookOps::Outcome PdfNotebookQuicks::run(const QString &projectDir, Action action,
                                               int currentPage,
                                               const PdfNotebookOps::ArtifactRotator &rotator)
{
    /// Asked here as well as by the caller: a rule the panel cannot show is not a rule. The page
    /// count comes from the manifest on disk rather than from a reader, because this is the class
    /// that has no reader.
    const PdfSessionManifest manifest = PdfSessionManifest::readFrom(
        PdfSession::manifestPath(projectDir), nullptr);
    if (!manifest.isValid()) {
        return refused(QStringLiteral("The notebook could not be read, so it was not changed."));
    }

    const int pages = manifest.pages.size();

    QString why;
    if (!available(action, pages, currentPage, &why)) {
        return refused(why);
    }

    switch (action) {
    case Action::MoveUp:
        return PdfNotebookOps::movePage(projectDir, currentPage, currentPage - 1, currentPage);
    case Action::MoveDown:
        return PdfNotebookOps::movePage(projectDir, currentPage, currentPage + 1, currentPage);
    case Action::Duplicate:
        return PdfNotebookOps::duplicatePage(projectDir, currentPage, currentPage);
    case Action::Delete:
        return PdfNotebookOps::deletePages(projectDir, currentPage, 1, currentPage);
    case Action::TurnLeft:
    case Action::TurnRight:
        return PdfNotebookOps::rotatePages(projectDir, currentPage, 1, degrees(action), rotator,
                                           currentPage);
    }

    return refused(QStringLiteral("That operation is not one this page offers."));
}
