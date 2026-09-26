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
 * WHAT THE DATA SAYS, because it is what shaped this reader. The PDFs the user actually imports are
 * classic PDF 1.4: an xref TABLE, plain objects, no object streams (measured over the real notebook
 * store; see docs/PDFIO-DESIGN-ASSEMBLE.md). That is what this reader reads, and NOTHING else is
 * guessed: a source that uses cross-reference streams, object streams or encryption is REFUSED with
 * its name and the reason, and the per-source export remains the fallback for it.
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
     * the effective /MediaBox and /Rotate written explicitly and the page's /Contents, /Resources
     * and /Group copied across. A source that cannot be read is named in \a why and stops the
     * assembly -- a half-assembled file that looks complete is the one outcome worse than a refusal.
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
