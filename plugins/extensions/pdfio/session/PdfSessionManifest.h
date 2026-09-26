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
 * One PDF a notebook's pages can be rendered from.
 *
 * A notebook used to have exactly one source, and the three fields on the manifest below still
 * describe it. It has a list now because a notebook can be given pages from another PDF -- inserted
 * or merged -- and each page then has to say which file its background comes from: the background
 * is never persisted, so "which PDF" is the only thing that can put it back.
 *
 * \c file is relative to the project directory and is checked by isSafeRelativePath() like every
 * other name the manifest carries.
 */
struct PdfSourceRecord {
    QString file;
    QByteArray sha256;
    qint64 byteSize = 0;
};

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

    /// Which entry of PdfSessionManifest::sources the page's background is rendered from. 0 is the
    /// notebook's own source, which is every page of a notebook made before this field existed.
    int source = 0;

    /**
     * A turn the NOTEBOOK applies on top of the source's own /Rotate, in whole degrees: 0 to 359.
     *
     * Kept apart from \c rotation, which is what the file declares: the source is immutable, so a
     * page the user rotates is a turn this manifest records rather than one the PDF is rewritten
     * with. \c sizePt stays the source's displayed size and displaySizePt() is what the user sees,
     * so the two cannot drift apart.
     *
     * Any angle is allowed, not only a right angle: a page can be set down at 37 degrees. What that
     * costs is a bigger page -- see displaySizePt() -- because the sheet is shown inside the
     * rectangle that holds it once it is turned.
     */
    int extraRotation = 0;

    /// The size the user sees: sizePt with extraRotation applied. A right angle swaps the sides
    /// exactly; any other angle gives the rectangle the turned sheet fits in.
    QSizeF displaySizePt() const;

    /**
     * The size a \a size rectangle has once it is turned by \a degrees. The same arithmetic as
     * displaySizePt(), for any rectangle rather than for a page's own sheet.
     *
     * Here rather than beside the code that first needed it, because three of them need it and they
     * have to agree: the record, the artifact the rotator produces, and the raster the renderer
     * turns. Two spellings of "the box a turned sheet occupies" is how a page ends up a pixel wider
     * than the rectangle every other part of the notebook measured.
     *
     * A whole number of right angles is exact -- sides swapped, or the same sheet back -- because
     * every later reader compares sizes. Any other angle is the rectangle the four corners land in,
     * taken from the corners rather than from a closed form: the same answer for a plain turn, and
     * the one that stays right when a page is placed rather than only turned.
     */
    static QSizeF turnedSize(const QSizeF &size, int degrees);
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
    /**
     * Bumped whenever the on-disk shape changes in a way older readers cannot handle.
     *
     * 2 adds sources[], pages[].source, pages[].extraRotation and nextPageNumber. A schema 1
     * manifest is upgraded in memory when it is read (see fromJson) and written back as 2 by the
     * next write; an older build meeting a 2 refuses it through this guard rather than rendering
     * pages from the wrong PDF, which is the whole reason the number is bumped instead of the new
     * fields being added quietly.
     *
     * A constexpr member, so that the default below can be the same value: a manifest built in
     * code that defaulted to the previous schema is refused by its own reader, which is exactly
     * what happened when the number lived in one place and the default in another.
     */
    static constexpr int CurrentSchema = 2;

    int schema = CurrentSchema;
    /// File name of the source inside the project directory, not a full path.
    QString sourceFile;
    /// Hex encoded SHA-256 of the source, so a moved or edited source is detected.
    QByteArray sourceSha256;
    qint64 sourceByteSize = 0;

    /**
     * Every PDF this notebook can draw a page from, sources[0] being the notebook's own source.
     *
     * Empty is legal and means "one source: the three fields above". A manifest built in code --
     * by a test, by a probe -- therefore stays valid without repeating itself. When the list is
     * there, its first entry must agree with those three fields; isValid() enforces that, because
     * the bundle, the docker and the export still read them.
     */
    QList<PdfSourceRecord> sources;

    /**
     * The next pages/pNNNN.kra number to hand out. Always greater than every number the page list
     * already names, so a number freed by deleting a page is never handed out again: a leftover
     * artifact would otherwise be read back as the ink of a page that never had any.
     */
    int nextPageNumber = 0;

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

    /// How many sources this manifest effectively has: the list when it is there, one otherwise.
    int sourceCount() const;
    /// What source \a index resolves to, or a default-constructed record when it is out of range.
    PdfSourceRecord sourceAt(int index) const;
    /// The source \a page is rendered from.
    PdfSourceRecord sourceForPage(const PdfPageRecord &page) const;
    /// The index of the source whose checksum is \a sha256, or -1. For "is this PDF already here?".
    int sourceIndexForSha(const QByteArray &sha256) const;

    /// The next artifact number, advancing the counter. Never returns a number a page already names.
    int allocatePageNumber();

    /**
     * The counter as it would be written: nextPageNumber raised past every number the page list
     * already names. What refreshNextPageNumber() stores, and what toJson() records, so a manifest
     * built in code and serialised without refreshing still reads back as the same value instead
     * of as a counter one write away from handing out a number a page is using.
     */
    int effectiveNextPageNumber() const;

    /// Raises nextPageNumber so that it is past every number the page list already names.
    void refreshNextPageNumber();

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

    /**
     * Whether writeTo() should fail after the new content has been written but before it is put in
     * place. The only caller is a test, and it is what proves that a failed write leaves the
     * manifest that was already there -- byte for byte -- instead of a truncated or half-edited
     * file. Nothing in the plugin calls it.
     */
    static void setFailBeforeCommitForTests(bool fail);

    /**
     * How many times writeTo() has committed a manifest in this process, and a reset for a test.
     *
     * The Notebook ops screen's whole promise is that a page list it built is committed ONCE, and a
     * test has to be able to see that rather than take it on trust. Nothing in the plugin reads
     * either of these.
     */
    static int writeCountForTests();
    static void resetWriteCountForTests();

    static PdfSessionManifest readFrom(const QString &path, QString *why = nullptr);

    static QByteArray sha256OfFile(const QString &path);
};

#endif // PDFSESSIONMANIFEST_H
