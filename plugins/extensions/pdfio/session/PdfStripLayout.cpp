/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfStripLayout.h"

#include <QtMath>

#include <limits>

namespace {

/// The blank space between two slots, in image pixels. Part of the cell, so it does not change the
/// image size when the window rolls.
constexpr int SlotGap = 112;

QSize pageSizeInPixels(const PdfSessionManifest &manifest, const PdfPageRecord &page, qreal dpi)
{
    Q_UNUSED(manifest);
    /// displaySizePt(), not sizePt: a page the notebook itself has turned occupies its turned
    /// shape on the strip. The two are the same until a rotate is recorded, which is exactly why
    /// the layout must ask the question that already knows about it. A right angle swaps the sides
    /// exactly; any other angle gives the bigger rectangle the sheet fits in once it is turned, and
    /// the slot has to be that rectangle rather than sizePt's or the page's own corners fall
    /// outside the strip image.
    const QSizeF visible = page.displaySizePt();
    if (!visible.isValid()) {
        return QSize();
    }
    /// Rounded, not ceilinged, because that is what the renderers actually produce: a 300 point
    /// page at 200 dpi comes back as 833 pixels, not 834. Deriving a raster size from pt/72*dpi
    /// instead of asking the renderer is the mistake this project already made once.
    return QSize(qRound(visible.width() * dpi / 72.0),
                 qRound(visible.height() * dpi / 72.0));
}

} // namespace

PdfStripLayout PdfStripLayout::forWindow(const PdfSessionManifest &manifest,
                                         int activePage,
                                         int scope,
                                         qreal dpi)
{
    PdfStripLayout layout;

    if (!manifest.isValid() || manifest.pages.isEmpty() || dpi <= 0) {
        return layout;
    }
    if (activePage < 0 || activePage >= manifest.pages.size()) {
        return layout;
    }

    /// Odd, and at least one.
    scope = qMax(1, scope);
    if (scope % 2 == 0) {
        ++scope;
    }
    scope = qMin(scope, manifest.pages.size());

    const int half = scope / 2;

    /// The window is centred on the active page as far as the ends of the notebook allow.
    int first = activePage - half;
    first = qBound(0, first, qMax(0, manifest.pages.size() - scope));
    const int last = qMin(manifest.pages.size(), first + scope);

    /// The image is the size THIS window needs: the widest page it holds, and its own packed height.
    ///
    /// It used to be the widest page of the WHOLE notebook and the tallest window of the whole
    /// notebook, so that the size would never change and the roll could refuse a differently sized
    /// window instead of resizing the document. That rule is gone: the window that is up is the only
    /// one being shown, and paying for the widest page a merged deck brought in made the document --
    /// and the desk, the paper and every content layer in it -- 4644x14432 px for a window of A4
    /// pages, measured on the tablet at 1.6 GiB and 2 GB of PSS. The roll resizes the document when
    /// it moves to a window of another size (PdfPageNavigator::rollToPage), so the size is free to
    /// be the window's own.
    ///
    /// One gap of slack under the last page, on top of the gaps the pages carry, so the last page's
    /// half-gap band and the bottom edge of the image are not the same line. For a notebook whose
    /// pages are all one size this is exactly sum(h_i + SlotGap) + SlotGap, which is what it always
    /// was: the common case does not change size from one window to the next, and so never resizes.
    QList<QSize> pageSizes;
    int cellWidth = 0;
    int imageHeight = SlotGap;
    for (int page = first; page < last; ++page) {
        const QSize size = pageSizeInPixels(manifest, manifest.pages.at(page), dpi);
        pageSizes.append(size);
        cellWidth = qMax(cellWidth, size.width());
        imageHeight += size.height() + SlotGap;
    }
    if (cellWidth <= 0 || pageSizes.isEmpty()) {
        return layout;
    }

    /// Each page is placed by its own height: y_0 = 0 and y_i = y_{i-1} + h_{i-1} + SlotGap.
    ///
    /// Every page used to be centred inside one cell sized for the largest page in the notebook,
    /// so the space between a small page and a large one was SlotGap plus the centring slack of
    /// both -- a small page sat in a hole, which is what the user saw. Packed by their own heights,
    /// every consecutive pair is separated by exactly SlotGap whatever the two pages' sizes are.
    /// Horizontally the pages are centred in the column the WINDOW needs, which is the widest page
    /// this window holds.
    const int halfGap = SlotGap / 2;
    int y = 0;
    for (int i = 0; i < pageSizes.size(); ++i) {
        Slot slot;
        slot.page = first + i;

        const QSize pageSize = pageSizes.at(i);

        /// Centred horizontally. Vertically there is no cell to centre in: the page sits at the top
        /// of its own band, which is what makes the gaps equal.
        const int x = (cellWidth - pageSize.width()) / 2;
        slot.rect = QRect(x, y, pageSize.width(), pageSize.height());

        /// The band the roll wipes and repaints: the page plus half a gap above and below, so the
        /// bands meet in the middle of every gap. The last band runs to the bottom of the image,
        /// absorbing the one gap of slack under it: the roll clears what it repaints band by band,
        /// so the bands have to cover every pixel of the image and no window may leave stale pixels
        /// below its last page.
        int cellBottom = y + pageSize.height() + halfGap;
        if (i == pageSizes.size() - 1) {
            cellBottom = imageHeight;
        }
        slot.cell = QRect(0, y - halfGap, cellWidth, cellBottom - (y - halfGap));

        y += pageSize.height() + SlotGap;

        if (slot.page == activePage) {
            layout.m_activeSlot = i;
        }

        layout.m_slots.append(slot);
    }

    layout.m_activePage = activePage;
    layout.m_imageSize = QSize(cellWidth, imageHeight);
    return layout;
}

bool PdfStripLayout::isValid() const
{
    return !m_slots.isEmpty() && m_imageSize.isValid() && m_activeSlot >= 0;
}

QSize PdfStripLayout::imageSize() const
{
    return m_imageSize;
}

QList<PdfStripLayout::Slot> PdfStripLayout::slots() const
{
    return m_slots;
}

int PdfStripLayout::activePage() const
{
    return m_activePage;
}

int PdfStripLayout::activeSlot() const
{
    return m_activeSlot;
}

int PdfStripLayout::slotForPage(int page) const
{
    for (int i = 0; i < m_slots.size(); ++i) {
        if (m_slots.at(i).page == page) {
            return i;
        }
    }
    return -1;
}

int PdfStripLayout::pageAt(const QPoint &point) const
{
    for (const Slot &slot : m_slots) {
        if (slot.page >= 0 && slot.rect.contains(point)) {
            return slot.page;
        }
    }
    return -1;
}

int PdfStripLayout::nearestPage(const QList<int> &pages, const QList<QRect> &rects,
                                const QPointF &point, qreal maxDistance,
                                int preferredPage, qreal hysteresis)
{
    int best = -1;
    qreal bestDistance = maxDistance;
    qreal preferredDistance = std::numeric_limits<qreal>::max();

    for (int slot = 0; slot < pages.size() && slot < rects.size(); ++slot) {
        if (pages.at(slot) < 0) {
            continue;
        }

        const QRectF rect(rects.at(slot));
        if (!rect.isValid()) {
            continue;
        }

        /// Distance from the point to the rectangle, zero inside it. A point beside the strip is
        /// measured to the corner, which is what makes the answer continuous as the centre moves.
        const qreal dx = qMax(qMax(rect.left() - point.x(), qreal(0)), point.x() - rect.right());
        const qreal dy = qMax(qMax(rect.top() - point.y(), qreal(0)), point.y() - rect.bottom());
        const qreal distance = qSqrt(dx * dx + dy * dy);

        if (pages.at(slot) == preferredPage) {
            preferredDistance = distance;
        }

        /// Strictly nearer, so the first slot keeps a tie: the answer must be a function of the
        /// point alone, not of the order the pages happen to be visited in.
        if (distance < bestDistance) {
            bestDistance = distance;
            best = pages.at(slot);
        }
    }

    /// Hysteresis: while the page that is already open is within \a hysteresis of the nearest one,
    /// it keeps the answer. Without it a centre resting exactly on the line where the two pages
    /// are equally near flips from tick to tick as the scroll position rounds, and a value that
    /// flips resets a settle timer as effectively as being -1 does.
    if (preferredPage >= 0 && preferredDistance < std::numeric_limits<qreal>::max()
        && preferredDistance <= bestDistance + hysteresis) {
        return preferredPage;
    }

    return best;
}
