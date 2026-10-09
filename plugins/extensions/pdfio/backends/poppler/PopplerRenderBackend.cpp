/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PopplerRenderBackend.h"

#include <QLatin1String>

namespace {

std::unique_ptr<Poppler::Page> pageAt(Poppler::Document *document, int index)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return document->page(index);
#else
    return std::unique_ptr<Poppler::Page>(document->page(index));
#endif
}

} // namespace

PopplerRenderBackend::PopplerRenderBackend() = default;
PopplerRenderBackend::~PopplerRenderBackend() = default;

bool PopplerRenderBackend::open(const QString &path)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_document = Poppler::Document::load(path);
#else
    m_document.reset(Poppler::Document::load(path));
#endif
    if (!m_document || m_document->isLocked()) {
        m_document.reset();
        return false;
    }
    m_document->setRenderHint(Poppler::Document::Antialiasing, true);
    m_document->setRenderHint(Poppler::Document::TextAntialiasing, true);
    return true;
}

bool PopplerRenderBackend::isOpen() const
{
    return bool(m_document);
}

int PopplerRenderBackend::pageCount() const
{
    return m_document ? m_document->numPages() : 0;
}

PdfPageInfo PopplerRenderBackend::pageInfo(int index) const
{
    PdfPageInfo info;
    if (!m_document) {
        return info;
    }

    std::unique_ptr<Poppler::Page> page = pageAt(m_document.get(), index);
    if (!page) {
        return info;
    }

    info.index = index;
    /// pageSizeF() already has /Rotate applied and ignores the MediaBox origin.
    info.sizePt = page->pageSizeF();

    switch (page->orientation()) {
    case Poppler::Page::Landscape:  info.rotation = 90;  break;
    case Poppler::Page::Portrait:   info.rotation = 0;   break;
    case Poppler::Page::Seascape:   info.rotation = 270; break;
    case Poppler::Page::UpsideDown: info.rotation = 180; break;
    }

    return info;
}

QImage PopplerRenderBackend::renderPage(int index, qreal dpi) const
{
    if (!m_document) {
        return QImage();
    }
    std::unique_ptr<Poppler::Page> page = pageAt(m_document.get(), index);
    if (!page) {
        return QImage();
    }
    return page->renderToImage(dpi, dpi);
}

QString PopplerRenderBackend::pageText(int index) const
{
    if (!m_document) {
        return QString();
    }
    std::unique_ptr<Poppler::Page> page = pageAt(m_document.get(), index);
    if (!page) {
        return QString();
    }
    return page->text(QRectF());
}
