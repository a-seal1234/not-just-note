/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSOURCERENDERERS_H
#define PDFSOURCERENDERERS_H

#include "backend/PdfRenderBackend.h"
#include "session/PdfSessionManifest.h"

#include <QHash>
#include <QImage>
#include <QString>

#include <functional>

/**
 * One open renderer per source, and the one place a notebook page number becomes a render.
 *
 * A page records which PDF its background comes from (PdfPageRecord::source), and a notebook that
 * was given pages from another file -- inserted, or merged -- has more than one. Everything that
 * renders goes through here, for two reasons:
 *
 *  - the page rendered is the record's own page inside ITS source, not the notebook position.
 *    The two are the same number only until a page is moved, deleted or inserted, and a render of
 *    the wrong page is silent: the paper comes back as some other sheet with the ink on top of it;
 *  - a source is parsed once and kept open. A strip builds five slots out of one file, a roll
 *    repaints slots from up to five, and a thumbnail walk asks for page after page of the same
 *    PDF, so opening the file again per page is the cost this cache exists to avoid.
 *
 * The factory is passed in rather than called here, so a test can hand over a backend it already
 * has; the plugin passes PdfRenderBackend::create().
 */
class PdfSourceRenderers
{
public:
    using Factory = std::function<PdfRenderBackend *()>;

    PdfSourceRenderers();
    explicit PdfSourceRenderers(Factory factory);
    ~PdfSourceRenderers();

    PdfSourceRenderers(const PdfSourceRenderers &) = delete;
    PdfSourceRenderers &operator=(const PdfSourceRenderers &) = delete;

    /// The open renderer for the source \a page comes from, opened on first use. \a why carries
    /// the reason when the source cannot be opened.
    PdfRenderBackend *forPage(const PdfSessionManifest &manifest, const QString &projectDir,
                              int page, QString *why = nullptr);

    /// The geometry of \a page's own page inside its source.
    PdfPageInfo pageInfo(const PdfSessionManifest &manifest, const QString &projectDir,
                         int page, QString *why = nullptr);

    /**
     * \a page's page inside its source, rendered at \a dpi and turned by the notebook's own
     * rotation: what the reader sees, which is what the paper under the ink has to be.
     *
     * \a dpi is the reader's resolution -- the strip's, or the budget's -- and the page's own size
     * follows from displaySizePt(). A page the notebook scaled is rendered at a LARGER SOURCE dpi
     * (dpi * extraScale) rather than being upscaled afterwards, so a 2x page is twice the pixels of
     * the same source and the pen's ink stays sharp; the memory budget is what decides how far that
     * can go. A page with a box renders its box only, and a box that reaches past the sheet (a
     * margin) comes back as blank paper rather than as a hole.
     */
    QImage renderPage(const PdfSessionManifest &manifest, const QString &projectDir,
                      int page, qreal dpi, QString *why = nullptr);

    /**
     * \a rendered turned by the notebook's rotation on top of whatever the source declares.
     *
     * Public because the single-page path builds its image from a render too: a page the notebook
     * has turned must come back turned whether it was rendered for a strip or for a page of its
     * own. The ink is turned with it -- the artifact is rotated when the page is -- which is what
     * keeps a stroke on the line it was drawn on.
     */
    static QImage turnedForDisplay(const QImage &rendered, int extraRotation);

    /**
     * \a rendered reduced to \a boxPt, the page's own rectangle in source points, at \a sourceDpi.
     *
     * Public for the same reason turnedForDisplay() is: the single-page path renders its own raster
     * and has to put the box on it too, or a cropped page opens as the whole sheet -- bigger than
     * the size its own record declares, with the ink the crop already cut missing from the paper
     * under it. The order is always crop first, turn second, which is the order the strip uses.
     */
    static QImage croppedToBox(const QImage &rendered, const QRectF &boxPt, qreal sourceDpi);

    /**
     * The paper of a page that has NO source: white paper of the page's own sheet, at \a dpi.
     *
     * The same arithmetic as renderPage() and in the same order -- the scale, the box, then the turn
     * -- so a blank page measures exactly what a rendered one would and every size comparison
     * downstream (the layout, the strip's slot, the budget, the exporter) holds without a special
     * case. The paper is OPAQUE white: a blank page is a sheet of paper, not a hole in the notebook.
     */
    static QImage blankPage(const PdfPageRecord &record, qreal dpi);

    /// Closes and forgets every renderer. For a notebook change.
    void clear();

    /// How many sources are open: for the log, and to make the cache observable from a test.
    int openSourceCount() const;

private:
    PdfRenderBackend *backendForFile(const PdfSourceRecord &source, const QString &projectDir,
                                     QString *why);

    Factory m_factory;
    /// Owned. Deleted by the destructor and by clear().
    QHash<QString, PdfRenderBackend *> m_backends;
};

#endif // PDFSOURCERENDERERS_H
