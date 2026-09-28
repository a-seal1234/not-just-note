/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "backends/image/ImageRenderBackend.h"

#include <QFileInfo>

QSizeF ImageRenderBackend::pageSizeForScannedPixels(const QSize &pixels, int dpi)
{
    if (pixels.isEmpty() || dpi <= 0) {
        return QSizeF();
    }
    const qreal perPixel = 72.0 / dpi;
    return QSizeF(pixels.width() * perPixel, pixels.height() * perPixel);
}

QSizeF ImageRenderBackend::pageSizeForNaturalShare(const QSize &pixels, int percent)
{
    if (pixels.isEmpty() || percent <= 0) {
        return QSizeF();
    }
    const qreal share = percent / 100.0;
    return QSizeF(pixels.width() * share, pixels.height() * share);
}

qreal ImageRenderBackend::scaleForScannedDpi(int dpi)
{
    /// 72 dpi is one pixel to one point, so anything finer is a smaller page: a 300 dpi scan is
    /// 72/300 = 0.24 of the sheet its pixels would otherwise make.
    return dpi > 0 ? 72.0 / dpi : 0.0;
}

qreal ImageRenderBackend::scaleForNaturalShare(int percent)
{
    return percent > 0 ? percent / 100.0 : 0.0;
}

bool ImageRenderBackend::open(const QString &path)
{
    m_picture = QImage();
    m_sizePt = QSizeF();

    if (!QFileInfo::exists(path)) {
        return false;
    }

    /// Through QImage, which reads what the platform's own image plugins read: a scan is a PNG or a
    /// JPEG and needs no dependency of its own. A file that is not a picture comes back null, and a
    /// null picture is a refusal rather than a blank page -- an import that quietly produced a white
    /// sheet where the user's scan should be is the kind of loss this project keeps designing out.
    QImage loaded(path);
    if (loaded.isNull()) {
        return false;
    }

    /// One pixel is one point (see the header). An image with an alpha channel keeps it: a PNG of a
    /// cut-out drawing is placed on the page, and the page's own paper stays white behind it.
    m_picture = loaded;
    m_sizePt = QSizeF(loaded.width(), loaded.height());
    return true;
}

bool ImageRenderBackend::isOpen() const
{
    return !m_picture.isNull();
}

int ImageRenderBackend::pageCount() const
{
    /// One picture is one page. A multi-page format (a TIFF stack, an animated GIF) is read as its
    /// first frame rather than grown into several pages nothing in the notebook asked for.
    return m_picture.isNull() ? 0 : 1;
}

PdfPageInfo ImageRenderBackend::pageInfo(int index) const
{
    PdfPageInfo info;
    if (m_picture.isNull() || index != 0) {
        return info;
    }
    info.index = 0;
    info.sizePt = m_sizePt;
    /// A picture has no /Rotate: what the file shows is what it is, and a turn of the page is the
    /// notebook's own extraRotation, recorded in the manifest like any other page's.
    info.rotation = 0;
    return info;
}

QImage ImageRenderBackend::renderPage(int index, qreal dpi) const
{
    const PdfPageInfo info = pageInfo(index);
    if (!info.isValid() || dpi <= 0.0) {
        return QImage();
    }

    /// The page's pixels at the resolution asked for: one point per pixel at 72 dpi, so the factor is
    /// dpi/72 and a page is never rendered at a size its own record does not describe. Read back as
    /// the returned image's size, the way every other renderer's is (PdfRenderBackend.h).
    const qreal perPoint = dpi / 72.0;
    const QSize wanted(qMax(1, qRound(info.sizePt.width() * perPoint)),
                       qMax(1, qRound(info.sizePt.height() * perPoint)));
    if (m_picture.size() == wanted) {
        /// Already the right size: handed over as it is, so a page shown at the resolution it was
        /// scanned at is the scan and not a resample of it.
        return m_picture;
    }
    return m_picture.scaled(wanted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QString ImageRenderBackend::pageText(int index) const
{
    /// A picture has no text layer. Empty rather than refused: the exporter's text check is about
    /// PDFs it re-writes, and it must not read "no text" as "the export destroyed the text".
    Q_UNUSED(index);
    return QString();
}
