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
#include <QPixmap>
#include <QSizeF>
#include <QStringList>

#include <functional>

class QEvent;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

/// The preview pane beside the list: the selected page, large, turned by hand. Defined with its
/// painting and its gestures in PdfNotebookOpsDialog.cpp.
class PdfPageCanvas;
/// The reorder's visual: the row being dragged, highlighted, and the line it would land on.
class PdfRowDropIndicator;

/**
 * \a source prepared for a widget that draws it into \a logicalSize at \a devicePixelRatio.
 *
 * A saved preview is fitted into a 1152 box: an A4 page's is 814x1152 pixels. A card is 46x62 LOGICAL
 * pixels, which on a 2-2.5x tablet is 115x155 DEVICE pixels -- and handing Qt a picture smaller than
 * the box it is drawn into, to be stretched by the screen's ratio in the painter, is what made the
 * thumbnails look
 * pixelated. The scale is done here instead, to the device size, and the result is TAGGED with the
 * ratio, so Qt draws the pixels it was given, one for one, rather than stretching them again.
 *
 * The scale is smooth: a turned page has already been resampled once by the time it arrives, and
 * resampling it again with a nearest filter is what turns an edge into a staircase.
 *
 * The result is \a logicalSize * \a devicePixelRatio device pixels, whatever the source held. Whether
 * the source had enough pixels to fill that is the caller's business: the canvas asks the navigator
 * for a fresh preview when it has not (see PdfNotebookOpsDialog::PreparedPreview).
 */
QPixmap pdfioPreviewForDisplay(const QPixmap &source, const QSize &logicalSize, qreal devicePixelRatio);

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
 *
 * Beside the list is the canvas: the selected page, large, which can be turned directly by dragging
 * it around its own centre (a pen and a mouse) or by twisting two fingers on it (touch). A page is
 * often set down on an angle that is not a right angle, and a screen that can only offer "Turn
 * left"/"Turn right" makes that a hunt for a button that does not exist. The canvas records the same
 * pending edit as every button -- the row's extraRotation -- and it writes nothing: Apply is still
 * the only thing that commits. A drag is a PREVIEW while the hand is down -- the picture and the
 * number under it turn and nothing else does -- and it lands as ONE pending edit on release, so one
 * drag is one change and one undo takes the whole turn back. The row's own preview is turned on
 * screen with it, rather than only described.
 *
 * Both the card and the canvas scale their preview to the pixels the screen will actually draw --
 * the size times the screen's device pixel ratio, tagged with that ratio -- because on a
 * high-density tablet the picture was being handed to Qt at a third of the size it was painted at.
 * When the row's preview still has fewer pixels than the canvas is about to draw, the page is asked
 * for again from the navigator rather than stretched, and the new preview is taken in when it
 * arrives.
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

    /**
     * A preview as the screen draws it: the pixels prepared for the screen, what the row's own
     * preview held, and the ratio the prepared pixmap carries.
     *
     * Public because the two things that make a preview crisp -- that it is prepared at the size the
     * screen will draw it at rather than at the size of the file, and that it is tagged with the
     * screen's device pixel ratio so Qt does not stretch it again -- are facts a test can check and
     * an eye cannot. "It looks sharper" is not a test, and this is.
     */
    struct PreparedPreview {
        /// The pixels as prepared: \c pixmap.size() is the drawn size in DEVICE pixels.
        QPixmap pixmap;
        /// The size the pixmap is drawn at, in logical pixels: \c pixmap.size() is this times
        /// \c devicePixelRatio, and the ratio is what keeps Qt from scaling it a second time.
        QSize logicalSize;
        /// What the row's preview held. Smaller than \c pixmap on a high-density screen, which is
        /// exactly the case the canvas asks the navigator about.
        QSize sourcePixels;
        qreal devicePixelRatio = 1.0;
    };
    /// The card for page \a row as it is drawn: prepared for the table's icon size at its ratio.
    PreparedPreview cardPreview(int row) const;
    /// The canvas's own preview: prepared for the pane it is drawn in, at its ratio. Empty until the
    /// canvas has painted, so ask it to paint (QWidget::grab) before reading.
    PreparedPreview canvasPreview() const;

private:
    struct Row {
        PdfPageRecord record;
        /// Where this page's files are now: the project, or the notebook a new page was copied from.
        QString fromDir;
        /**
         * The page's preview, read once and kept in memory.
         *
         * The record's own thumbFile is the durable name, and a turn drops it -- nothing on disk
         * shows the turn yet, and a preview left as it was would show the page the wrong way up. The
         * pixels are what the canvas shows while the change is still pending, so they are kept here:
         * the canvas and the row's own card are turned from them rather than from a file that is on
         * its way out.
         */
        QPixmap preview;
        /// The turn \c preview was drawn at, so what is drawn on top of it is the difference this
        /// screen is holding rather than the whole turn a second time.
        int previewRotation = 0;
        /// A fresh preview of this page has been asked of the navigator. Asked once: a preview that
        /// is still small after the ask is not asked for again on every repaint.
        bool previewRequested = false;
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
    /**
     * Puts the row at \a from where \a to is now, moving the rows between them rather than swapping
     * the two: this is the whole of a reorder. "Move up"/"Move down" and a drop both come through
     * here, so a reorder is one edit of the working copy whichever way it was asked for.
     */
    void moveRow(int from, int to);
    /// Arms the press-and-hold: a press that stays put for the hold's length becomes a grab.
    void beginHold();
    /// The hold fired: the pressed row is now held, and the drag owns the gesture until release.
    void startGrab();
    /// The pointer moved while a row is held: the insertion point follows it.
    void updateGrab(const QPoint &at);
    /// The button came up. \a dropped is false when the pointer was outside the table, which cancels
    /// the drag and leaves the list alone.
    void finishGrab(bool dropped);
    /// Hides the reorder's visual and forgets the drag. Called however a drag ends.
    void endGrab();
    /// The gap (0..m_rows.size()) the pointer at \a y would insert into: above a row's middle is
    /// before it, below it is after it.
    int dropGapAt(int y) const;
    /// Draws the held row and the insertion line, and shows them over the table.
    void showDropFeedback();
    void duplicateSelected();
    void deleteSelected();
    void keepSelected();
    void rotateSelected(int degrees);
    /// Turns \a row by \a degrees. The buttons, the swipe and the canvas all come through here, so
    /// no two ways of turning a page can record a different amount or leave a different trail.
    void turnRow(int row, int degrees);
    /// Turns the selected row to \a degrees, the whole-degree angle the canvas reports. Any angle is
    /// legal, and it lands in the same record field the buttons write to.
    void setSelectedTurn(int degrees);
    void dropThumbnail(Row &row);
    /// Reads the row's preview into memory, once, when the file is there. A row that already has one,
    /// or whose record names no preview at all, is left alone.
    void loadPreview(Row &row);
    /// Puts the row's card on the table: the preview prepared for the icon size at the table's
    /// device pixel ratio, turned by whatever this screen is holding on top of it.
    void applyCardIcon(int row);
    /// The navigator's page index for \a record, or -1 when the navigator has no notebook open or a
    /// different one -- so a preview is never asked of, or written into, the wrong project.
    int navigatorIndexFor(const PdfPageRecord &record) const;
    /// Asks the navigator for a fresh preview of the row's page when the row's own preview has fewer
    /// pixels than \a devicePixels, which is the size the canvas is about to draw at. Asked once per
    /// row; the answer arrives through adoptFreshPreview().
    void requestPreviewIfTooSmall(Row &row, const QSize &devicePixels);
    /// The navigator has a preview for its page \a navigatorIndex: the rows showing that page take
    /// the new pixels, because the ones they hold are what it held before.
    void adoptFreshPreview(int navigatorIndex);
    /// Puts the selected page on the canvas, with the turn it already carries and the turn this
    /// screen is holding, and writes the angle in the readout beside it. Called whenever the
    /// selection or the pending turn changes, so the two can never disagree.
    void refreshCanvas();
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
     * The page list's own gestures, which share one press: a swipe turns a page, a hold reorders it.
     *
     * A swipe left or right across a row turns that page. The buttons are still the way to do it
     * with a mouse, but a page is turned far more often than a page is deleted, and on a tablet a
     * button is a small thing to find while a list of pages is a large thing to swipe. One swipe is
     * one quarter turn: a swipe carries a direction and no angle, so it cannot mean "37 degrees".
     * The canvas beside the list is where an angle is asked for.
     *
     * The same press also arms a reorder, because on a tablet there is no room for a drag handle and
     * a press-and-hold is the gesture everyone already knows. The two are one state machine and not
     * two: a press that moves more than a few pixels before the hold fires is the swipe, and a press
     * that stays put long enough becomes a grab -- from then on the DRAG owns the gesture until the
     * button comes up, and the turn cannot happen on that release at all.
     *
     * What a reorder decides, so that none of it is accidental: a page marked for deletion cannot be
     * grabbed (it is on its way out, the same reason "Move up"/"Move down" are off for it); the first
     * and last positions are legal drops; a drop where the row already is changes nothing and leaves
     * Apply exactly as it was; and a release outside the table cancels the drag rather than dropping
     * the page on whatever row happens to be nearest.
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
    /// The selected page, large, and turned by hand; and the live readout of its angle.
    PdfPageCanvas *m_canvas = nullptr;
    QLabel *m_angle = nullptr;
    /// Where a swipe started and which row it started on: the gesture acts on the row it was made
    /// on, not on whatever was selected before it.
    QPoint m_swipeFrom;
    int m_swipeRow = -1;
    /// The press-and-hold: armed on a press, disarmed the moment the pointer moves, and fired by the
    /// timer below when the press really stays put.
    QTimer *m_hold = nullptr;
    bool m_holding = false;
    /// A row is held and the drag owns the gesture. Nothing is edited until the button comes up.
    bool m_grabbing = false;
    int m_grabRow = -1;
    /// The gap (0..m_rows.size()) the held row would be inserted into if it were dropped now.
    int m_dropGap = -1;
    PdfRowDropIndicator *m_dropIndicator = nullptr;
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
