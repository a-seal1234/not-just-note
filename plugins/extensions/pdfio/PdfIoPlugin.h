/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFIOPLUGIN_H
#define PDFIOPLUGIN_H

#include <QPointer>
#include <QString>

#include <KisActionPlugin.h>

class QAction;

/**
 * Entry point of the pdfio extension: PDF pages as locked backgrounds with an ink group on
 * top, plus a materialised note project around them.
 *
 * The plugin is deliberately not gated with if(ANDROID) the way the pen extensions are:
 * desktop is the only platform where the whole session, geometry and export logic can be
 * tested, so it has to keep building there.
 */
class PdfIoPlugin : public KisActionPlugin
{
    Q_OBJECT
public:
    PdfIoPlugin(QObject *parent, const QVariantList &);
    ~PdfIoPlugin() override;

    /**
     * Wraps a PDF into a note project and opens its first page in a view.
     *
     * Deliberately not private: the action and the probe both go through here, so whatever the
     * probe reports is the path the user takes.
     */
    bool openNotebook(const QString &pdfPath);

    /**
     * Picks a PDF and inserts its pages into the notebook after the page that is open.
     *
     * Deliberately not private, like openNotebook() above: the "Notebook ops" submenu is built by a
     * helper in this file rather than by a member, and a helper cannot take the address of a private
     * slot. The picker is platform-specific -- a file dialog on the desktop, the system document
     * picker on Android -- which is what makes it a slot rather than a lambda in the menu.
     */
    void slotInsertPages();

private Q_SLOTS:
    void slotOpenNotebook();

    /// Picks an image and puts it on the page that is open as a content layer. Named "Insert
    /// image..." and enabled with a page open, like the document entries around it.
    void slotInsertImage();

    void slotSavePage();
    void slotNextPage();
    void slotPreviousPage();
    void slotExportPdf();
    void slotSaveNotebook();

    /// Writes the whole notebook -- every page's ink and the source it was made from -- as one
    /// file the user can carry to another device.
    void slotSaveNotebookAsBundle();

    /// Opens a notebook that arrived as one file. There was no inverse of "Open PDF as notebook"
    /// before this, which is exactly why a notebook could not leave the device it was made on.
    void slotOpenNotebookBundle();

    /**
     * Switches between one page at a time (design A) and a strip of three (design B), and re-lays
     * out the page that is open so the change is visible immediately rather than on the next open.
     *
     * The strip was reachable only through the PDFIO_PROBE_STRIP environment variable, which the
     * user cannot set on a tablet; this is the action that makes it testable by hand.
     */
    void slotToggleStripMode();

private:
    /// Turns a picked image into a content layer on the page that is open; defined beside the other
    /// page helpers in the .cpp.
    void placeInsertedImage(const QString &picked, const QString &why, const QString &projectDir);

    void registerActions();

    /// How many times a missing main window has been waited for while putting the menu up. The
    /// window is still being built when the plugin is constructed, so a few retries are the
    /// difference between a menu on the first screen and none at all.
    int m_menuTries = 0;

    /// Puts the checked state of the strip action back in step with the navigator's scope, which is
    /// the mode that is actually in force.
    void updateStripAction();

    /**
     * Rebuilds the open page at the scope that is now set.
     *
     * PdfPageNavigator::showPage() deliberately does no work when the page asked for is already in
     * the open strip -- that is what makes turning to a neighbouring page free -- so a scope change
     * cannot take effect while the old strip document is alive. The document is closed first and
     * this waits for that close, which Krita defers, then opens the page again.
     */
    void rebuildForScope(int pageIndex, int attemptsLeft);

    /// The strip switch, kept so its checked state can show which mode is in force.
    QPointer<QAction> m_stripAction;

    /**
     * Unpacks \a bundlePath under the project root and opens it as the current notebook.
     *
     * \a replaceWithoutAsking is for Android, where the system picker is the only dialog there is;
     * on the desktop a notebook that is already there is a question for the user, because two
     * notebooks for the same source means choosing between them.
     */
    void openBundleFile(const QString &bundlePath, bool replaceWithoutAsking);

    /// The name to offer in the save dialog: <source>.pnb, or notebook.pnb with nothing open.
    QString bundleSuggestion() const;

    /// Temporary measurement: opens N pages in a row and reports resident memory and the
    /// number of live documents after each, so the scaling with page count is visible rather
    /// than guessed.
    void runScaleProbe(int pages);

    /// Temporary: draws on a page, turns away from it and comes back, which is the round trip a
    /// user actually performs and the only way to see whether the ink survived it.
    void runRestoreProbe();

    /// Temporary: reports how far the canvas lets a page be panned past its own edge. The answer
    /// decides whether any of the turning-by-scrolling ideas can work, because a page that cannot
    /// be left behind never puts the centre of the view over a neighbour.
    void runPanProbe();

    /// Temporary: asks for a thumbnail of every page and reports which ones appeared, so the lazy
    /// path is checked without opening the docker by hand.
    void runThumbnailProbe();

    /// Temporary: the strip, end to end. Opens with a scope above one, draws a mark at a place
    /// that is known in the page's own coordinates, turns away and back, and then checks that the
    /// artifact is the size of the page and holds the mark where it was drawn. Cropping that is
    /// wrong is silent, which is what makes this worth an explicit check.
    void runStripProbe();
};

#endif // PDFIOPLUGIN_H
