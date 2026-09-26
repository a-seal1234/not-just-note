/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGEROTATOR_H
#define PDFPAGEROTATOR_H

#include <QString>

struct PdfPageRecord;

/**
 * Turns a page artifact on disk, so that the ink is turned with the paper.
 *
 * Rotating a page is a turn the notebook records (PdfPageRecord::extraRotation), and the paper
 * comes back turned because a render is turned before it is drawn. The ink does not: it lives
 * in the artifact as pixels, and a page whose paper turned while its ink did not is a page whose
 * notes are suddenly on the wrong lines. So the artifact's layers are turned by the same angle,
 * about the same centre.
 *
 * The turn is written to a destination the caller chooses -- a temporary name, in practice -- and
 * the original is left alone. That is what lets the caller journal the original, put the turned
 * file in its place and commit the manifest as one step, and put everything back if any of it
 * fails. This class does not decide when that happens, and it does not touch the manifest.
 *
 * An artifact that is not there is not an error and writes nothing: a page that was never drawn on
 * has no artifact, and its rotation is recorded in the manifest alone.
 */
class PdfPageRotator
{
public:
    /**
     * Reads the artifact at \a sourceKra, turns its layers by \a degrees about the page's centre,
     * and writes the result at \a destinationKra with the same sidecar an ordinary page save
     * writes beside it.
     *
     * Returns false, with \a why, when the artifact exists but cannot be read or the turned copy
     * cannot be written and read back. \a destinationKra's leftovers are removed on failure; the
     * source is never touched either way.
     */
    static bool rotateInto(const QString &sourceKra, const QString &destinationKra, int degrees,
                           QString *why = nullptr);

    /**
     * Whether \a degrees is a whole number of right angles: 90, 180 or 270 (or -90/-180/-270, and 0).
     *
     * Not a restriction -- any angle can be turned -- but the difference between the two kinds of
     * turn is real: a right angle is exactly a transpose of the pixels, while any other angle has to
     * be resampled, and the paper is turned with the filter that choice implies.
     */
    static bool isRightAngle(int degrees);

    /**
     * Reads the artifact at \a sourceKra and writes the page it holds as \a to describes it at
     * \a destinationKra: the part of the page inside \a to's box, turned the way \a to's record
     * says.
     *
     * This is what makes Box mode mean what it says. A crop that only changed the manifest would
     * leave the whole page's ink on disk, and the roll -- which scales a stored artifact into the
     * page's current rectangle -- would then squeeze the whole page into the crop: the user's marks
     * moved to the wrong place, which is worse than losing the ones the crop removes. So the pixels
     * outside the box are dropped here, before the manifest that describes the smaller page is
     * committed.
     *
     * \a from is the record the artifact was written under: its box is the region the artifact
     * covers, and its turn is the orientation the artifact is already in. \a to is the record the
     * page becomes. Pixels are never invented -- a margin the user dragged outwards comes back as
     * transparency, and the page is not re-rendered -- and the artifact keeps its resolution: the
     * roll's scaling path is what places it at the new size.
     *
     * An artifact that is not there is not an error and writes nothing, exactly as a turn of a page
     * that was never drawn on is not.
     */
    static bool clipInto(const QString &sourceKra, const QString &destinationKra,
                         const PdfPageRecord &from, const PdfPageRecord &to,
                         QString *why = nullptr);

private:
    PdfPageRotator() = delete;
};

#endif // PDFPAGEROTATOR_H
