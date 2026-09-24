/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSTRIPBUILDER_H
#define PDFSTRIPBUILDER_H

#include "session/PdfStripLayout.h"

#include <QString>

#include <kis_types.h>

class PdfRenderBackend;

/**
 * Builds the strip image: several pages in one document, which is design B.
 *
 * Each slot gets a locked background layer holding the page, and an Ink group of its own. Every
 * slot but the active one is locked, which is how "the neighbouring pages are not yours to draw
 * on" is enforced rather than merely implied: Krita refuses the stroke.
 *
 * The image is the layout's size and never changes for a given scope, so making another page
 * active is unlocking a group and asking for it, not building a document.
 */
class PdfStripBuilder
{
public:
    struct Strip {
        KisImageSP image;

        /// The paper layer of each slot, in slot order. Held so that rolling the window can
        /// repaint the one slot that changes without building the strip again.
        QList<KisNodeSP> paperLayers;
        /// The paint layer inside the active slot's Ink group, to be the active node.
        KisNodeSP activeInkLayer;
        PdfStripLayout layout;
    };

    /**
     * \a projectDir is where the page artifacts live; a page that has been drawn on before has its
     * ink put back, and one that has not is left blank.
     */
    static Strip build(const PdfSessionManifest &manifest,
                       int activePage,
                       int scope,
                       qreal dpi,
                       PdfRenderBackend &backend,
                       const QString &projectDir,
                       QString *why = nullptr);

    /// The name of the Ink group of a slot, so a page can be found again inside the strip.
    static QString inkGroupName(int page);
    static QString backgroundLayerName(int page);
    static QString inkLayerName(int page);

    /// Whether \a name belongs to the room a slot's page sits in rather than to the page: the desk
    /// the strip is laid out on, or one of the renders. What a page artifact never persists.
    static bool isPageRenderLayerName(const QString &name);
};

#endif // PDFSTRIPBUILDER_H
