/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSESSION_H
#define PDFSESSION_H

#include "session/PdfSessionManifest.h"

#include <QString>

class PdfRenderBackend;

/**
 * The on-disk shape of a note project.
 *
 * A directory, not a single file, because a per page save then rewrites one small ink file
 * instead of the whole project, and a crash mid-save cannot take the other pages with it.
 * The single file form is reserved for backup and sharing, where rebuilding is safe.
 *
 *   <project>/manifest.json     schema, source identity, pages, generations
 *   <project>/source.pdf        the original bytes, copied once, never rewritten
 *   <project>/pages/pNNNN.kra   ink only, no merged image
 *   <project>/thumbs/pNNNN.png  small preview for the page list
 */
class PdfSession
{
public:
    /// Wraps an existing source PDF into a fresh project directory. Refuses to clobber.
    static PdfSessionManifest createProject(const QString &projectDir,
                                            const QString &sourcePdf,
                                            PdfRenderBackend &backend,
                                            QString *why = nullptr);

    /// Reads a project back and checks that its source is still the one recorded.
    static PdfSessionManifest openProject(const QString &projectDir, QString *why = nullptr);

    /**
     * Whether \a relative, joined onto \a projectDir, stays inside it.
     *
     * The manifest rule refuses the names that escape before a consumer ever sees one; this is the
     * join itself, checked where it is made. openProject() runs it on the source before reading
     * anything, so a project directory placed on the machine by hand cannot make the session step
     * outside itself even if the rule above is ever relaxed or a caller builds a manifest in code.
     */
    static bool isPathInsideProject(const QString &projectDir,
                                    const QString &relative,
                                    QString *why = nullptr);

    /**
     * Where the notebook directories live.
     *
     * Documents, so the working format is somewhere the user can find it, on the desktop. The
     * app-private location on Android, and that is not a preference: the manifest targets SDK 36
     * and asks for neither requestLegacyExternalStorage nor MANAGE_EXTERNAL_STORAGE, so scoped
     * storage refuses a direct-path write into shared Documents and a page save would simply fail.
     * Getting a notebook into Documents on Android is the SAF bundle's job, not this one's.
     *
     * Falls back to legacyProjectRoot() when the Documents location is empty, missing or not
     * writable: a machine without a Documents folder must not be unable to open a notebook.
     */
    static QString projectRoot();

    /// The root used before the notebook folder moved to Documents, and still the root on Android.
    static QString legacyProjectRoot();

    /**
     * The directory the notebook called \a name is opened from, moving it out of the legacy root
     * the first time it is opened under the new one.
     *
     *  - already under projectRoot(), or under neither root: the projectRoot() path.
     *  - under the legacy root: moved into projectRoot(), checked to be a readable notebook before
     *    and after the move, and the new path is returned.
     *  - the move or the check failed: the legacy path, with \a why set, so the caller keeps
     *    reading the notebook where it is. Nothing is deleted, and a failed move leaves exactly one
     *    complete copy.
     */
    static QString migrateFromLegacy(const QString &name, QString *why = nullptr);

    /**
     * Points the Documents half of projectRoot() at \a path, for tests; an empty string restores
     * the real location. Nothing in the plugin calls it.
     */
    static void setDocumentsLocationForTests(const QString &path);

    static QString manifestPath(const QString &projectDir);
    static QString sourcePath(const QString &projectDir, const QString &sourceFile);
    static QString pageFileName(int index);
    static QString thumbFileName(int index);

    /**
     * The artifact name a page NUMBER maps to: 7 gives "pages/p0007.kra".
     *
     * pageFileName() takes the zero-based page index, which is what a notebook's own pages use;
     * this is the one to pair with PdfSessionManifest::allocatePageNumber(), because the number
     * the allocator hands out is the number in the name rather than an index.
     */
    static QString pageFileNameForNumber(int number);

    /// The same, for a page's preview: 7 gives "thumbs/p0007.png".
    static QString thumbFileNameForNumber(int number);
};

#endif // PDFSESSION_H
