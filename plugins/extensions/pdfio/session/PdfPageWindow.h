/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGEWINDOW_H
#define PDFPAGEWINDOW_H

#include <QList>
#include <QSet>
#include <QString>

#include <functional>

/**
 * Which pages are allowed to stay open at once, and what has to happen before one of them goes.
 *
 * A page costs about 41 MB resident (measured across twelve pages), so a notebook cannot keep all
 * of them: without a bound, opening page 200 of a 500-page scan is the same as opening all of
 * them. The bound is the easy half. The half that loses work is what happens when the window is
 * full and the page that has to make room is the one the user just drew on.
 *
 * The policy is one page at a time, exactly:
 *
 *  - Pages are held in least-recently-used order. Opening a page that is already in the window
 *    just moves it to the front of the queue.
 *  - When opening a page would push the window past its capacity, the least recently used *clean*
 *    page is evicted. A clean page has nothing on it that is not already on disk, so dropping it
 *    costs only the render that opening it again will do anyway.
 *  - A page that is dirty -- it carries ink that is not on disk -- is never dropped. If every slot
 *    in the window is dirty, the least recently used one is *saved* first and only then evicted,
 *    so what leaves the window is on disk.
 *  - If that save fails, the eviction does not happen at all, the page that is open stays open,
 *    the new page is not opened, and open() reports why. Losing ink to make room is never an
 *    option; a page turn that cannot be completed safely is a failed page turn.
 *
 * The class is deliberately dependency-light: Qt's containers, a std::function for the save, and
 * nothing from the application. That is what lets the policy be exercised in ctest the way
 * PdfSessionManifest is, instead of only inside a running Krita, which is where the bug this
 * exists to prevent would otherwise hide.
 *
 * It is not a cache of page rasters. It is the bookkeeping that decides what may be closed; the
 * document and the render behind an open page belong to the caller.
 */
class PdfPageWindow
{
public:
    /**
     * Writes the ink of \a index so the page can be dropped. Returns false and sets \a why when
     * it could not. Called synchronously, from inside open(), because the decision to evict depends
     * on the answer.
     */
    using SaveFn = std::function<bool(int index, QString *why)>;

    explicit PdfPageWindow(int capacity = 1);

    /// The number of pages that may stay open at once. Never below one.
    int capacity() const;

    /**
     * Changes the bound. Pages already open are not thrown out by this: eviction happens when a
     * page is opened, so a window that is temporarily over a shrunken bound empties on the next
     * open rather than discarding something behind the caller's back.
     */
    void setCapacity(int capacity);

    void setSaver(SaveFn saver);

    /**
     * Makes \a index one of the open pages, evicting as described above when the window is full.
     *
     * Returns false without opening anything when making room would have to discard a dirty page
     * whose save failed (or when there is no saver at all). \a why is set to the reason.
     */
    bool open(int index, QString *why = nullptr);

    /**
     * Notes that \a index carries ink that is not on disk yet, or that it no longer does.
     *
     * Marking a page clean is the caller's statement that the page is on disk; the policy trusts
     * it, and a page marked clean can be evicted without a save.
     */
    void setDirty(int index, bool dirty = true);
    bool isDirty(int index) const;

    /// The pages believed to carry ink that is not on disk yet, in ascending order -- what the
    /// notebook's idle write flushes, and what a caller can ask for without walking the open
    /// list one page at a time.
    QList<int> dirtyPages() const;

    bool isOpen(int index) const;
    int openCount() const;

    /// The open pages, least recently used first. The last entry is the page just opened.
    QList<int> openPages() const;

    /// Forgets every page, without saving anything. For a notebook being closed, never a page turn:
    /// a dirty page dropped through here is ink the policy was supposed to protect.
    void clear();

    /// How many pages this policy has evicted, and of those how many were saved to get there.
    int evictionCount() const;
    int savedBeforeEvictionCount() const;

    /// How many times an eviction was refused because the save it needed failed.
    int blockedEvictionCount() const;

private:
    int m_capacity = 1;
    /// Least recently used first, most recently used last.
    QList<int> m_open;
    QSet<int> m_dirty;
    SaveFn m_saver;

    int m_evictions = 0;
    int m_savedBeforeEviction = 0;
    int m_blockedEvictions = 0;
};

#endif // PDFPAGEWINDOW_H
