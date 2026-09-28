/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef PDFNOTEBOOKRESIZEDIALOG_H
#define PDFNOTEBOOKRESIZEDIALOG_H

#include "session/PdfSessionManifest.h"

#include <QDialog>
#include <QList>
#include <QSize>

class QLabel;
class QListWidget;
class QSpinBox;

/** Pixel-bound resize dialog shared by Manage Pages and the PDF Notebook Resize menu. */
class PdfNotebookResizeDialog : public QDialog
{
public:
    struct Request {
        /// Zero-based positions in the manifest as shown when the dialog opened.
        QList<int> pageIndices;
        /// A maximum box; every page is scaled uniformly to fit inside it.
        QSize boundsPx;
        int dpi = 300;
    };

    PdfNotebookResizeDialog(const QList<PdfPageRecord> &pages, int currentPage, bool batch,
                           QWidget *parent = nullptr);

    Request request() const;

private:
    void updateApplyEnabled();

    bool m_batch = false;
    int m_currentPage = 0;
    QListWidget *m_pages = nullptr;
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QSpinBox *m_dpi = nullptr;
    QLabel *m_selectionSummary = nullptr;
};

#endif // PDFNOTEBOOKRESIZEDIALOG_H
