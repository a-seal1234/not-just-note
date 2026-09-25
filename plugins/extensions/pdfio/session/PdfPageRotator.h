/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGEROTATOR_H
#define PDFPAGEROTATOR_H

#include <QString>

/**
 * Turns a page artifact on disk, so that the ink is turned with the paper.
 *
 * Rotating a page is a quarter turn the notebook records (PdfPageRecord::extraRotation), and the
 * paper comes back turned because a render is turned before it is drawn. The ink does not: it lives
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

    /// Whether \a degrees is one of the three turns a page can have: 90, 180 or 270 (or -90/-180/-270).
    static bool isQuarterTurn(int degrees);

private:
    PdfPageRotator() = delete;
};

#endif // PDFPAGEROTATOR_H
