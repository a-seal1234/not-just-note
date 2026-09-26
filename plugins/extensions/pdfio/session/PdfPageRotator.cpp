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
#include <QScopedPointer>
#include <QTimer>

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
