/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PDFIODOCKER_H
#define PDFIODOCKER_H

#include <QDockWidget>

class QEvent;
class QLabel;
class QListWidget;
class QPushButton;

/**
 * The page strip of a notebook.
 *
 * One page is one document, so there is no page control in Krita to reuse: this is the thing
 * that turns pages. The list is filled from the open notebook and the entries are cheap labels
 * rather than thumbnails, because painting one thumbnail per page would cost exactly what the
 * bounded set exists to avoid.
 */
class PdfIoDocker : public QDockWidget
{
    Q_OBJECT
public:
    PdfIoDocker();
    ~PdfIoDocker() override;

    /**
     * Puts the open notebook's own name on the title, right after a rename.
     *
     * The docker reads the name from the manifest itself, so a notebook opened after a rename is
     * already titled correctly; this is only what makes the change visible without a page turn.
     */
    void reloadNotebookName();

private Q_SLOTS:
    void refresh(int index, int pageCount, const QString &label);
    void openSelected();

    /// Asks for a thumbnail of every page the list is showing. Generation is queued and one at a
    /// time, so scrolling through a long notebook fills it in as it goes.
    void queueThumbnails();

    void updateThumbnail(int index);

protected:
    /// The cards are sized from the room the panel has. A fixed grid looks right while the docker
    /// is tall and clips into itself as soon as it is not, which is what the preview did.
    void resizeEvent(QResizeEvent *event) override;

    /// The list's viewport is what actually holds the room the cards are fitted to. The docker's
    /// own resize arrives before the list has been given its new width -- and no second docker
    /// resize follows the layout that gives it -- so the refit is driven from the child that
    /// changed. Without this the cards keep the width they were given at construction.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// Recomputes the card size for the room the list has now.
    void refitCards();

    /// The open notebook's own name, read from its manifest once per notebook. Empty when there is
    /// no notebook, or when the manifest carries no name -- the caller falls back to the label.
    QString notebookName();

    QLabel *m_status = nullptr;
    /// The project directory the cached name belongs to, so a notebook change re-reads it.
    QString m_nameDir;
    QString m_name;
    QListWidget *m_pages = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
};

/// Registers the docker once per process; a view plugin is created per view.
void registerPdfIoDocker();

#endif // PDFIODOCKER_H
