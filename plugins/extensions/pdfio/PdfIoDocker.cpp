/*
 *  SPDX-FileCopyrightText: 2026 PerfectNote contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QApplication>
#include "PdfIoDocker.h"
#include "PdfIoNotebookActions.h"
#include "PdfPageNavigator.h"
#include "session/PdfSession.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
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
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QVBoxLayout>

#include <KisMainWindow.h>
#include <KisPart.h>

#include <KoDockFactoryBase.h>
#include <KoDockRegistry.h>

namespace {

QString pageLabel(int index) {
    return QStringLiteral("Page %1").arg(index + 1);
}

/// Where a card's prepared picture is carried. The item's own decoration is left empty on purpose:
/// the delegate below draws the picture itself, because the style's path cannot draw it at the
/// right size.
constexpr int CardPreviewRole = Qt::UserRole + 1;

/// The strip under the picture that the page's name is drawn in.
constexpr int CardLabelHeight = 18;

/**
 * Draws a notebook card: the panel, the page's picture, and the page's name.
 *
 * The picture is drawn here rather than through the item's icon because QIcon::pixmap() answers in
 * LOGICAL pixels whatever device ratio the pixmap carries. A picture prepared at the card's device
 * size was therefore smooth-downscaled to the logical size and then stretched back by the screen,
 * which is the pixelation that showed up on the tablet. Measured on Qt 5.15 at a scale factor of
 * 2.5: 42% of the card's pixels came out mid-grey through the icon path, under 1% through this
 * one. Drawing with both rectangles explicit keeps one source pixel per device pixel.
 */
class PdfCardDelegate : public QStyledItemDelegate
{
public:
    explicit PdfCardDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem cell = option;
        initStyleOption(&cell, index);
        const QPixmap prepared = index.data(CardPreviewRole).value<QPixmap>();

        /// The panel and the selection are the style's business; the picture and the name are not,
        /// so neither is offered to it. The style would centre its own icon and label, and this
        /// delegate is about to draw both.
        cell.icon = QIcon();
        cell.text.clear();
        QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &cell, painter, option.widget);

        if (!prepared.isNull()) {
            const qreal ratio = prepared.devicePixelRatio() > 0 ? prepared.devicePixelRatio() : 1.0;
            const QSize logical(qRound(prepared.width() / ratio),
                                qRound(prepared.height() / ratio));
            QRect area = option.rect;
            area.setBottom(area.bottom() - CardLabelHeight);
            /// The label strip is not the picture's, and what is left of the cell is what the page
            /// has to fit in. A page nearly as tall as the card -- an A4 sheet is -- would otherwise
            /// be drawn a few pixels past the edge and clipped there. Shrunk into the strip rather
            /// than drawn through it, so the whole page is always inside its box.
            const QSize drawn = (logical.width() > area.width() || logical.height() > area.height())
                ? logical.scaled(area.size(), Qt::KeepAspectRatio)
                : logical;
            QRect card(QPoint(0, 0), drawn);
            card.moveCenter(area.center());
            painter->drawPixmap(card, prepared, QRectF(prepared.rect()));
        }

        const QString label = index.data(Qt::DisplayRole).toString();
        if (label.isEmpty()) {
            return;
        }
        QRect strip = option.rect;
        strip.setTop(strip.bottom() - CardLabelHeight);
        style->drawItemText(painter, strip, Qt::AlignHCenter | Qt::AlignVCenter, option.palette,
                            true, label, QPalette::Text);
    }
};

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
    /// The cards are drawn by a delegate of our own rather than by the style: see PdfCardDelegate
    /// for why the style cannot draw the picture at the size the screen actually has.
    m_pages->setItemDelegate(new PdfCardDelegate(m_pages));
    layout->addWidget(m_pages);

    auto *buttons = new QHBoxLayout();
    m_previous = new QPushButton(QStringLiteral("◀"), content);
    m_next = new QPushButton(QStringLiteral("▶"), content);
    buttons->addWidget(m_previous);
    buttons->addWidget(m_next);
    layout->addLayout(buttons);

    /// Only the turns are here. The panel is on screen while reading, so turning the page under the
    /// reader's hand is exactly what it is for. Everything else changes the page list -- how many
    /// pages there are and in what order -- and that is a decision to make with the whole list in
    /// front of you, on the Notebook ops page; a panel button that deletes a page is a button that
    /// deletes a page by accident.
    auto *quick = new QGridLayout();
    int quickRow = 0;
    int quickColumn = 0;
    for (PdfNotebookQuicks::Action action : PdfNotebookQuicks::all()) {
        if (!PdfNotebookQuicks::isTurn(action)) {
            continue;
        }

        auto *button = new QPushButton(pdfIoQuickTitle(action), content);
        connect(button, &QPushButton::clicked, this, [this, action]() { runQuick(action); });
        quick->addWidget(button, quickRow, quickColumn);
        m_quicks.append(qMakePair(action, button));
        if (++quickColumn == 2) {
            quickColumn = 0;
            ++quickRow;
        }
    }
    layout->addLayout(quick);

    /// The way back from the change that was just made. The submenu has had "Undo the last notebook
    /// change" all along, and it is the one operation a person reaches for in the seconds after an
    /// insert or a merge, so it belongs next to the notebook rather than two menus deep. It undoes
    /// one change, which is the whole depth the notebook's undo has.
    m_undo = new QPushButton(QStringLiteral("Undo last change"), content);
    connect(m_undo, &QPushButton::clicked, this, []() { pdfIoUndoNotebookChange(); });
    layout->addWidget(m_undo);

    /// The way to the page the rest of the operations live on. Below the turns rather than beside
    /// them: it is a door, not a page operation.
    m_manage = new QPushButton(QStringLiteral("Manage pages..."), content);
    connect(m_manage, &QPushButton::clicked, this, []() { openPdfIoNotebookOpsScreen(); });
    layout->addWidget(m_manage);

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

    /// And every card that is no longer on screen gives its picture back. A card's picture is
    /// prepared at the size the screen draws it -- 3.6 MB at a 2.5 ratio -- so keeping one for every
    /// page of a long notebook is a hundred megabytes of pictures nobody is looking at, and scrolling
    /// through the notebook is what fills that in. The visible cards are asked for again by
    /// queueThumbnails() the moment they come back, so nothing is lost but the memory.
    const QRect visible = m_pages->viewport()->rect();
    for (int i = 0; i < m_pages->count(); ++i) {
        QListWidgetItem *item = m_pages->item(i);
        if (!item || i == PdfPageNavigator::instance()->currentIndex()) {
            continue;
        }
        if (m_pages->visualItemRect(item).intersects(visible)) {
            continue;
        }
        item->setData(CardPreviewRole, QVariant());
    }

    /// The thumbnails already on screen were scaled for the size the cards had a moment ago;
    /// re-reading them is what keeps a resize from leaving the old-sized icons behind.
    for (int i = 0; i < m_pages->count(); ++i) {
        if (QListWidgetItem *item = m_pages->item(i)) {
            if (!item->data(CardPreviewRole).value<QPixmap>().isNull()) {
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

    /// The panel can be shown long after a notebook was opened, so its buttons are enabled from the
    /// state the notebook is in now rather than from the last page this signal carried.
    refreshQuickButtons();
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

    const bool hasNotebook = PdfPageNavigator::instance()->hasNotebook();
    m_status->setText(pageCount > 0
                          ? QStringLiteral("%1 — page %2 of %3").arg(shown).arg(index + 1).arg(pageCount)
                          : (hasNotebook
                                 ? QStringLiteral("No pages — insert pages from a PDF to continue")
                                 : QStringLiteral("No notebook is open")));

    if (index >= 0 && index < m_pages->count()) {
        const QSignalBlocker blocker(m_pages);
        m_pages->setCurrentRow(index);
    }

    m_previous->setEnabled(index > 0);
    m_next->setEnabled(index >= 0 && index + 1 < pageCount);

    refreshQuickButtons();
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

    const PdfPageRecord &record = navigator->manifest().pages.at(index);

    /// An empty name is "no preview yet", and never the project directory: joining it would hand
    /// QPixmap the directory, which is not a picture, and the page would lose the card it is about
    /// to be given back by the ask below.
    const QString thumbPath = record.thumbFile.isEmpty()
        ? QString()
        : QDir(navigator->projectDir()).filePath(record.thumbFile);
    const QPixmap pixmap(thumbPath);
    if (pixmap.isNull()) {
        /// The preview is gone rather than late: a turn or a notebook change drops it, and the
        /// next save writes a new one. Left as it was, the item keeps the picture of the page as
        /// it used to be -- which is how a page that was turned went on showing its old preview
        /// and looked like the wrong page in the list.
        m_pages->item(index)->setData(CardPreviewRole, QVariant());
        return;
    }

    /// The box the page is drawn in comes from the PAGE, not from the file: displaySizePt() is the
    /// one place the reader's size comes from -- sheet, box, turn, scale -- so a turned, scaled or
    /// boxed page keeps its own shape here. A file still written at the old shape is drawn INTO
    /// that box rather than fitted by its own, which is the difference between a preview of the
    /// page and a picture clipped by its box. Fitting the box into the card also means the whole
    /// page is always inside it.
    ///
    /// Scaled to that box in DEVICE pixels and handed to the delegate with the ratio: a card is
    /// measured in logical pixels, so on a tablet with a 2-2.5x screen a picture sized for the
    /// logical card was drawn at two and a half times the size it had, which is the pixelation the
    /// user reported. The delegate draws it with both rectangles explicit, one source pixel per
    /// device pixel.
    const qreal ratio = m_pages->devicePixelRatioF() > 0 ? m_pages->devicePixelRatioF() : 1.0;
    const QSize card = m_pages->iconSize();
    const QSize box = PdfPageNavigator::previewBoxFor(record, card);
    if (box.isEmpty()) {
        m_pages->item(index)->setData(CardPreviewRole, QVariant());
        return;
    }
    const QSize deviceBox(qMax(1, qRound(box.width() * ratio)), qMax(1, qRound(box.height() * ratio)));
    QPixmap scaled = pixmap.scaled(deviceBox, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    scaled.setDevicePixelRatio(ratio);
    m_pages->item(index)->setData(CardPreviewRole, scaled);
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

void PdfIoDocker::refreshQuickButtons()
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();

    /// A reload takes the page list out from under these buttons for a moment, and nothing they
    /// could ask for in it is possible while the notebook is being rebuilt.
    const bool busy = navigator->reloadPending();

    for (const auto &entry : m_quicks) {
        QString why;
        const bool possible =
            !busy && PdfNotebookQuicks::available(entry.first, navigator->pageCount(),
                                                  navigator->currentIndex(), &why);
        QPushButton *button = entry.second;
        button->setEnabled(possible);
        /// The reason and not only the grey: a button that cannot do anything says why, the same way
        /// the screen's hint line does, instead of standing there mute.
        button->setToolTip(possible ? pdfIoQuickTitle(entry.first) : why);
    }

    if (m_undo) {
        /// Asked rather than remembered: an operation the user just ran is what turns this on, and
        /// the notebook has no other signal for it.
        m_undo->setEnabled(!busy && pdfIoCanUndoNotebookChange());
    }

    if (m_manage) {
        m_manage->setEnabled(navigator->hasNotebook() && !busy);
    }
}

void PdfIoDocker::runQuick(PdfNotebookQuicks::Action action)
{
    /// Asked again here rather than trusted from the last repaint: a click can arrive after the page
    /// turned underneath it, and the rule is one call away. The application asks it once more before
    /// it writes anything, so this is the earliest of three asks rather than the only one.
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    QString why;
    if (!PdfNotebookQuicks::available(action, navigator->pageCount(), navigator->currentIndex(),
                                      &why)) {
        qWarning() << "pdfio: that page operation is not possible:" << why;
        refreshQuickButtons();
        return;
    }

    /// Deferred out of the click handler for the reason openSelected() is: an operation writes the
    /// open page, applies a manifest and closes the view that is showing it, so the whole
    /// application reacts while this button is still inside its own signal.
    QTimer::singleShot(0, this, [action]() { runPdfIoQuickAction(action); });
}

void registerPdfIoDocker()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    KoDockRegistry::instance()->add(new PdfIoDockFactory());

#if defined(Q_OS_ANDROID)
    /// The tablet's notebook layout must not carry the "Touch Docker": it is a mobile drawing aid,
    /// not part of a notebook, and the user asked for it gone.
    ///
    /// It cannot be said in the shipped workspace file. KisMainWindow's serializer only writes the
    /// dock widgets it manages, so a docker the state never mentions is left by restoreState() at
    /// its own default -- hidden here or there, never written. It is hidden here instead: at once,
    /// a moment later when the saved layout has been restored, and again every ten seconds for ten
    /// minutes so a workspace switched to shortly after start is covered too. A user who
    /// deliberately re-opens it after that is not fought forever.
    auto hideTouchDocker = []() {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (!window) {
            return;
        }
        const QList<QDockWidget *> dockers = window->findChildren<QDockWidget *>();
        for (QDockWidget *docker : dockers) {
            if (docker->objectName() == QLatin1String("TouchDocker") && docker->isVisible()) {
                docker->hide();
                qWarning() << "pdfio: hid the Touch Docker; the notebook layout does not use it";
            }
        }
    };
    QTimer::singleShot(3000, qApp, hideTouchDocker);
    QTimer::singleShot(8000, qApp, hideTouchDocker);
    QTimer::singleShot(20000, qApp, hideTouchDocker);
    auto *touchWatch = new QTimer(qApp);
    touchWatch->setInterval(10000);
    auto *touchWatchTicks = new int(0);
    QObject::connect(touchWatch, &QTimer::timeout, touchWatch,
                     [hideTouchDocker, touchWatch, touchWatchTicks]() {
                         hideTouchDocker();
                         if (++(*touchWatchTicks) >= 60) {
                             touchWatch->stop();
                             touchWatch->deleteLater();
                             delete touchWatchTicks;
                         }
                     });
    touchWatch->start();

#endif
}
