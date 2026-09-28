/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfSourceRenderers.h"

#include "backends/image/ImageRenderBackend.h"

#include <QDir>
#include <QPainter>
#include <QTransform>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

/**
 * \a rendered -- the source page as the renderer displays it, at \a sourceDpi -- reduced to
 * \a boxPt, the page's own rectangle in source points.
 *
 * The box is cut out of the raster rather than the raster scaled into it, because the two mean
 * opposite things: a crop removes what is outside the box, and a squeeze would put it back on the
 * paper in the wrong place. The part of the box that lies outside the sheet -- a margin the user
 * dragged outwards -- comes back as paper white, because the page really is bigger there; leaving it
 * transparent would show the desk through a page that has paper on it.
 *
 * The alpha channel is kept even though the box starts white: a turn that is not a right angle
 * exposes corners the sheet never covered, and turnedForDisplay() needs somewhere to put the
 * nothing that is really there.
 */
QImage PdfSourceRenderers::croppedToBox(const QImage &rendered, const QRectF &boxPt, qreal sourceDpi)
{
    const qreal perPoint = sourceDpi / 72.0;
    const QRect wanted(qRound(boxPt.x() * perPoint), qRound(boxPt.y() * perPoint),
                       qMax(1, qRound(boxPt.width() * perPoint)),
                       qMax(1, qRound(boxPt.height() * perPoint)));

    QImage cropped(wanted.size(), QImage::Format_ARGB32_Premultiplied);
    if (cropped.isNull()) {
        return QImage();
    }
    cropped.fill(Qt::white);

    const QRect fromSource = wanted.intersected(rendered.rect());
    if (!fromSource.isEmpty()) {
        QPainter painter(&cropped);
        painter.drawImage(QRect(fromSource.topLeft() - wanted.topLeft(), fromSource.size()),
                          rendered, fromSource);
    }
    return cropped;
}

PdfSourceRenderers::PdfSourceRenderers() = default;

PdfSourceRenderers::PdfSourceRenderers(Factory factory)
    : m_factory(std::move(factory))
{
}

PdfSourceRenderers::~PdfSourceRenderers()
{
    clear();
}

void PdfSourceRenderers::clear()
{
    qDeleteAll(m_backends);
    m_backends.clear();
}

int PdfSourceRenderers::openSourceCount() const
{
    return m_backends.size();
}

PdfRenderBackend *PdfSourceRenderers::forPage(const PdfSessionManifest &manifest,
                                             const QString &projectDir,
                                             int page,
                                             QString *why)
{
    if (page < 0 || page >= manifest.pages.size()) {
        fail(why, QStringLiteral("page %1 is not part of the notebook").arg(page + 1));
        return nullptr;
    }

    const PdfSourceRecord source = manifest.sourceForPage(manifest.pages.at(page));
    if (source.file.isEmpty()) {
        fail(why, QStringLiteral("page %1 does not record which PDF its background comes from")
                      .arg(page + 1));
        return nullptr;
    }

    return backendForFile(source, projectDir, why);
}

PdfPageInfo PdfSourceRenderers::pageInfo(const PdfSessionManifest &manifest,
                                         const QString &projectDir,
                                         int page,
                                         QString *why)
{
    if (page < 0 || page >= manifest.pages.size()) {
        fail(why, QStringLiteral("page %1 is outside the notebook").arg(page + 1));
        return PdfPageInfo();
    }

    /// A page with no source has its geometry in its own record: there is no file to ask, and asking
    /// one anyway is how a blank page would end up measured from somebody else's PDF.
    const PdfPageRecord &record = manifest.pages.at(page);
    if (record.isBlank()) {
        PdfPageInfo info;
        info.sizePt = record.sizePt;
        info.rotation = 0;
        return info;
    }

    PdfRenderBackend *backend = forPage(manifest, projectDir, page, why);
    if (!backend) {
        return PdfPageInfo();
    }
    return backend->pageInfo(record.index);
}

QImage PdfSourceRenderers::blankPage(const PdfPageRecord &record, qreal dpi)
{
    /// The same order as renderPage(): the notebook's own scale, then the box, then the turn.
    const qreal scale = record.extraScale > 0.0 ? record.extraScale : 1.0;
    const qreal sourceDpi = dpi * scale;
    const QRectF box = record.boxPt.isValid() ? record.boxPt : QRectF(QPointF(0, 0), record.sizePt);
    const qreal perPoint = sourceDpi / 72.0;

    QImage paper(QSize(qMax(1, qRound(box.width() * perPoint)),
                       qMax(1, qRound(box.height() * perPoint))),
                 QImage::Format_ARGB32_Premultiplied);
    if (paper.isNull()) {
        return QImage();
    }
    paper.fill(Qt::white);
    return turnedForDisplay(paper, record.extraRotation);
}

QImage PdfSourceRenderers::renderPage(const PdfSessionManifest &manifest,
                                      const QString &projectDir,
                                      int page,
                                      qreal dpi,
                                      QString *why)
{
    if (page < 0 || page >= manifest.pages.size()) {
        fail(why, QStringLiteral("page %1 is not part of the notebook").arg(page + 1));
        return QImage();
    }
    const PdfPageRecord &record = manifest.pages.at(page);

    /// A page with no source: its paper is WHITE PAPER of its own sheet, and there is no file to
    /// open. This is the ONLY place the reader's paper comes from -- the strip, the single page, the
    /// thumbnail and the exporter all come through here -- so it has to be answered BEFORE any
    /// backend is looked for. It was not, for one round: forPage() runs first, a blank page has no
    /// source to open, and the null render it returned left the strip's band the colour of the desk
    /// (a grey sheet where the user's blank page should be) and the page with no preview at all.
    if (record.isBlank()) {
        return blankPage(record, dpi);
    }

    PdfRenderBackend *backend = forPage(manifest, projectDir, page, why);
    if (!backend) {
        return QImage();
    }

    /// A scaled page is rendered at a larger SOURCE dpi and never upscaled: the same source at more
    /// dpi is more pixels of the same page, which is what keeps the pen's ink sharp and what makes
    /// the memory budget the thing that decides how far a scale can go. turnedSize() is linear in
    /// its argument, so the turn below still lands the page in exactly displaySizePt() * dpi / 72.
    const qreal scale = record.extraScale > 0.0 ? record.extraScale : 1.0;
    const qreal sourceDpi = dpi * scale;

    const QImage rendered = backend->renderPage(record.index, sourceDpi);
    if (rendered.isNull() || !record.boxPt.isValid()) {
        /// The whole sheet is the common case and stays byte for byte what it was.
        return turnedForDisplay(rendered, record.extraRotation);
    }
    return turnedForDisplay(croppedToBox(rendered, record.boxPt, sourceDpi), record.extraRotation);
}

QImage PdfSourceRenderers::turnedForDisplay(const QImage &rendered, int extraRotation)
{
    if (rendered.isNull() || extraRotation == 0) {
        return rendered;
    }

    /// FastTransformation for a right angle: that is exactly a transpose of the pixels, so the
    /// smooth filter would blur the page for nothing. Any other angle is resampled, because the
    /// alternative is a page whose own edges are stairs -- and on a sheet of paper that is the one
    /// thing the eye finds immediately. The same call turns the artifact's layers, so the two agree
    /// about which way is clockwise.
    const int turn = ((extraRotation % 360) + 360) % 360;
    const bool rightAngle = turn % 90 == 0;
    const Qt::TransformationMode mode =
        rightAngle ? Qt::FastTransformation : Qt::SmoothTransformation;

    /// A turn that is not a right angle exposes corners the sheet never covered, and transformed()
    /// fills those with zero. In an opaque format zero is BLACK -- a page set down at an angle would
    /// come back with black triangles at its corners instead of the nothing that is really there.
    /// Given an alpha channel first, so the paper's corners are as empty as the artifact's, which is
    /// turned the same way and does have one. A right angle exposes nothing and is left alone: it
    /// stays byte-identical, which is what the tests over the transposed page assert.
    QImage source = rendered;
    if (!rightAngle && !source.hasAlphaChannel()) {
        source = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    QImage turned = source.transformed(QTransform().rotate(turn), mode);

    /// The transform sizes the result as the bounding box rounded outwards, which lands a pixel or
    /// two past the box the record, the layout, the artifact and the exporter all measure. Rescaled
    /// to that box rather than left as it comes: a raster 2729 wide in a slot 2728 wide is drawn
    /// past its own rectangle, and every later reader of the size would have to know which is which.
    /// A right angle is already exact and is left alone -- byte-identical, which is what the tests
    /// over the transposed page assert.
    const QSizeF box = PdfPageRecord::turnedSize(QSizeF(rendered.size()), turn);
    const QSize wanted(qRound(box.width()), qRound(box.height()));
    if (!wanted.isEmpty() && turned.size() != wanted) {
        turned = turned.scaled(wanted, Qt::IgnoreAspectRatio, mode);
    }

    return turned;
}

PdfRenderBackend *PdfSourceRenderers::backendForFile(const PdfSourceRecord &source,
                                                     const QString &projectDir,
                                                     QString *why)
{
    /// The same relative name can identify different files in different notebook directories. Include
    /// the recorded content hash as well, so replacing a source at that path cannot reuse an open
    /// backend for its previous contents.
    const QString sourceFile = source.file;
    const QString cacheKey = QDir(projectDir).absoluteFilePath(sourceFile)
        + QLatin1Char('\n') + QString::fromLatin1(source.sha256);
    PdfRenderBackend *backend = m_backends.value(cacheKey, nullptr);
    if (backend && backend->isOpen()) {
        return backend;
    }

    if (!backend) {
        /// A picture is one page and needs no platform renderer: the same file, read the same way,
        /// on every platform this runs on. A PDF needs the one this build has.
        if (source.isImage()) {
            backend = new ImageRenderBackend();
        } else {
            if (!m_factory) {
                fail(why, QStringLiteral("this notebook has no renderer for %1").arg(sourceFile));
                return nullptr;
            }
            backend = m_factory();
            if (!backend) {
                fail(why, QStringLiteral("no PDF render backend on this platform"));
                return nullptr;
            }
        }
        m_backends.insert(cacheKey, backend);
    }

    /// A backend whose source has gone (or whose open failed earlier) is opened again rather than
    /// thrown away: the file may be back, and a stale handle would answer with nothing.
    if (!backend->open(QDir(projectDir).filePath(sourceFile))) {
        fail(why, source.isImage()
                     ? QStringLiteral("the picture %1 cannot be read").arg(sourceFile)
                     : QStringLiteral("the renderer cannot open %1").arg(sourceFile));
        return nullptr;
    }

    return backend;
}
