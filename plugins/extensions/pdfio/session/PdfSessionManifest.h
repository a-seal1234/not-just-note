/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFSESSIONMANIFEST_H
#define PDFSESSIONMANIFEST_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QSizeF>
#include <QString>

/**
 * One page of a note project.
 *
 * A record describes where the page sits in the PDF and which ink file belongs to it; it
 * never holds the page raster, which is derivable from the bundled source and is therefore
 * not persisted. That decision is what keeps a project proportional to its ink.
 */
struct PdfPageRecord {
    int index = -1;
    /// Displayed size in points, /Rotate already applied.
    QSizeF sizePt;
    /// The /Rotate the file declares, kept so the manifest survives a renderer change.
    int rotation = 0;
    /// Paths are relative to the project directory, so a project can be moved. Both are checked
    /// by PdfSessionManifest::isSafeRelativePath() before anything joins them onto a directory.
    QString kraFile;
    /**
     * The page's preview, relative to the project directory.
     *
     * An empty string is legal and means "no thumbnail yet": a page whose preview has not been
     * made records none, and refusing it would make notebooks that open today stop opening. It
     * does not mean the project directory, so a consumer has to skip an empty thumbnail rather
     * than join it.
     */
    QString thumbFile;
    /// Bumped on every committed save of this page; used to reason about recovery.
    int generation = 0;
};

/**
 * manifest.json: the single durable description of a note project.
 *
 * The source PDF is immutable and never written back to, so the manifest only has to record
 * the identity of the source it was built from, the page list, and the ink that has since
 * been committed on top.
 */
class PdfSessionManifest
{
public:
    /// Bumped whenever the on-disk shape changes in a way older readers cannot handle.
    static const int CurrentSchema;

    int schema = 1;
    /// File name of the source inside the project directory, not a full path.
    QString sourceFile;
    /// Hex encoded SHA-256 of the source, so a moved or edited source is detected.
    QByteArray sourceSha256;
    qint64 sourceByteSize = 0;

    /**
     * The notebook's own name, as the user sees it: the docker's title and the suggestion the
     * export dialog offers.
     *
     * Empty is legal and means "no name of its own yet", which is every notebook made before this
     * field existed; displayName() then stands in the source file's name. It is a display label,
     * never a path: nothing joins it onto a directory, which is why it is not checked by
     * isSafeRelativePath() the way the file names are.
     */
    QString name;

    QList<PdfPageRecord> pages;

    /// The name to show: name when there is one, otherwise the source file's own base name.
    QString displayName() const;

    /**
     * Whether \a path is a file name this manifest may carry, and the one rule for all of them.
     *
     * Every field that names a file -- the source, each page's ink, each page's thumbnail -- is
     * joined onto the project directory by whoever reads it: PdfSession::sourcePath(), the
     * navigator, the page saver, the docker. A value that is absolute or climbs out of the
     * directory turns opening a document into a read or a write of the manifest's choosing, so
     * the rule is applied in isValid() -- which readFrom(), fromJson() and openProject() all run
     * -- rather than at each of those joins, and every consumer can then trust the file.
     *
     * A name is safe when it is relative, uses forward slashes, has no empty, "." or ".."
     * component, does not use a backslash as a separator, and does not start with a drive letter.
     * Spaces, dots, unicode and subdirectories inside the project are all legitimate.
     */
    static bool isSafeRelativePath(const QString &path, QString *why = nullptr);

    bool isValid(QString *why = nullptr) const;

    QJsonObject toJson() const;
    static PdfSessionManifest fromJson(const QJsonObject &object, QString *why = nullptr);

    bool writeTo(const QString &path, QString *why = nullptr) const;
    static PdfSessionManifest readFrom(const QString &path, QString *why = nullptr);

    static QByteArray sha256OfFile(const QString &path);
};

#endif // PDFSESSIONMANIFEST_H
