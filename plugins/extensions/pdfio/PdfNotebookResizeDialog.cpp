/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "PdfNotebookResizeDialog.h"

#include "session/PdfNotebookOps.h"

#include <KLocalizedString>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
constexpr int DefaultDpi = 300;
constexpr int MinimumDpi = 36;
constexpr int MaximumDpi = 2400;
constexpr int MaximumPixelBound = 100000;

QSize initialPixelBounds(const QList<PdfPageRecord> &pages, int currentPage)
{
    if (pages.isEmpty()) {
        return QSize(2480, 3508);
    }
    const int index = qBound(0, currentPage, pages.size() - 1);
    const QSize size = PdfNotebookOps::pixelSizeAtDpi(pages.at(index), DefaultDpi);
    return size.isValid() ? size : QSize(2480, 3508);
}
}

PdfNotebookResizeDialog::PdfNotebookResizeDialog(const QList<PdfPageRecord> &pages, int currentPage,
                                                 bool batch, QWidget *parent)
    : QDialog(parent)
    , m_batch(batch)
    , m_currentPage(pages.isEmpty() ? 0 : qBound(0, currentPage, pages.size() - 1))
{
    setWindowTitle(batch ? i18n("Batch resize pages by pixels")
                         : i18n("Resize page by pixels"));
    setObjectName(QStringLiteral("pdfio_pixel_resize_dialog"));

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(
        i18n("The source PDF is not rasterized. Each page is uniformly scaled to fit inside the pixel box "
             "at the selected DPI; proportions are preserved, so one side may be smaller."), this);
    description->setWordWrap(true);
    layout->addWidget(description);

    if (m_batch) {
        auto *label = new QLabel(i18n("Pages to resize (all pages start selected):"), this);
        layout->addWidget(label);
        m_pages = new QListWidget(this);
        m_pages->setObjectName(QStringLiteral("pdfio_pixel_resize_pages"));
        m_pages->setSelectionMode(QAbstractItemView::NoSelection);
        m_pages->setMaximumHeight(260);
        for (int i = 0; i < pages.size(); ++i) {
            const QSize pixels = PdfNotebookOps::pixelSizeAtDpi(pages.at(i), DefaultDpi);
            const QString dimensions = pixels.isValid()
                ? QStringLiteral("%1 × %2 px at %3 dpi").arg(pixels.width()).arg(pixels.height()).arg(DefaultDpi)
                : i18n("size unavailable");
            auto *item = new QListWidgetItem(i18n("Page %1 — %2", i + 1, dimensions), m_pages);
            item->setData(Qt::UserRole, i);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(pixels.isValid() ? Qt::Checked : Qt::Unchecked);
            if (!pixels.isValid()) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            }
        }
        layout->addWidget(m_pages);

        auto *pageButtons = new QHBoxLayout;
        auto *all = new QPushButton(i18n("All pages"), this);
        all->setObjectName(QStringLiteral("pdfio_pixel_resize_all"));
        auto *none = new QPushButton(i18n("None"), this);
        none->setObjectName(QStringLiteral("pdfio_pixel_resize_none"));
        m_selectionSummary = new QLabel(this);
        m_selectionSummary->setObjectName(QStringLiteral("pdfio_pixel_resize_selection"));
        pageButtons->addWidget(all);
        pageButtons->addWidget(none);
        pageButtons->addStretch(1);
        pageButtons->addWidget(m_selectionSummary);
        layout->addLayout(pageButtons);

        QObject::connect(all, &QPushButton::clicked, this, [this]() {
            for (int i = 0; i < m_pages->count(); ++i) {
                QListWidgetItem *item = m_pages->item(i);
                if (item->flags() & Qt::ItemIsEnabled) {
                    item->setCheckState(Qt::Checked);
                }
            }
        });
        QObject::connect(none, &QPushButton::clicked, this, [this]() {
            for (int i = 0; i < m_pages->count(); ++i) {
                m_pages->item(i)->setCheckState(Qt::Unchecked);
            }
        });
        QObject::connect(m_pages, &QListWidget::itemChanged, this,
                         [this](QListWidgetItem *) { updateApplyEnabled(); });
    } else {
        const QSize current = initialPixelBounds(pages, m_currentPage);
        auto *currentSize = new QLabel(i18n("Current page: %1 × %2 px at %3 dpi",
                                             current.width(), current.height(), DefaultDpi), this);
        currentSize->setObjectName(QStringLiteral("pdfio_pixel_resize_current_size"));
        layout->addWidget(currentSize);
    }

    auto *form = new QFormLayout;
    const QSize defaults = initialPixelBounds(pages, m_currentPage);
    m_width = new QSpinBox(this);
    m_width->setObjectName(QStringLiteral("pdfio_pixel_resize_width"));
    m_width->setRange(1, MaximumPixelBound);
    m_width->setSuffix(i18n(" px"));
    m_width->setValue(qBound(1, defaults.width(), MaximumPixelBound));
    m_height = new QSpinBox(this);
    m_height->setObjectName(QStringLiteral("pdfio_pixel_resize_height"));
    m_height->setRange(1, MaximumPixelBound);
    m_height->setSuffix(i18n(" px"));
    m_height->setValue(qBound(1, defaults.height(), MaximumPixelBound));
    m_dpi = new QSpinBox(this);
    m_dpi->setObjectName(QStringLiteral("pdfio_pixel_resize_dpi"));
    m_dpi->setRange(MinimumDpi, MaximumDpi);
    m_dpi->setSuffix(i18n(" dpi"));
    m_dpi->setValue(DefaultDpi);
    form->addRow(i18n("Maximum width:"), m_width);
    form->addRow(i18n("Maximum height:"), m_height);
    form->addRow(i18n("Resolution:"), m_dpi);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("pdfio_pixel_resize_buttons"));
    buttons->button(QDialogButtonBox::Ok)->setText(i18n("Resize"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateApplyEnabled();
    adjustSize();
}

PdfNotebookResizeDialog::Request PdfNotebookResizeDialog::request() const
{
    Request result;
    result.boundsPx = QSize(m_width->value(), m_height->value());
    result.dpi = m_dpi->value();
    if (!m_batch) {
        result.pageIndices.append(m_currentPage);
        return result;
    }

    for (int row = 0; row < m_pages->count(); ++row) {
        const QListWidgetItem *item = m_pages->item(row);
        if (item->checkState() == Qt::Checked) {
            result.pageIndices.append(item->data(Qt::UserRole).toInt());
        }
    }
    return result;
}

void PdfNotebookResizeDialog::updateApplyEnabled()
{
    QDialogButtonBox *buttons = findChild<QDialogButtonBox *>(QStringLiteral("pdfio_pixel_resize_buttons"));
    if (!buttons) {
        return;
    }

    int selected = 0;
    if (!m_batch) {
        selected = 1;
    } else if (m_pages) {
        for (int row = 0; row < m_pages->count(); ++row) {
            if (m_pages->item(row)->checkState() == Qt::Checked) {
                ++selected;
            }
        }
    }
    if (m_selectionSummary) {
        m_selectionSummary->setText(i18n("%1 selected", selected));
    }
    buttons->button(QDialogButtonBox::Ok)->setEnabled(selected > 0);
}
