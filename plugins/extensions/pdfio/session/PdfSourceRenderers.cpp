/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "session/PdfSourceRenderers.h"

#include <QDir>
#include <QTransform>

namespace {

void fail(QString *why, const QString &message)
{
    if (why) {
        *why = message;
    }
}

} // namespace

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

    return backendForFile(source.file, projectDir, why);
}

PdfPageInfo PdfSourceRenderers::pageInfo(const PdfSessionManifest &manifest,
                                         const QString &projectDir,
                                         int page,
                                         QString *why)
{
    PdfRenderBackend *backend = forPage(manifest, projectDir, page, why);
    if (!backend) {
        return PdfPageInfo();
    }
    return backend->pageInfo(manifest.pages.at(page).index);
}

QImage PdfSourceRenderers::renderPage(const PdfSessionManifest &manifest,
                                      const QString &projectDir,
                                      int page,
                                      qreal dpi,
                                      QString *why)
{
    PdfRenderBackend *backend = forPage(manifest, projectDir, page, why);
    if (!backend) {
        return QImage();
    }
    return turnedForDisplay(backend->renderPage(manifest.pages.at(page).index, dpi),
                            manifest.pages.at(page).extraRotation);
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

PdfRenderBackend *PdfSourceRenderers::backendForFile(const QString &sourceFile,
                                                     const QString &projectDir,
                                                     QString *why)
{
    PdfRenderBackend *backend = m_backends.value(sourceFile, nullptr);
    if (backend && backend->isOpen()) {
        return backend;
    }

    if (!backend) {
        if (!m_factory) {
            fail(why, QStringLiteral("this notebook has no renderer for %1").arg(sourceFile));
            return nullptr;
        }
        backend = m_factory();
        if (!backend) {
            fail(why, QStringLiteral("no PDF render backend on this platform"));
            return nullptr;
        }
        m_backends.insert(sourceFile, backend);
    }

    /// A backend whose source has gone (or whose open failed earlier) is opened again rather than
    /// thrown away: the file may be back, and a stale handle would answer with nothing.
    if (!backend->open(QDir(projectDir).filePath(sourceFile))) {
        fail(why, QStringLiteral("the renderer cannot open %1").arg(sourceFile));
        return nullptr;
    }

    return backend;
}
