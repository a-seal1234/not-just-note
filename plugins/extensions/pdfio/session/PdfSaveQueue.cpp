/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfSaveQueue.h"

#include <QEventLoop>
#include <QTimer>
#include <QtGlobal>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

PdfSaveQueue::PdfSaveQueue(int landingTimeoutMs)
    : m_landingTimeoutMs(qMax(1, landingTimeoutMs))
{
    /// Owned by nothing but the queue: it is the watchdog of this queue's own in-flight write,
    /// and it must die with the queue so a queued timeout cannot call into a dead one.
    m_watchdog = new QTimer();
    m_watchdog->setSingleShot(true);
    QObject::connect(m_watchdog, &QTimer::timeout, m_watchdog, [this]() {
        if (m_active < 0) {
            return;
        }
        /// The write never reported. Declaring it failed is what keeps one wedge from becoming
        /// the state of every write behind it; the late report this write may still produce is
        /// ignored by the generation check in finish().
        ++m_watchdogs;
        const int page = m_active;
        finish(page, m_generation.value(page, 0), false);
    });
}

void PdfSaveQueue::setStarter(StartFn starter)
{
    m_starter = std::move(starter);
}

int PdfSaveQueue::landingTimeout() const
{
    return m_landingTimeoutMs;
}

void PdfSaveQueue::request(int page)
{
    if (page < 0) {
        return;
    }

    const State state = m_state.value(page, State::Idle);
    if (state == State::Queued || state == State::InFlight) {
        /// Already coming. A second request for a page that is being written is not a second
        /// write: it is the same ink, and starting it again would be the overlap this class
        /// exists to prevent.
        return;
    }

    m_state[page] = State::Queued;
    m_order.append(page);
    pump();
}

bool PdfSaveQueue::saveNow(int page, QString *why)
{
    request(page);

    const bool settled = waitUntil(
        [this, page]() {
            const State state = m_state.value(page, State::Idle);
            return state == State::Landed || state == State::Failed;
        },
        why,
        QStringLiteral("page %1 was not written in time").arg(page + 1));

    if (!settled) {
        return false;
    }
    if (m_state.value(page, State::Idle) == State::Failed) {
        fail(why, QStringLiteral("writing page %1 failed").arg(page + 1));
        return false;
    }
    return true;
}

bool PdfSaveQueue::waitIdle(QString *why)
{
    return waitUntil([this]() { return isIdle(); }, why,
                     QStringLiteral("the notebook still had writes in flight"));
}

bool PdfSaveQueue::isIdle() const
{
    return m_active < 0 && m_order.isEmpty();
}

int PdfSaveQueue::pendingCount() const
{
    return m_order.size() + (m_active >= 0 ? 1 : 0);
}

bool PdfSaveQueue::hasLanded(int page) const
{
    return m_state.value(page, State::Idle) == State::Landed;
}

bool PdfSaveQueue::hasFailed(int page) const
{
    return m_state.value(page, State::Idle) == State::Failed;
}

int PdfSaveQueue::startedCount() const
{
    return m_started;
}

int PdfSaveQueue::failedCount() const
{
    return m_failed;
}

int PdfSaveQueue::watchdogCount() const
{
    return m_watchdogs;
}

void PdfSaveQueue::pump()
{
    if (m_active >= 0 || m_order.isEmpty()) {
        return;
    }

    if (!m_starter) {
        /// Without a starter nothing can ever start, and the pages queued behind that are not
        /// being written either. Say so per page instead of waiting for a bound that nothing
        /// armed.
        while (!m_order.isEmpty()) {
            const int page = m_order.takeFirst();
            m_state[page] = State::Failed;
            ++m_failed;
        }
        wakeWaiters();
        return;
    }

    const int page = m_order.takeFirst();
    const int generation = m_generation.value(page, 0) + 1;
    m_generation[page] = generation;
    m_state[page] = State::InFlight;
    m_active = page;
    ++m_started;

    m_watchdog->start(m_landingTimeoutMs);

    const bool started = m_starter(page, [this, page, generation](bool ok) {
        finish(page, generation, ok);
    });
    if (!started) {
        /// It never started, so nothing will report back: settle it here rather than waiting out
        /// the watchdog for a write that does not exist.
        finish(page, generation, false);
    }
}

void PdfSaveQueue::finish(int page, int generation, bool ok)
{
    if (m_active != page || m_generation.value(page, 0) != generation) {
        /// A late report from a write the watchdog already gave up on, or a duplicate of one
        /// that has been answered. Either way it must not overwrite the outcome of the write
        /// that is current now.
        return;
    }

    m_active = -1;
    m_watchdog->stop();
    m_state[page] = ok ? State::Landed : State::Failed;
    if (!ok) {
        ++m_failed;
    }

    wakeWaiters();

    /// The next write is started from the event loop, never from inside sigSavingFinished of the
    /// write before it: the earlier chain that grew a call stack through callbacks instead of
    /// through a loop is what died with SIGSEGV (core 243431, recorded in saveStripPages()).
    QTimer::singleShot(0, m_watchdog, [this]() { pump(); });
}

void PdfSaveQueue::wakeWaiters()
{
    /// The values, copied: a waiter only re-checks its own condition and quits a loop, but a
    /// copy keeps that a promise rather than an assumption, and the ids nobody needs here are
    /// left alone.
    const QList<std::function<void()>> waiters = m_waiters.values();
    for (const std::function<void()> &wake : waiters) {
        if (wake) {
            wake();
        }
    }
}

bool PdfSaveQueue::waitUntil(std::function<bool()> ready, QString *why, const QString &what)
{
    if (ready()) {
        return true;
    }

    QEventLoop loop;
    bool satisfied = false;
    const int id = ++m_waiterSeq;

    /// The condition is re-checked on every finish rather than on a timer of its own: the queue
    /// is what knows when a write has landed, and a waiter that waits on anything else can only
    /// guess.
    m_waiters.insert(id, [ready, &loop, &satisfied]() {
        if (ready()) {
            satisfied = true;
            loop.quit();
        }
    });

    /// A safety net under the watchdog, not the primary bound: the watchdog fires first for an
    /// in-flight write, and this only ends a wait the queue somehow stopped driving at all.
    QTimer::singleShot(m_landingTimeoutMs + 1000, &loop, &QEventLoop::quit);
    loop.exec();

    /// The entry goes before the locals its closure captures do. The queue keeps finishing
    /// writes after this waiter has left, and none of them may call into a dead loop -- this is
    /// why the waiters are keyed by id and not held in a list of closures that cannot be told
    /// apart.
    m_waiters.remove(id);

    if (!satisfied) {
        fail(why, what);
        return false;
    }
    return true;
}
