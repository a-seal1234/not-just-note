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
#include <QPoint>
#include <QSizeF>
#include <QStringList>

#include <functional>

class QEvent;
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
     * Insert and merge are NOT here: they change this notebook, so they are edits of the page list
     * like any other, and they join the pending change (their PDF and their files are described in
     * PageEdits before anything is committed). Extract is: it writes a notebook of its own somewhere
     * else, so it cannot be part of this notebook's Apply and is offered only while the list is
     * untouched.
     */
    enum RequestedAction {
        NoAction,
        ExtractRangeAction,
    };
    RequestedAction requestedAction() const { return m_requested; }

    /// What the screen needs from the application to add pages from a PDF: the picker, the one range
    /// dialog, and the reading of that PDF's pages. The screen knows nothing about renderers.
    struct PdfToAdd {
        QString path;
        QList<QSizeF> displayedSizes;
    };
    struct SourceAdder {
        std::function<bool(PdfToAdd *pdf)> pickAndRead;
        std::function<bool(int available, int *first, int *count)> askRange;
    };
    void setSourceAdder(const SourceAdder &adder);

    /**
     * What the screen needs to merge another notebook in: where its files are and what it holds.
     *
     * The application owns that directory -- for a .pnb it is a temporary unpack -- and keeps it
     * alive until Apply has read every file out of it.
     */
    struct NotebookToMerge {
        QString dir;
        PdfSessionManifest manifest;
    };
    struct NotebookMerger {
        std::function<bool(NotebookToMerge *notebook)> pickNotebook;
    };
    void setNotebookMerger(const NotebookMerger &merger);

private:
    struct Row {
        PdfPageRecord record;
        /// Where this page's files are now: the project, or the notebook a new page was copied from.
        QString fromDir;
        /// The user dropped it: still listed, struck through, until Apply or Cancel.
        bool removed = false;
        /// It did not exist when the screen opened.
        bool isNew = false;
        /// Where a new page came from, for the words the legend uses: a PDF added here, or another
        /// notebook merged in.
        bool fromAddedPdf = false;
        bool fromMergedNotebook = false;
    };

    void buildUi();
    void refresh();
    void refreshFooter();
    void moveSelected(int delta);
    void duplicateSelected();
    void deleteSelected();
    void keepSelected();
    void rotateSelected(int degrees);
    /// Turns \a row by \a degrees. Both the buttons and the swipe come through here, so the two
    /// cannot turn a page by different amounts or leave a different trail behind.
    void turnRow(int row, int degrees);
    void dropThumbnail(Row &row);
    void insertPagesFromPdf();
    void mergeNotebookIn();
    void addPagesFromSource(const PdfToAdd &pdf, int first, int count);
    void addNotebook(const NotebookToMerge &notebook);
    /**
     * The source index a record of another file should carry: the entry this notebook already has
     * for that content, an addition already registered here, or a new addition. -1 when it cannot be
     * resolved, with \a why filled.
     */
    int sourceIndexFor(const PdfSourceRecord &source, const QString &absoluteFile, QString *why);

    /// A PDF this change brings in: where it is and the content that identifies it, which is what
    /// decides whether a second copy is needed at all.
    struct Addition {
        QString path;
        QByteArray sha256;
    };
    /// The additions in index order, and the assets another notebook carried.
    QList<Addition> m_additions;
    QStringList m_assets;

    /**
     * The page list's own gesture: a swipe left or right across a row turns that page.
     *
     * The buttons are still the way to do it with a mouse, but a page is turned far more often than
     * a page is deleted, and on a tablet a button is a small thing to find while a list of pages is
     * a large thing to swipe. One swipe is one quarter turn -- the unit the notebook records -- so
     * the gesture cannot land the paper on an angle the ink was never turned to.
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

    int selectedRow() const;
    void selectRow(int row);
    int keptCount() const;
    bool keptAbove(int row) const;
    bool keptBelow(int row) const;
    bool hasPendingEdits() const;
    QStringList pendingDescriptions() const;
    QString availabilityHint() const;
    QString summaryLine() const;
    /// Where a page comes from, in words: the PDF and the page inside it. A page of a PDF this change
    /// is bringing in says so with that file's name, not the notebook's own.
    QString sourceLabel(const PdfPageRecord &record) const;

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
    /// Where a swipe started and which row it started on: the gesture acts on the row it was made
    /// on, not on whatever was selected before it.
    QPoint m_swipeFrom;
    int m_swipeRow = -1;
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
    SourceAdder m_adder;
    NotebookMerger m_merger;
};

#endif // PDFNOTEBOOKOPSDIALOG_H
