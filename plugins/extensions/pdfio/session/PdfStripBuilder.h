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

class PdfSourceRenderers;

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
     *
     * \a renderers is what turns a slot into a render: it resolves the page's own source
     * (PdfPageRecord::source) and its own page inside that source (PdfPageRecord::index), which is
     * not the slot's position the moment pages have been moved, deleted or inserted.
     */
    static Strip build(const PdfSessionManifest &manifest,
                       int activePage,
                       int scope,
                       qreal dpi,
                       PdfSourceRenderers &renderers,
                       const QString &projectDir,
                       QString *why = nullptr);

    /// The name of the Ink group of a slot, so a page can be found again inside the strip.
    static QString inkGroupName(int page);

    /// The locked layer a slot's page is rendered into, named after the page's SOURCE page number
    /// and not its position in the notebook: a locked "PDF page N" that changed when the notebook
    /// was reordered would name a different sheet than the one it holds, while the single-page
    /// builder has always named it from the record.
    static QString backgroundLayerName(int page);
    static QString inkLayerName(int page);

    /// Whether \a name belongs to the room a slot's page sits in rather than to the page: the desk
    /// the strip is laid out on, or one of the renders. What a page artifact never persists.
    static bool isPageRenderLayerName(const QString &name);
};

#endif // PDFSTRIPBUILDER_H
