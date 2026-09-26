/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PDFNOTEBOOKOPSDIALOG_H
#define PDFNOTEBOOKOPSDIALOG_H

#include "session/PdfNotebookOps.h"
#include "session/PdfSessionManifest.h"

#include <QDialog>
#include <QList>
#include <QPair>
#include <QStringList>

class QLabel;
class QPushButton;
class QTableWidget;

/**
 * The Notebook ops screen: the notebook's pages as one list, worked on as a whole.
 *
 * The menu entries act on the page that is open, one operation at a time. This screen shows every
 * page at once -- its preview, where it comes from, how big it is, which way up it is, whether it
 * has been drawn on -- and lets a person build a whole change before any of it is written: move
 * pages, duplicate them, turn them, delete them, and then press Apply once.
 *
 * The model is a working copy of the page list and nothing else. Every button edits that copy; no
 * file is created and no manifest is touched while the screen is open, so Cancel changes nothing and
 * has nothing to undo. Apply hands the finished list to PdfNotebookOps::applyPageEdits(), which
 * commits it as ONE change -- one journal entry, one manifest write, one reload, one undo step --
 * through exactly the protocol every other operation uses. The screen cannot bypass it because it
 * never writes anything itself.
 *
 * Two things the footer keeps honest: Apply is disabled while nothing has changed (no operation is
 * run for a no-op), and the hint line says why an action is not available -- the last page cannot be
 * deleted, the first page cannot move up -- rather than letting a greyed button stand there mute.
 */
class PdfNotebookOpsDialog : public QDialog
{
    Q_OBJECT

public:
    PdfNotebookOpsDialog(const QString &projectDir, const PdfSessionManifest &manifest,
                         int anchorPage, QWidget *parent = nullptr);

    /// The change the user built. Meaningful only after exec() came back Accepted: Cancel leaves
    /// nothing behind, because nothing was ever written.
    PdfNotebookOps::PageEdits edits() const;

    /// The page the reader was on when the screen opened, so the notebook reopens where they were.
    int anchorPage() const { return m_anchor; }

    /**
     * The whole-notebook action the user asked for instead of a page-list change, if any.
     *
     * Insert, extract and merge bring a change of their own -- a source to copy in, a notebook to
     * write -- and they are entered from this screen so that everything a person can do to a
     * notebook is in one place. They are offered only while the page list is untouched: a notebook
     * cannot be both half-edited and merged into, and saying so is better than quietly applying one
     * of the two. Folding an insert into the same commit needs the engine to describe a source
     * addition ahead of the commit, which is the next step rather than this one.
     */
    enum RequestedAction {
        NoAction,
        InsertPagesAction,
        ExtractRangeAction,
        MergeNotebookAction,
    };
    RequestedAction requestedAction() const { return m_requested; }

private:
    struct Row {
        PdfPageRecord record;
        /// Where this page's files are now: the project, or the notebook a new page was copied from.
        QString fromDir;
        /// The user dropped it: still listed, struck through, until Apply or Cancel.
        bool removed = false;
        /// It did not exist when the screen opened.
        bool isNew = false;
    };

    void buildUi();
    void refresh();
    void refreshFooter();
    void moveSelected(int delta);
    void duplicateSelected();
    void deleteSelected();
    void keepSelected();
    void rotateSelected(int degrees);
    void dropThumbnail(Row &row);

    int selectedRow() const;
    void selectRow(int row);
    int keptCount() const;
    bool keptAbove(int row) const;
    bool keptBelow(int row) const;
    bool hasPendingEdits() const;
    QStringList pendingDescriptions() const;
    QString availabilityHint() const;
    QString summaryLine() const;

    QString m_projectDir;
    PdfSessionManifest m_original;
    QList<Row> m_rows;
    int m_anchor = 0;

    /// The files the change brings in: absolute source -> relative destination, and whole sidecar
    /// directories the same way.
    QList<QPair<QString, QString>> m_copies;
    QList<QPair<QString, QString>> m_copyDirs;
    /// The relative paths the change displaces: a deleted page's artifact, a preview a turn made
    /// stale.
    QStringList m_removals;
    /// The next artifact number this notebook would hand out; the screen takes one per new page.
    int m_nextNumber = 0;

    QTableWidget *m_table = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_apply = nullptr;
    QPushButton *m_moveUp = nullptr;
    QPushButton *m_moveDown = nullptr;
    QPushButton *m_duplicate = nullptr;
    QPushButton *m_delete = nullptr;
    QPushButton *m_keep = nullptr;
    QPushButton *m_rotateLeft = nullptr;
    QPushButton *m_rotateRight = nullptr;
    QPushButton *m_insert = nullptr;
    QPushButton *m_extract = nullptr;
    QPushButton *m_merge = nullptr;
    RequestedAction m_requested = NoAction;
};

#endif // PDFNOTEBOOKOPSDIALOG_H
