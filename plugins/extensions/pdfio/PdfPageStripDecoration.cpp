/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfPageStripDecoration.h"
#include "PdfPageNavigator.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QWidget>
#include <QPainter>
#include <QPixmap>

#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>
#include <kis_canvas_widget_base.h>
#include <kis_image.h>
#include <kis_paint_device.h>

#include <KisDocument.h>
#include <KisView.h>

PdfPageStripDecoration::PdfPageStripDecoration(const QString &id, QPointer<KisView> parent)
    : KisCanvasDecoration(id, parent)
{
    /// The base class starts hidden and paint() returns immediately for a decoration that is not
    /// visible, so a decoration that never says otherwise draws nothing at all. That is exactly
    /// what this one did until it was noticed that no thumbnails were appearing on the canvas.
    setVisible(true);
}

void PdfPageStripDecoration::drawDecoration(QPainter &gc,
                                            const QRectF &updateArea,
                                            const KisCoordinatesConverter *converter,
                                            KisCanvas2 *canvas)
{
    Q_UNUSED(updateArea);
    Q_UNUSED(canvas);

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!converter || !navigator->hasNotebook()) {
        return;
    }

    KisDocument *document = navigator->currentDocument();
    if (!document || !document->image()) {
        return;
    }

    const PdfSessionManifest &manifest = navigator->manifest();
    const int index = navigator->currentIndex();
    if (index < 0 || index >= manifest.pages.size()) {
        return;
    }

    KisImageSP image = document->image();

    /// The open page's own geometry, in the document's pixels. Everything else is derived from it,
    /// because the manifest speaks in points and the canvas in pixels.
    const QRectF pageInDocument(0, 0, image->width(), image->height());
    /// displaySizePt(), not sizePt: the document holds the page as the reader shows it, and a page
    /// set down at an angle is its bounding box rather than the source's rectangle. Dividing the
    /// image by the unturned width would scale every angled page's neighbour up.
    const QSizeF pageBoxPt = manifest.pages.at(index).displaySizePt();
    const qreal pixelsPerPoint = pageBoxPt.width() > 0 ? image->width() / pageBoxPt.width() : 1.0;

    /// The zoom is read off the page's own on-screen width rather than asked for: the converter
    /// exposes setZoom and clampZoom but no accessor, and the ratio is right here anyway.
    const QRectF pageInWidget = converter->documentToWidget(pageInDocument);
    const qreal zoom = pageInDocument.width() > 0 ? pageInWidget.width() / pageInDocument.width() : 1.0;

    const QDir project(navigator->projectDir());

    /// Which previews this paint actually used, so the cache can be pruned to them at the end.
    QSet<QString> drawnPreviews;

    for (int direction : { -1, 1 }) {
        const int other = index + direction;
        if (other < 0 || other >= manifest.pages.size()) {
            continue;
        }

        const QSizeF neighbourPixels = manifest.pages.at(other).displaySizePt() * pixelsPerPoint;
        if (neighbourPixels.isEmpty()) {
            continue;
        }

        /// Directly below the page for the next one and directly above for the previous, with the
        /// gap measured in widget pixels so it does not grow with zoom.
        const qreal gapInDocument = Gap / qMax(qreal(0.0001), zoom);
        const qreal top = direction > 0
            ? pageInDocument.bottom() + gapInDocument
            : pageInDocument.top() - gapInDocument - neighbourPixels.height();

        QRectF neighbourInWidget = converter->documentToWidget(
            QRectF(pageInDocument.left(), top, neighbourPixels.width(), neighbourPixels.height()));

        /// Only the top part of a tall neighbour is worth drawing.
        if (neighbourInWidget.height() > MaxNeighbourHeight) {
            neighbourInWidget.setHeight(MaxNeighbourHeight);
        }

        const QString thumbPath = project.filePath(manifest.pages.at(other).thumbFile);
        const QPixmap thumbnail = previewFor(thumbPath);
        drawnPreviews.insert(thumbPath);

        if (thumbnail.isNull()) {
            /// Never drawn on, so there is nothing to show: an empty sheet, so the strip still
            /// reads as pages rather than as a gap.
            gc.fillRect(neighbourInWidget, QColor(255, 255, 255, 235));
        } else {
            gc.drawPixmap(neighbourInWidget, thumbnail, QRectF(thumbnail.rect()));
        }

        gc.setPen(QPen(QColor(120, 120, 120, 180), 1, Qt::DashLine));
        gc.drawRect(neighbourInWidget.adjusted(0.5, 0.5, -0.5, -0.5));

        gc.setPen(QColor(90, 90, 90, 220));
        gc.drawText(neighbourInWidget.adjusted(6, 4, -6, -4),
                    Qt::AlignTop | Qt::AlignLeft,
                    QStringLiteral("Page %1").arg(other + 1));
    }

    /// And forget the previews of the pages that are no longer beside this one. The decoration
    /// draws two neighbours, so anything else held here is a decoded page a tablet should not be
    /// carrying: turning through a notebook would otherwise keep two more per page.
    for (auto it = m_previews.begin(); it != m_previews.end();) {
        if (drawnPreviews.contains(it.key())) {
            ++it;
        } else {
            it = m_previews.erase(it);
        }
    }
}

QPixmap PdfPageStripDecoration::previewFor(const QString &path)
{
    const QFileInfo info(path);
    const QDateTime modified = info.lastModified();
    const qint64 bytes = info.size();

    const auto known = m_previews.constFind(path);
    if (known != m_previews.constEnd() && known->modified == modified && known->bytes == bytes) {
        return known->pixmap;
    }

    CachedPreview entry;
    /// A null pixmap is cached too, for a page that has never been drawn on: the caller draws an
    /// empty sheet for it, and re-reading the missing file on every frame would buy nothing.
    entry.pixmap = QPixmap(path);
    entry.modified = modified;
    entry.bytes = bytes;
    m_previews.insert(path, entry);
    return entry.pixmap;
}
