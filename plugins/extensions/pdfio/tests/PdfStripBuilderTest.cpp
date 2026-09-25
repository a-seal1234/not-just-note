/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/poppler/PopplerRenderBackend.h"
#include "session/PdfSourceRenderers.h"
#include "session/PdfStripBuilder.h"

#include <QFileInfo>

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
    void testASlotIsRenderedFromItsRecordAndNotItsPosition();

private:
    QString fixturePath() const
    {
        return QStringLiteral(FILES_DATA_DIR) + QStringLiteral("text-fixture.pdf");
    }

    /// The renderers resolve a source against the project directory -- a manifest records a source
    /// relative to the notebook it belongs to -- so the fixture's own directory is the project here.
    QString fixtureDir() const
    {
        return QStringLiteral(FILES_DATA_DIR);
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
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               renderers, fixtureDir(), &why);
    QVERIFY2(strip.image, qPrintable(why));
    QVERIFY(strip.layout.isValid());
    QCOMPARE(QSize(strip.image->width(), strip.image->height()), strip.layout.imageSize());
}

void PdfStripBuilderTest::testEverySlotHasAPageAndAnInkGroup()
{
    PopplerRenderBackend backend;
    QVERIFY(backend.open(fixturePath()));

    QString why;
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               renderers, fixtureDir(), &why);
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
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               renderers, fixtureDir(), &why);
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
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               renderers, fixtureDir(), &why);
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
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               renderers, fixtureDir(), &why);
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
    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    const PdfStripBuilder::Strip strip = PdfStripBuilder::build(manifestFor(backend), 1, 3, 200.0,
                                                               renderers, fixtureDir(), &why);
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

/**
 * A slot is rendered from its record's own source and its own page inside that source, and the
 * locked layer is named after that source page rather than after the slot's position.
 *
 * The two numbers agree in a notebook that was just created, which is what made the positional
 * render invisible: the strip asked the renderer for renderPage(slot.page) and opened "the source".
 * The moment pages arrive from another PDF, or in an order other than the source's, a slot's
 * position and its source page are different sheets -- and a page-sized render of the wrong sheet
 * still fits the slot, so nothing complains. The paper under the ink is simply a different page.
 *
 * Here the two records point at page 3 of one file and page 1 of another, and the file each one
 * came from renders at a different pixel size than the page the slot's position would have picked,
 * so a positional build cannot pass by coincidence.
 */
void PdfStripBuilderTest::testASlotIsRenderedFromItsRecordAndNotItsPosition()
{
    const QString other = QStringLiteral(FILES_DATA_DIR) + QStringLiteral("ex-rotations.pdf");
    QVERIFY2(QFileInfo::exists(other), qPrintable(other));

    PopplerRenderBackend fixture;
    QVERIFY(fixture.open(fixturePath()));
    PopplerRenderBackend rotated;
    QVERIFY(rotated.open(other));

    /// The two files have to render differently for this test to say anything: record 0 is the
    /// fixture's third page, and the page the slot's position would have chosen is a different size.
    const QImage expectedFirst = fixture.renderPage(2, 200.0);
    const QImage positionalFirst = fixture.renderPage(0, 200.0);
    const QImage expectedSecond = rotated.renderPage(0, 200.0);
    const QImage positionalSecond = fixture.renderPage(1, 200.0);
    QVERIFY(!expectedFirst.isNull());
    QVERIFY(!expectedSecond.isNull());
    QVERIFY(expectedFirst.size() != positionalFirst.size());
    QVERIFY(expectedSecond.size() != positionalSecond.size());

    PdfSessionManifest manifest;
    manifest.sourceFile = QFileInfo(fixturePath()).fileName();
    manifest.sourceSha256 = QByteArrayLiteral("0000");
    manifest.sourceByteSize = 1;
    PdfSourceRecord first;
    first.file = manifest.sourceFile;
    first.sha256 = manifest.sourceSha256;
    first.byteSize = 1;
    PdfSourceRecord second;
    second.file = QFileInfo(other).fileName();
    second.sha256 = QByteArrayLiteral("1111");
    second.byteSize = 1;
    manifest.sources << first << second;

    const PdfPageInfo firstPage = fixture.pageInfo(2);
    const PdfPageInfo secondPage = rotated.pageInfo(0);
    manifest.pages.append(PdfPageRecord{firstPage.index, firstPage.sizePt, firstPage.rotation,
                                        QStringLiteral("pages/p0001.kra"), QString(), 0, 0, 0});
    manifest.pages.append(PdfPageRecord{secondPage.index, secondPage.sizePt, secondPage.rotation,
                                        QStringLiteral("pages/p0002.kra"), QString(), 0, 1, 0});
    QVERIFY(manifest.isValid());

    PdfSourceRenderers renderers([]() { return new PopplerRenderBackend(); });
    QString why;
    const PdfStripBuilder::Strip strip =
        PdfStripBuilder::build(manifest, 0, 3, 200.0, renderers, fixtureDir(), &why);
    QVERIFY2(strip.image, qPrintable(why));

    const int slotFirst = strip.layout.slotForPage(0);
    const int slotSecond = strip.layout.slotForPage(1);
    QVERIFY(slotFirst >= 0);
    QVERIFY(slotSecond >= 0);

    /// The locked layer is named after the SOURCE page the record holds, which is also what the
    /// single-page builder has always done (PdfProjectBuilder names it from record.index).
    KisNodeSP paperFirst =
        childNamed(strip.image, PdfStripBuilder::backgroundLayerName(firstPage.index));
    KisNodeSP paperSecond =
        childNamed(strip.image, PdfStripBuilder::backgroundLayerName(secondPage.index));
    QVERIFY2(paperFirst, qPrintable(PdfStripBuilder::backgroundLayerName(firstPage.index)));
    QVERIFY2(paperSecond, qPrintable(PdfStripBuilder::backgroundLayerName(secondPage.index)));
    QVERIFY(paperFirst->userLocked());
    QVERIFY(paperSecond->userLocked());

    /// And the paper really is that page, from that file: the painted rectangle is the size of the
    /// render the record asks for, and not the size of the page the slot's position would have
    /// picked out of the first source.
    const QRect boundsFirst = paperFirst->paintDevice()->exactBounds();
    const QRect boundsSecond = paperSecond->paintDevice()->exactBounds();
    qInfo("slot 0 -> %s: %dx%d (positional would be %dx%d); slot 1 -> %s: %dx%d (positional %dx%d)",
          qPrintable(paperFirst->name()), boundsFirst.width(), boundsFirst.height(),
          positionalFirst.width(), positionalFirst.height(), qPrintable(paperSecond->name()),
          boundsSecond.width(), boundsSecond.height(), positionalSecond.width(),
          positionalSecond.height());

    QCOMPARE(boundsFirst.size(), expectedFirst.size());
    QCOMPARE(boundsSecond.size(), expectedSecond.size());
    QVERIFY(boundsFirst.size() != positionalFirst.size());
    QVERIFY(boundsSecond.size() != positionalSecond.size());
}

QTEST_MAIN(PdfStripBuilderTest)
#include "PdfStripBuilderTest.moc"
