/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfSaveQueue.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QTimer>
#include <QtTest>

namespace {

/// A starter that starts writes but decides itself when -- and whether -- they land, the way a
/// disk that has not made up its mind yet holds a real one. Three behaviours: hold the
/// completion for the test to fire by hand, land after a delay from the event loop, or never
/// report at all (the wedge) while keeping the late report to play by hand.
class HeldWriteStarter
{
public:
    PdfSaveQueue::StartFn fn()
    {
        return [this](int page, PdfSaveQueue::DoneFn done) {
            ++started;
            startedOrder.append(page);

            if (refusesToStart) {
                return false;
            }
            if (neverCompletes) {
                wedged.append(done);
                return true;
            }
            if (completesAfterMs < 0) {
                ++inFlight;
                maxInFlight = qMax(maxInFlight, inFlight);
                /// The landing is fired by the test by hand, so the in-flight count has to come
                /// down there too -- wrapped around the done the queue is owed, which passes the
                /// verdict straight through.
                held.append([this, done](bool ok) {
                    --inFlight;
                    done(ok);
                });
                return true;
            }

            ++inFlight;
            maxInFlight = qMax(maxInFlight, inFlight);
            const int delay = completesAfterMs;
            QTimer::singleShot(delay, [this, done]() {
                --inFlight;
                done(true);
            });
            return true;
        };
    }

    int completesAfterMs = -1;
    bool neverCompletes = false;
    bool refusesToStart = false;

    int started = 0;
    QList<int> startedOrder;
    int inFlight = 0;
    int maxInFlight = 0;
    QList<PdfSaveQueue::DoneFn> held;
    QList<PdfSaveQueue::DoneFn> wedged;
};

} // namespace

/**
 * The pipeline's contract, without Krita behind it: one write at a time, in order,
 * deduplicated, with a failure that belongs to its page, a watchdog that reclaims a write that
 * never reports, and waiters that only proceed when the write they asked for has landed.
 *
 * Every waiting test runs the waiter's own event loop -- saveNow() and waitIdle() block by
 * design, which is what lets the navigator keep a page turn from outrunning the disk.
 */
class PdfSaveQueueTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOneWriteAtATimeInRequestOrder();
    void testAskingTwiceForAPageIsOneWrite();
    void testSaveNowWaitsForThisPageToLand();
    void testAFailedWriteDoesNotStopTheRest();
    void testStarterRefusalIsAFailureNotAWait();
    void testWatchdogReclaimsAWedgedWriteAndIgnoresItsLateReport();
    void testWaitIdleDrainsEveryQueuedWrite();
    void testAQueueWithNoStarterFailsFastInsteadOfHanging();
};

void PdfSaveQueueTest::testOneWriteAtATimeInRequestOrder()
{
    PdfSaveQueue queue(2000);
    HeldWriteStarter starter;
    queue.setStarter(starter.fn());

    queue.request(1);
    queue.request(2);
    queue.request(3);

    /// Only the first write has begun; two and three wait their turn instead of running
    /// alongside it. Two background saves in the air at once is the wedge this prevents.
    QCOMPARE(starter.startedOrder, QList<int>({ 1 }));
    QCOMPARE(queue.pendingCount(), 3);
    QCOMPARE(queue.startedCount(), 1);

    /// Each landing lets exactly the next write start, and only from the event loop -- never
    /// from inside the completion of the write before it, which is the stack that once grew
    /// through callbacks until the application died.
    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);
    QCOMPARE(starter.startedOrder, QList<int>({ 1, 2 }));

    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);
    QCOMPARE(starter.startedOrder, QList<int>({ 1, 2, 3 }));

    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);

    QVERIFY2(queue.isIdle(), "all three writes have landed; the queue must be idle");
    QCOMPARE(queue.startedCount(), 3);
    QCOMPARE(queue.failedCount(), 0);
    QCOMPARE(queue.pendingCount(), 0);
    QVERIFY(queue.hasLanded(1));
    QVERIFY(queue.hasLanded(2));
    QVERIFY(queue.hasLanded(3));

    /// The invariant the whole class exists for, measured rather than asserted in a comment:
    /// never more than one write in flight.
    QCOMPARE(starter.maxInFlight, 1);
}

void PdfSaveQueueTest::testAskingTwiceForAPageIsOneWrite()
{
    PdfSaveQueue queue(2000);
    HeldWriteStarter starter;
    queue.setStarter(starter.fn());

    queue.request(7);
    queue.request(7);
    queue.request(7);

    /// One write started, not three: the ink of a page asked for three times is one file.
    QCOMPARE(starter.started, 1);
    QCOMPARE(queue.pendingCount(), 1);

    /// And asking again while it is still in flight changes nothing either.
    queue.request(7);
    QCOMPARE(starter.started, 1);

    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);

    QVERIFY(queue.hasLanded(7));
    QCOMPARE(starter.started, 1);

    /// Once it has landed, asking again is a new write: the previous answer must not swallow
    /// ink that arrived after it.
    queue.request(7);
    QCOMPARE(starter.started, 2);
    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);
    QVERIFY(queue.hasLanded(7));
    QCOMPARE(queue.failedCount(), 0);
}

void PdfSaveQueueTest::testSaveNowWaitsForThisPageToLand()
{
    /// An ordinary asynchronous write: when saveNow() returns true the page is on disk, which is
    /// the promise every eviction and every slot wipe is made against.
    PdfSaveQueue queue(2000);
    HeldWriteStarter starter;
    starter.completesAfterMs = 20;
    queue.setStarter(starter.fn());

    QString why;
    QVERIFY2(queue.saveNow(4, &why), qPrintable(why));
    QVERIFY(why.isEmpty());
    QVERIFY(queue.hasLanded(4));
    QCOMPARE(queue.startedCount(), 1);

    /// A write that reports failure is a false saveNow with a reason -- and the block really did
    /// wait for the landing: the completion only fires from the event loop inside saveNow().
    PdfSaveQueue second(2000);
    second.setStarter([](int, PdfSaveQueue::DoneFn done) {
        QTimer::singleShot(10, [done]() { done(false); });
        return true;
    });

    QString failWhy;
    QVERIFY(!second.saveNow(0, &failWhy));
    QVERIFY2(!failWhy.isEmpty(), "a refused write has to say why");
    QVERIFY(second.hasFailed(0));
    QCOMPARE(second.failedCount(), 1);
}

void PdfSaveQueueTest::testAFailedWriteDoesNotStopTheRest()
{
    PdfSaveQueue queue(2000);
    HeldWriteStarter starter;
    queue.setStarter(starter.fn());

    /// Page 1 fails the way a full disk does: it started, and it reports it did not make it.
    queue.request(1);
    queue.request(2);

    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(false);
    QTest::qWait(5);

    /// The queue moved on to page 2 on its own. One page that cannot be written does not stop
    /// every page behind it -- which is what a queue that waited for success would do, and how
    /// one refused eviction used to become a run of refused saves.
    QCOMPARE(starter.startedOrder, QList<int>({ 1, 2 }));

    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);

    QVERIFY(queue.hasFailed(1));
    QVERIFY(queue.hasLanded(2));
    QCOMPARE(queue.failedCount(), 1);
    QVERIFY(queue.isIdle());
}

void PdfSaveQueueTest::testStarterRefusalIsAFailureNotAWait()
{
    PdfSaveQueue queue(80);
    HeldWriteStarter starter;
    starter.refusesToStart = true;
    queue.setStarter(starter.fn());

    /// A starter that returns false owes no completion call; the queue settles the page itself
    /// instead of waiting out the watchdog for a write that does not exist.
    QString why;
    QVERIFY(!queue.saveNow(5, &why));
    QVERIFY(!why.isEmpty());
    QVERIFY(queue.hasFailed(5));
    QCOMPARE(queue.failedCount(), 1);
    QCOMPARE(queue.watchdogCount(), 0);
    QVERIFY(queue.isIdle());
}

void PdfSaveQueueTest::testWatchdogReclaimsAWedgedWriteAndIgnoresItsLateReport()
{
    PdfSaveQueue queue(80);
    HeldWriteStarter starter;
    starter.neverCompletes = true;
    queue.setStarter(starter.fn());

    /// The wedge, exactly as the notebook hit it: the write starts and never reports back. The
    /// watchdog turns that from "every save from now on is stuck" into one failed page.
    const qint64 began = QDateTime::currentMSecsSinceEpoch();
    QString why;
    QVERIFY(!queue.saveNow(0, &why));
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - began;

    QVERIFY2(queue.hasFailed(0), "the wedged write has to be declared failed");
    QCOMPARE(queue.watchdogCount(), 1);
    QVERIFY2(queue.isIdle(), "the watchdog has to hand the queue back, not leave it stuck");
    QVERIFY2(elapsed < 5000, "the wait has to be bounded by the watchdog, not by luck");

    /// And the queue goes on being usable: the very next write runs and lands.
    starter.neverCompletes = false;
    starter.completesAfterMs = 10;
    QVERIFY(queue.saveNow(1));
    QVERIFY(queue.hasLanded(1));
    QCOMPARE(queue.startedCount(), 2);

    /// Page 0 is asked for again -- a new write, a new generation -- and is held in flight, so
    /// the wedged write's late report arrives in the middle of its replacement.
    starter.completesAfterMs = -1;
    queue.request(0);
    QCOMPARE(queue.startedCount(), 3);
    QVERIFY2(!queue.hasLanded(0), "the new write is in flight; nothing has landed for page 0");

    QCOMPARE(starter.wedged.size(), 1);
    starter.wedged.takeFirst()(true);
    QTest::qWait(5);

    QVERIFY2(queue.hasLanded(0) == false,
             "a late report from the abandoned write must not answer for the write in flight");
    QVERIFY2(queue.pendingCount() == 1, "the replacement write must still be in flight");
    QCOMPARE(queue.startedCount(), 3);

    /// The replacement lands properly and is the outcome that sticks.
    QVERIFY(!starter.held.isEmpty());
    starter.held.takeFirst()(true);
    QTest::qWait(5);
    QVERIFY(queue.hasLanded(0));
    QVERIFY(queue.isIdle());
}

void PdfSaveQueueTest::testWaitIdleDrainsEveryQueuedWrite()
{
    PdfSaveQueue queue(2000);

    /// Two asynchronous writes with different landing times. waitIdle() must see both, not just
    /// the first one that happens to finish.
    QList<int> landed;
    queue.setStarter([&landed](int page, PdfSaveQueue::DoneFn done) {
        QTimer::singleShot(page == 1 ? 8 : 30, [&landed, page, done]() {
            landed.append(page);
            done(true);
        });
        return true;
    });

    queue.request(2);
    queue.request(1);

    QVERIFY(queue.waitIdle());
    QCOMPARE(queue.pendingCount(), 0);
    QCOMPARE(landed.size(), 2);
    QCOMPARE(queue.startedCount(), 2);
    QCOMPARE(queue.failedCount(), 0);
    /// Both, the slower one included: waitIdle() answers for the queue being empty, which is
    /// only true once every write has landed.
    QVERIFY(landed.contains(1));
    QVERIFY(landed.contains(2));
}

void PdfSaveQueueTest::testAQueueWithNoStarterFailsFastInsteadOfHanging()
{
    PdfSaveQueue queue(80);

    QString why;
    QVERIFY(!queue.saveNow(3, &why));
    QVERIFY2(!why.isEmpty(), "nothing to start with has to be said, not waited on");
    QVERIFY(queue.hasFailed(3));
    QCOMPARE(queue.failedCount(), 1);
    QVERIFY(queue.isIdle());
    QCOMPARE(queue.watchdogCount(), 0);
}

QTEST_GUILESS_MAIN(PdfSaveQueueTest)
#include "PdfSaveQueueTest.moc"
