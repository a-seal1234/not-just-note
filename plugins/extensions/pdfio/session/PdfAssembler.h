/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFASSEMBLER_H
#define PDFASSEMBLER_H

#include "session/PdfSessionManifest.h"

#include <QList>
#include <QSizeF>
#include <QString>

/**
 * Builds ONE PDF out of a notebook whose pages come from several sources.
 *
 * The exporter rebuilds the page tree of a single PDF; it cannot carry a second file's objects, so
 * a notebook built by the app's own merge and insert features could not be exported at all. This
 * assembles such a notebook's pages into one file instead: each page's objects are COPIED into a
 * fresh document with every reference renumbered, streams byte for byte and never re-encoded, so
 * the text layer, the fonts and the embedded images come out exactly as they were.
 *
 * WHAT THE DATA SAYS, because it is what shaped this reader. The user's own manual is HYBRID: a
 * PDF 1.4 header, a classic xref TABLE, and an /XRefStm that names an xref STREAM whose decoded
 * entries are ~4100 objects packed in /ObjStm streams; every page inherits /Resources and /MediaBox
 * from an ancestor (measured over the real notebook store; see docs/PDFIO-DESIGN-ASSEMBLE.md and
 * docs/PDFIO-STATUS.md). The reader therefore ports the EXPORTER's PROVEN half -- classic tables,
 * hybrid /XRefStm supplements, standalone xref streams, /ObjStm type-2 entries, FlateDecode, LZWDecode
 * and the PNG/TIFF predictors -- rather than inventing one. LZW uses the shared bounded decoder and
 * honors /EarlyChange. What it cannot read is still REFUSED with the source's name and reason: encryption,
 * and a structural filter or predictor the reader does not implement. Page content streams are copied.
 * The project-aware live export uses this assembler for multi-source notebooks; the existing
 * single-source exporter remains unchanged.
 *
 * THE INHERITANCE RULE IS THE POINT. In the user's own file every page has no /Resources, no
 * /MediaBox and no /CropBox of its own -- they live on the /Pages node. A page copied without them
 * renders as blank paper, so the walk resolves the effective values by carrying the ancestors'
 * attributes down the tree, and the assembled page is written with them explicit.
 *
 * The assembled file is a valid single-source PDF. The EXISTING exporter is what applies the
 * notebook's turn, scale, crop and ink to it, so the tested export path does not move.
 */
class PdfAssembler
{
public:
    /// One notebook page as the assembly will write it, with inheritance already resolved.
    struct PagePlan
    {
        int sourceIndex = -1;   ///< which entry of the manifest's sources
        int sourcePage = -1;    ///< which page of that source
        int pageObject = -1;    ///< the page object in that source

        /**
         * A page the assembly WRITES rather than copies: a BLANK page, whose paper is a sheet of its
         * own size and comes from no file at all, or a page whose paper is a PICTURE the notebook
         * holds. Neither has a page object in a PDF to copy, so sizePt is the sheet the page IS and
         * the object fields stay -1.
         *
         * The notebook's own box, turn and scale are NOT applied here: the assembled file is a
         * single-source PDF and the existing exporter puts those on it, exactly as it does for a page
         * copied from a source. What is written here is the page's own sheet, which is what the
         * exporter treats as "the source page".
         */
        bool synthesized = false;
        QSizeF sizePt;          ///< the EFFECTIVE size: the page's own /MediaBox, or an ancestor's
        int rotation = 0;       ///< the EFFECTIVE /Rotate, normalised to 0/90/180/270
        bool inheritsResources = false; ///< the page carries no /Resources of its own
        bool inheritsMediaBox = false;  ///< the page carries no /MediaBox of its own
    };

    /**
     * What the notebook's pages will be, in notebook order, with inheritance resolved. Reads every
     * source and refuses (naming the file) anything it cannot read. Exposed so the inheritance rule
     * can be checked on a file without writing one.
     */
    static bool plan(const QString &projectDir,
                     const PdfSessionManifest &manifest,
                     QList<PagePlan> *pages,
                     QString *why = nullptr);

    /**
     * Writes the notebook's pages from every source into ONE file at \a outPath.
     *
     * Every page of the notebook is written in notebook order, whatever source it came from, with
     * the effective /MediaBox and /Rotate written explicitly and the page's /Contents, /Resources,
     * /Group and supported untagged local /Link annotations copied across. Link destinations and
     * annotation /P references are remapped to unique output pages. Tagged links, unsupported
     * actions and transformed annotation geometry are refused rather than partially copied. A
     * source that cannot be read is named in \a why and stops assembly -- a half-file that looks
     * complete is worse than a refusal.
     *
     * \a written, when given, receives the plan that was actually written.
     */
    static bool assemble(const QString &projectDir,
                         const PdfSessionManifest &manifest,
                         const QString &outPath,
                         QList<PagePlan> *written = nullptr,
                         QString *why = nullptr);
};

#endif // PDFASSEMBLER_H
