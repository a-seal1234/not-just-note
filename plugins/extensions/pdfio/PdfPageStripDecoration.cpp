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
        const QPixmap thumbnail(thumbPath);

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
}
