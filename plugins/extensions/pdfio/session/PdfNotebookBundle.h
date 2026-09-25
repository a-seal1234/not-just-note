/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFNOTEBOOKBUNDLE_H
#define PDFNOTEBOOKBUNDLE_H

#include "session/PdfSessionManifest.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * A whole notebook as ONE file.
 *
 * A note project is a directory under QStandardPaths::AppDataLocation, which is the right shape
 * while it is being written to -- a page save rewrites one small ink file instead of the whole
 * notebook -- and the wrong shape the moment it has to leave the device. A directory cannot be
 * mailed, put on a USB stick or handed to another application, so this is the transport form:
 * the same tree, in a plain ZIP, at the same relative paths, in one file.
 *
 *   manifest.json        the project manifest, byte for byte as it is on disk
 *   <source>.pdf         the original bytes, at the name manifest.source records
 *   sources/<sha8>-<name>.pdf   every other PDF the notebook's pages are drawn from
 *   pages/pNNNN.kra      ink only, as in the project
 *   thumbs/pNNNN.png     the page previews
 *   bundle.json          the index: what this file carries, and what it deliberately does not
 *
 * Every source travels. A notebook whose pages were inserted from another PDF draws those pages'
 * backgrounds from that file, so a bundle carrying only the first would unpack into a notebook
 * that renders blank paper where they are -- with nothing said at either end.
 *
 * The archive is written with KZip, which is the KArchive code PdfInkLoader already reads a .kra
 * with: an existing plugin dependency that works on Android, so the bundle costs no new one. It is
 * a plain ZIP and nothing else -- \c unzip opens it, and a zip of a project directory made by hand
 * (without bundle.json) is accepted too.
 *
 * The extension is .pnb. It is a guess: the project name is still provisional, and the extension
 * only has to be one the file managers do not already claim for something else. Nothing in the
 * format depends on it; inspect() and extract() read any name.
 *
 * Reading is the dangerous half. An archive is attacker-controlled input, so extract() refuses to
 * write anything until every entry has been checked: a path that is absolute or climbs out of the
 * destination (zip slip), a manifest from a schema this build does not know, a source whose bytes
 * do not match the checksum the manifest recorded, a manifest naming a page or thumb the archive
 * does not carry, and an archive that is truncated or edited. Entries that are not part of a
 * notebook are ignored and reported, never extracted because they happened to be in the file.
 *
 * Extraction is atomic: the notebook is rebuilt in a temporary directory beside the destination
 * and renamed into place, so a refusal leaves no half-written notebook behind and the destination
 * is never overwritten unless the caller asks for it.
 */
class PdfNotebookBundle
{
public:
    /// Without the dot. \sa the note in the class comment about why this string is provisional.
    static QString extension();
    /// For QFileDialog: "PDF note bundles (*.pnb)" plus the plain-zip spelling.
    static QString fileFilter();

    /// One file the bundle carries, with what it takes to prove it arrived intact.
    struct Entry {
        QString path;
        qint64 bytes = 0;
        QByteArray sha256;
    };

    /**
     * What inspect() can say about a bundle without extracting it.
     *
     * \c entries, \c withheld and \c unknown are the whole point: a caller can tell the user
     * exactly what the file holds -- and what it does not -- before a single byte is written.
     */
    struct Info {
        PdfSessionManifest manifest;
        /// Where the source sits in the archive. Normally manifest.sourceFile.
        QString sourceEntry;

        /**
         * Where every source sits in the archive, in the manifest's order: entry 0 is the same
         * name as \ref sourceEntry. A notebook whose pages were inserted from another PDF has more
         * than one, and all of them have to travel for those pages to render at all.
         */
        QStringList sourceEntries;
        /// The files the bundle index lists as carried.
        QList<Entry> entries;
        /**
         * Artifacts the manifest names that were not in the project when it was bundled: a page
         * that was never drawn on has no ink file, and thumbs are made on demand. Declared in the
         * bundle index, so a bundle missing one of these is complete while a bundle missing
         * anything else is refused.
         */
        QStringList withheld;
        /// Entries the archive carries that are not part of a notebook: ignored, and reported.
        QStringList unknown;
        /// Whether the archive carried the index. A bundle made by hand has none.
        bool hasIndex = false;
        qint64 bundleBytes = 0;

        bool isValid() const
        {
            return manifest.isValid() && !sourceEntry.isEmpty();
        }
    };

    struct ExtractOptions {
        /**
         * Write over a project directory that is already there.
         *
         * Off by default, and deliberately: opening a bundle whose source is already on this
         * device means choosing between two notebooks with the same key, and that is a person's
         * decision, not a side effect.
         */
        bool replaceExisting = false;
    };

    /**
     * Writes \a projectDir into \a outPath as one file, replacing whatever is there.
     *
     * The file is written beside the destination and renamed onto it, so an interrupted save
     * leaves the old bundle (or no bundle) rather than a half-written one. Returns false and sets
     * \a why when the project has no manifest, no source, a source that no longer matches the
     * checksum recorded for it, or cannot be read.
     */
    static bool save(const QString &projectDir, const QString &outPath, QString *why = nullptr);

    /**
     * Reads the bundle's manifest and checks the archive, without writing anything.
     *
     * Returns an invalid Info and sets \a why for every refusal extract() makes, so the same
     * verdict can be shown before the user commits to unpacking anything.
     */
    static Info inspect(const QString &bundlePath, QString *why = nullptr);

    /**
     * Rebuilds the project directory from the bundle, atomically.
     *
     * Everything is written into a temporary directory beside \a destProjectDir and renamed into
     * place only once the result has been read back and checked, so a failure leaves the
     * destination exactly as it was. \a ignoredEntries, when given, receives the entries that
     * were in the archive but are not part of a notebook.
     */
    static bool extract(const QString &bundlePath,
                        const QString &destProjectDir,
                        QString *why = nullptr,
                        QStringList *ignoredEntries = nullptr);

    /// The same, with the decision about an existing project directory made explicit.
    static bool extract(const QString &bundlePath,
                        const QString &destProjectDir,
                        const ExtractOptions &options,
                        QString *why = nullptr,
                        QStringList *ignoredEntries = nullptr);

    /**
     * The path rule, exposed so the refusal can be tested on its own.
     *
     * A path is safe when it is relative, uses forward slashes, has no empty, "." or ".."
     * component, and does not start with a drive letter. It is applied to two different sets of
     * names and both matter: the archive's entries, and the file names the manifest itself
     * declares (source.file, pages[].kraFile, thumbs[].thumbFile), which are joined onto the
     * staging directory and written to just the same.
     *
     * On the absolute case: KArchive's zip reader strips a leading "/" from an entry name, so an
     * entry stored as "/tmp/x" is handed back as "tmp/x" and there is nothing left to refuse --
     * it arrives as an ordinary relative name and is reported as an unknown entry instead. The
     * check stays, because the name is the archive's and not ours, and a reader that does not
     * normalise it would otherwise be handed an absolute path; the guarantee that is tested end to
     * end is the one that matters, that an absolute entry never escapes the destination.
     */
    static bool isSafeEntryPath(const QString &path, QString *why = nullptr);

    /**
     * The directory name a bundle should be unpacked under: <base>-<hash8>.
     *
     * Exactly the name PdfPageNavigator::openNotebook() gives the same notebook -- the base name
     * of the source and the first eight hex digits of its SHA-256 -- so opening the extracted
     * source finds the project that was just written instead of making a second, empty one.
     */
    static QString extractDirName(const PdfSessionManifest &manifest);

    /// Where a bundle is unpacked by default: PdfSession::projectRoot(), which is the same root
    /// the navigator keys notebooks under -- see extractDirName() for the name used inside it.
    static QString defaultProjectRoot();

private:
    PdfNotebookBundle() = delete;
};

#endif // PDFNOTEBOOKBUNDLE_H
