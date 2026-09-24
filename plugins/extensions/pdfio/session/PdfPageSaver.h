/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGESAVER_H
#define PDFPAGESAVER_H

#include <QRect>
#include <QString>

#include <kis_types.h>

class KisDocument;
class KisImage;

/**
 * Writes one page of a project.
 *
 * The page BACKGROUND is never persisted. It is a full render of an A4 page -- several megabytes
 * per copy -- and it is derivable from the bundled source PDF, with manifest.json recording the
 * geometry needed to put it back underneath. Measured on the three page fixture: saving the
 * editing document costs 265,120 bytes, of which the page layer alone is 178,429; saving a
 * document that holds the page's own layers costs 37,254, and that number does not grow when the
 * source page gets heavier.
 *
 * Everything else IS persisted. That is what changed: the save used to take the contents of the
 * Ink group and nothing else -- the strip's single layer called "Ink" -- so a layer the user made
 * outside it was dropped without a word, and a group inside it lost its contents to a flat copy
 * that skipped anything which was not a paint layer. What an artifact holds now is the page's own
 * layers, whatever the user drew them on; what PdfInkLoader reads back is their flatten.
 *
 * The editing document cannot simply be saved, because it contains the background. Instead the
 * caller asks for a document of the page's own layers, saves that, and keeps it alive until the
 * save finishes: Krita saves in the background, and a nested event loop is not an option --
 * waiting for sigSavingFinished that way wedged on the second save.
 */
class PdfPageSaver
{
public:
    /**
     * Whether \a node is one of the layers the plugin rendered the source page into, and therefore
     * one of the ones a page artifact never carries.
     *
     * By name, not by the user-locked flag: the builders lock the paper, but so may the user, and a
     * layer that is locked because someone wanted a hand kept off it is still their work. The names
     * belong to the builders that write them -- PdfProjectBuilder::backgroundLayerName(),
     * PdfStripBuilder::backgroundLayerName() for any page, and the strip's desk.
     */
    static bool isPageBackground(const KisNodeSP &node);

    /**
     * Every layer of \a source that belongs to the page rather than to its render: the root
     * children that are not backgrounds. Contents of a group are carried by the copy, which keeps
     * the group.
     */
    static QList<KisNodeSP> pageLayers(const KisImageSP &source);

    /**
     * A document holding every layer of \a source that is not the page background, in the same
     * geometry and colour space. Returns nullptr and sets \a why on failure.
     *
     * The caller owns it and has to delete it once sigSavingFinished has arrived.
     */
    static KisDocument *createPageLayersDocument(const KisImageSP &source, QString *why = nullptr);

    /**
     * The same for one page of a strip: the page's own layers cropped to \a area and shifted back
     * to the origin, so the artifact is the page and nothing about the strip leaks into it.
     */
    static KisDocument *createPageLayersDocument(const KisImageSP &source,
                                                 const QRect &area,
                                                 QString *why = nullptr);

    /**
     * Saves asynchronously through KisDocument::saveAs, which writes no merged image on its own
     * terms but does honour the document contents. Returns false when the save could not start.
     */
    static bool saveDocument(KisDocument *document, const QString &path, QString *why = nullptr);
};

#endif // PDFPAGESAVER_H
