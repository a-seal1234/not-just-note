/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSAVEQUEUE_H
#define PDFSAVEQUEUE_H

#include <QHash>
#include <QList>
#include <QString>

#include <functional>

class QTimer;

/**
 * One page write at a time, in order, and nobody moves on until the write has landed.
 *
 * This class exists because of two things that were measured in the running notebook:
 *
 *  - Krita wedges when a second background save is started while the first is still running
 *    (the note in saveStripPages() records the earlier attempt that died that way). A notebook
 *    that turns pages while saving -- which is exactly what scrolling does -- reaches that state
 *    without trying: an eviction save, an autosave, and the next eviction can all be in the air
 *    within a second of each other. A wedged write never reports back, so the ink it was carrying
 *    reaches disk nowhere, and every later write behind it is stuck too.
 *  - Everything downstream decides from "the write landed": the bounded page window evicts a
 *    dirty page only after its save, the strip roll wipes a slot only after the ink in it has
 *    been written, and a page being rebuilt reads its ink back out of the artifact. None of that
 *    is worth anything if "started" is taken for "finished", or if a read can happen while the
 *    write it depends on is still in the air.
 *
 * So the queue is the single door every page write goes through:
 *
 *  - Never more than one write in flight. A request that arrives while its page is already
 *    queued or being written is the same write, not a second one.
 *  - FIFO, so a page asked for twice is written twice but never reordered against its neighbours.
 *  - A write that never reports is declared failed after \ref landingTimeoutMs and the queue
 *    moves on. The watchdog is what keeps one wedged save from stopping every save after it;
 *    a late report from the old write is ignored rather than allowed to answer for a newer one.
 *  - A failure belongs to its page. One page that cannot be written does not stop the rest of
 *    the queue, and it does not fail a waiter that is waiting for a different page.
 *
 * Dependency-light -- Qt's containers, a std::function for the write, one timer -- the same
 * bargain PdfPageWindow makes, so the ordering, the deduplication, the watchdog and the bounded
 * waits are exercised in ctest instead of only inside a running Krita, which is where the wedge
 * this exists to remove was first observed.
 *
 * It does not know what a page is or how one is written: the navigator owns both. It only
 * decides when a write may start and who is allowed to proceed before it has landed.
 */
class PdfSaveQueue
{
public:
    /// Called exactly once per started write: true when it landed, false when it terminally failed.
    using DoneFn = std::function<void(bool ok)>;

    /**
     * Starts the write for \a page.
     *
     * Returns false when the write could not even start; \a done must then NOT be called.
     * Returns true having promised exactly one \a done call, eventually -- immediately is fine
     * when there was nothing to write. A starter that keeps its promise makes the watchdog
     * irrelevant; one that cannot is why the watchdog exists.
     */
    using StartFn = std::function<bool(int page, DoneFn done)>;

    /**
     * \a landingTimeoutMs bounds one write: one that has not reported by then is recorded as
     * failed and the queue goes on. Callers wait landingTimeoutMs plus a small margin, so the
     * watchdog -- not the waiter -- is what ends a wedged write, and a waiter that times out
     * anyway is reporting a queue that could not even be watched.
     */
    explicit PdfSaveQueue(int landingTimeoutMs = 15000);

    void setStarter(StartFn starter);

    int landingTimeout() const;

    /**
     * Queues \a page and starts the next write if the queue is idle. Asking for a page that is
     * already queued or in flight does nothing: it is the same write. The answer arrives through
     * saveNow() or waitIdle(), not through this call.
     */
    void request(int page);

    /**
     * Queues \a page and waits for that page's own write to have landed. True only when THIS
     * page is on disk now; another page's failure does not fail it, and its own failure does not
     * stop the pages behind it. A page that was already in flight is waited for, not rewritten.
     */
    bool saveNow(int page, QString *why = nullptr);

    /**
     * Waits until nothing is queued or being written. This is the reader's side: a page must not
     * be rebuilt from its artifact while any write is still landing, because the write may be
     * that very page's. True when the queue drained; false when it could not within the bound
     * (individual write outcomes are the waiters' business, see failedCount()).
     */
    bool waitIdle(QString *why = nullptr);

    /// No page queued and none being written.
    bool isIdle() const;

    /// Queued plus being written: what has to finish before a reader may read.
    int pendingCount() const;

    /// Whether \a page's most recent write has landed, or terminally failed.
    bool hasLanded(int page) const;
    bool hasFailed(int page) const;

    /// How many writes the queue has started, how many ended failed, and how many of those the
    /// watchdog had to declare rather than being told. Exposed the way PdfPageWindow exposes its
    /// counters: observable without a debugger, and the number the wedge would hide.
    int startedCount() const;
    int failedCount() const;
    int watchdogCount() const;

private:
    enum class State {
        Idle,     ///< Never asked for, or reset: nothing is known about this page.
        Queued,   ///< Waiting for its turn in FIFO order.
        InFlight, ///< Its write has started and has not reported yet.
        Landed,   ///< Its last write reached the disk.
        Failed,   ///< Its last write failed, or the watchdog gave up on it.
    };

    void pump();
    /// The single writer of state: records the outcome, wakes every waiter, and schedules the
    /// next write on the next turn of the event loop so no save is ever started from inside the
    /// signal handler of the save before it.
    void finish(int page, int generation, bool ok);
    void wakeWaiters();
    bool waitUntil(std::function<bool()> ready, QString *why, const QString &what);

    int m_landingTimeoutMs;
    StartFn m_starter;

    QHash<int, State> m_state;
    /// Queued pages, least recently requested first.
    QList<int> m_order;
    /// The page being written, or -1. Only finish() with the matching generation may clear it.
    int m_active = -1;
    /// Which write of a page is current, so a report from a write the watchdog already gave up
    /// on cannot answer for the newer write now in its place.
    QHash<int, int> m_generation;
    QTimer *m_watchdog = nullptr;

    /// Callbacks to re-check their own condition after every write finishes, keyed by the id
    /// waitUntil() handed out. A waiter removes its own entry the moment it leaves, before the
    /// locals its closure captures go out of scope -- the queue keeps finishing writes long
    /// after a waiter has stopped waiting.
    QHash<int, std::function<void()>> m_waiters;
    int m_waiterSeq = 0;

    int m_started = 0;
    int m_failed = 0;
    int m_watchdogs = 0;
};

#endif // PDFSAVEQUEUE_H
