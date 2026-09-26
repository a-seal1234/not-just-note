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

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QPointF>
#include <QScopedPointer>
#include <QTimer>
#include <QTransform>

#include <KisDocument.h>
#include <KisPart.h>
#include <KoColorSpaceRegistry.h>

#include <kis_image.h>

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

/// Hands a document back the way Krita wants it: removeDocument(document, true) deletes it, and
/// documents made with KisPart::createDocument() are not ours to delete directly.
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
    const QSize pageSize = PdfInkLoader::artifactSize(sourceKra, why);
    if (pageSize.isEmpty()) {
        fail(why, QStringLiteral("the artifact %1 has no page in it").arg(sourceKra));
        return false;
    }

    const KoColorSpace *colorSpace = KoColorSpaceRegistry::instance()->rgb8();
    if (!colorSpace) {
        fail(why, QStringLiteral("no RGB color space is available"));
        return false;
    }

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

    /// The image is built at the size the artifact already has, and the artifact's own layers go
    /// into it as layers -- names, order and groups kept, which reading them as images would lose.
    KisImageSP image = new KisImage(scratch->createUndoStore(), pageSize.width(), pageSize.height(),
                                    colorSpace, QStringLiteral("rotated page"));
    scratch->setCurrentImage(image, false);

    if (!PdfInkLoader::loadInkLayersInto(sourceKra, image, image->root(), why)) {
        fail(why, QStringLiteral("the artifact %1 holds no readable layer").arg(sourceKra));
        return false;
    }

    /// Krita turns an image with QTransform::rotateRadians(), which is the very call the paper's
    /// turn is made with (PdfSourceRenderers::turnedForDisplay), so both go the same way and the
    /// stroke stays on its line. A right angle enlarges the image to the turned size by itself.
    image->rotateImage(degrees * M_PI / 180.0);
    image->waitForDone();

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

    /// The turned layers go into a document of their own, which is what gets written: the artifact
    /// holds the page's own layers, never the image they were turned in.
    ScopedDocument document(PdfPageSaver::createPageLayersDocument(image, why));
    if (!document) {
        return false;
    }

    if (!saveAndWait(document.data(), destinationKra, why)) {
        fail(why, QStringLiteral("the turned page was not written to %1").arg(destinationKra));
        removeDestination(destinationKra);
        return false;
    }

    /// Read back before the caller is told it may replace anything: a turn that wrote a file
    /// nothing can open must not take the original's place.
    const QSize written = PdfInkLoader::artifactSize(destinationKra, nullptr);
    if (written != turned) {
        fail(why, QStringLiteral("the turned page reads back as %1x%2, not %3x%4")
                      .arg(written.width()).arg(written.height())
                      .arg(turned.width()).arg(turned.height()));
        removeDestination(destinationKra);
        return false;
    }

    return true;
}

bool PdfPageRotator::clipInto(const QString &sourceKra, const QString &destinationKra,
                              const PdfPageRecord &from, const PdfPageRecord &to, QString *why)
{
    if (!QFileInfo::exists(sourceKra)) {
        /// A page that was never drawn on has no artifact: nothing to clip, and nothing to say.
        return true;
    }
    const QSize artifact = PdfInkLoader::artifactSize(sourceKra, why);
    if (artifact.isEmpty()) {
        fail(why, QStringLiteral("the artifact %1 has no page in it").arg(sourceKra));
        return false;
    }

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

    KisImageSP image = new KisImage(scratch->createUndoStore(), artifact.width(), artifact.height(),
                                    colorSpace, QStringLiteral("clipped page"));
    scratch->setCurrentImage(image, false);

    if (!PdfInkLoader::loadInkLayersInto(sourceKra, image, image->root(), why)) {
        fail(why, QStringLiteral("the artifact %1 holds no readable layer").arg(sourceKra));
        return false;
    }
    image->waitForDone();

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
    const qreal dpiX = artifact.width() * 72.0 / fromFramePt.width();
    const qreal dpiY = artifact.height() * 72.0 / fromFramePt.height();

    /// Both boxes are in the reader's frame, whose origin is the page's top left: the frame the ops
    /// pane drags in and the renderer crops in. The artifact's own pixel frame is that frame turned
    /// by \a from's turn, which is the same normalization turnedForDisplay() produces.
    const QRectF fromBox =
        from.boxPt.isValid() ? from.boxPt : QRectF(QPointF(0, 0), from.boxedSizePt());
    const QRectF toBox = to.boxPt.isValid() ? to.boxPt : QRectF(QPointF(0, 0), to.boxedSizePt());

    const QRectF fromWindow(0, 0, fromBox.width() * dpiX, fromBox.height() * dpiY);
    const QRectF toWindow((toBox.x() - fromBox.x()) * dpiX, (toBox.y() - fromBox.y()) * dpiY,
                          toBox.width() * dpiX, toBox.height() * dpiY);
    const QRect keep = pixelRectOf(toWindow, fromWindow, QTransform().rotate(fromTurn));
    if (keep.isEmpty()) {
        fail(why, QStringLiteral("the box of page %1 has no area to keep").arg(to.kraFile));
        return false;
    }

    /// The clip itself: what the box does not cover is dropped, and a box that reaches past the
    /// sheet (a margin) is padded, which Krita does with the layers' default pixel -- transparent,
    /// because the page's paper is re-rendered from the source and the artifact holds ink only.
    image->cropImage(keep);
    image->waitForDone();

    /// A box and a turn in the same change: the artifact is in \a from's orientation and the page
    /// becomes \a to's, so the box's pixels are turned by the difference. Turning the box's own
    /// pixels by the difference is the same page the whole sheet's turn would have produced, and it
    /// is why the clip carries the turn rather than a second pass over the file.
    const int delta = ((to.extraRotation - from.extraRotation) % 360 + 360) % 360;
    if (delta != 0) {
        image->rotateImage(delta * M_PI / 180.0);
        image->waitForDone();
    }

    /// The clipped artifact has to measure what the page now says it measures, or the ink is no
    /// longer lying on the paper it was drawn on. Two pixels of slack, like the turn's own check:
    /// the point-to-pixel rounding of a dpi is not a disagreement.
    const QSizeF expectedPt = PdfPageRecord::turnedSize(toBox.size(), to.extraRotation);
    const QSize expected(qRound(expectedPt.width() * dpiX), qRound(expectedPt.height() * dpiY));
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

    ScopedDocument document(PdfPageSaver::createPageLayersDocument(image, why));
    if (!document) {
        return false;
    }
    if (!saveAndWait(document.data(), destinationKra, why)) {
        fail(why, QStringLiteral("the clipped page was not written to %1").arg(destinationKra));
        removeDestination(destinationKra);
        return false;
    }

    /// Read back before the caller is told it may replace anything: a clip that wrote a file nothing
    /// can open must not take the original's place.
    const QSize written = PdfInkLoader::artifactSize(destinationKra, nullptr);
    if (written != clipped) {
        fail(why, QStringLiteral("the clipped page reads back as %1x%2, not %3x%4")
                      .arg(written.width()).arg(written.height())
                      .arg(clipped.width()).arg(clipped.height()));
        removeDestination(destinationKra);
        return false;
    }

    return true;
}
