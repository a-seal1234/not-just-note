/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfPageRotator.h"

#include <QtMath>

#include "session/PdfInkLoader.h"
#include "session/PdfPageSaver.h"
#include "session/PdfSessionManifest.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QPointF>
#include <QScopedPointer>
#include <QStringList>
#include <QTimer>
#include <QTransform>

#include <KisDocument.h>
#include <KisPart.h>
#include <KoColorSpaceRegistry.h>

#include <kis_group_layer.h>
#include <kis_image.h>
#include <kis_paint_layer.h>

#include <cmath>

namespace {

/// The same bound every page write has: this waits for a save the way the notebook does, and a
/// write that never reports must not hold a rotation -- and the operation behind it -- forever.
constexpr int LandingTimeoutMs = 20000;

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

/// Hands \a document to Krita and waits for the write to land, with the same bound every page
/// write has.
///
/// A local event loop rather than the notebook's PdfSaveQueue on purpose: that queue is a
/// long-lived member of the navigator -- it re-arms itself from the event loop after every write --
/// and a stack temporary of it is destroyed while its deferred pump is still queued, which is a
/// dangling this. Nothing here needs the queue's ordering either: one document, one write.
///
/// The report is connected BEFORE the save starts, and that order matters as much: a small artifact
/// can be written within the same turn of the event loop, and a connection made afterwards would
/// wait for a signal that had already fired, until the guard below gave up on a write that in fact
/// succeeded.
bool saveAndWait(KisDocument *document, const QString &path, QString *why)
{
    bool finished = false;
    QEventLoop loop;
    QObject::connect(document, &KisDocument::sigSavingFinished, &loop,
                     [&finished, &loop](const QString &) {
                         finished = true;
                         loop.quit();
                     });

    /// The bound, in a timer the loop owns: a write that never reports must end here rather than
    /// hold a rotation -- and the operation behind it -- forever.
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(LandingTimeoutMs);

    if (!PdfPageSaver::saveDocument(document, path, why)) {
        return false;
    }
    if (!finished) {
        loop.exec();
    }
    return finished;
}

/// The resident set of this process, in whole megabytes, read from /proc -- the number an OOM is
/// judged by, and one a tablet has. \a peak asks for the high-water mark, which is the one that says
/// what a stage cost at its worst. -1 when the file cannot be read, which a log line shows as such
/// rather than as a zero.
int rssMb(bool peak)
{
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) {
        return -1;
    }

    const QByteArray wanted = peak ? QByteArrayLiteral("VmHWM:") : QByteArrayLiteral("VmRSS:");
    for (const QByteArray &line : status.readAll().split('\n')) {
        if (!line.startsWith(wanted)) {
            continue;
        }
        const QList<QByteArray> parts = line.simplified().split(' ');
        return parts.size() >= 2 ? parts.at(1).toInt() / 1024 : -1;
    }
    return -1;
}

/**
 * What rewriting a page costs, stage by stage, in one line of the log.
 *
 * A crop or a turn runs on the UI thread while the user waits, and both complaints about it -- "it
 * takes forever" and "it ends in an OOM" -- are answered by numbers rather than by reading the code.
 * The stages are the ones that hold a whole page in memory (a read, a crop, a projection, a write, a
 * read-back), so the line says which of them spent the time and what the process was holding at each
 * one. The destructor prints, so the line is written on every way out, a refusal included.
 */
class StageLog
{
public:
    explicit StageLog(const QString &what)
        : m_what(what)
    {
        m_clock.start();
    }

    ~StageLog()
    {
        qWarning().noquote() << QStringLiteral("pdfio: %1: %2 ms total (%3)")
                                    .arg(m_what)
                                    .arg(m_clock.elapsed())
                                    .arg(m_stages.join(QStringLiteral("; ")));
    }

    /// Ends the stage that just ran, naming it. The resident set is read here, right after it, and
    /// the line goes out at once: a stage that never ends is exactly what this is for, and a total
    /// printed at the end is a total nobody gets to read when the operation does not come back.
    void mark(const QString &stage)
    {
        const qint64 elapsed = m_clock.elapsed();
        const QString line = QStringLiteral("%1 %2 ms, rss %3 MB, peak %4 MB")
                                 .arg(stage)
                                 .arg(elapsed - m_last)
                                 .arg(rssMb(false))
                                 .arg(rssMb(true));
        m_stages.append(line);
        m_last = elapsed;
        qWarning().noquote() << QStringLiteral("pdfio: %1: %2").arg(m_what, line);
    }

private:
    QString m_what;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
    QStringList m_stages;
};

/// Hands a document back the way Krita wants it: removeDocument(document, true) deletes it, and
/// documents made with KisPart::createDocument() are not ours to delete directly.
///
/// The delete stays queued, the way Krita arranges it: KisDocument's destructor waits on the image's
/// scheduler, so forcing it here would raise a busy-wait dialog from inside the rewrite itself. The
/// memory a crop costs is bought by making one document instead of four (PdfInkLoader reads the
/// artifact once, and writePageDocument packs only a graph that needs it).
struct DocumentRemover {
    static void cleanup(KisDocument *document)
    {
        if (document) {
            KisPart::instance()->removeDocument(document, true);
        }
    }
};

using ScopedDocument = QScopedPointer<KisDocument, DocumentRemover>;

/// Everything rotateInto() may have written, removed again. The source is never in this list.
void removeDestination(const QString &destinationKra)
{
    QFile::remove(destinationKra);
    QFile::remove(destinationKra + QStringLiteral(".layers.txt"));
    QDir(destinationKra + QStringLiteral(".layers")).removeRecursively();
}

/// Whether the artifact's graph has to be packed into a page image of its own before it is written.
///
/// Everything this project's writers produce is a flat stack of paint layers, and that is written as
/// it stands. Two things are not, and both need the packing pass: a layer that is not paint (a file
/// layer has no PNG of its own, so its pixels would be lost) and a group (Krita's .kra export and
/// the artifact's PNG sidecar both read the ROOT's paint layers, so a group's ink would be lost on
/// the way back in). A page background is packed away as well, for the reason it is never written:
/// the paper is re-rendered from the source, not stored.
bool needsPacking(KisImageSP image)
{
    if (!image || !image->root()) {
        return true;
    }

    for (quint32 i = 0; i < image->root()->childCount(); ++i) {
        KisNodeSP child = image->root()->at(i);
        if (!qobject_cast<KisPaintLayer *>(child.data()) || PdfPageSaver::isPageBackground(child)) {
            return true;
        }
    }
    return false;
}

/**
 * Writes \a image as the artifact at \a destinationKra, and proves what it wrote before the caller
 * is told it may replace anything.
 *
 * \a document owns \a image and is what the save runs through. The image is written as it stands
 * when it is a flat stack of paint layers -- the normal shape of a page artifact -- and is packed
 * into a page image of its own only when it is not (see needsPacking).
 */
bool writePageDocument(KisImageSP image, KisDocument *document, const QString &destinationKra,
                       StageLog &stages, QString *why)
{
    /// The merged image is what a reader that has no sidecar falls back to, and Krita computes it in
    /// an update job: a document saved in the same breath has none yet, which is the trap
    /// PdfPageSaver documents in the other direction. ONE refresh, on the image that is about to be
    /// written.
    image->refreshGraphAsync(image->root(), { image->bounds() }, image->bounds());
    image->waitForDone();
    stages.mark(QStringLiteral("project"));

    ScopedDocument packed;
    if (needsPacking(image)) {
        packed.reset(PdfPageSaver::createPageLayersDocument(image, why));
        if (!packed) {
            return false;
        }
        document = packed.data();
    }

    if (!saveAndWait(document, destinationKra, why)) {
        fail(why, QStringLiteral("the rewritten page was not written to %1").arg(destinationKra));
        removeDestination(destinationKra);
        return false;
    }
    stages.mark(QStringLiteral("write"));

    /// Read back before the caller is told it may replace anything: a page that wrote a file nothing
    /// can open must not take the original's place. Through the archive's own header, not through a
    /// document -- the old check decoded the file it had just written, another whole page per layer
    /// in memory to answer what the PNG's first bytes say. The fallback keeps the answer for an
    /// archive that cannot give one.
    QSize written = PdfInkLoader::artifactSizeFromArchive(destinationKra);
    if (written.isEmpty()) {
        written = PdfInkLoader::artifactSize(destinationKra, nullptr);
    }
    const QSize expected = image->bounds().size();
    if (written != expected) {
        fail(why, QStringLiteral("the written page reads back as %1x%2, not %3x%4")
                      .arg(written.width()).arg(written.height())
                      .arg(expected.width()).arg(expected.height()));
        removeDestination(destinationKra);
        return false;
    }
    stages.mark(QStringLiteral("verify"));
    return true;
}

/// The rectangle \a inside expressed in the pixels of a raster of \a size that holds the window
/// \a window, when both are rectangles in the same frame and \a window's origin is the raster's.
/// Used for the one conversion a clip needs: where the page's new box sits in the artifact.
QRect pixelRectOf(const QRectF &inside, const QRectF &window, const QTransform &turn)
{
    /// The turn is applied to the rectangle the artifact holds, so the artifact's own coordinates are
    /// the turned window with its bounding box starting at the origin -- the same normalization
    /// turnedForDisplay() and the page's own artifact use.
    const QRectF turnedWindow = turn.mapRect(window);
    const QRectF local(inside.x() - window.x(), inside.y() - window.y(),
                       inside.width(), inside.height());
    const QRectF turnedInside = turn.mapRect(local);
    const QRectF placed = turnedInside.translated(-turnedWindow.topLeft());

    return QRect(qRound(placed.x()), qRound(placed.y()), qRound(placed.width()),
                 qRound(placed.height()));
}

} // namespace

bool PdfPageRotator::isRightAngle(int degrees)
{
    const int turn = ((degrees % 360) + 360) % 360;
    return turn == 0 || turn == 90 || turn == 180 || turn == 270;
}

bool PdfPageRotator::rotateInto(const QString &sourceKra, const QString &destinationKra,
                                int degrees, QString *why)
{
    if (!QFileInfo::exists(sourceKra)) {
        /// A page that was never drawn on has no artifact: nothing to turn, and nothing to say.
        return true;
    }

    StageLog stages(QStringLiteral("the turn of %1").arg(QFileInfo(sourceKra).fileName()));

    /// A document of our own. It is not only for the undo store KisImage::rotateImage() needs: the
    /// image has to BELONG to a document while it is turned. An image no document owns has no
    /// running update scheduler, and the turn's update jobs are then never worked off -- the
    /// destructor of such an image waits for them forever, which is exactly how this hung the first
    /// time. Owned by a document, the jobs run and the image is destroyed in Krita's own teardown.
    ScopedDocument scratch(KisPart::instance()->createDocument());
    if (!scratch) {
        fail(why, QStringLiteral("no document could be made to turn %1 in").arg(sourceKra));
        return false;
    }

    /// ONE read of the artifact, into a page image of our own. What this replaced is why rewriting a
    /// page took minutes and ended in an OOM on the tablet (2026-09-28): artifactSize() decoded the
    /// whole artifact, loadInkLayersInto() decoded it AGAIN and cloned every layer into a second
    /// image, createPageLayersDocument() cloned them a third time into a page image of its own, and
    /// the read-back after the write decoded the file a fourth time -- four whole pages, a page per
    /// layer each, alive at once for one turn. The image below is our own: the file's copy is
    /// released by the loader before it returns, so the wait right after it never lands on a graph
    /// that is still loading -- which is a busy-wait dialog in the application, and in a test a
    /// crash inside Qt's own dialog placement.
    /// Not const: KisSharedPtr hands back a const KisImage through a const smart pointer, and
    /// the crop, the turn and the projection all write to it.
    KisImageSP image = PdfInkLoader::loadArtifactAsPage(scratch.data(), sourceKra, why);
    if (!image) {
        if (!why || why->isEmpty()) {
            *why = QStringLiteral("the artifact %1 holds no readable page").arg(sourceKra);
        }
        return false;
    }
    const QSize pageSize = image->bounds().size();
    if (pageSize.isEmpty()) {
        fail(why, QStringLiteral("the artifact %1 has no page in it").arg(sourceKra));
        return false;
    }
    image->waitForDone();
    stages.mark(QStringLiteral("read"));

    /// Krita turns an image with QTransform::rotateRadians(), which is the very call the paper's
    /// turn is made with (PdfSourceRenderers::turnedForDisplay), so both go the same way and the
    /// stroke stays on its line. A right angle enlarges the image to the turned size by itself.
    image->rotateImage(degrees * M_PI / 180.0);
    image->waitForDone();
    stages.mark(QStringLiteral("turn"));

    /// The turned artifact has to measure what the page says it measures, or the ink is no longer
    /// lying on the paper it was drawn on. Compared against the shape the turn implies rather than
    /// against the size it started at: a square page turned a right angle keeps its size, which is
    /// correct, and was refused as "a turn that did not happen" while a quarter turn was the only
    /// turn there was.
    const QSize turned = image->bounds().size();
    /// The box the record, the layout, the renderer and this all have to agree about, from the one
    /// place that computes it: the artifact is in pixels, the record is in points, and two spellings
    /// of the arithmetic would drift by the rounding of a dpi.
    const QSizeF expectedBox = PdfPageRecord::turnedSize(QSizeF(pageSize), degrees);
    const QSize expected(qRound(expectedBox.width()), qRound(expectedBox.height()));
    const auto closeEnough = [](int a, int b) { return qAbs(a - b) <= 1; };
    if (turned.isEmpty() || !closeEnough(turned.width(), expected.width())
        || !closeEnough(turned.height(), expected.height())) {
        fail(why, QStringLiteral("turning %1 by %2 degrees left it %3x%4, where the page's own shape "
                                 "says %5x%6")
                      .arg(sourceKra).arg(degrees)
                      .arg(turned.width()).arg(turned.height())
                      .arg(expected.width()).arg(expected.height()));
        return false;
    }
    stages.mark(QStringLiteral("check"));

    return writePageDocument(image, scratch.data(), destinationKra, stages, why);
}

bool PdfPageRotator::clipInto(const QString &sourceKra, const QString &destinationKra,
                              const PdfPageRecord &from, const PdfPageRecord &to, QString *why)
{
    if (!QFileInfo::exists(sourceKra)) {
        /// A page that was never drawn on has no artifact: nothing to clip, and nothing to say.
        return true;
    }
    StageLog stages(QStringLiteral("the crop of %1").arg(QFileInfo(sourceKra).fileName()));

    /// The artifact as PICTURES, and the page they cover. A crop only ever moves pixels, and this is
    /// where the time and the memory of the old path went: it built a Krita document graph, cloned
    /// every layer into a second image, called cropImage() -- a processing-applicator stroke whose
    /// jobs need the very event loop the GUI thread is blocked in, measured on the desktop at 31.7
    /// SECONDS for one page while Krita's busy-wait dialog spanned the time -- and then decoded the
    /// file it had written to prove it. Read from the sidecar, a crop is one QImage copy per layer
    /// and nothing that waits at all.
    QSize artifact;
    const QList<PdfInkLoader::ArtifactLayer> layers =
        PdfInkLoader::loadLayersForRewrite(sourceKra, &artifact, why);
    if (layers.isEmpty() || artifact.isEmpty()) {
        if (!why || why->isEmpty()) {
            *why = QStringLiteral("the artifact %1 holds no readable layer").arg(sourceKra);
        }
        return false;
    }
    stages.mark(QStringLiteral("read"));

    /// The frame the artifact is in: the page the \a from record describes, turned by that record's
    /// own turn. Its point size is turnedSize(from's box, from's turn); the artifact is that frame at
    /// whatever dpi it was written at, which is why the factors are read off the artifact rather than
    /// assumed -- a page written before a budget change is at another resolution and must clip by the
    /// same rule.
    const int fromTurn = ((from.extraRotation % 360) + 360) % 360;
    const QSizeF fromFramePt = PdfPageRecord::turnedSize(from.boxedSizePt(), fromTurn);
    if (fromFramePt.isEmpty()) {
        fail(why, QStringLiteral("the page %1 has no size to clip to").arg(sourceKra));
        return false;
    }
    /// POINTS TO PIXELS, which is artifact / frame and NOT a dpi: a box is in points and the
    /// artifact is in pixels, so the factor is (pixels per point). Multiplying a point size by a DPI
    /// instead -- which is what this did -- makes the crop rectangle 72 times too big on each side,
    /// and a crop of an A4 page becomes an image of about 21600 x 61100 pixels: Krita allocates it,
    /// which is instant RAM exhaustion (measured: a 31.7 SECOND stall and a process the OOM killer
    /// took, and the user's own crop that ate all of theirs).
    const qreal pixelsPerPtX = artifact.width() / fromFramePt.width();
    const qreal pixelsPerPtY = artifact.height() / fromFramePt.height();

    /// Both boxes are in the reader's frame, whose origin is the page's top left: the frame the ops
    /// pane drags in and the renderer crops in. The artifact's own pixel frame is that frame turned
    /// by \a from's turn, which is the same normalization turnedForDisplay() produces.
    const QRectF fromBox =
        from.boxPt.isValid() ? from.boxPt : QRectF(QPointF(0, 0), from.boxedSizePt());
    const QRectF toBox = to.boxPt.isValid() ? to.boxPt : QRectF(QPointF(0, 0), to.boxedSizePt());

    const QRectF fromWindow(0, 0, fromBox.width() * pixelsPerPtX, fromBox.height() * pixelsPerPtY);
    const QRectF toWindow((toBox.x() - fromBox.x()) * pixelsPerPtX,
                          (toBox.y() - fromBox.y()) * pixelsPerPtY,
                          toBox.width() * pixelsPerPtX, toBox.height() * pixelsPerPtY);
    const QRect keep = pixelRectOf(toWindow, fromWindow, QTransform().rotate(fromTurn));
    if (keep.isEmpty()) {
        fail(why, QStringLiteral("the box of page %1 has no area to keep").arg(to.kraFile));
        return false;
    }

    /// A box and a turn in the same change: the artifact is in \a from's orientation and the page
    /// becomes \a to's, so the box's pixels are turned by the difference. Turning the box's own
    /// pixels by the difference is the same page the whole sheet's turn would have produced, and it
    /// is why the clip carries the turn rather than a second pass over the file.
    const int delta = ((to.extraRotation - from.extraRotation) % 360 + 360) % 360;

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return false;
    }

    ScopedDocument scratch(KisPart::instance()->createDocument());
    if (!scratch) {
        fail(why, QStringLiteral("no document could be made to clip %1 in").arg(sourceKra));
        return false;
    }

    /// The clip itself: what the box does not cover is dropped, and a box that reaches past the
    /// sheet (a margin) is padded -- QImage::copy leaves the outside of the rectangle transparent,
    /// which is what Krita's default pixel is here, because the page's paper is re-rendered from the
    /// source and the artifact holds ink only.
    ///
    /// \a keep is in the artifact's own pixels, whose origin is the page's top left. A layer that
    /// was written at an offset carries its own, so the rectangle is moved into that layer's pixels
    /// and the copy goes back at the page's own origin.
    KisImageSP image = new KisImage(scratch->createUndoStore(), keep.width(), keep.height(),
                                    colorSpace, QStringLiteral("clipped page"));
    for (const PdfInkLoader::ArtifactLayer &layer : layers) {
        QImage pixels = layer.pixels.copy(QRect(keep.x() - layer.x, keep.y() - layer.y,
                                                keep.width(), keep.height()));
        if (pixels.isNull()) {
            continue;
        }
        /// QTransform::rotate(), the very call Krita's own rotateImage() is built on and the one the
        /// paper's turn is made with, so a box and a turn in one change go the same way round.
        if (delta != 0) {
            pixels = pixels.transformed(QTransform().rotate(delta));
        }

        KisPaintLayerSP out = new KisPaintLayer(image, layer.name, layer.opacity);
        out->paintDevice()->convertFromQImage(pixels, nullptr, 0, 0);
        image->addNode(out, image->root());
    }
    scratch->setCurrentImage(image, false);
    if (image->root()->childCount() == 0) {
        fail(why, QStringLiteral("the box of page %1 leaves no layer to draw on").arg(to.kraFile));
        return false;
    }
    stages.mark(delta != 0 ? QStringLiteral("crop+turn") : QStringLiteral("crop"));

    /// The clipped artifact has to measure what the page now says it measures, or the ink is no
    /// longer lying on the paper it was drawn on. Two pixels of slack, like the turn's own check:
    /// the point-to-pixel rounding of a dpi is not a disagreement.
    const QSizeF expectedPt = PdfPageRecord::turnedSize(toBox.size(), to.extraRotation);
    const QSize expected(qRound(expectedPt.width() * pixelsPerPtX),
                        qRound(expectedPt.height() * pixelsPerPtY));
    const QSize clipped = image->bounds().size();
    const auto closeEnough = [](int a, int b) { return qAbs(a - b) <= 2; };
    if (clipped.isEmpty() || !closeEnough(clipped.width(), expected.width())
        || !closeEnough(clipped.height(), expected.height())) {
        fail(why, QStringLiteral("clipping %1 to %2x%3 points left it %4x%5, where the page's own "
                                 "box says %6x%7")
                      .arg(sourceKra)
                      .arg(toBox.width()).arg(toBox.height())
                      .arg(clipped.width()).arg(clipped.height())
                      .arg(expected.width()).arg(expected.height()));
        return false;
    }
    stages.mark(QStringLiteral("check"));

    return writePageDocument(image, scratch.data(), destinationKra, stages, why);
}
