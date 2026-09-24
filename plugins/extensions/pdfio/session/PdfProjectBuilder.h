/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPROJECTBUILDER_H
#define PDFPROJECTBUILDER_H

#include "session/PdfSessionManifest.h"

#include <QString>

#include <kis_types.h>

class PdfRenderBackend;

/**
 * Turns one page record into the layer stack the user actually draws on.
 *
 * The PDF page becomes a **locked background layer**: it is the original artwork, not
 * something to paint over, and locking it is also what makes "my notes are always in the Ink
 * group" enforceable rather than a convention. Notes go into an empty "Ink" group above it,
 * which is the only layer the session ever writes.
 *
 * Top level of an image is the page, not the document: one KisDocument per page is an
 * accepted adaptation, and keeping the page as the image bounds means the PDF geometry and
 * Krita's own coordinate system are the same thing.
 */
class PdfProjectBuilder
{
public:
    /**
     * How many pixels one rendered page may take.
     *
     * Not a limit: no such ceiling exists in Krita, Qt or Android. It is derived from what the
     * device reports it has, because a page whose box is tagged at the wrong resolution can ask
     * for tens of megapixels, and on Android a bitmap comes out of the Java heap rather than out
     * of memory in general.
     */
    static qint64 maxPagePixels();

    static QString backgroundLayerName();
    static QString inkLayerName();

    /// Whether \a name is what backgroundLayerName() hands out: the layer the page's render is
    /// carried in, which a page artifact never persists because the source PDF and the manifest
    /// can put it back.
    static bool isPageRenderLayerName(const QString &name);

    /**
     * The paint layer inside the Ink group.
     *
     * A group layer cannot be painted on, so the group alone is not enough to draw into:
     * selecting "Ink" and trying to paint does nothing at all. The group ships with this paint
     * layer, and it is the node that should be active when the page opens.
     */
    static QString inkStrokeLayerName();
    static KisNodeSP inkStrokeLayer(const KisImageSP &image);

    /**
     * Renders a page and builds the image around it. Returns a null image and sets a why on
     * failure, for instance when the renderer cannot produce the page.
     */
    static KisImageSP buildPageImage(const PdfPageRecord &page,
                                     PdfRenderBackend &backend,
                                     qreal dpi,
                                     QString *why = nullptr);
};

#endif // PDFPROJECTBUILDER_H
