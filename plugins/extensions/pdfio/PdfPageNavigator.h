/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGENAVIGATOR_H
#define PDFPAGENAVIGATOR_H

/// Included rather than forward declared: the navigator holds a PdfSourceRenderers member, and
/// that needs the complete type for its destructor. It brings PdfRenderBackend.h with it.
#include "session/PdfSourceRenderers.h"
#include "session/PdfPageWindow.h"
#include "session/PdfSaveQueue.h"
#include "session/PdfSessionManifest.h"
#include "session/PdfStripLayout.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <QRect>

#include <functional>

#include <kis_types.h>

class KisDocument;
class KisView;

/**
 * Which page of which notebook is open, held once per process.
 *
 * Not per plugin instance: a Krita view plugin is created per view, and paging closes the view
 * of the page being left behind, which would take the instance that is doing the paging with it.
 *
 * A page costs about 41 MB resident, measured across twelve pages, so the notebook keeps a
 * bounded set: opening a page closes the one before it. The page artwork is not lost by that,
 * because it is rendered again from the bundled source.
 */
class PdfPageNavigator : public QObject
{
    Q_OBJECT
public:
    static PdfPageNavigator *instance();

    /// Wraps \a pdfPath into a project if needed and shows its first page.
    bool openNotebook(const QString &pdfPath, QString *why = nullptr);

    /**
     * Opens a notebook from its DIRECTORY, rather than from the PDF it was made from.
     *
     * Two notebooks can draw on one PDF -- a range extracted from a notebook carries the same
     * source as the notebook it came from -- and a source path cannot tell them apart: openNotebook()
     * keys a project by the source's own hash, so it would find the notebook that already exists
     * rather than the one just made. Everything that remembers a notebook (Recent notebooks, the
     * Start screen) stores the directory, and this is the door that opens what it stored.
     */
    bool openNotebookDir(const QString &projectDir, QString *why = nullptr);

    bool showPage(int index, QString *why = nullptr);
    bool next(QString *why = nullptr);
    bool previous(QString *why = nullptr);

    bool hasNotebook() const;

    /// The document of the page that is open, if one is.
    KisDocument *currentDocument() const;

    /**
     * Writes every page that is currently in the strip, not only the one that is open.
     *
     * The strip holds its ink in one layer, and which page a stroke belongs to is decided by the
     * rectangle it sits in -- so the cropping can be done whenever, for as many pages as are open,
     * rather than only for the page being left.
     */
    bool saveStripPages();

    /**
     * The document a page write starts from: \a page's own layers, cropped to the rectangle \a index
     * occupies in the open strip, or the whole document when it holds a single page.
     *
     * This is the crop the roll writes through, and the one "Insert image..." has to write through
     * too: the insert built its document from the whole image, so on a strip page 1's artifact was
     * the whole strip until the next roll rewrote it, and a close, a reopen or an export right after
     * the insert read a page-sized rectangle full of strip.
     *
     * Returns nullptr and sets \a why when there is nothing to write. The caller owns the document
     * and has to delete it once the save has reported.
     */
    KisDocument *pageLayersDocument(KisDocument *page, int index, QString *why = nullptr);

    /**
     * Makes the notebook safe to change on disk: writes every page the open document holds and
     * clears the document's modified flag, so the operation that follows cannot be applied on top
     * of ink that is still in the air, and the document can be closed afterwards without Krita
     * asking to save it.
     *
     * Returns false when a page could not be written, and the caller must then NOT change the
     * notebook: a change applied over unsaved ink is a change that loses it.
     */
    bool prepareForNotebookChange(QString *why = nullptr);

    /**
     * Re-reads the notebook from disk -- after something changed its manifest -- and opens
     * \a anchorPage (clamped to the pages that exist) in a fresh document and view.
     *
     * Refuses when the open page still carries ink that is not on disk, because the page list has
     * just changed meaning: page 3 of the old manifest is not necessarily the page that is open.
     * The caller is expected to have run prepareForNotebookChange() before the change.
     *
     * Everything that describes the old page list is thrown away -- the window, the strip, its
     * cells and paper layers, the slot bookkeeping, the queued thumbnails -- and the rebuild runs
     * on the event loop, because the old view has to be gone before the new document is built.
     * \ref reloadFinished() says when it is done.
     */
    bool reloadNotebook(int anchorPage, QString *why = nullptr);

    /// Whether a reload is waiting for the event loop.
    bool reloadPending() const;

    /**
     * Writes what the open page -- every page the window holds -- still has unsaved, so that
     * closing a tab or quitting Krita cannot lose ink.
     *
     * Returns true when the document is safe to close: either it had nothing unsaved, or every
     * page's ink reached its artifact. Returns false when a write could not be completed, and the
     * caller must then NOT close silently -- Krita's own "do you want to save it?" prompt is what
     * should run, so the user is told rather than the ink dropped.
     *
     * Hung off the view (see showImage()) and off the application's aboutToQuit, so both closing
     * our tab and quitting reach it before Krita would ask about the document.
     */
    bool prepareForClose();

    /// The view showing it, for code that needs the canvas rather than the document.
    KisView *currentView() const;

    /**
     * How many pages are held at full resolution around the open one.
     *
     * One is design A: a document per page, and a page turn rebuilds it. More is design B: a strip
     * of pages in one document, where turning to a page that is already in the strip is unlocking
     * its slot rather than building anything. See docs/PDFIO-DESIGN-STRIP.md.
     */
    int scope() const;

    /**
     * Sets how many pages the strip holds: 1 turns it off (design A), three or more is a strip of
     * that many pages centred on the active one.
     *
     * Persisted -- the page count and whether the strip is on are kept apart, under
     * pdfio/stripPages and pdfio/stripOn, so turning the strip OFF does not forget how many pages it
     * had -- and applied to what is on screen through the roll's own resize path. A different page
     * count is a different WINDOW, not a different notebook: the per-slot bands are re-cut inside the
     * roll and the document is resized to the new window, rather than the document being rebuilt.
     *
     * The count is a SETTING, and it is deliberately not clamped to whichever notebook happens to be
     * open when it is set: it is set before the notebook it is meant for as often as after. The
     * WINDOW is what the notebook bounds -- PdfStripLayout::forWindow cannot extend past the first
     * and last page -- so a count larger than the open notebook is kept as the setting and the slots
     * on screen are the notebook's answer.
     *
     * An OFF (1) changes the setting and leaves the document alone, because one page at a time is a
     * different document rather than a different window, and the switch that asks for it is the one
     * that rebuilds. The strip is symmetric, so an even count is laid out as the next odd one.
     */
    void setScope(int scope);

    /**
     * How many pages the strip holds when it is on: the switch's "how many" half, kept apart from
     * whether it is on.
     *
     * Persisted under pdfio/stripPages and never rewritten by turning the strip off, so the count is
     * still there when it is turned back on. Five on a settings file that has never been touched --
     * the count the notebook has always opened at.
     */
    int stripPageCount() const;

    /// Whether what is OPEN is a strip rather than one page. Both halves are needed: the document has
    /// to be there, and its slot bookkeeping has to hold a window -- m_stripPages is empty for design
    /// A and holds one entry per slot for design B. Read by the menu so a count change that is
    /// already on screen is not rebuilt a second time, and a slot list that outlived its view is not
    /// mistaken for one.
    bool stripIsOpen() const;

    /// The device's physical memory in megabytes, as Krita itself reports it (0 when it cannot say).
    static int deviceRamMb();

    /**
     * What the rendered page of the strip is allowed to cost, in megabytes, or 0 for no limit.
     *
     * A page can be LANDSCAPE, so a bound on the pixels of its longest side buys a different amount
     * of memory for every shape: the same target is 1.8 Mpx on a 16:9 page and 2.5 Mpx on an A4
     * portrait, and a window of five slides costs several times a window of five A4 sheets. The knob
     * is therefore the MEMORY, and the resolution follows from it for the window that is up: lay the
     * window out at the reference dpi, take the pixels it needs, and solve for the dpi whose image
     * fits the budget across every full-size layer the strip has.
     *
     * The megabyte here is 1000000 bytes, so what the window really costs is at or below the number
     * on the menu. 0 -- the default, and what every notebook had before this existed -- is the fixed
     * 200 dpi the pages have always been rendered at, exactly.
     *
     * It is NOT free: the budget is the pages' rendered resolution, which is also the resolution the
     * pen's ink is stored at from then on, and lowering it is not freely reversible. The source PDF
     * and everything already stored are untouched.
     */
    int memoryBudgetMb() const;

    /**
     * The dpi the budget implies for the window that is up: what the strip was built at and is
     * repainted at. Exactly 200 when there is no budget.
     *
     * Exposed because anything that has to work out where a page sits in the OPEN strip -- the strip
     * probe's own mark, a test -- has to lay the window out at the resolution the document really
     * has, and deriving that a second time is how the two come to disagree.
     */
    qreal currentRenderDpi() const;

    /**
     * The longest side, in pixels, the window's longest page comes out at under \a megabytes -- or
     * at 200 dpi for 0.
     *
     * What the menu shows beside each budget, so the memory and what it costs in quality are read
     * together. Read-only: asking changes nothing.
     */
    int longestPagePixelsForBudget(int megabytes) const;

    /**
     * What the window that is up would cost at \a megabytes, in megabytes: the pixels the layout
     * really has at the dpi that budget buys, across every full-size layer.
     *
     * What a TYPED budget is worth is not the number typed: a budget above what 200 dpi already costs
     * does not spend itself, and this is the figure that says so. Shown beside the page size in the
     * dialog, so a typed number never hides either half of what it buys.
     */
    int windowCostMbForBudget(int megabytes) const;

    /**
     * The same two figures for a window of \a scope pages, which is what the strip-size menu shows
     * beside each choice: what that many pages would cost at the budget in force, and how big the
     * longest page in it would be.
     *
     * The window is the one the layout would make -- centred on the active page and bounded by the
     * notebook -- and the layer count is the shape the builder would make for it, one band per slot
     * plus Ink, because the bands of a window that does not exist yet cannot be counted. A \a scope
     * larger than the notebook holds is therefore answered for the window the notebook can really
     * show, so the figure beside an entry is the figure picking it would produce. 0 when there is no
     * notebook, or no page size to name.
     */
    int windowCostMbForScope(int scope) const;
    int longestPagePixelsForScope(int scope) const;

    /**
     * The floor and the ceiling a typed budget is held between, so a dialog and the setter cannot
     * disagree about what may be asked for. 0 is deliberately not in the range: it is how "no limit"
     * is asked for, and it is its own entry on the menu.
     */
    static int minMemoryBudgetMb();
    static int maxMemoryBudgetMb();

    /**
     * Sets the budget and applies it to what is on screen. A typed value is held between
     * minMemoryBudgetMb() and maxMemoryBudgetMb(); 0 means no limit and is left alone.
     *
     * Persisted under pdfio/memoryBudgetMb, so the choice survives a restart, and applied
     * immediately through the roll's own resize path -- the one mechanism that can change the
     * resolution of a strip -- because the user changed it and the screen has to show it. A single
     * page has no strip to roll and takes the new budget the next time it is built.
     */
    void setMemoryBudgetMb(int megabytes);


    /**
     * Makes sure the page has a thumbnail, rendering one at thumbnail resolution when it has none.
     *
     * Queueing is cheap and the work happens one page at a time, so a page selector can ask for
     * every page it shows without the window stalling. Pages that were drawn on already have a
     * thumbnail, taken from their own projection; this is for the ones that were never opened.
     */
    void ensureThumbnail(int index);

    /**
     * Whether panning past the edge of a page turns to the next one.
     *
     * Off is a reasonable choice: the gesture that turns a page is the same one used to look at
     * the bottom of a page, and someone who reads that way will not want the notebook moving
     * under them.
     */
    bool scrollFollowEnabled() const;
    void setScrollFollowEnabled(bool enabled);

    /**
     * How long the page under the centre of the viewport has to stay there before the strip turns to
     * it, in milliseconds. The value the follow's own log line reports as "settled on page N after
     * X ms".
     *
     * It is the knob the user asked for: a LONGER settle means a burst of scrolling does not drag
     * the strip through every page it passes -- the candidate is reset on each page the centre
     * crosses, so only the page the scrolling RESTS on is reached (fewer writes, less churn); a
     * SHORTER one means the strip follows the finger, and 0 is allowed and turns on the first 150 ms
     * tick after the page under the centre changes.
     *
     * There is no ceiling: no value breaks anything. A value so large that the follow never fires is
     * the same thing as the "Turn pages by panning" switch being off, and that switch is the honest
     * way to say it. A negative value is refused at 0, because there is no such thing as settling
     * before the reading is taken. Persisted under pdfio/scrollSettleMs; 450 by default, which is
     * what every build before this setting did.
     */
    int scrollSettleMs() const;
    void setScrollSettleMs(int milliseconds);

    /**
     * How much of the viewport height the ACTIVE page takes when the strip is fitted, in percent:
     * the three fifths (60) the fit has always used.
     *
     * The rule and what it buys: the fit is deliberately not full-screen -- fitting the page exactly
     * filled the viewport and the page that comes next was then simply not on screen, "even at page
     * three you cannot see four" -- so two fifths of the height are left for the neighbours. A
     * SMALLER share shows more of them (the strip looks further ahead, and less scrolling turns the
     * page); a LARGER one shows less (a page fills more of the screen, and the reading page changes
     * after more scrolling). It is the knob behind "how much of a page is on screen", and it changes
     * which page a given scroll leaves under the centre of the viewport.
     *
     * Held between minReadingSharePercent() and maxReadingSharePercent(): a share of zero is a page
     * with no height (and fitZoomFor's own 2% floor would take over), and above 100% the page is
     * taller than the viewport, so the neighbour this rule exists to show is gone again. Persisted
     * under pdfio/readingSharePercent; 60 by default.
     */
    int readingSharePercent() const;
    void setReadingSharePercent(int percent);
    static int minReadingSharePercent();
    static int maxReadingSharePercent();

    /**
     * The zoom the fit chooses for a page of \a pageRect in \a viewport at the share in force: the
     * same arithmetic the strip is fitted with, exposed because "how much scrolling turns the page"
     * is that zoom times the scroll, and a canvas cannot be built in a test.
     */
    qreal fitZoomForViewport(const QSize &viewport, const QRect &pageRect) const;

    /// What the export needs to walk the notebook and find its source.
    const PdfSessionManifest &manifest() const;

    /**
     * Whether \a name is one of the INTERNAL names this application makes for a notebook, rather
     * than a name a person chose.
     *
     * Android's picker copies the chosen file into the app's cache under a name the app itself
     * supplies -- "pdfio-picked.pdf" for an import, "pdfio-picked-pages.pdf" for an insert,
     * "pdfio-picked-notebook.pnb" for a notebook bundle, "pdfio-picked-image.png" for an image --
     * and an earlier build produced "pdfio-picked-notes". Those are paths, not titles, and one of
     * them reaching the manifest is how a notebook came to be called "pdfio-picked.pdf" on the Start
     * screen. The stems are a LIST, matched exactly (with the file extension and the "(2)" a copy
     * gets removed first): the names we write are ours and finite, while a name that merely starts
     * the same way -- "pdfio-picked-up-the-wrong-file" -- is one only a person would type, and a name
     * a person typed is never rewritten.
     *
     * ONE place: every reader and every writer of a notebook's name asks this, so the rule cannot
     * drift into a string comparison at each call site.
     */
    static bool isInternalNotebookName(const QString &name);

    /// The name a notebook is given when it has none a person chose: a human default that says what
    /// it is and when it arrived, never a path and never one of the internal names.
    static QString defaultNotebookName();

    /// The name to SHOW for a manifest that holds \a name: the name itself when a person chose it,
    /// \ref defaultNotebookName() when it is empty or internal. The reader half of
    /// \ref isInternalNotebookName().
    static QString usableNotebookName(const QString &name);

    /**
     * Settles the open notebook's name in its manifest, and says so in the log.
     *
     * A name a person chose is never touched; an internal one is replaced with the human default, and
     * -- when \a nameWhenEmpty -- so is a manifest with no name at all. \a nameWhenEmpty is false on
     * Android's picker path, where the provider's own name is asked for straight afterwards: a
     * default written first would look like a chosen name and the provider's answer would then be
     * refused. The replacement of an INTERNAL name happens either way, because an internal name is
     * never a name a person chose.
     *
     * The manifest is the one place worth fixing: the tab, the Start screen, the recent list and the
     * export suggestion all read the notebook's name from it.
     */
    bool ensureNotebookName(bool nameWhenEmpty = true, QString *why = nullptr);

    /// Which pages are open, and what the policy has had to do to keep that bounded. Exposed so
    /// the page selector can show the window and so the counters are observable without a debugger.
    const PdfPageWindow &pageWindow() const;
    QString sourcePath() const;
    int pageCount() const;
    int currentIndex() const;
    QString projectDir() const;

    /**
     * Closes the notebook that is open and forgets it.
     *
     * prepareForClose() is the gate: every page the open document holds is written first, and a
     * notebook whose ink cannot be written is left open rather than closed over it. On success the
     * view and its document go -- the strip's document and its layers are the largest thing the
     * application holds, so a notebook that is deleted while open must not leave them behind -- and
     * the window, the strip, the slot bookkeeping, the queued thumbnails, the ink-change watch, the
     * open renderers, the manifest and the project directory all go with it. \ref pageChanged() then
     * says there is no notebook, so the menu, the docker and the Start screen stop describing one.
     *
     * Ending the notebook in memory is all this does; removing its directory is
     * \ref removeNotebookStore().
     */
    bool closeNotebook(QString *why = nullptr);

    /**
     * Removes the notebook store at \a projectDir, and forgets it in the recent list with it.
     *
     * Refuses, with \a why, a directory that is not inside \ref projectRoot() -- the same rule the
     * rest of the plugin follows for a path it did not make -- and refuses when the directory cannot
     * be removed, so the caller can leave the notebook open rather than half-delete it. Nothing is
     * removed before either check: a caller that is refused has lost nothing.
     *
     * A directory that is already gone is not an error: the entry outlived it, and forgetting the
     * entry is the whole of the work.
     */
    static bool removeNotebookStore(const QString &projectDir, QString *why = nullptr);

    /**
     * Whether \a projectDir is a directory this store may remove: inside \ref projectRoot() and not
     * the root itself.
     *
     * The check removeNotebookStore() makes before it removes anything, exposed separately so a
     * caller can ask BEFORE it asks the user: a confirmation that then refuses is worse than no
     * confirmation at all.
     */
    static bool isInsideNotebookStore(const QString &projectDir);

    /**
     * The notebooks this application has opened, most recent first.
     *
     * An entry is "<project directory>" + \ref recentNotebookSeparator() + "<the name to show>". The
     * list is kept with the notebook store rather than with the menus that display it, because
     * removing a notebook has to forget its entry at the same moment, and one place has to know
     * where the list lives.
     */
    static QStringList recentNotebooks();
    static void setRecentNotebooks(const QStringList &entries);
    static QString recentNotebookDir(const QString &entry);
    static QChar recentNotebookSeparator();
    /// Drops \a projectDir from the recent list, if it is there. Also drops entries that name no
    /// directory at all, the way the menus' own pruning does.
    static void forgetRecentNotebook(const QString &projectDir);

    /**
     * Whether a roll should behave as if its document had gone away between its write phase and its
     * redraw. The only caller is a test, and it is what proves the guard that otherwise can only be
     * reached by a reload landing inside the write phase's nested event loops -- timing no test can
     * promise. Nothing in the plugin calls it.
     */
    static void setDocumentGoneAfterWritesForTests(bool gone);

Q_SIGNALS:
    /// Emitted whenever the open page or the notebook itself changes, so a navigator widget can
    /// follow along without polling.
    void pageChanged(int index, int pageCount, const QString &label);

    /// A thumbnail that was missing has been written, so a view showing that page can update.
    void thumbnailReady(int index);

    /// A reload has finished: \a index is the page now open, \a ok whether it opened at all.
    void reloadFinished(int index, bool ok);

private:
    /// The half of reloadNotebook() that waits for the old view to go: see it for the contract.
    void finishReload();

    /// Installs the window's save hook and sets its bound to the current scope, once. The page
    /// switch then cannot run without the policy in front of it.
    PdfPageNavigator();

    /// Closes the page that is open, freeing its document and its view.
    void closeCurrentPage();

    /// The tail every notebook open shares: write the page being replaced, clear the window, the
    /// strip and the slot bookkeeping, adopt the manifest, and show \a anchorPage.
    bool adoptNotebook(const QString &projectDir, const PdfSessionManifest &manifest, int anchorPage,
                       const QString &label, QString *why);

    /**
     * Writes the ink of the page that is open, if one is.
     *
     * Called before a page is left behind, not only when the user asks: turning a page used to
     * remove the document, and nothing had ever been saved from it, so the ink went with it.
     */
    bool saveCurrentPage(QString *why = nullptr, int index = -1, std::function<void()> then = nullptr);

    /// The rectangle \a index occupies in the open strip, or an invalid one when the document holds
    /// a single page or that page is not in the window. The one place the crop is worked out, so
    /// every writer of a page uses the same rectangle.
    QRect pageAreaFor(int index) const;

    /// Saves one page and waits for the file to have been written -- through the queue, so a
    /// second write never starts while one is in the air.
    bool savePageAndWait(int index, QString *why);

    /**
     * Writes \a index through the save queue and waits for the write to land: queued behind
     * whatever is already in the air, started when it is this page's turn, and true only once
     * the artifact holds it. This is the only way anything here saves -- the page window's
     * saver, the strip roll, the close path -- because "the write started" is not an answer any
     * of them may act on.
     *
     * While it waits, \ref m_savingPages is held: the follow timer and the queued roll both
     * watch it, and neither may turn a page inside the very event loop this wait is spinning.
     */
    bool saveThroughQueue(int index, QString *why = nullptr);

    /**
     * The value KisCanvasController::setPreferredCenter() wants so that \a imagePoint comes to
     * rest under the middle of \a view's viewport.
     *
     * Not the image point itself. The controller's preferred centre is the offset of that point
     * from the image's top-left corner in WIDGET pixels -- preferredCenter() is
     * widgetCenterPoint() - imageRectInWidgetPixels().topLeft(), and setPreferredCenter() builds
     * the new offset from it the same way -- so passing image pixels asks the widget centre to sit
     * that many WIDGET pixels into the image. At a zoom of 0.26 that is nearly four pages too far:
     * measured on the tablet, the canvas came to rest past the bottom of the strip, every reading
     * the follow took fell outside the image and was dropped, and the notebook sat on the last
     * page of the window with nobody having asked it to move. Every centring call in this plugin
     * had the same unit mistake.
     */
    static QPointF preferredCenterFor(KisView *view, const QPointF &imagePoint);

    /// Waits for every write in flight -- the readers' side of the same rule. A page rebuilt
    /// from its artifact must not read the file while the write it depends on is still landing.
    /// Same \ref m_savingPages hold while it waits.
    bool drainWrites(QString *why = nullptr);

    /// Marks every page the open document holds as carrying ink that is not on disk, which is
    /// what a stroke means. All of a strip's pages rather than the active one: which page a
    /// stroke belongs to is decided at save time by the rectangle, not at stroke time.
    void markOpenPagesDirty();

    /// The idle half of the pipeline: once the ink has been quiet for a beat, writes whatever is
    /// marked unsaved, so a page turn is never the first thing to find out what was never saved.
    void pumpAutoSave();

    /// Hangs prepareForClose() on the application's own quit, once.
    void hookApplicationQuitOnce();

    static QString projectRoot();

    /// Acts on where the middle of the view has settled.
    void checkScrollFollow();

    /**
     * The dpi \a megabytes buys for the window around \a activePage -- ONE derivation, and the only
     * place a rendered page's resolution is decided. Exactly 200 with no budget.
     *
     * \a layers is how many full-size layers the window will cost: the caller's own count when it has
     * one (a roll of the strip that is up knows it exactly), or 0 for the shape the builder is about
     * to make -- one band per slot plus the Ink layer. A content layer an artifact carries is not
     * known before it has been read back, which is why a build uses the planned count and a roll
     * uses the real one (see stripLayerCount).
     *
     * The arithmetic, and the correction the layout forces on it: the layout scales linearly with the
     * dpi, so the image's area goes as its square, which gives the dpi from the reference area in one
     * square root. The gaps between the pages do not scale, though, so that estimate spends the whole
     * budget on the pages and then pays for the gaps on top. The result is therefore walked down
     * against the layout itself until the area it really has fits, which is what makes a budget a
     * bound rather than a hope.
     */
    qreal dpiForBudget(int megabytes, int activePage, int layers) const;

    /**
     * The same derivation for a window of \a scope pages rather than for the one that is up: what the
     * strip-size menu needs, because the window it names does not exist yet. \a layers as in
     * dpiForBudget().
     */
    qreal dpiForBudgetInScope(int megabytes, int activePage, int scope, int layers) const;

    /// The longest side, in points, of the longest page the window of \a scope pages around
    /// \a activePage holds.
    qreal longestSidePtInWindow(int activePage, int scope) const;

    /**
     * Clears the open document's modified flag: a strip is a view of the notebook, not a document to
     * write.
     *
     * cropImage()/resizeImage() run through KisProcessingApplicator, which pushes an undo command and
     * marks the document modified -- and Krita then autosaves it. On the tablet the tab gained an
     * asterisk and the status bar said "Saving Document... 76%", writing a .kra of a 400 MB strip
     * nobody asked for, and that save is the prime suspect for the memory the user watched climb.
     * Nothing is lost by clearing it: every page of the window was written before any of this ran,
     * and which pages still carry ink is the page window's own dirty set, not this flag.
     *
     * ONE helper, called from every path that changes the strip's pixels -- where the resize makes the
     * mark, at the end of a roll (after the layer adoption, which can mark it again), after the
     * budget is applied from the menu or a typed value, and on the closes. A path that forgets it is
     * a strip Krita autosaves, and that is the memory the user paid for.
     */
    void clearModifiedFlag();

    /// The layers a budget is divided by for \a window: the document's own count when one is open, or
    /// the shape the builder is about to make.
    int layersForBudget(const PdfStripLayout &window) const;

    /**
     * How many full-size layers the document that is open holds: the per-slot bands, the ink layer
     * the pages are drawn on, and any content layer a page's artifact restored. Each one is a
     * full-size allocation, so this is the count a budget is divided by.
     *
     * 0 when no document is open: a caller that is about to build one then uses the shape the
     * builder will make instead (one band per slot plus Ink), because a content layer is not known
     * until an artifact has been read back.
     */
    int stripLayerCount() const;

    /// Which slot of the open window the given document point is over, or -1. Window-local: it
    /// reads m_stripCells and nothing about the notebook.
    int windowSlotFor(const QPointF &point) const;

    /**
     * Which page the given document point falls on: the open one, a neighbour above or below it,
     * or -1 for none. The layout is the one the strip decoration draws, so the two agree about
     * where the neighbouring pages are.
     */
    int pageAtDocumentPoint(const QPointF &point, qreal zoom) const;

    QString m_projectDir;
    PdfSessionManifest m_manifest;
    QPointer<KisDocument> m_document;
    QPointer<KisView> m_view;
    int m_index = -1;

    bool m_scrollFollow = true;

    /// True while a page turn was started by the view following its centre instead of by the user
    /// asking for the next page. Such a turn must NOT move the view: the canvas is already where
    /// the user put it, and centring it again drags the centre back over the boundary it has just
    /// crossed -- which is exactly what "the active page does not change" looked like.
    bool m_turnFromScroll = false;

    /// Nothing is decided about the centre before this moment. The view is zoomed and centred by
    /// this code when a page opens; a tick that lands inside that has been seen to report a zoom of
    /// 47 while the canvas was at 0.14, and a centre at the document origin.
    qint64 m_viewSettleUntil = 0;

    /// Where a window move has already put the middle of the viewport, and whether the two places
    /// that would otherwise re-centre on the active page should leave the canvas alone because of
    /// it.
    ///
    /// A roll moves the window under a canvas that does not move with it, so the same pixels end
    /// up holding a different page -- measured on the tablet, a roll taken while page 5 filled
    /// the view left page 8 under the same middle, and the follow then turned to 8. The page
    /// being read is not the roll's to change: rollToPage() records the point that was in the
    /// middle and puts the canvas back on it, and while this is set neither activateWithinStrip()
    /// nor the 350 ms follow-up moves the view.
    QPointF m_rollAnchor;
    bool m_rollAnchored = false;

    /// When the last dropped reading was written out, so a canvas that never settles cannot flood.
    qint64 m_lastRejectLog = 0;

    /// When the follow last said anything at all. The switch being off and the follow running
    /// without ever deciding anything used to look exactly the same in the log -- an empty one --
    /// and telling them apart cost an afternoon.
    qint64 m_lastFollowLog = 0;

    /// True while a roll of the strip window is in progress. rollToPage() calls back into
    /// activateWithinStrip(), which is the very place that decides to roll -- without this the two
    /// called each other and the window rolled once a second, repainting every slot it held.
    bool m_rollingWindow = false;

    /// When the window last rolled, so a page sitting on the edge cannot roll it on every tick.
    qint64 m_lastWindowRoll = 0;

    /// True while several pages are being written one after another. The write waits on a nested
    /// event loop, and the follow timer keeps firing inside it: without this a page turn could run
    /// in the middle of the save that is cropping the very layer it would move.
    bool m_savingPages = false;

    /// The document the fit zoom has already been applied to. Fitting again on every page turn is
    /// what made the page under the centre jump around: the zoom changed, the centre moved with it,
    /// and the page the follow computed changed twice in 600 ms (0.25 <-> 0.667 in the log).
    QPointer<KisDocument> m_zoomPlacedFor;

    /// The view's own page system: which slot of the window the middle of the viewport is over.
    /// Window-local geometry (0..slots-1) that knows nothing about notebook page numbers. The two
    /// systems meet in one place -- windowSlotFor() and the single m_stripPages.at(slot) that turns
    /// a slot into a page -- and after a roll this is reset from the active page, so a changed
    /// mapping can never be mistaken for the user having moved.
    int m_windowSlot = -1;
    QTimer *m_scrollWatch = nullptr;

    /// Whether prepareForClose() has been hung off the application's quit already.
    bool m_quitHookInstalled = false;

    /// How many pages the open document holds at full resolution, and at what resolution.
    ///
    /// Three, which is design B. Verified end to end before it was turned on: a mark drawn a
    /// hundred pixels into the page comes back a hundred pixels into an artifact the size of the
    /// page and not of the strip, and turning between pages already in the strip builds nothing.
    /// One was the safe answer while the cropping was unproven, because saving a strip without
    /// cropping writes several pages into one page's ink, quietly.
    /// Three: the page being written on, one above it and one below.
    ///
    /// It was five, to keep a run of turns inside one strip. Five also means the pages two away
    /// stay in the document, and the page a long way back is still there to scroll to, which is not
    /// what a notebook should show -- one above and one below is what was asked for.
    ///
    /// The price is that the window is rebuilt every other turn, because rolling it -- repainting
    /// the one slot that goes out instead of building a new strip -- is not written yet. That is
    /// the piece that will make a run of turns continuous.
    /// One page at a time, which is design A: one document per page, one view per page, the shape
    /// this was built and verified in -- open, draw, turn with an automatic save, export, and the
    /// ink coming back where it was drawn.
    ///
    /// The strip, design B, is behind this number and is not working well enough to ship: pages
    /// above and below in one document, the window rolling rather than rebuilding, the active page
    /// following the middle of the viewport. Raising it to three turns that on. It is left at one
    /// deliberately, so the notebook is usable while the strip is finished.
    /// Five pages at a time, which is what the notebook ships with: the active page with two on
    /// each side. One page at a time is still reachable from the menu, and the default used to be
    /// that -- a page with no neighbours on screen, which is not what the notebook is for.
    int m_scope = 5;

    /// How many pages the strip holds when it is ON: the switch's "how many" half, kept apart from
    /// whether it is on. Read from pdfio/stripPages in the constructor, written by setScope(), and
    /// never rewritten by turning the strip off -- so the switch turns it back on at the count it
    /// had. Five, the count the notebook has always shipped with.
    int m_stripPageCount = 5;

    /// The memory budget for the rendered window, in megabytes, or 0 for no limit -- the fixed
    /// 200 dpi the pages are rendered at. Read from QSettings in the constructor, written by
    /// setMemoryBudgetMb().
    int m_memoryBudgetMb = 0;

    /// The resolution the document that is up was built at. Stored rather than derived on demand
    /// because the derivation depends on the layer count of the window being built, and the answer
    /// has to be the number the document in front of the reader really has. 0 until a window is
    /// built, which currentRenderDpi() reports as the 200 dpi default.
    qreal m_renderedDpi = 0.0;

    /// How long the page under the centre must stay there before the follow turns to it, in
    /// milliseconds. Read from pdfio/scrollSettleMs in the constructor, written by
    /// setScrollSettleMs(). 450 is what every build before the setting did.
    int m_scrollSettleMs = 450;

    /// The share of the viewport height the active page takes at fit, in percent. Read from
    /// pdfio/readingSharePercent in the constructor, written by setReadingSharePercent(). 60 is the
    /// three fifths every build before the setting used.
    int m_readingSharePercent = 60;

    /// The share as the fraction the fit arithmetic wants (60 -> 0.6).
    qreal readingShare() const
    {
        return qreal(m_readingSharePercent) / 100.0;
    }

    /**
     * Which pages may stay open, and the rule that keeps closing one from losing ink.
     *
     * Its bound is \ref m_scope, so design A is a window of one and design B would be a window of
     * three. What it adds over the old "opening a page closes the one before it" is the dirty
     * flag: a page with unsaved ink is never the one that is dropped. When every slot holds such a
     * page, the least recently used one is saved to make room, and a save that fails refuses the
     * page turn instead of discarding the page.
     */
    PdfPageWindow m_window;

    /**
     * The single door every page write in the notebook goes through.
     *
     * One write in the air at a time, because Krita wedges when a second background save is
     * started while the first is still running -- and scrolling a notebook reaches that on its
     * own, with an eviction, an idle write and the next eviction inside a second of each other.
     * Everything that has to be sure the ink is on disk waits here for a landing rather than for
     * a start: the window evicting a page, the roll wiping a slot, a page rebuilt from its
     * artifact, a tab closing.
     */
    PdfSaveQueue m_saves;

    /// What the ink looked like when the current write took its copy. The page counts as clean
    /// when that write lands only if nothing has changed since: a stroke that landed while the
    /// write was in the air must not be forgiven by a write that never saw it.
    QHash<int, qint64> m_saveStamps;

    /// Why the last write could not even start, for the log and the refusal messages -- the
    /// queue knows that a write failed, not what the disk said about it.
    QString m_lastWriteError;

    /// The idle-write cadence: its timer, when the ink last changed, and when a write last ran.
    QTimer *m_autoSaveTimer = nullptr;
    qint64 m_lastInkChange = 0;
    qint64 m_lastAutoSave = 0;

    /// Which image's change signal is being watched. The document is replaced on every page
    /// turn, and the previous one's signal must not go on marking pages of the new one.
    QMetaObject::Connection m_inkChangeConnection;

    /// True while a page turn is being made. A second turn arriving from inside an event loop
    /// this one is waiting in -- a click while a write is landing -- must be refused, not
    /// interleaved: two turns deciding at once where the ink belongs is how it ends up neither
    /// where it was drawn nor on disk.
    bool m_inPageTurn = false;

    /// The pages the open strip holds and where each sits. Empty when the document is a single
    /// page, which is also how the code tells the two apart.
    QList<int> m_stripPages;
    QList<QRect> m_stripRects;

    /// The band each slot owns, in slot order. The cells tile the strip; the page rectangles inside
    /// them do not when the pages are of different sizes, and it is the band that decides which page
    /// the centre of the viewport is over.
    QList<QRect> m_stripCells;

    /// The paper layer of each slot, in slot order, for repainting one of them.
    QList<KisNodeSP> m_stripPaper;
    int m_stripActiveSlot = -1;

    void makeOneThumbnail();

    /**
     * Opens \a index by building a document for it alone, or for a strip of pages around it.
     *
     * A page that is already in the open strip needs neither: activateWithinStrip unlocks its slot
     * and asks for it, which is the difference between a page turn that costs 650 ms and one that
     * costs nothing. The 650 ms is mostly the document and the view, measured on the tablet, and
     * the strip exists to not pay it.
     */
    bool buildForSinglePage(int index, QString *why);
    bool buildForStrip(int index, QString *why);

    /**
     * Moves the window one page without building a new document.
     *
     * Two phases, in that order: every page the window currently holds is written and has
     * LANDED, then every slot -- all of them, changed or not -- is redrawn from what was just
     * written: paper rendered again from the source, ink read back out of its artifact. After a
     * window move the strip and the disk are the same thing, checked against each other on every
     * roll instead of assumed to agree. It still costs no new document or view, which is what a
     * rebuild per turn would pay and what the roll exists to avoid; what it costs is the writes
     * and the renders for the whole window, once per window move rather than once per slot.
     *
     * A write that cannot be made, or ink that keeps arriving through the writes, refuses the
     * roll with the strip untouched -- never a wipe of pixels the disk does not have.
     */
    /// a centreOn says which page the new window is centred on; the page that becomes active is
    /// a index either way. Rolling down leaves the active page one slot in from the top rather
    /// than in the middle, so the page just left stays visible above it and the follow does not
    /// turn straight back to it. -1 centres on a index.
    ///
    /// \a keepTheReadingPage is for a window move the VIEW decided (a scroll): the page in the
    /// middle of the viewport is where the reader is, and the move must leave it there -- the
    /// window shifting underneath does not mean the book turned. An explicit turn passes false,
    /// because there the user asked for a page and the canvas is meant to move to it.
    bool rollToPage(int index, QString *why, int centreOn = -1, bool keepTheReadingPage = false);
    bool activateWithinStrip(int index, QString *why);

    /// Creates the document, its view and the strip decoration, and gives the page that was open
    /// back to the event loop. Shared by both ways of opening one.
    bool showImage(KisImageSP image, KisNodeSP activeNode, int index,
                   const PdfStripLayout &layout, QString *why);

    QList<int> m_thumbnailQueue;
    QTimer *m_thumbnailTimer = nullptr;

    /// What reloadNotebook() is going to open once the old view has gone, and whether one is
    /// waiting. The manifest is read BEFORE the reload is scheduled, so a notebook that cannot be
    /// read back is the caller's answer rather than a reload that half happened on the event loop.
    PdfSessionManifest m_reloadManifest;
    int m_reloadAnchor = 0;
    bool m_reloadPending = false;

    /// One open renderer per source, and where "render the page" is answered: the record's own page
    /// inside the record's own source, never the notebook position. Kept open between thumbnails,
    /// between the slots of a strip and between the frames of a roll, because parsing a PDF once is
    /// worth more than all of them.
    PdfSourceRenderers m_sourceRenderers;

    /// So one continued gesture does not turn several pages.
    qint64 m_lastTurn = 0;

    /// The page the middle of the view is over, and since when, so a page is only turned to once
    /// the view has stopped there.
    int m_candidatePage = -1;
    qint64 m_candidateSince = 0;


};

#endif // PDFPAGENAVIGATOR_H
