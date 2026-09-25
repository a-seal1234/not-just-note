/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "PdfIoDocker.h"
#include "PdfPageNavigator.h"
#include "session/PdfSession.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QSignalBlocker>
#include <QTimer>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
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
    /// Not uniform: the cells are set explicitly from gridSize in refitCards(). With the uniform
    /// cache on, the view takes every item's size from the first one it measured -- which is a
    /// text-only item while the notebook is being filled -- and the cards stayed tiny on the tablet
    /// however the icon and grid sizes were set.
    m_pages->setUniformItemSizes(false);
    m_pages->setSpacing(4);
    /// The cards follow the list's own viewport, so the refit is driven from it rather than only
    /// from the docker: see eventFilter().
    m_pages->viewport()->installEventFilter(this);
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

    /// One card per row is the shape a page selector wants: a page is recognised as a page, as
    /// large as the panel allows. Two only when the panel is genuinely wide enough for two
    /// comfortable cards, and never more than two -- a narrower panel must not turn the page into
    /// a small strip beside another one.
    static constexpr int MinCardWidth = 140;
    static constexpr int MinComfortableCardWidth = 190;
    static constexpr int CardMargin = 24;
    static constexpr int MaxCardWidth = 320;
    static constexpr qreal CardAspect = 1.414; // a portrait page: height / width
    static constexpr int CardSpacing = 12;

    const int viewportWidth = m_pages->viewport()->width();
    const int columns = viewportWidth >= 2 * MinComfortableCardWidth + CardMargin ? 2 : 1;

    /// The floor is the point: a card too small to read is not a preview. When the panel is
    /// narrower than the floor the card keeps the floor and the list scrolls, rather than the page
    /// being shrunk to a sliver -- which is what the user was looking at on the tablet.
    const int cardWidth = qBound(MinCardWidth, (viewportWidth - CardMargin) / columns, MaxCardWidth);

    /// The card is the page's own shape, so the whole page fits with no cropping; a thumbnail of a
    /// different shape is letterboxed by the list's KeepAspectRatio scaling, never cut.
    const int cardHeight = qRound(cardWidth * CardAspect);
    const QSize iconSize(cardWidth, cardHeight);
    const QSize gridSize(cardWidth + CardSpacing, cardHeight + CardSpacing);

    if (m_pages->iconSize() == iconSize && m_pages->gridSize() == gridSize) {
        return;
    }

    m_pages->setIconSize(iconSize);
    m_pages->setGridSize(gridSize);

    /// Each cell is also given to the items explicitly, so a cell size can never be left over from
    /// the size an item had before its thumbnail arrived.
    for (int i = 0; i < m_pages->count(); ++i) {
        if (QListWidgetItem *item = m_pages->item(i)) {
            item->setSizeHint(gridSize);
        }
    }

    /// One line per change, so the device log says what was computed for the room it had.
    qWarning() << "pdfio: cards refit for a" << viewportWidth << "px viewport ->" << columns
               << "column(s), icon" << iconSize << "grid" << gridSize;

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

bool PdfIoDocker::eventFilter(QObject *watched, QEvent *event)
{
    if (m_pages && m_pages->viewport() == watched && event->type() == QEvent::Resize) {
        refitCards();
    }
    return QDockWidget::eventFilter(watched, event);
}

void PdfIoDocker::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);

    /// The first layout gives the docker the width it will really have, after the constructor has
    /// already sized the cards for a placeholder; the queued call is for the layout that lands
    /// after this event. Both are needed: on the tablet neither the docker's resizeEvent nor the
    /// viewport's was reached with the final width, and the cards stayed at the construction size.
    refitCards();
    QTimer::singleShot(0, this, &PdfIoDocker::refitCards);
}

QString PdfIoDocker::notebookName()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook()) {
        m_nameDir.clear();
        m_name.clear();
        return QString();
    }

    const QString dir = navigator->projectDir();
    if (dir == m_nameDir) {
        return m_name;
    }

    m_nameDir = dir;
    m_name.clear();

    /// Read from the manifest on disk rather than from the navigator's copy: the navigator loaded
    /// that when the notebook was opened and a rename is written after it, so its copy would be one
    /// rename behind -- which is exactly the title still showing the old name after a rename.
    QString why;
    const PdfSessionManifest manifest =
        PdfSessionManifest::readFrom(PdfSession::manifestPath(dir), &why);
    if (manifest.isValid()) {
        m_name = manifest.displayName();
    }
    return m_name;
}

void PdfIoDocker::reloadNotebookName()
{
    m_nameDir.clear();

    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    const QString fallback = navigator->hasNotebook()
        ? QFileInfo(navigator->manifest().sourceFile).completeBaseName()
        : QString();
    refresh(navigator->currentIndex(), navigator->pageCount(), fallback);
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
            auto *item = new QListWidgetItem(pageLabel(i));
            if (m_pages->gridSize().isValid()) {
                item->setSizeHint(m_pages->gridSize());
            }
            m_pages->addItem(item);
        }
    }

    if (index >= 0 && index < pageCount) {
        updateThumbnail(index);
    }

    /// Whatever is on screen, including the pages either side of it, so scrolling finds the next
    /// thumbnails already there.
    queueThumbnails();

    /// The notebook's own name when it has one, and the label the navigator sent otherwise: the
    /// docker is named after the notebook, not after the file it was copied from.
    const QString name = notebookName();
    const QString shown = name.isEmpty() ? label : name;
    setWindowTitle(shown.isEmpty() ? QStringLiteral("Notebook") : shown);

    m_status->setText(pageCount > 0
                          ? QStringLiteral("%1 — page %2 of %3").arg(shown).arg(index + 1).arg(pageCount)
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

    /// KeepAspectRatio, never a crop: a thumbnail of a different shape is letterboxed inside the
    /// card so the whole page is always visible.
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
