/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSTRIPLAYOUT_H
#define PDFSTRIPLAYOUT_H

#include "session/PdfSessionManifest.h"

#include <QList>
#include <QPointF>
#include <QRect>
#include <QSize>

/**
 * Where each page of a strip sits inside the strip image.
 *
 * Pure geometry, separated from everything that builds or paints, because this is the part of
 * design B that has to be right and the part that can be tested without a canvas.
 *
 * The strip is vertical and every page is centred in the same column, packed under the page above
 * it by its own height: the space between two pages is exactly the gap whatever their sizes are.
 * The image is the size THIS window needs -- the widest page it holds, and its own packed height --
 * because that is the only size being shown. It used to be the widest page of the WHOLE notebook
 * and the tallest window of the whole notebook, so that a window move could never change the
 * document's size: measured on the tablet, a notebook of 38 pages of five sizes (the widest from a
 * merged deck) was a 4644x14432 document holding a window of A4 pages, 1.6 GiB, and every roll
 * repainted every slot into those layers until the application was holding 2 GB. The roll now
 * RESIZES the document when the window it moves to needs a different size (see
 * PdfPageNavigator::rollToPage), so the constant size it used to depend on is not needed. For a
 * notebook whose pages are all one size every window is the same size, which is the common case and
 * the one that must never resize anything: the image is then exactly the pages plus one gap each.
 *
 * Pages are laid out in page order, so the strip reads downwards, and the window is centred on
 * the active page as far as the ends of the notebook allow.
 */
class PdfStripLayout
{
public:
    struct Slot {
        /// The page this slot holds, or -1 when the window runs off either end of the notebook.
        int page = -1;
        /// Where the page sits in the strip image, in pixels: the page's displayed box, which for
        /// a page set down at an angle is the rectangle that holds the turned sheet.
        QRect rect;

        /// The whole band this slot owns, page or no page: the page plus half the gap above and
        /// half below, so the bands meet in the middle of every gap; the last band runs to the
        /// bottom of the image so the bands cover it completely. Clearing a slot means clearing
        /// this, because the page that was there may have been larger than the one arriving.
        QRect cell;
    };

    PdfStripLayout() = default;

    /**
     * The window of \a scope slots centred on \a activePage.
     *
     * \a scope is forced odd so that the active page has as many pages behind it as ahead, which
     * is what lets it move within the window before the window has to roll.
     */
    static PdfStripLayout forWindow(const PdfSessionManifest &manifest,
                                    int activePage,
                                    int scope,
                                    qreal dpi);

    bool isValid() const;
    QSize imageSize() const;
    QList<Slot> slots() const;
    int activePage() const;
    int activeSlot() const;
    int slotForPage(int page) const;

    /// Which page a point of the strip image falls on, or -1 for a gap or an empty slot.
    int pageAt(const QPoint &point) const;

    /**
     * Which page is nearest to \a point, or -1 when the nearest one is further than
     * \a maxDistance.
     *
     * pageAt() is exact, which is right for "what is under the cursor" and wrong for "what is the
     * view resting on": the centre of the viewport spends real time in the gap between two pages,
     * and pageAt() answers -1 for all of it. A caller that arms a settle timer on that answer never
     * sees the page change, however long the view sits still -- the two-slot-wide answer is stable
     * instead: it is a function of the point alone, and every point of the gap belongs to the page
     * it is nearer to.
     *
     * \a pages and \a rects are parallel lists, the same shape PdfPageNavigator keeps as
     * m_stripPages and m_stripRects; a page of -1 is skipped. Ties inside the gap resolve to the
     * first slot in order, so the answer never depends on the order the pages were visited in --
     * a value that flickers between two pages resets a settle timer just as badly as one that is
     * always -1.
     */
    static int nearestPage(const QList<int> &pages, const QList<QRect> &rects,
                           const QPointF &point, qreal maxDistance,
                           int preferredPage = -1, qreal hysteresis = 0.0);


private:
    QList<Slot> m_slots;
    QSize m_imageSize;
    int m_activeSlot = -1;
    int m_activePage = -1;
};

#endif // PDFSTRIPLAYOUT_H
