/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFINKLOADER_H
#define PDFINKLOADER_H

#include <QImage>
#include <QString>

#include <kis_types.h>

/**
 * Reads the ink back out of a saved page artifact.
 *
 * The export needs the ink as an image, and going through Krita to get it would mean opening a
 * document per page and waiting for each one, which is exactly the round trip the project format
 * exists to avoid. A .kra is a zip and a paint layer is a PNG inside it, so the ink can be read
 * directly.
 */
class PdfInkLoader
{
public:
    /**
     * The ink of \a kraPath, or a null image when the page has none. \a why is set only on a
     * real failure, not for a page that was simply never drawn on.
     */
    static QImage loadInk(const QString &kraPath, QString *why = nullptr);

    /**
     * Puts the artifact's own layers into \a parent of \a target, keeping their names, order and
     * opacity, and returns true when at least one layer came back.
     *
     * A .kra artifact holds the page's layers (see PdfPageSaver), so a page can be rebuilt with
     * the layers the user actually made instead of one flattened picture. False means "nothing was
     * restored" as much as "could not be read": a page that was never drawn on has no artifact at
     * all, and one whose artifact cannot be read as a document is left to the merged-image path
     * the caller already has. \a why carries the reason when there was one.
     */
    static bool loadInkLayersInto(const QString &kraPath, const KisImageSP &target,
                                  KisNodeSP parent, QString *why = nullptr);

private:
    PdfInkLoader() = delete;
};

#endif // PDFINKLOADER_H
