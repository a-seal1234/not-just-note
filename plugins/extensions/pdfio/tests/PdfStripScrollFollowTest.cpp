/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfStripLayout.h"

#include <QList>
#include <QPoint>
#include <QRect>
#include <QStringList>
#include <QtTest>

#include <limits>

/**
 * The bug the user reported: "when we ran strip (B), it did not track that the view was sitting
 * centred on a page, and once it stopped moving it never switched the active page."
 *
 * PdfPageNavigator::checkScrollFollow() has all the parts for it. m_scrollWatch ticks every 150 ms
 * while idle, and it only turns when the page under the centre of the viewport has been the same
 * for SettleMs (450) and the last turn is older than TurnCooldownMs (700)
 * (PdfPageNavigator.cpp:281-450). The page under the centre comes from pageAtDocumentPoint(),
 * whose strip branch is an exact rectangle test over m_stripRects (:530).
 *
 * What this replica takes as given is the centre itself, and that is where the reported "the
 * coordinates of each page are wrong, and at the end it stays on the second to last page" came
 * from: the navigator read the centre out of the vertical scrollbar in the wrong unit, so the
 * answer drifted with the zoom and could not reach the last pages. It is now the converter's own
 * widgetToImage(widgetCenterPoint()). The mapping from a sane centre to a page -- everything
 * below -- did not change, and is what these numbers are about.
 *
 * That leaves two candidate causes and this test distinguishes them with numbers, because the fix
 * is different for each:
 *
 *   (a) the centre of the viewport is in the GAP between two pages (or in an empty slot at the end
 *       of the notebook), so pageAtDocumentPoint() answers -1 and the candidate is -1 for ever; or
 *   (b) near a boundary the answer flips between a page and -1 from one tick to the next, so
 *       m_candidateSince is reset on every tick and the 450 ms is never reached.
 *
 * The state machine below is a replica of checkScrollFollow() with the navigator's own constants,
 * because the real one cannot be called from here: it is a private member of a singleton tied to
 * KisPart and to a live canvas. The replica is line-referenced so it can be checked against the
 * source; what it cannot cover is stated in docs/verify/MULTIPAGE-RENDER-TEST.md.
 */
namespace {

/// PdfPageNavigator.cpp:117-121.
constexpr qint64 TurnCooldownMs = 700;
constexpr qint64 SettleMs = 450;

/// PdfPageNavigator.cpp:710: m_scrollWatch->start(150).
constexpr qint64 TickMs = 150;

/// How the centre of the viewport is mapped to a page: exactly as the navigator does today, or
/// through PdfStripLayout::nearestPage().
enum class Rule { Exact, Nearest };

/// The exact test of PdfPageNavigator::pageAtDocumentPoint()'s strip branch and of
/// PdfStripLayout::pageAt().
int exactPageAt(const QList<int> &pages, const QList<QRect> &rects, const QPoint &point)
{
    for (int slot = 0; slot < pages.size() && slot < rects.size(); ++slot) {
        if (pages.at(slot) >= 0 && rects.at(slot).contains(point)) {
            return pages.at(slot);
        }
    }
    return -1;
}

struct Follow {
    int candidate = -1;
    qint64 candidateSince = 0;
    qint64 lastTurn = 0;
    int page = -1;          ///< the page that is open, the navigator's m_index
    int turns = 0;
    int resets = 0;         ///< how often the candidate changed and the settle timer was reset
    qint64 maxAge = 0;
    int ticksWithNoPage = 0;
    QStringList trace;
};

/// PdfPageNavigator::checkScrollFollow(), :176-210, reduced to the state that decides a turn.
/// The nearest rule is given the candidate as its preferred page, which is the navigator's
/// m_candidatePage: hysteresis is about not letting a new page take over until it is clearly
/// nearer, not about pinning the page that is open.
void tick(Follow &state, const QList<int> &pages, const QList<QRect> &rects,
          const QPoint &centre, qint64 now, Rule rule, bool trace, qreal hysteresis = 0.0)
{
    const int pageUnderCentre = rule == Rule::Nearest
        ? PdfStripLayout::nearestPage(pages, rects, QPointF(centre),
                                      std::numeric_limits<qreal>::max(),
                                      state.candidate, hysteresis)
        : exactPageAt(pages, rects, centre);

    if (pageUnderCentre < 0) {
        ++state.ticksWithNoPage;
    }

    /// :196-199 -- any change resets the settle timer. A value that flickers on every tick never
    /// grows an age, which is the second candidate cause.
    if (pageUnderCentre != state.candidate) {
        if (trace) {
            state.trace.append(QStringLiteral("t=%1ms centre=%2,%3 pageUnderCentre=%4 "
                                              "candidate %5 -> %6 (age reset)")
                                   .arg(now).arg(centre.x()).arg(centre.y())
                                   .arg(pageUnderCentre)
                                   .arg(state.candidate).arg(pageUnderCentre));
        }
        state.candidate = pageUnderCentre;
        state.candidateSince = now;
        ++state.resets;
        return;
    }

    const qint64 age = now - state.candidateSince;
    state.maxAge = qMax(state.maxAge, age);

    /// :183-189.
    if (pageUnderCentre < 0 || pageUnderCentre == state.page) {
        if (trace) {
            state.trace.append(QStringLiteral("t=%1ms centre=%2,%3 pageUnderCentre=%4 age=%5ms "
                                              "(no turn: %6)")
                                   .arg(now).arg(centre.x()).arg(centre.y())
                                   .arg(pageUnderCentre).arg(age)
                                   .arg(pageUnderCentre < 0 ? QStringLiteral("no page")
                                                            : QStringLiteral("already open")));
        }
        return;
    }

    if (trace) {
        state.trace.append(QStringLiteral("t=%1ms centre=%2,%3 pageUnderCentre=%4 age=%5ms")
                               .arg(now).arg(centre.x()).arg(centre.y())
                               .arg(pageUnderCentre).arg(age));
    }

    if (now - state.candidateSince < SettleMs || now - state.lastTurn < TurnCooldownMs) {
        return;
    }

    /// :191-192.
    state.lastTurn = now;
    state.candidate = -1;
    state.page = pageUnderCentre;
    ++state.turns;
}

/// A three-slot strip over the same mixed sizes as the mr-strip-3page.pdf fixture: A4 portrait,
/// A5 landscape, a 300x300 square, at 200 dpi as the plugin builds it.
PdfSessionManifest stripManifest(int pages = 3)
{
    PdfSessionManifest manifest;
    manifest.sourceFile = QStringLiteral("mr-strip-3page.pdf");
    manifest.sourceSha256 = QByteArrayLiteral("0123456789abcdef");
    manifest.sourceByteSize = 1;

    const QSizeF sizes[] = { QSizeF(595, 842), QSizeF(595, 420), QSizeF(300, 300) };
    for (int i = 0; i < pages; ++i) {
        PdfPageRecord page;
        page.index = i;
        page.sizePt = sizes[i];
        page.rotation = 0;
        page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
        page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
        manifest.pages.append(page);
    }
    return manifest;
}

} // namespace

class PdfStripScrollFollowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testTheGapIsWhereTheCentreStopsBeingOnAPage();
    void testTheExactRuleNeverSettlesInTheGap();
    void testTheNearestRuleSettlesAndTurns();
    void testWobbleInsideTheGap();
    void testWobbleAcrossTheMidlineNeedsHysteresis();
    void testBeyondTheStripStillReportsNoPage();
};

void PdfStripScrollFollowTest::testTheGapIsWhereTheCentreStopsBeingOnAPage()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(stripManifest(), 1, 3, 200.0);
    QVERIFY(layout.isValid());

    const QList<PdfStripLayout::Slot> slots = layout.slots();
    QCOMPARE(slots.size(), 3);

    for (int i = 0; i + 1 < slots.size(); ++i) {
        const QRect upper = slots.at(i).rect;
        const QRect lower = slots.at(i + 1).rect;
        const int gap = lower.top() - upper.bottom() - 1;

        QVERIFY2(gap > 0, "the slots are supposed to be separated by a gap");

        /// Every row of the gap answers -1 to the exact rule the navigator uses.
        int rowsNoPage = 0;
        for (int y = upper.bottom() + 1; y < lower.top(); ++y) {
            if (exactPageAt({ slots.at(i).page, slots.at(i + 1).page }, { upper, lower },
                            QPoint(upper.center().x(), y)) < 0) {
                ++rowsNoPage;
            }
        }
        QCOMPARE(rowsNoPage, gap);

        /// And the nearest rule owns the whole of it: no row of the gap is on no page.
        int rowsOwned = 0;
        for (int y = upper.bottom() + 1; y < lower.top(); ++y) {
            if (layout.nearestPage({ slots.at(i).page, slots.at(i + 1).page }, { upper, lower },
                                   QPointF(upper.center().x(), y),
                                   std::numeric_limits<qreal>::max()) >= 0) {
                ++rowsOwned;
            }
        }
        QCOMPARE(rowsOwned, gap);

        qInfo("gap between page %d and page %d: %d document px; exact rule reports no page for all "
              "%d of them, nearest rule reports a page for all %d",
              slots.at(i).page + 1, slots.at(i + 1).page + 1, gap, rowsNoPage, rowsOwned);
    }
}

void PdfStripScrollFollowTest::testTheExactRuleNeverSettlesInTheGap()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(stripManifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    const QList<int> pages = { slots.at(1).page, slots.at(2).page };
    const QList<QRect> rects = { slots.at(1).rect, slots.at(2).rect };

    /// The view is on page 2 and the user scrolls until the centre of the viewport sits in the gap
    /// just past page 2, 60 per cent of the way to page 3 -- nearer page 3, but still in the gap.
    const int gapTop = slots.at(1).rect.bottom() + 1;
    const int gapBottom = slots.at(2).rect.top();
    const int restY = gapTop + (gapBottom - gapTop) * 6 / 10;
    const QPoint rest(slots.at(1).rect.center().x(), restY);

    Follow state;
    state.page = 1;   ///< page 2 (index 1) is open
    for (int i = 0; i < 20; ++i) {
        tick(state, pages, rects, rest, qint64(i) * TickMs, Rule::Exact, true);
    }

    qInfo("CAUSE (a), exact rule, centre parked in the gap (rows %d..%d, resting at %d), 20 ticks:",
          gapTop, gapBottom, restY);
    for (const QString &line : state.trace.mid(0, 6)) {
        qInfo("  %s", qPrintable(line));
    }

    QCOMPARE(state.ticksWithNoPage, 20);
    QCOMPARE(state.turns, 0);
    QVERIFY2(state.candidate == -1, "the candidate is -1 for ever, so the settle timer never arms");
}

void PdfStripScrollFollowTest::testTheNearestRuleSettlesAndTurns()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(stripManifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    const QList<int> pages = { slots.at(1).page, slots.at(2).page };
    const QList<QRect> rects = { slots.at(1).rect, slots.at(2).rect };

    const int gapTop = slots.at(1).rect.bottom() + 1;
    const int gapBottom = slots.at(2).rect.top();
    const int restY = gapTop + (gapBottom - gapTop) * 6 / 10;
    const QPoint rest(slots.at(1).rect.center().x(), restY);

    Follow state;
    state.page = 1;
    state.lastTurn = -TurnCooldownMs;   ///< the cooldown has long since passed

    for (int i = 0; i < 20; ++i) {
        tick(state, pages, rects, rest, qint64(i) * TickMs, Rule::Nearest, true);
    }

    qInfo("same centre, nearest rule: %d turn(s) in 3000 ms of ticks:",
          state.turns);
    for (const QString &line : state.trace.mid(0, 6)) {
        qInfo("  %s", qPrintable(line));
    }

    QCOMPARE(state.ticksWithNoPage, 0);
    QCOMPARE(state.turns, 1);
    QCOMPARE(state.page, 2);
}

void PdfStripScrollFollowTest::testWobbleInsideTheGap()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(stripManifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    const QList<int> pages = { slots.at(1).page, slots.at(2).page };
    const QList<QRect> rects = { slots.at(1).rect, slots.at(2).rect };

    /// The centre rests well past the halfway point of the gap and jitters by a pixel -- a
    /// trackpad, a stylus, a canvas that rounds its scroll position.
    const int gapTop = slots.at(1).rect.bottom() + 1;
    const int gapBottom = slots.at(2).rect.top();
    const int restY = gapTop + (gapBottom - gapTop) * 7 / 10;

    auto run = [&](Rule rule) {
        Follow state;
        state.page = 1;
        state.lastTurn = -TurnCooldownMs;
        for (int i = 0; i < 20; ++i) {
            const int y = (i % 2 == 0) ? restY : restY + 1;
            tick(state, pages, rects, QPoint(slots.at(1).rect.center().x(), y),
                 qint64(i) * TickMs, rule, false);
        }
        return state;
    };

    const Follow exact = run(Rule::Exact);
    const Follow nearest = run(Rule::Nearest);

    qInfo("wobble inside the gap (y=%d/%d), 20 ticks: exact rule candidate %d, resets %d, turns %d, "
          "no-page ticks %d; nearest rule candidate %d, resets %d, turns %d, no-page ticks %d",
          restY, restY + 1, exact.candidate, exact.resets, exact.turns, exact.ticksWithNoPage,
          nearest.candidate, nearest.resets, nearest.turns, nearest.ticksWithNoPage);

    /// The exact rule answers -1 to both positions, so the candidate never becomes a page.
    QCOMPARE(exact.ticksWithNoPage, 20);
    QCOMPARE(exact.turns, 0);

    /// The nearest rule gives page 3 for both, so the age grows past 450 ms and the turn happens.
    QCOMPARE(nearest.ticksWithNoPage, 0);
    QCOMPARE(nearest.turns, 1);
    QCOMPARE(nearest.page, 2);
}

void PdfStripScrollFollowTest::testWobbleAcrossTheMidlineNeedsHysteresis()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(stripManifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    const QList<int> pages = { slots.at(1).page, slots.at(2).page };
    const QList<QRect> rects = { slots.at(1).rect, slots.at(2).rect };

    /// The centre rests exactly where the two pages are equally near and jitters by two pixels, so
    /// the nearest page alternates between page 2 and page 3 from one tick to the next.
    const int gapTop = slots.at(1).rect.bottom() + 1;
    const int gapBottom = slots.at(2).rect.top();
    const int mid = gapTop + (gapBottom - gapTop) / 2;

    auto run = [&](qreal hysteresis) {
        Follow state;
        state.page = 1;
        state.lastTurn = -TurnCooldownMs;
        for (int i = 0; i < 20; ++i) {
            const int y = (i % 2 == 0) ? mid - 2 : mid + 2;
            tick(state, pages, rects, QPoint(slots.at(1).rect.center().x(), y),
                 qint64(i) * TickMs, Rule::Nearest, false, hysteresis);
        }
        return state;
    };

    const Follow plain = run(0.0);
    const Follow damped = run(6.0);

    qInfo("wobble across the midline (y=%d/%d), 20 ticks: nearest without hysteresis candidate %d, "
          "resets %d, max age %lld ms, turns %d; with 6 px hysteresis candidate %d, resets %d, "
          "max age %lld ms, turns %d",
          mid - 2, mid + 2, plain.candidate, plain.resets, plain.maxAge, plain.turns,
          damped.candidate, damped.resets, damped.maxAge, damped.turns);

    /// Without hysteresis the candidate flips between the two pages, every tick resets the settle
    /// timer, and 450 ms is never reached: the same failure as cause (b), just between two pages
    /// instead of between a page and -1.
    QVERIFY2(plain.resets >= 19, "the candidate is supposed to flip every tick here");
    QCOMPARE(plain.turns, 0);

    /// With hysteresis the candidate is decided once and then holds, so the age grows and a switch
    /// follows as soon as the user moves decisively past the band.
    QVERIFY2(damped.resets <= 2, "hysteresis is supposed to stop the flipping");
    QVERIFY2(damped.maxAge >= SettleMs, "and to let the settle timer finally reach its threshold");
}

void PdfStripScrollFollowTest::testBeyondTheStripStillReportsNoPage()
{
    /// Whether "the centre is in an empty slot at the end of the notebook" can be a real cause of
    /// the bug: it cannot, through the path the plugin uses. PdfStripLayout::forWindow() clamps the
    /// scope to the page count (scope = qMin(scope, manifest.pages.size())), so a two-page notebook
    /// asked for three slots gets two and every slot holds a page; the Slot::page == -1 branch is
    /// not reachable that way today. What still has to be right is the primitive: a caller that
    /// bounds the distance it will accept must be told when a point is beyond the strip.
    const PdfStripLayout layout = PdfStripLayout::forWindow(stripManifest(2), 0, 3, 200.0);
    QVERIFY(layout.isValid());
    QCOMPARE(layout.slots().size(), 2);
    for (const PdfStripLayout::Slot &slot : layout.slots()) {
        QVERIFY(slot.page >= 0);
    }

    const QList<int> pages = { 0, 1 };
    const QList<QRect> rects = { QRect(0, 0, 100, 100), QRect(0, 200, 100, 100) };

    /// 160 is nearer the lower page (40 px) than the upper one (60 px); 150 is the tie and resolves
    /// to the first slot.
    QCOMPARE(layout.nearestPage(pages, rects, QPointF(50, 160),
                                std::numeric_limits<qreal>::max()), 1);
    QCOMPARE(layout.nearestPage(pages, rects, QPointF(50, 150),
                                std::numeric_limits<qreal>::max()), 0);

    /// Far outside, the caller that allows only 50 px is refused; the one that allows anything is
    /// answered. The band between the two pages is refused by the same bound.
    QCOMPARE(layout.nearestPage(pages, rects, QPointF(50, 900), 50.0), -1);
    QCOMPARE(layout.nearestPage(pages, rects, QPointF(50, 900),
                                std::numeric_limits<qreal>::max()), 1);
    QCOMPARE(layout.nearestPage(pages, rects, QPointF(50, 150), 10.0), -1);

    qInfo("nearestPage: a 50 px bound refuses the gap and a point 600 px past the last page; with "
          "no bound both resolve, the tie in the gap goes to the first slot, and a real layout has "
          "no page-less slot to worry about");
}

QTEST_MAIN(PdfStripScrollFollowTest)
#include "PdfStripScrollFollowTest.moc"
