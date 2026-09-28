/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IMAGERENDERBACKEND_H
#define IMAGERENDERBACKEND_H

#include "backend/PdfRenderBackend.h"

/**
 * A PICTURE as the one page of a source: a scan, a photo, a screenshot the user brought in.
 *
 * An image page is not a special kind of page. It is a page whose source is one picture instead of
 * one PDF page, and giving it the same interface the PDF backends have is what makes the layout, the
 * strip, the ink, the previews, the crop and turn arithmetic and the export need no case of their
 * own -- the whole notebook already knows how to put a page's paper on screen.
 *
 * ONE PIXEL IS ONE POINT. A picture has pixels and a PDF page has points, and something has to say
 * how big the sheet is: an image file's own dpi is metadata a scan may or may not carry, and a page
 * sized from metadata nobody can see is a page whose size the user cannot predict. So the picture
 * becomes a page as many points across as it has pixels, and the user says how big it really is with
 * Scale and Box -- the two modes that already change a page's size.
 */
class ImageRenderBackend : public PdfRenderBackend
{
public:
    bool open(const QString &path) override;
    bool isOpen() const override;
    int pageCount() const override;
    PdfPageInfo pageInfo(int index) const override;
    QImage renderPage(int index, qreal dpi) const override;
    QString pageText(int index) const override;

    /// The picture as it was loaded, for a caller that wants the pixels and not a page.
    QImage picture() const { return m_picture; }

    /**
     * The page a picture of \a pixels becomes when it is said to have been SCANNED at \a dpi: the
     * physical size of that sheet, in points -- pixels / dpi * 72.
     *
     * This is what the import asks for when it offers "the size it was scanned at", because a scan
     * has no size of its own in the file that a person can see: 2480 x 3508 pixels is a number, and
     * "A4 at 300 dpi" is the same page. The ONE place the arithmetic lives, so the size the dialog
     * previews is the size the page is given.
     */
    static QSizeF pageSizeForScannedPixels(const QSize &pixels, int dpi);

    /**
     * The same for "a share of its natural size", natural being one pixel to one point: a picture
     * brought in at half size is half the sheet it would otherwise be.
     */
    static QSizeF pageSizeForNaturalShare(const QSize &pixels, int percent);

    /**
     * The SCALE a picture is imported at, which is what the notebook records: how big the page is
     * shown against the picture's own pixels, one pixel to one point being 1.0.
     *
     * It is the page's `extraScale` rather than its size for a reason the renderer settles: a page's
     * paper is rendered at `dpi * extraScale`, so a scan imported at 24% is a page that is 24 points
     * across for every 100 pixels and comes back at its OWN pixels when the reader is at 300 dpi --
     * sharp, because the picture was never resampled to get there. Writing the smaller size into
     * `sizePt` instead would leave the raster and the size the page declares disagreeing.
     */
    static qreal scaleForScannedDpi(int dpi);
    static qreal scaleForNaturalShare(int percent);

private:
    QImage m_picture;
    /// The page's own size in points: the picture's pixels, one for one. Kept rather than derived per
    /// call so the page cannot change size under a reader while the file stays the same.
    QSizeF m_sizePt;
};

#endif // IMAGERENDERBACKEND_H
