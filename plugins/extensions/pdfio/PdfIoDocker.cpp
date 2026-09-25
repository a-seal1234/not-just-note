/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoDocker.h"
#include "PdfPageNavigator.h"

#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QSignalBlocker>
#include <QTimer>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

namespace {

QString pageLabel(int index) {
    return QStringLiteral("Page %1").arg(index + 1);
}

} // namespace

class PdfIoDockFactory : public KoDockFactoryBase
{
public:
    QString id() const override { return QStringLiteral("PdfIoDocker"); }

    DockPosition defaultDockPosition() const override { return DockRight; }

    QDockWidget *createDockWidget() override
    {
        PdfIoDocker *docker = new PdfIoDocker();
        docker->setObjectName(id());
        return docker;
    }
};

PdfIoDocker::PdfIoDocker()
{
    setWindowTitle(QStringLiteral("Notebook"));

    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);

    m_status = new QLabel(QStringLiteral("No notebook is open"), content);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_pages = new QListWidget(content);
    /// A wall of pages rather than a column of names: this is the page selector, and the whole
    /// point of it is recognising a page before opening it.
    m_pages->setViewMode(QListView::IconMode);
    /// The cards are sized from the room the panel has, here and on every resize: a fixed grid is
    /// right while the docker is tall and clips into itself as soon as it is not.
    refitCards();
    m_pages->setResizeMode(QListView::Adjust);
    m_pages->setMovement(QListView::Static);
    m_pages->setWordWrap(true);
    m_pages->setUniformItemSizes(true);
    m_pages->setSpacing(4);
    layout->addWidget(m_pages);

    auto *buttons = new QHBoxLayout();
    m_previous = new QPushButton(QStringLiteral("◀"), content);
    m_next = new QPushButton(QStringLiteral("▶"), content);
    buttons->addWidget(m_previous);
    buttons->addWidget(m_next);
    layout->addLayout(buttons);

    setWidget(content);

    connect(m_previous, &QPushButton::clicked, this, []() {
        QString why;
        PdfPageNavigator::instance()->previous(&why);
    });
    connect(m_next, &QPushButton::clicked, this, []() {
        QString why;
        PdfPageNavigator::instance()->next(&why);
    });
    connect(m_pages, &QListWidget::itemActivated, this, &PdfIoDocker::openSelected);
    connect(m_pages, &QListWidget::itemClicked, this, &PdfIoDocker::openSelected);

    /// The navigator is the single source of truth about which page is open, so the strip follows
    /// it instead of keeping a second copy of that state.
    connect(PdfPageNavigator::instance(), &PdfPageNavigator::pageChanged,
            this, &PdfIoDocker::refresh);
    connect(PdfPageNavigator::instance(), &PdfPageNavigator::thumbnailReady,
            this, &PdfIoDocker::updateThumbnail);
    connect(m_pages->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &PdfIoDocker::queueThumbnails);
}

PdfIoDocker::~PdfIoDocker() = default;

void PdfIoDocker::refitCards()
{
    if (!m_pages || !m_pages->viewport()) {
        return;
    }

    /// The room the viewport really has. The list keeps its own frame and every card is separated
    /// from its neighbours by the item spacing, so the usable width is the panel minus both.
    const int spacing = qMax(0, m_pages->spacing());
    const int available = m_pages->viewport()->width() - 2 * spacing;
    if (available <= 0) {
        return;
    }

    /// Cards are portrait-ish, because a page is taller than it is wide: a square card leaves two
    /// bands beside the sheet. The height is bounded so a very wide panel does not grow cards
    /// without end and a very short one does not leave the label under a sliver.
    static constexpr int MinCardWidth = 72;
    static constexpr int MaxCardWidth = 320;
    static constexpr int MinCardHeight = 96;
    static constexpr int MaxCardHeight = 320;
    static constexpr qreal CardAspect = 1.3; // height / width of the sheet

    /// As many columns as fit at the smallest readable card; the width that is left is shared out
    /// evenly, so the cards fill the panel instead of leaving a ragged right edge.
    const int columns = qMax(1, (available + spacing) / (MinCardWidth + spacing));
    const int cardWidth = qBound(MinCardWidth, available / columns - spacing, MaxCardWidth);

    /// The label under the icon wraps onto a second line for the two-digit pages, so it is given
    /// the room for two lines before the icon gets the rest of the card.
    const int labelHeight = m_pages->fontMetrics().height() * 2 + 8;
    const int iconHeight = qBound(MinCardHeight, qRound(cardWidth * CardAspect), MaxCardHeight);
    const QSize iconSize(cardWidth, iconHeight);
    const QSize gridSize(cardWidth + spacing, iconHeight + labelHeight);

    if (m_pages->iconSize() == iconSize && m_pages->gridSize() == gridSize) {
        return;
    }

    m_pages->setIconSize(iconSize);
    m_pages->setGridSize(gridSize);

    /// The thumbnails already on screen were scaled for the size the cards had a moment ago;
    /// re-reading them is what keeps a resize from leaving the old-sized icons behind.
    for (int i = 0; i < m_pages->count(); ++i) {
        if (QListWidgetItem *item = m_pages->item(i)) {
            if (!item->icon().isNull()) {
                updateThumbnail(i);
            }
        }
    }
}

void PdfIoDocker::resizeEvent(QResizeEvent *event)
{
    QDockWidget::resizeEvent(event);
    refitCards();
}

void PdfIoDocker::refresh(int index, int pageCount, const QString &label)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const QDir project(navigator->projectDir());

    /// Rebuilt when the notebook changes, and the entry for the page that just changed is given a
    /// fresh icon: a page saves its thumbnail on the way out, so by the time this runs the page
    /// that was left behind has one and the list should show it.
    if (m_pages->count() != pageCount) {
        /// Blocked while the list is rebuilt: clearing it destroys the very item a click may be
        /// arriving from, and the widget must not be told about that from inside its own signal.
        const QSignalBlocker blocker(m_pages);
        m_pages->clear();
        for (int i = 0; i < pageCount; ++i) {
            m_pages->addItem(new QListWidgetItem(pageLabel(i)));
        }
    }

    if (index >= 0 && index < pageCount) {
        updateThumbnail(index);
    }

    /// Whatever is on screen, including the pages either side of it, so scrolling finds the next
    /// thumbnails already there.
    queueThumbnails();

    m_status->setText(pageCount > 0
                          ? QStringLiteral("%1 — page %2 of %3").arg(label).arg(index + 1).arg(pageCount)
                          : QStringLiteral("No notebook is open"));

    if (index >= 0 && index < m_pages->count()) {
        const QSignalBlocker blocker(m_pages);
        m_pages->setCurrentRow(index);
    }

    m_previous->setEnabled(index > 0);
    m_next->setEnabled(index >= 0 && index + 1 < pageCount);
}

void PdfIoDocker::queueThumbnails()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        return;
    }

    const QRect visible = m_pages->viewport()->rect();
    for (int i = 0; i < m_pages->count(); ++i) {
        QListWidgetItem *item = m_pages->item(i);
        if (item && m_pages->visualItemRect(item).intersects(visible)) {
            navigator->ensureThumbnail(i);
        }
    }
}

void PdfIoDocker::updateThumbnail(int index)
{
    if (index < 0 || index >= m_pages->count()) {
        return;
    }

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (index >= navigator->manifest().pages.size()) {
        return;
    }

    const QString thumbPath = QDir(navigator->projectDir())
                                  .filePath(navigator->manifest().pages.at(index).thumbFile);
    const QPixmap pixmap(thumbPath);
    if (pixmap.isNull()) {
        return;
    }

    m_pages->item(index)->setIcon(QIcon(pixmap.scaled(m_pages->iconSize(),
                                                      Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation)));
}

void PdfIoDocker::openSelected()
{
    const int row = m_pages->currentRow();
    if (row < 0 || row == PdfPageNavigator::instance()->currentIndex()) {
        return;
    }

    /// Deferred out of the click handler, and this is not tidiness. Opening a page builds a
    /// document and a view and asks Krita to activate a node, so the whole application reacts
    /// while the list widget is still inside its own signal for the item that was clicked.
    /// Deferring this exact kind of work has already fixed three other hangs and crashes here: the
    /// plugin constructor, the Android activity result callback, and the view close.
    QTimer::singleShot(0, this, [row]() {
        QString why;
        if (!PdfPageNavigator::instance()->showPage(row, &why)) {
            qWarning() << "pdfio: cannot open that page:" << why;
        }
    });
}

void registerPdfIoDocker()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    KoDockRegistry::instance()->add(new PdfIoDockFactory());
}
