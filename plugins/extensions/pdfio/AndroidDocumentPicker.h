/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ANDROIDDOCUMENTPICKER_H
#define ANDROIDDOCUMENTPICKER_H

#include <QObject>
#include <QString>

#include <functional>

/**
 * Asks Android for a PDF and hands back a path the renderer can open.
 *
 * QFileDialog is not usable for this: the application has no broad filesystem access on a modern
 * device, so the file arrives as a content:// URI instead of a path. Nothing of the vendor or of
 * ours is added to the APK for it -- Qt's own activity-result plumbing is enough -- but the
 * content still has to be copied out, because PdfRenderer needs a seekable descriptor and a
 * content URI only offers a stream.
 */
class AndroidDocumentPicker : public QObject
{
    Q_OBJECT
public:
    explicit AndroidDocumentPicker(QObject *parent = nullptr);
    ~AndroidDocumentPicker() override;

    /**
     * Opens the system picker for a PDF. \a onPicked is called with the local copy, or with an
     * empty string and a reason when the user cancels or the copy fails.
     */
    void pickPdf(std::function<void(const QString &localPath, const QString &why)> onPicked);

    /**
     * Asks Android where to put \a localFile and writes it there. \a onWritten is called with
     * false and a reason when the user cancels or the destination refuses the write.
     */
    void createPdf(const QString &suggestedName,
                   const QString &localFile,
                   std::function<void(bool, const QString &)> onWritten);

    /// The same two things for a notebook bundle. A .pnb is a zip, which is the whole difference:
    /// the picker is told application/zip rather than application/pdf, and SAF copies the chosen
    /// content out and back exactly as it does for a PDF.
    void pickBundle(std::function<void(const QString &localPath, const QString &why)> onPicked);
    void createBundle(const QString &suggestedName,
                      const QString &localFile,
                      std::function<void(bool, const QString &)> onWritten);

    /**
     * The general pair the two above are.
     *
     * \a mimeType is what the system picker filters on and what a created document is declared as.
     * \a cacheFileName is the name the chosen content is copied to while it is worked on locally:
     * both PdfRenderer and KArchive need a real file, and a content URI is only a stream.
     */
    void pickFile(const QString &mimeType,
                  const QString &cacheFileName,
                  std::function<void(const QString &localPath, const QString &why)> onPicked);
    void createFile(const QString &mimeType,
                    const QString &suggestedName,
                    const QString &localFile,
                    std::function<void(bool, const QString &)> onWritten);

    /**
     * The content:// URI the last pick handed over, or an empty string when there was none.
     *
     * Kept, not used, here: the provider is asked for the file's display name LATER, on the event
     * loop, because asking for it from inside the activity-result callback is what took the
     * application down. The copy itself is untouched -- it still happens in the callback.
     */
    QString pickedContentUri() const;

    /**
     * The name the provider has for \a contentUri, from OpenableColumns.DISPLAY_NAME, or an empty
     * string when there is no name to be had.
     *
     * Every failure is an answer of "no name": no activity, no resolver, no URI, no cursor, no
     * column, no value. The caller keeps the cache name then, so nothing here can fail an import.
     */
    static QString displayNameForContentUri(const QString &contentUri);

private:
    struct Private;
    Private *d;
};

#endif // ANDROIDDOCUMENTPICKER_H
