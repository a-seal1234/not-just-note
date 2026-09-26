/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfStripLayout.h"

#include <QtMath>
#include <QtTest>

/**
 * The geometry design B rests on. The load bearing property is that the image size does not change
 * as the window moves: rolling the window repaints one slot rather than rebuilding the document,
 * and that only works if the image stays the same size.
 */
class PdfStripLayoutTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWindowIsCentred();
    void testWindowClampsAtTheEnds();
    void testAWindowIsSizedForThePagesInIt();
    void testSlotsHoldWholePages();
    void testPageAtDistinguishesGapsFromPages();
    void testEvenScopeIsMadeOdd();
    void testScopeLargerThanTheNotebook();
    void testRefusesNonsense();
    void testCellBandsCoverTheirPagesAndTheImage();
    void testAngledPageFillsItsBoundingBox();
    void testTheTiltedPagesWindowIsTheBiggerOne();
    void testRightAnglesAreUnchanged();
    void testAWindowMoveLeavesBandsOutsideTheNewCells();

private:
    /// Three pages: A4, then A5 rotated, then a small square. Mixed on purpose.
    PdfSessionManifest manifest() const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;

        const QSizeF sizes[] = { QSizeF(595, 842), QSizeF(420, 595), QSizeF(300, 300) };
        for (int i = 0; i < 3; ++i) {
            PdfPageRecord page;
            page.index = i;
            page.sizePt = sizes[i];
            page.rotation = i == 1 ? 90 : 0;
            page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
            page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
            manifest.pages.append(page);
        }
        return manifest;
    }

    /**
     * Three pages, the middle one an A4 sheet set down at 37 degrees -- the angle the model
     * contract names. Its bounding box is over a third taller than the sheet, and that bigger box
     * is what the slot has to make room for.
     */
    PdfSessionManifest angledManifest() const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;

        const QSizeF sizes[] = { QSizeF(420, 595), QSizeF(595, 842), QSizeF(300, 300) };
        const int turns[] = { 0, 37, 0 };
        for (int i = 0; i < 3; ++i) {
            PdfPageRecord page;
            page.index = i;
            page.sizePt = sizes[i];
            page.extraRotation = turns[i];
            page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
            page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
            manifest.pages.append(page);
        }
        return manifest;
    }

    /// A notebook of same-size letter pages, one of them set down at an angle: the user's
    /// notebook, whose windows are what a move between the page holding the big box and one that
    /// does not has to survive.
    static PdfSessionManifest letterBook(int count, int rotatedPage, int degrees)
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;

        for (int i = 0; i < count; ++i) {
            PdfPageRecord page;
            page.index = i;
            page.sizePt = QSizeF(612, 792);
            page.extraRotation = i == rotatedPage ? degrees : 0;
            page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
            page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
            manifest.pages.append(page);
        }
        return manifest;
    }

    /// The reported notebook's shape, smaller: A4 pages, then the wide slides a merged deck brought
    /// in, then A4-shaped pages again, so a window can hold one kind or the other. Two sizes rather
    /// than the reported five, because two are enough to show which one a window is sized for.
    static PdfSessionManifest mixedDeckBook()
    {
        const QSizeF sizes[] = { QSizeF(612, 792),   QSizeF(612, 792),   QSizeF(612, 792),
                                 QSizeF(1672, 941),  QSizeF(1672, 941),  QSizeF(1672, 941),
                                 QSizeF(1536, 1024), QSizeF(595, 842),   QSizeF(595, 842),
                                 QSizeF(595, 842) };

        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;

        for (int i = 0; i < 10; ++i) {
            PdfPageRecord page;
            page.index = i;
            page.sizePt = sizes[i];
            page.kraFile = QStringLiteral("pages/p%1.kra").arg(i + 1, 4, 10, QLatin1Char('0'));
            page.thumbFile = QStringLiteral("thumbs/p%1.png").arg(i + 1, 4, 10, QLatin1Char('0'));
            manifest.pages.append(page);
        }
        return manifest;
    }

    /// One page of \a size turned by \a turn, alone: the plainest possible question.
    static PdfSessionManifest onePageManifest(const QSizeF &size, int turn)
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;

        PdfPageRecord page;
        page.index = 0;
        page.sizePt = size;
        page.extraRotation = turn;
        page.kraFile = QStringLiteral("pages/p0001.kra");
        page.thumbFile = QStringLiteral("thumbs/p0001.png");
        manifest.pages.append(page);
        return manifest;
    }

    /// The bounding box a \a size sheet occupies once it is turned by \a degrees, in points.
    ///
    /// Written out here from the geometry the model contract describes rather than taken from
    /// PdfPageRecord::displaySizePt(), so the layout is measured against the contract and not
    /// against itself.
    static QSizeF turnedBoxPt(const QSizeF &size, int degrees)
    {
        const int turn = ((degrees % 360) + 360) % 360;
        if (turn == 0 || turn == 180) {
            return size;
        }
        if (turn == 90 || turn == 270) {
            return QSizeF(size.height(), size.width());
        }
        const qreal radians = qDegreesToRadians(qreal(turn));
        const qreal cosine = qAbs(qCos(radians));
        const qreal sine = qAbs(qSin(radians));
        return QSizeF(size.width() * cosine + size.height() * sine,
                      size.width() * sine + size.height() * cosine);
    }
};

void PdfStripLayoutTest::testWindowIsCentred()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);

    QVERIFY(layout.isValid());
    QCOMPARE(layout.activePage(), 1);
    QCOMPARE(layout.activeSlot(), 1);

    const QList<PdfStripLayout::Slot> slots = layout.slots();
    QCOMPARE(slots.size(), 3);
    QCOMPARE(slots.at(0).page, 0);
    QCOMPARE(slots.at(1).page, 1);
    QCOMPARE(slots.at(2).page, 2);
}

void PdfStripLayoutTest::testWindowClampsAtTheEnds()
{
    /// With three pages and a scope of three there is only one possible window.
    const PdfStripLayout first = PdfStripLayout::forWindow(manifest(), 0, 3, 200.0);
    QCOMPARE(first.slots().at(first.activeSlot()).page, 0);

    const PdfStripLayout last = PdfStripLayout::forWindow(manifest(), 2, 3, 200.0);
    QCOMPARE(last.slots().at(last.activeSlot()).page, 2);
}

/**
 * A window is the size the pages IN IT need.
 *
 * This replaces a test that asserted the opposite: every window of one scope used to be the same
 * size -- the widest page of the whole notebook by the tallest window of the whole notebook -- so
 * that a window move never had to resize the document. The reported notebook is 38 pages of five
 * sizes, one of them a merged deck 1672 pt wide, and a window of A4 pages was a 4644x14432
 * document: 1.6 GiB by Krita's own report, 2.04 GB of PSS measured, and climbing while scrolling
 * because every roll repainted every slot into those layers. The roll resizes now, so the size is
 * the window's own.
 */
void PdfStripLayoutTest::testAWindowIsSizedForThePagesInIt()
{
    const PdfSessionManifest manifest = mixedDeckBook();
    const int scope = 3;
    const int gap = 112;

    const PdfStripLayout a4 = PdfStripLayout::forWindow(manifest, 1, scope, 200.0);
    const PdfStripLayout wide = PdfStripLayout::forWindow(manifest, 4, scope, 200.0);
    QVERIFY(a4.isValid());
    QVERIFY(wide.isValid());

    /// One window of A4 pages and one of the wide slides, and neither holds the other's kind.
    QCOMPARE(a4.slots().at(0).page, 0);
    QVERIFY(a4.slots().last().page < 3);
    QCOMPARE(wide.slots().at(0).page, 3);
    QCOMPARE(wide.slots().last().page, 5);

    const int a4Width = qRound(612.0 * 200.0 / 72.0);
    const int a4Height = qRound(792.0 * 200.0 / 72.0);
    const int wideWidth = qRound(1672.0 * 200.0 / 72.0);
    const int wideHeight = qRound(941.0 * 200.0 / 72.0);

    QCOMPARE(a4.imageSize().width(), a4Width);
    QCOMPARE(wide.imageSize().width(), wideWidth);
    QVERIFY2(a4.imageSize().width() < wide.imageSize().width(),
             "the A4 window is still paying for the widest page in the notebook");

    /// The height is that window's own packed height plus the one gap of slack the last cell
    /// absorbs, so there is no desk beyond the last page's band: the cells still end the image.
    QCOMPARE(a4.imageSize().height(), scope * (a4Height + gap) + gap);
    QCOMPARE(wide.imageSize().height(), scope * (wideHeight + gap) + gap);
    QCOMPARE(a4.slots().last().cell.bottom() + 1, a4.imageSize().height());
    QCOMPARE(wide.slots().last().cell.bottom() + 1, wide.imageSize().height());

    /// And the pages are centred in their own column, which is now the column this window needs.
    QCOMPARE(a4.slots().at(0).rect.x(), 0);
    QCOMPARE(a4.slots().at(0).rect.width(), a4Width);
    QCOMPARE(wide.slots().at(0).rect.x(), 0);
    QCOMPARE(wide.slots().at(0).rect.width(), wideWidth);

    /// What the old rule cost, said as a number: the A4 window was as wide as the slide and as tall
    /// as the window holding the tallest page (1536x1024 pt), which is none of the pages in it.
    QVERIFY2(a4.imageSize().height() < scope * (qRound(1024.0 * 200.0 / 72.0) + gap) + gap,
             "the A4 window is still as tall as the tallest window in the notebook");
}

void PdfStripLayoutTest::testSlotsHoldWholePages()
{
    /// The manifest is held rather than rebuilt per slot: pages.at() hands back a reference INTO
    /// it, and binding that reference to a temporary's element left it dangling -- the test read a
    /// page that had already been destroyed.
    const PdfSessionManifest notebook = manifest();
    const PdfStripLayout layout = PdfStripLayout::forWindow(notebook, 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    for (const PdfStripLayout::Slot &slot : slots) {
        QVERIFY(slot.page >= 0);

        /// The slot's rect is the page at its own size, inside the strip. extraRotation is 0 on
        /// every page here, so displaySizePt() is sizePt exactly.
        const PdfPageRecord &page = notebook.pages.at(slot.page);
        QCOMPARE(slot.rect.width(), qRound(page.sizePt.width() * 200.0 / 72.0));
        QCOMPARE(slot.rect.height(), qRound(page.sizePt.height() * 200.0 / 72.0));

        /// And it is inside the image.
        QVERIFY(QRect(QPoint(0, 0), layout.imageSize()).contains(slot.rect));
    }

    /// The slots do not overlap each other.
    for (int i = 1; i < slots.size(); ++i) {
        QVERIFY(slots.at(i).rect.top() > slots.at(i - 1).rect.bottom());
    }
}

void PdfStripLayoutTest::testPageAtDistinguishesGapsFromPages()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();

    QCOMPARE(layout.pageAt(slots.at(0).rect.center()), 0);
    QCOMPARE(layout.pageAt(slots.at(1).rect.center()), 1);
    QCOMPARE(layout.pageAt(slots.at(2).rect.center()), 2);

    /// Below the last page, in the gap between slots, there is no page.
    const QPoint inGap(slots.at(0).rect.center().x(), slots.at(0).rect.bottom() + 4);
    QCOMPARE(layout.pageAt(inGap), -1);
}

void PdfStripLayoutTest::testEvenScopeIsMadeOdd()
{
    /// An even scope would put the active page off centre, so it is rounded up.
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 3 - 1, 2, 200.0);
    QVERIFY(layout.isValid());
    QCOMPARE(layout.slots().size(), 3);
}

void PdfStripLayoutTest::testScopeLargerThanTheNotebook()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 9, 200.0);
    QVERIFY(layout.isValid());
    QCOMPARE(layout.slots().size(), 3);
    QCOMPARE(layout.activeSlot(), 1);
}

void PdfStripLayoutTest::testRefusesNonsense()
{
    QVERIFY(!PdfStripLayout::forWindow(PdfSessionManifest(), 0, 3, 200.0).isValid());
    QVERIFY(!PdfStripLayout::forWindow(manifest(), 9, 3, 200.0).isValid());
    QVERIFY(!PdfStripLayout::forWindow(manifest(), 0, 3, 0.0).isValid());
}

/**
 * The bands the roll wipes and repaints: each contains its page, and together they tile the image.
 *
 * A band that did not contain its page would leave part of that page unpainted when the roll
 * repaints from the artifact; bands that left a row uncovered would leave stale pixels from the
 * window before. Both are checked on the mixed-size manifest, where the pages do not fill the
 * height the tallest window needs.
 */
void PdfStripLayoutTest::testCellBandsCoverTheirPagesAndTheImage()
{
    const PdfStripLayout layout = PdfStripLayout::forWindow(manifest(), 1, 3, 200.0);
    const QList<PdfStripLayout::Slot> slots = layout.slots();
    QCOMPARE(slots.size(), 3);

    for (const PdfStripLayout::Slot &slot : slots) {
        QVERIFY2(slot.cell.contains(slot.rect), "the band must contain the page it belongs to");
        QCOMPARE(slot.cell.width(), layout.imageSize().width());
    }

    /// They meet, with no gap and no overlap, from the top edge to the bottom.
    QVERIFY2(slots.first().cell.top() <= 0, "the first band reaches the top edge");
    for (int i = 1; i < slots.size(); ++i) {
        QCOMPARE(slots.at(i).cell.top(), slots.at(i - 1).cell.bottom() + 1);
    }
    QCOMPARE(slots.last().cell.bottom() + 1, layout.imageSize().height());

    qInfo("cells %d,%d %dx%d / %d,%d %dx%d / %d,%d %dx%d tile a %dx%d image",
          slots.at(0).cell.x(), slots.at(0).cell.y(),
          slots.at(0).cell.width(), slots.at(0).cell.height(),
          slots.at(1).cell.x(), slots.at(1).cell.y(),
          slots.at(1).cell.width(), slots.at(1).cell.height(),
          slots.at(2).cell.x(), slots.at(2).cell.y(),
          slots.at(2).cell.width(), slots.at(2).cell.height(),
          layout.imageSize().width(), layout.imageSize().height());
}

/**
 * A page set down at an angle occupies its bounding box, and the strip has to make room for THAT
 * rectangle rather than for the unturned sheet. The box is the whole cost of a free angle: the
 * sheet is shown inside the rectangle that holds it once it is turned, so it is bigger, and a slot
 * sized from sizePt would put the corners of the page outside the image.
 */
void PdfStripLayoutTest::testAngledPageFillsItsBoundingBox()
{
    const QSizeF sheet(595, 842);
    const QSizeF box = turnedBoxPt(sheet, 37);

    /// The premise, first: the box really is bigger than the sheet, and bigger by a lot rather
    /// than by a rounding error.
    QVERIFY(box.width() > sheet.width());
    QVERIFY(box.height() > sheet.height());

    const qreal dpi = 200.0;
    const PdfStripLayout layout = PdfStripLayout::forWindow(angledManifest(), 1, 3, dpi);
    QVERIFY(layout.isValid());

    const QList<PdfStripLayout::Slot> slots = layout.slots();
    QCOMPARE(slots.size(), 3);
    QCOMPARE(layout.activeSlot(), 1);
    QCOMPARE(slots.at(1).page, 1);

    /// The slot is the box at the renderer's pixels, not the unturned sheet's.
    const QSize expected(qRound(box.width() * dpi / 72.0), qRound(box.height() * dpi / 72.0));
    QCOMPARE(slots.at(1).rect.size(), expected);

    /// What sizePt alone would have made room for is over a third shorter; that this is not it is
    /// the whole point of the change.
    QVERIFY(slots.at(1).rect.width() > qRound(sheet.width() * dpi / 72.0));
    QVERIFY(slots.at(1).rect.height() > qRound(sheet.height() * dpi / 72.0));

    /// Big enough for the box itself: the slot may not be narrower than the rectangle the turned
    /// sheet measures at the outward rounding.
    QVERIFY2(slots.at(1).rect.width() >= qCeil(box.width() * dpi / 72.0),
             "the slot is narrower than the box the turned page measures");
    QVERIFY2(slots.at(1).rect.height() >= qCeil(box.height() * dpi / 72.0),
             "the slot is shorter than the box the turned page measures");

    /// And the strip is wide enough to hold that slot, so the angled page is not clipped.
    QVERIFY(layout.imageSize().width() >= slots.at(1).rect.width());
    QVERIFY(QRect(QPoint(0, 0), layout.imageSize()).contains(slots.at(1).rect));

    /// The angled page is still found where its box is -- exact under the cursor, and by the
    /// nearest-page rule the strip's settle timer rests on.
    QCOMPARE(layout.pageAt(slots.at(1).rect.center()), 1);

    const QList<int> pages = { 0, 1, 2 };
    const QList<QRect> rects = { slots.at(0).rect, slots.at(1).rect, slots.at(2).rect };
    QCOMPARE(PdfStripLayout::nearestPage(pages, rects, QPointF(slots.at(1).rect.center()), 112.0), 1);
}

/**
 * The window that holds the tilted page is the big one, and the window that does not is A4.
 *
 * This replaces a test that asserted the opposite -- that the image stayed the same size whatever
 * page was active, so the angled page's box was paid for by every window. That is the rule the
 * reported 4644x14432 document came from, and the roll resizes now, so a window is sized for what
 * it holds: A4 where the pages are A4, the tilted box where the tilted page is.
 */
void PdfStripLayoutTest::testTheTiltedPagesWindowIsTheBiggerOne()
{
    const PdfSessionManifest manifest = letterBook(18, 0, 221);
    const int scope = 5;
    const int gap = 112;

    const PdfStripLayout holding = PdfStripLayout::forWindow(manifest, 0, scope, 200.0);
    const PdfStripLayout a4 = PdfStripLayout::forWindow(manifest, 8, scope, 200.0);
    QVERIFY(holding.isValid());
    QVERIFY(a4.isValid());

    QCOMPARE(holding.slots().at(0).page, 0);
    QCOMPARE(a4.slots().at(0).page, 6);

    /// The window that does not hold it is A4 and only A4.
    QCOMPARE(a4.imageSize().width(), qRound(612.0 * 200.0 / 72.0));
    QCOMPARE(a4.imageSize().height(), scope * (qRound(792.0 * 200.0 / 72.0) + gap) + gap);

    /// The one that does is bigger in both directions, because the tilted sheet's box is bigger.
    QVERIFY(holding.imageSize().width() > a4.imageSize().width());
    QVERIFY(holding.imageSize().height() > a4.imageSize().height());
    QVERIFY(QRect(QPoint(0, 0), holding.imageSize())
                .contains(holding.slots().at(0).rect));
}

/**
 * 0, 90, 180 and 270 are unchanged: the box is the sheet, or the sheet with its sides exactly
 * swapped, so those pages take the same pixels of the strip as they did before a free angle was
 * allowed. A regression here would be a notebook of upright pages that no longer renders as it
 * was.
 */
void PdfStripLayoutTest::testRightAnglesAreUnchanged()
{
    const QSizeF sheet(595, 842);
    const qreal dpi = 200.0;
    const QSize upright(qRound(sheet.width() * dpi / 72.0), qRound(sheet.height() * dpi / 72.0));

    for (int turn : { 0, 180 }) {
        const PdfStripLayout layout = PdfStripLayout::forWindow(onePageManifest(sheet, turn), 0, 1, dpi);
        QVERIFY(layout.isValid());
        QCOMPARE(layout.slots().at(0).rect.size(), upright);
    }

    for (int turn : { 90, 270 }) {
        const PdfStripLayout layout = PdfStripLayout::forWindow(onePageManifest(sheet, turn), 0, 1, dpi);
        QVERIFY(layout.isValid());
        QCOMPARE(layout.slots().at(0).rect.size(), QSize(upright.height(), upright.width()));
    }

    /// And the box a right angle reports is the sheet or its exact transpose, which is what keeps
    /// the equality above exact rather than approximate.
    QCOMPARE(turnedBoxPt(sheet, 0), sheet);
    QCOMPARE(turnedBoxPt(sheet, 180), sheet);
    QCOMPARE(turnedBoxPt(sheet, 90), QSizeF(sheet.height(), sheet.width()));
    QCOMPARE(turnedBoxPt(sheet, 270), QSizeF(sheet.height(), sheet.width()));
}

/**
 * The precondition of the stale-strip bug, as geometry: moving the window from the one holding a
 * freely rotated page to one that does not hold it, and back, leaves the top of every slot's old
 * band OUTSIDE the band it owns afterwards.
 *
 * This is why the roll has to clear the paper over the old band as well as the new one. The two
 * windows are now DIFFERENT sizes -- the one holding the tilted page is the bigger one, and the
 * roll resizes the document between them -- so the bands move as well as the pages: the top of a
 * slot's old band can end up outside the band it owns afterwards, and that band has to be cleared.
 */
void PdfStripLayoutTest::testAWindowMoveLeavesBandsOutsideTheNewCells()
{
    const PdfSessionManifest manifest = letterBook(18, 0, 221);
    const int scope = 5;
    const PdfStripLayout holding = PdfStripLayout::forWindow(manifest, 0, scope, 200.0);
    const PdfStripLayout away = PdfStripLayout::forWindow(manifest, 8, scope, 200.0);

    QVERIFY(holding.isValid());
    QVERIFY(away.isValid());

    /// The two windows are the user's: the first holds the tilted page, the second does not. They
    /// need different sizes, which is the resize the roll now does between them.
    QCOMPARE(holding.slots().at(0).page, 0);
    QCOMPARE(away.slots().at(0).page, 6);
    QVERIFY(holding.slots().at(0).rect.height() > away.slots().at(0).rect.height());
    QVERIFY2(holding.imageSize().width() > away.imageSize().width()
                 && holding.imageSize().height() > away.imageSize().height(),
             "the two windows have to need different sizes or this test says nothing about a resize");

    /// And the bands really do not line up: every slot but the first starts lower in the window
    /// holding the tilted page, so the top of its previous band sticks out above the new one.
    int shifted = 0;
    for (int i = 0; i < holding.slots().size(); ++i) {
        const QRect nowOwned = holding.slots().at(i).cell;
        const QRect wasHeld = away.slots().at(i).cell;
        if (nowOwned.top() > wasHeld.top()) {
            ++shifted;
            /// The band left behind is a real band, not a rounding: the tilted page is over two
            /// hundred rows taller than the sheet it replaces.
            QVERIFY(nowOwned.top() - wasHeld.top() > 100);
        }
    }
    QCOMPARE(shifted, holding.slots().size() - 1);

    /// The cells of the window that IS up still tile the image, which is the other half of the
    /// story: a content layer is wiped over every one of them, so the whole content layer is
    /// cleared each roll, and only the paper needs its old band.
    QVERIFY(holding.slots().first().cell.top() <= 0);
    for (int i = 1; i < holding.slots().size(); ++i) {
        QCOMPARE(holding.slots().at(i).cell.top(), holding.slots().at(i - 1).cell.bottom() + 1);
    }
    QCOMPARE(holding.slots().last().cell.bottom() + 1, holding.imageSize().height());
}

QTEST_MAIN(PdfStripLayoutTest)
#include "PdfStripLayoutTest.moc"
