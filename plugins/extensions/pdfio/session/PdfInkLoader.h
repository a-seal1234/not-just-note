/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFINKLOADER_H
#define PDFINKLOADER_H

#include <QImage>
#include <QList>
#include <QPair>
#include <QString>

#include <kis_types.h>

class KisDocument;

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
     * One layer of an artifact, as the picture it is.
     *
     * \a pixels covers the page area the layer's own \a x and \a y place it at, the way the artifact
     * writes it; \a opacity is the layer's own. This is what a rewrite that only MOVES pixels needs:
     * a crop takes a rectangle out of each picture, and nothing about it needs a document graph.
     */
    struct ArtifactLayer {
        QString name;
        QImage pixels;
        qreal opacity = 1.0;
        int x = 0;
        int y = 0;
    };

    /**
     * The artifact's layers as pictures and the size of the page they cover, read without opening a
     * document where the PNG sidecar is there.
     *
     * A crop has to know both before it can cut anything, and the paths that answer it by building a
     * Krita document -- the read, the clone, cropImage(), the packing pass -- each hold another whole
     * page and then wait on the update scheduler for it, which on the GUI thread is a busy-wait
     * dialog and, on the tablet, minutes. An artifact written before the sidecar existed still
     * reads: it falls back to the document, and says so by the size it hands back.
     */
    static QList<ArtifactLayer> loadLayersForRewrite(const QString &kraPath, QSize *artifactSize,
                                                     QString *why = nullptr);
    /**
     * The ink of \a kraPath, or a null image when the page has none. \a why is set only on a
     * real failure, not for a page that was simply never drawn on.
     */
    static QImage loadInk(const QString &kraPath, QString *why = nullptr);

    /**
     * The pixel size of the page an artifact was written for, or an empty size when there is no
     * artifact or it cannot be read.
     *
     * A rotation needs this before it loads anything: the image it turns has to be built at the
     * size the artifact already has. The merged image is no way to ask -- an ink-only artifact
     * need not carry one.
     */
    static QSize artifactSize(const QString &kraPath, QString *why = nullptr);

    /**
     * The artifact as the document graph it holds, read ONCE, owned by \a document.
     *
     * \a document must be a document of our own -- KisPart::instance()->createDocument() -- and the
     * image it hands back dies with it, so it has to outlive the image.
     *
     * This is what a page rewritten in place -- turned, clipped -- loads with. Asking artifactSize()
     * and then loadInkLayersInto() reads the whole artifact twice and holds two copies of every
     * layer while it does, and on the tablet that memory ended in an OOM (2026-09-28). A null image
     * means there is nothing to rewrite; \a why carries the reason when there was one.
     */
    static KisImageSP loadArtifactInto(KisDocument *document, const QString &kraPath,
                                       QString *why = nullptr);

    /**
     * The artifact as a page image of its own, belonging to \a document, read ONCE.
     *
     * This is loadInkLayersInto() with the size taken from the same read. A page rewritten in place
     * has to know the frame it is in before it can crop it, and asking artifactSize() first meant
     * decoding the whole artifact twice -- a page per layer per read, on a tablet that then ran out
     * of memory (2026-09-28) -- for one answer the same read already had.
     *
     * The layers come back as layers, cloned into the new image: the file's own copy is released
     * before this returns, and what the caller gets is its own image, so nothing it does afterwards
     * waits on the loader.
     */
    static KisImageSP loadArtifactAsPage(KisDocument *document, const QString &kraPath,
                                         QString *why = nullptr);

    /**
     * The size the artifact's merged image announces, read from the PNG's own header inside the
     * archive: no document, no layer graph, no decode.
     *
     * A page write proves what it wrote before the new file takes the old one's place, and the
     * obvious way to prove it -- artifactSize() -- decodes the file again, a whole page per layer,
     * for a question its first 24 bytes answer. Empty means "this archive cannot tell"; a caller
     * that needs an answer falls back to the full read.
     */
    static QSize artifactSizeFromArchive(const QString &kraPath);

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

    /**
     * One entry per layer of the artifact: the layer's own name and its pixels over the page's
     * area, in the order they are stacked. Empty when the page has no artifact or it cannot be
     * read as a document.
     *
     * This is what the strip reads back with: its content lives in one layer per kind, spanning
     * the whole strip, so a page is the part of each of those layers inside that page's rectangle
     * and the reload puts every entry back into the layer of the same name.
     */
    static QList<QPair<QString, QImage>> loadInkLayers(const QString &kraPath,
                                                       QString *why = nullptr);

    /**
     * The same list, read from the PNG sidecar an artifact is saved with -- one image per layer,
     * with no document opened at all.
     *
     * This is what the strip reads with. Opening a document per page is what ends in a tombstone on
     * the tablet during a window move (five of them per move); a PNG is a QImage and nothing else.
     * Falls back to loadInkLayers() when there is no sidecar, so an artifact written before this
     * existed still reads.
     */
    static QList<QPair<QString, QImage>> loadInkLayersFromSidecar(const QString &kraPath,
                                                                  QString *why = nullptr);

private:
    PdfInkLoader() = delete;
};

#endif // PDFINKLOADER_H
