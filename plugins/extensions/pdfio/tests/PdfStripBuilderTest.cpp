/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfStripBuilder.h"

#include <QtTest>

#include <limits>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>

/**
 * The strip, as a layer tree. The assertion that matters is the locking: the promise that the
 * neighbouring pages are shown but not yours to draw on is only true if Krita refuses the stroke,
 * which means both the group and its paint layer have to be locked on every slot but the active
 * one.
 */
class PdfStripBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testImageIsTheLayoutSize();
    void testEverySlotHasAPageAndAnInkGroup();
    void testOnlyTheInkLayerIsPaintable();
    void testPagesAreWhereTheLayoutSays();
    void testPaperIsBelowEveryInkGroup();
    void testConsecutiveGapsAreEqual();

private:
    QString fixturePath() const
    {
        return QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    }

    PdfSessionManifest manifestFor(PdfRenderBackend &backend) const
    {
        PdfSessionManifest manifest;
        manifest.sourceFile = QStringLiteral("text-fixture.pdf");
        manifest.sourceSha256 = QByteArrayLiteral("0000");
        manifest.sourceByteSize = 1;
        for (int i = 0; i < backend.pageCount(); ++i) {
            const PdfPageInfo info = backend.pageInfo(i);
            manifest.pages.append(PdfPageRecord{i, info.sizePt, info.rotation,
                                                QStringLiteral("pages/p%1.kra")
                                                    .arg(i + 1, 4, 10, QLatin1Char('0')),
                                                QString(), 0});
        }
        return manifest;
    }

    KisNodeSP childNamed(const KisImageSP &image, const QString &name) const
    {
        KisNodeSP root = image->root();
        for (quint32 i = 0; i < root->childCount(); ++i) {
            if (root->at(i)->name() == name) {
                return root->at(i);
            }
        }
        return KisNodeSP();
    }
};

void PdfStripBuilderTest::testImageIsTheLayoutSize()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));
    QVERIFY(strip.layout.isValid());
    QCOMPARE(QSize(strip.image->width(), strip.image->height()), strip.layout.imageSize());
}

void PdfStripBuilderTest::testEverySlotHasAPageAndAnInkGroup()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// The desk, one layer of paper per page, and one ink group over all of them with the stroke
    /// layer inside it.
    QCOMPARE(strip.image->root()->childCount(), 5u);
    QCOMPARE(strip.image->root()->at(0)->name(), QStringLiteral("Desk"));

    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        QVERIFY(slot.page >= 0);

        KisNodeSP background = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        QVERIFY(background);
        QVERIFY(qobject_cast<KisPaintLayer *>(background.data()));
        QVERIFY(background->userLocked());
    }

    KisNodeSP inkGroup = childNamed(strip.image, QStringLiteral("Ink"));
    QVERIFY(inkGroup);
    QVERIFY2(qobject_cast<KisGroupLayer *>(inkGroup.data()),
             "the strip's Ink has to be a group, the same shape a single page has");
    QCOMPARE(inkGroup->childCount(), 1u);

    KisNodeSP ink = inkGroup->at(0);
    QCOMPARE(ink->name(), QStringLiteral("Ink"));
    QVERIFY(qobject_cast<KisPaintLayer *>(ink.data()));
    QCOMPARE(KisNodeSP(ink), strip.activeInkLayer);

    /// The tree itself, said out loud: the group is the root child, and the stroke is inside it
    /// rather than a bare layer beside the papers.
    QStringList tree;
    for (quint32 i = 0; i < strip.image->root()->childCount(); ++i) {
        KisNodeSP child = strip.image->root()->at(i);
        if (child->name() == QStringLiteral("Ink")) {
            tree.append(QStringLiteral("%1 [%2] -> %3")
                            .arg(child->name())
                            .arg(qobject_cast<KisGroupLayer *>(child.data()) ? QStringLiteral("group")
                                                                            : QStringLiteral("layer"))
                            .arg(child->childCount() > 0 ? child->at(0)->name()
                                                         : QStringLiteral("(empty)")));
        } else {
            tree.append(child->name());
        }
    }
    qInfo("strip root: %s", qPrintable(tree.join(QStringLiteral(", "))));
}

void PdfStripBuilderTest::testOnlyTheInkLayerIsPaintable()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// The paper is never the user's to edit, and the ink layer is the one a stroke lands in.
    ///
    /// There used to be one ink group per page whose lock decided which page was writable, and that
    /// needed Krita to be told which node was active. That never worked in the running application,
    /// so there is one ink layer now and the page it belongs to is decided when the page is saved.
    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        KisNodeSP background = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        QVERIFY(background);
        QVERIFY(background->userLocked());
    }

    KisNodeSP inkGroup = childNamed(strip.image, QStringLiteral("Ink"));
    QVERIFY(inkGroup);
    QVERIFY(qobject_cast<KisGroupLayer *>(inkGroup.data()));
    QVERIFY2(!inkGroup->userLocked(), "the Ink group must not be locked");
    QVERIFY(inkGroup->childCount() > 0);
    QVERIFY2(!inkGroup->at(0)->userLocked(), "the ink stroke layer must be paintable");
}

void PdfStripBuilderTest::testPagesAreWhereTheLayoutSays()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// The page is painted at its own place in the strip, not at the origin: the top of its own
    /// slot, which is where the ink is expected to line up too.
    for (const PdfStripLayout::Slot &slot : strip.layout.slots()) {
        KisNodeSP background = childNamed(strip.image, PdfStripBuilder::backgroundLayerName(slot.page));
        const QRect bounds = background->paintDevice()->exactBounds();

        QCOMPARE(bounds.width(), slot.rect.width());
        QCOMPARE(bounds.height(), slot.rect.height());
        QCOMPARE(bounds.top(), slot.rect.top());
        QCOMPARE(bounds.left(), slot.rect.left());
    }
}

void PdfStripBuilderTest::testPaperIsBelowEveryInkGroup()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    /// All of the paper first, then all of the ink. Adding a slot at a time puts the next page's
    /// paper above this page's ink, and a stroke that strays outside its own page disappears behind
    /// the page below it -- reported exactly as "the active page did not change", because the
    /// stroke was there and hidden.
    int inkIndex = -1;
    int highestPaper = -1;

    KisNodeSP root = strip.image->root();
    for (quint32 i = 0; i < root->childCount(); ++i) {
        const QString name = root->at(i)->name();
        if (name == QStringLiteral("Desk")) {
            continue;
        }

        if (name == QStringLiteral("Ink")) {
            inkIndex = int(i);
        } else {
            highestPaper = qMax(highestPaper, int(i));
        }
    }

    QVERIFY(inkIndex >= 0);
    QVERIFY(highestPaper >= 0);
    QVERIFY2(inkIndex > highestPaper,
             "the ink has to sit above every page, or ink vanishes behind the page below it");
}

/**
 * The space between two pages is the same however different their sizes are.
 *
 * Every page used to be centred inside a cell sized for the largest page in the notebook, so the
 * gap the user saw between a small page and a large one was SlotGap plus the centring slack of
 * both. text-fixture.pdf carries three real sizes (A4 595x842, A5 420x595, square 300x300), which
 * is the case this measures; the three heights are checked to differ first, so equality of the
 * gaps below cannot pass by being trivial.
 */
void PdfStripBuilderTest::testConsecutiveGapsAreEqual()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               backend, QString(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    const QList<PdfStripLayout::Slot> slots = strip.layout.slots();
    QCOMPARE(slots.size(), 3);

    /// Three pages, three sizes -- otherwise the assertion below would hold just as well on the
    /// old layout.
    QVERIFY(slots.at(0).rect.height() != slots.at(1).rect.height());
    QVERIFY(slots.at(1).rect.height() != slots.at(2).rect.height());

    /// The blank space between two pages, measured the way it is seen: the rows between the
    /// bottom of one page rectangle and the top of the next.
    const int gap = slots.at(1).rect.top() - slots.at(0).rect.bottom() - 1;
    QVERIFY2(gap > 0, "the pages must not touch");
    for (int i = 2; i < slots.size(); ++i) {
        QCOMPARE(slots.at(i).rect.top() - slots.at(i - 1).rect.bottom() - 1, gap);
    }

    qInfo("text-fixture.pdf: slot heights %d, %d, %d; every gap %d px",
          slots.at(0).rect.height(), slots.at(1).rect.height(), slots.at(2).rect.height(), gap);
}

QTEST_MAIN(PdfStripBuilderTest)
#include "PdfStripBuilderTest.moc"
