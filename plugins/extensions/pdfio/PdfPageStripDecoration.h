/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFPAGESTRIPDECORATION_H
#define PDFPAGESTRIPDECORATION_H

#include <QDateTime>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QString>

#include <kis_canvas_decoration.h>
#include <kis_types.h>

class KisView;

/**
 * Shows the pages either side of the open one, above and below it, so the notebook reads as a
 * continuous strip instead of a single sheet with nothing around it.
 *
 * A decoration rather than part of the image, and that is the whole point: a Krita document holds
 * one page at one size, and pages here are of different sizes, so the neighbours cannot live in
 * the image without breaking the one invariant everything else rests on -- that the image is the
 * page. Painted in widget space over the canvas, they cost a scaled pixmap each instead of a
 * document, and the areas they occupy are not part of the image, so they cannot be drawn on.
 *
 * Each neighbour's pixmap is decoded once and kept until the file it came from changes. This is a
 * paint callback, so reading the PNG in it re-decoded up to 3.7 MB per neighbour per repaint.
 *
 * The thumbnails are the ones written when a page is saved. The page that has never been drawn on
 * has none yet, and shows as an empty sheet.
 *
 * Measured, and disappointing: at a working zoom the page is far larger than the viewport -- 81978
 * by 116000 widget pixels against a 644 by 580 viewport in one measurement, about fifty times -- so
 * the neighbours are tens of thousands of pixels away and the canvas will not pan there. It is
 * correct and it costs nothing, and it is of little use until the page is small enough to leave
 * room around it. Design B is the answer to the same want: with the neighbours inside the image,
 * scrolling reaches them. See docs/PDFIO-DESIGN-STRIP.md.
 */
class PdfPageStripDecoration : public KisCanvasDecoration
{
    Q_OBJECT
public:
    PdfPageStripDecoration(const QString &id, QPointer<KisView> parent);

protected:
    void drawDecoration(QPainter &gc,
                        const QRectF &updateArea,
                        const KisCoordinatesConverter *converter,
                        KisCanvas2 *canvas) override;

private:
    /// The blank space between two pages, in widget pixels, so it stays proportionate on screen
    /// however far the canvas is zoomed.
    static constexpr int Gap = 16;

    /// How much of the neighbouring page is shown before the strip is cut off. A page taller than
    /// this would otherwise push the strip past the widget and be mostly wasted.
    static constexpr int MaxNeighbourHeight = 1400;

    /// One neighbour's decoded preview, and what the file looked like when it was decoded.
    struct CachedPreview {
        QPixmap pixmap;
        QDateTime modified;
        qint64 bytes = 0;
    };

    /**
     * The preview at \a path, decoded only when it is not here already or the file has changed.
     *
     * drawDecoration() is a paint callback: it used to call QPixmap(path) for both neighbours on
     * every repaint of the canvas, which is 184 KB of PNG decode per neighbour per frame at the old
     * preview size and about 3.7 MB at the new one, on a tablet. What is left per frame is one stat
     * per neighbour; the decode happens when the path changes, or when the file's modification time
     * or size does. A preview rewritten within the same second and with a byte-identical size is
     * served from the cache for that frame -- the filesystem's timestamp resolution is the limit
     * here, and this strip is a hint around the open page rather than the page itself.
     */
    QPixmap previewFor(const QString &path);

    /**
     * The previews of the pages beside this one, keyed by path.
     *
     * The decoration draws exactly two neighbours, so this is pruned to the ones the last paint
     * actually drew: a tablet holds two decoded pages and not the whole notebook's worth. The
     * decoration itself is built once per document (PdfPageNavigator::showImage), so a notebook
     * change is a new decoration with an empty map.
     */
    QHash<QString, CachedPreview> m_previews;
};

#endif // PDFPAGESTRIPDECORATION_H
