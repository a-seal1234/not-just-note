/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "PdfNotebookOpsDialog.h"

#include "PdfPageNavigator.h"
#include "session/PdfSession.h"

#include <KLocalizedString>

#include <QAbstractItemView>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTouchEvent>
#include <QTransform>
#include <QVBoxLayout>

#include <QtMath>

#include <functional>

namespace {

/// The columns the table shows, in order.
enum Column {
    ThumbnailColumn,
    PositionColumn,
    SourceColumn,
    SizeColumn,
    TurnColumn,
    InkColumn,
    ColumnCount,
};

/// How far sideways a swipe has to go before it is a turn rather than a slipped click.
constexpr int SwipeTurnPixels = 60;

/// Where a card's prepared pixmap is kept on its table item, so the delegate that draws it needs
/// nothing from the dialog.
constexpr int CardPixmapRole = Qt::UserRole + 1;

/// How long a press has to stay put before it becomes a grab. Half a second is the gesture everyone
/// already knows from a tablet, and it is long enough that a swipe -- which moves at once -- can
/// never be mistaken for it.
constexpr int HoldDelayMs = 450;

/// How far a press may wander before it is a swipe and not a hold. A few pixels, because a finger
/// on glass is never perfectly still.
constexpr int HoldSlopPixels = 8;

/// How much room the page is given inside the canvas.
constexpr int CanvasMargin = 12;

/// How far a grip has to be from the page's centre before its direction means anything. A grip
/// taken on the centre itself has no direction -- the first stray pixel would send the page
/// spinning to whatever angle it happened to imply -- so such a drag accumulates from where the hand
/// goes next instead.
constexpr qreal GripRadius = 24.0;

/// A whole number of degrees in 0..359, whatever was asked for: a page can be turned either way and
/// past a full circle, and a record only ever holds one of the 360 angles.
int normalizedTurn(int degrees)
{
    return ((degrees % 360) + 360) % 360;
}

/// The way from one direction to another, taken the short way round: a drag across the +/-180 seam
/// has to keep going the way the hand went rather than spin the page the long way.
int signedDelta(int from, int to)
{
    return ((to - from + 180) % 360 + 360) % 360 - 180;
}

/// How far \a point is from the middle of \a area, in that area's own pixels.
qreal distanceFromCentre(const QPointF &point, const QRect &area)
{
    const QPointF offset = point - QRectF(area).center();
    return qSqrt(offset.x() * offset.x() + offset.y() * offset.y());
}

/// Where a mouse event happened, in the widget's own coordinates. Qt 6 renamed the accessor and
/// deprecated the old one, and this file is built against both.
QPoint mousePosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

QString sizeLabel(const PdfPageRecord &record)
{
    const QSizeF size = record.displaySizePt();
    return QStringLiteral("%1 x %2 pt")
        .arg(QString::number(size.width(), 'f', 0), QString::number(size.height(), 'f', 0));
}

/// The turn in words. A right angle keeps the phrase it has always had, and every one of them says
/// the number: any angle is legal now, so a page set down at 37 degrees has to be able to say so
/// rather than fall back to "as it is".
QString turnLabel(int degrees)
{
    switch (normalizedTurn(degrees)) {
    case 0:
        return i18n("as it is (0°)");
    case 90:
        return i18n("turned right (90°)");
    case 180:
        return i18n("upside down (180°)");
    case 270:
        return i18n("turned left (270°)");
    default:
        return i18n("turned by %1°", normalizedTurn(degrees));
    }
}

/// \a source turned by \a degrees, for the row's card and the canvas. A right angle is a transpose
/// of the pixels and stays exact and cheap; any other angle has to be resampled, and the smooth
/// filter is the one that keeps a turned page from looking like a staircase.
QPixmap turnedPixmap(const QPixmap &source, int degrees)
{
    const int angle = normalizedTurn(degrees);
    if (source.isNull() || angle == 0) {
        return source;
    }

    QTransform transform;
    transform.rotate(angle);
    return source.transformed(transform,
                              angle % 90 == 0 ? Qt::FastTransformation : Qt::SmoothTransformation);
}

/// The live touches of \a event that have not been lifted, in the widget's own coordinates.
///
/// Qt 6 renamed the accessors the way it renamed the mouse one, and this file is built against both.
QList<QPointF> liveTouchPositions(const QTouchEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QList<QTouchEvent::TouchPoint> points = event->points();
#else
    const QList<QTouchEvent::TouchPoint> points = event->touchPoints();
#endif

    QList<QPointF> positions;
    for (const QTouchEvent::TouchPoint &point : points) {
        if (point.state() == Qt::TouchPointReleased) {
            continue;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        positions.append(point.position());
#else
        positions.append(point.pos());
#endif
        if (positions.size() == 2) {
            break;
        }
    }
    return positions;
}

} // namespace

QPixmap pdfioPreviewForDisplay(const QPixmap &source, const QSize &logicalSize, qreal devicePixelRatio)
{
    if (source.isNull() || logicalSize.isEmpty()) {
        return QPixmap();
    }

    const qreal ratio = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    const QSize deviceSize(qMax(1, qRound(logicalSize.width() * ratio)),
                           qMax(1, qRound(logicalSize.height() * ratio)));

    /// IgnoreAspectRatio because the caller passes the box it is about to draw into, already fitted
    /// to the sheet's own shape -- the canvas works it out from the turned page's bounding box, the
    /// card from the icon size. Smooth because this is a picture of text, and a nearest-neighbour
    /// scale of one is a page of broken letters.
    QPixmap prepared = source.scaled(deviceSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    prepared.setDevicePixelRatio(ratio);
    return prepared;
}

/**
 * The preview pane: the selected page, large, and turned by hand.
 *
 * A page is often set down on an angle that is not a right angle -- a crooked scan, a sheet that
 * belongs sideways on the paper -- and the ops screen used to offer only "Turn left" and "Turn
 * right". The canvas is the answer: take hold of the page anywhere away from its centre and swing it
 * around that centre, the way one turns a sheet on a desk, and the angle the hand lands on is the
 * angle the record gets. A pen and a mouse arrive as the same drag; two fingers on a touch screen
 * twist the page by the angle between them.
 *
 * It owns no edit, and it makes only one of them. While the hand is moving the page is a PREVIEW:
 * what turns is the drawn picture, and the readout follows it, but the record, the list and the
 * pending change are untouched -- a drag is not thirty edits, it is a picture of one. When the hand
 * comes up the angle the gesture landed on is reported once through \c turned, and the dialog puts
 * that into the pending list exactly as it does a button's turn. Nothing here touches a file, so
 * Apply remains the one thing that writes, and one drag is one change: one undo takes the whole
 * turn back.
 *
 * What is drawn is the preview the dialog read, turned by the difference between the turn it was
 * drawn at and the turn the screen is holding. A page with no preview stands in as an empty sheet of
 * its own proportions, so a page can still be turned before it has ever been saved.
 *
 * The picture is prepared for the pane at the screen's own device pixel ratio, so the pane is drawn
 * from the pixels it will put on the glass rather than from a small file stretched by the painter.
 * When that still asks for more pixels than the row's preview holds -- a saved preview is 180x256
 * for an A4 page, and a 3K tablet's pane is several times that -- the widget says so through \c
 * needsPreview rather than stretching: the dialog asks the navigator for a fresh one.
 */
class PdfPageCanvas : public QWidget
{
public:
    explicit PdfPageCanvas(QWidget *parent = nullptr);

    /// Shows \a preview at \a previewRotation, with \a pendingRotation the screen is holding on top
    /// of it. An empty \a preview draws an empty sheet of \a pageSizePt instead. All angles are whole
    /// degrees.
    void setPage(const QPixmap &preview, int previewRotation, int pendingRotation,
                 const QSizeF &pageSizePt);
    void clearPage();

    /// The whole-degree angle being previewed while the hand is still down: the dialog moves the
    /// live readout and nothing else, so the list, the Turn column and the pending change stay
    /// exactly as they were until the hand comes up.
    std::function<void(int)> previewed;
    /// The whole-degree angle the gesture ended on. This is ONE pending edit, exactly as a button's
    /// turn is, so one drag from 0 to 37 degrees is one change when Apply runs.
    std::function<void(int)> turned;

    /// The DEVICE pixels the pane has just prepared its picture for, reported while painting. The
    /// dialog compares them with what the row's preview holds and asks the navigator for a fresh one
    /// when the picture would otherwise be stretched.
    std::function<void(const QSize &)> needsPreview;

    /// Whether a turn is in flight. The dialog leaves the canvas alone meanwhile: a preview that
    /// arrived mid-gesture would move the page under the hand.
    bool isTurning() const { return m_gesture; }
    /// The pixels prepared for the last paint (device pixels), and what the row's preview held.
    QPixmap preparedPreview() const { return m_prepared; }
    QSize sourcePixels() const { return m_sourcePixels; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

private:
    /// The direction of \a at from the page's centre, in whole degrees.
    int directionAt(const QPoint &at) const;
    /// Whether a grip at \a at is far enough from the centre for that direction to mean something.
    bool directionIsMeaningful(const QPointF &at) const;
    /// Starts a turn: the angle the record has now becomes what the gesture moves away from.
    void beginGesture();
    /// Draws \a degrees and tells the dialog, as a preview. Nothing is recorded.
    void previewAt(int degrees);
    /// Ends the gesture. A gesture that landed somewhere else reports the angle ONCE, and that one
    /// report is the pending edit.
    void endGesture();
    void handleTouch(QTouchEvent *event);

    /// The preview as it was read, and the turn it was drawn at: everything painted on top of it is
    /// a difference from that turn rather than the whole turn a second time.
    QPixmap m_preview;
    int m_previewRotation = 0;
    int m_rotation = 0;
    QSizeF m_pageSizePt;

    /// What the last paint prepared and drew, kept so the dialog can be told what was drawn: the
    /// pixels in DEVICE units, and the pixels the row's own preview had to make them from.
    QPixmap m_prepared;
    QSize m_sourcePixels;

    /// A turn in progress, either a drag or a two-finger twist. While it is set, m_rotation is a
    /// preview and no record has been touched.
    bool m_gesture = false;
    /// Set when the gesture is a two-finger twist rather than a drag.
    bool m_twisting = false;
    /// Set when the grip had a direction of its own; a grip on the centre accumulates instead.
    bool m_anchored = false;
    /// The angle the record had when the gesture began: what the hand has moved away from.
    int m_gestureStart = 0;
    int m_grabDirection = 0;
    int m_lastDirection = 0;
    /// The direction between the two fingers when the twist began.
    int m_twistFrom = 0;
};

PdfPageCanvas::PdfPageCanvas(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("pdfio_ops_canvas"));
    setMinimumSize(300, 300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::OpenHandCursor);
    /// The hand's position is the gesture, so the widget watches it: without this a move arrives
    /// only while a button is held, and a hover would be invisible to it.
    setMouseTracking(true);
    /// Without this a two-finger twist never arrives: it is a touch event, and a widget that has not
    /// asked for touch events is handed mouse events only.
    setAttribute(Qt::WA_AcceptTouchEvents);
}

void PdfPageCanvas::setPage(const QPixmap &preview, int previewRotation, int pendingRotation,
                            const QSizeF &pageSizePt)
{
    m_preview = preview;
    m_previewRotation = normalizedTurn(previewRotation);
    m_rotation = normalizedTurn(pendingRotation);
    m_pageSizePt = pageSizePt;
    update();
}

void PdfPageCanvas::clearPage()
{
    m_preview = QPixmap();
    m_previewRotation = 0;
    m_rotation = 0;
    m_pageSizePt = QSizeF();
    m_prepared = QPixmap();
    m_sourcePixels = QSize();
    m_gesture = false;
    m_twisting = false;
    update();
}

int PdfPageCanvas::directionAt(const QPoint &at) const
{
    const QPointF offset = QPointF(at) - QRectF(rect()).center();
    return normalizedTurn(qRound(qRadiansToDegrees(qAtan2(offset.y(), offset.x()))));
}

bool PdfPageCanvas::directionIsMeaningful(const QPointF &at) const
{
    return distanceFromCentre(at, rect()) >= GripRadius;
}

void PdfPageCanvas::beginGesture()
{
    m_gesture = true;
    m_twisting = false;
    m_gestureStart = m_rotation;
}

void PdfPageCanvas::previewAt(int degrees)
{
    const int angle = normalizedTurn(degrees);
    if (angle == m_rotation) {
        return;
    }

    m_rotation = angle;
    update();
    if (previewed) {
        previewed(angle);
    }
}

void PdfPageCanvas::endGesture()
{
    if (!m_gesture) {
        return;
    }

    const bool landedSomewhere = m_rotation != m_gestureStart;
    m_gesture = false;
    m_twisting = false;

    /// One report, at the end. This is what makes a whole drag one pending edit rather than a
    /// hundred: Apply commits one change, and one undo takes the turn back in one step.
    if (landedSomewhere && turned) {
        turned(m_rotation);
    }
}

void PdfPageCanvas::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor(0x33, 0x33, 0x33));

    const QRectF area =
        QRectF(rect()).adjusted(CanvasMargin, CanvasMargin, -CanvasMargin, -CanvasMargin);
    const QSizeF sheet = m_preview.isNull() ? m_pageSizePt : QSizeF(m_preview.size());
    if (area.width() < 2 || area.height() < 2 || sheet.isEmpty()) {
        return;
    }

    /// The turn the picture still needs: the file was made with m_previewRotation already applied,
    /// so drawing the whole angle again would turn an already-sideways page twice.
    const qreal turn = normalizedTurn(m_rotation - m_previewRotation);
    const qreal radians = qDegreesToRadians(turn);
    const qreal cosine = qAbs(qCos(radians));
    const qreal sine = qAbs(qSin(radians));

    /// The sheet is scaled so its bounding box -- not the sheet itself -- fits the pane: a page set
    /// down at an angle takes more room than the same page upright, and the pane is what it has.
    const qreal boxWidth = sheet.width() * cosine + sheet.height() * sine;
    const qreal boxHeight = sheet.width() * sine + sheet.height() * cosine;
    const qreal scale = qMin(area.width() / boxWidth, area.height() / boxHeight);
    const QSizeF drawn(sheet.width() * scale, sheet.height() * scale);
    const QRectF target(-drawn.width() / 2, -drawn.height() / 2, drawn.width(), drawn.height());

    /// Prepared at the size the pane is about to paint it at, in DEVICE pixels, and tagged with the
    /// ratio: Qt then puts those pixels on the glass one for one instead of stretching a small
    /// picture by the screen's ratio inside the painter.
    const qreal ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    const QSize logical(qMax(1, qRound(drawn.width())), qMax(1, qRound(drawn.height())));
    const QSize deviceNeed(qMax(1, qRound(logical.width() * ratio)),
                           qMax(1, qRound(logical.height() * ratio)));

    m_sourcePixels = m_preview.size();
    m_prepared = pdfioPreviewForDisplay(m_preview, logical, ratio);

    /// The pane has just worked out how many device pixels it is about to draw. The dialog hears
    /// that and asks the navigator for a fresh preview when the row's own cannot fill it; a pane
    /// that asked again on every repaint would hammer the navigator, so it only ever asks.
    if (needsPreview) {
        needsPreview(deviceNeed);
    }

    painter.translate(QRectF(rect()).center());
    painter.rotate(turn);

    if (m_preview.isNull()) {
        /// No preview: the page has never been saved, or a turn dropped the stale one. An empty
        /// sheet of the page's own proportions still gives the hand something to turn.
        painter.setPen(QPen(QColor(0xb0, 0xb0, 0xb0), 1));
        painter.setBrush(QColor(0xff, 0xff, 0xff));
        painter.drawRect(target);
        return;
    }

    /// The whole source onto the whole target, explicitly: the target is in logical coordinates
    /// and the pixmap holds target * ratio device pixels, so the painter's own device transform is
    /// what maps one to the other -- one source pixel per device pixel, and no second scaling by
    /// the pixmap's ratio tag. The three-argument form because QPainter has no (QRectF, QPixmap)
    /// overload, and toRect() would throw the pane's fractional position away.
    painter.drawPixmap(target, m_prepared, QRectF(m_prepared.rect()));
}

void PdfPageCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPointF at = QPointF(mousePosition(event));
    beginGesture();
    m_anchored = directionIsMeaningful(at);
    m_grabDirection = directionAt(at.toPoint());
    m_lastDirection = m_grabDirection;
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void PdfPageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_gesture) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const int direction = directionAt(mousePosition(event));
    if (m_anchored) {
        /// The page follows the hand: the angle between where it was taken hold of and where the
        /// hand is now, added to the angle it was at. This is what makes an arbitrary angle -- 37
        /// degrees is exactly as easy as 90.
        previewAt(m_gestureStart + signedDelta(m_grabDirection, direction));
    } else {
        /// Taken hold of on the centre, where there is no direction to anchor on: each movement adds
        /// the turn the hand made since the last one.
        previewAt(m_rotation + signedDelta(m_lastDirection, direction));
    }
    m_lastDirection = direction;
    event->accept();
}

void PdfPageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    /// The hand is up: whatever it landed on becomes the one pending edit of this gesture.
    endGesture();
    setCursor(Qt::OpenHandCursor);
    event->accept();
}

bool PdfPageCanvas::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        handleTouch(static_cast<QTouchEvent *>(event));
        event->accept();
        return true;
    default:
        break;
    }
    return QWidget::event(event);
}

void PdfPageCanvas::handleTouch(QTouchEvent *event)
{
    const QList<QPointF> positions = liveTouchPositions(event);

    if (positions.size() >= 2) {
        /// Two fingers: the page turns by the angle the line between them turns by, which is the
        /// twist everyone already knows from a photograph viewer.
        const QPointF between = positions.at(1) - positions.at(0);
        const int direction =
            normalizedTurn(qRound(qRadiansToDegrees(qAtan2(between.y(), between.x()))));
        if (!m_gesture || !m_twisting) {
            beginGesture();
            m_twisting = true;
            m_twistFrom = direction;
            return;
        }
        previewAt(m_gestureStart + signedDelta(m_twistFrom, direction));
        return;
    }

    if (positions.isEmpty()) {
        /// Every finger is up: the gesture is over, and what it landed on becomes the pending edit.
        endGesture();
        return;
    }

    /// A twist that lost a finger ends here rather than turning into a drag in mid-air.
    if (m_gesture && m_twisting) {
        endGesture();
        return;
    }

    /// One finger is the drag the mouse path already knows how to do.
    const QPointF at = positions.first();
    const int direction = directionAt(at.toPoint());
    if (!m_gesture) {
        beginGesture();
        m_anchored = directionIsMeaningful(at);
        m_grabDirection = direction;
        m_lastDirection = direction;
        return;
    }

    if (m_anchored) {
        previewAt(m_gestureStart + signedDelta(m_grabDirection, direction));
    } else {
        previewAt(m_rotation + signedDelta(m_lastDirection, direction));
    }
    m_lastDirection = direction;
}

/**
 * The reorder's visual: the row being dragged, and the line it would land on.
 *
 * A child of the table's viewport, painted over the rows and transparent to the mouse, so it can sit
 * there without changing what the table does with a click. It is shown only while a drag is in
 * flight: the highlight says which page is in the hand, and the line across the viewport says where
 * the page will land. The line is the conventional shape for this, and on a tablet it is the
 * difference between a drop that is obvious and one that has to be guessed at.
 */
class PdfRowDropIndicator : public QWidget
{
public:
    explicit PdfRowDropIndicator(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("pdfio_ops_drop_indicator"));
        /// The drag's own moves have to reach the viewport, not this.
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    /// \a held is the held row's rectangle and \a lineY the insertion point, both in the viewport's
    /// own coordinates. A null \a held paints the line alone.
    void setDrop(const QRect &held, int lineY)
    {
        m_held = held;
        m_line = lineY;
        update();
    }

    void clearDrop()
    {
        m_held = QRect();
        m_line = -1;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);

        QPainter painter(this);
        /// A sheet of glass over the rows: it starts transparent, so everything not painted here
        /// shows the table underneath -- and what is painted is unambiguous.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor accent(0x1e, 0x88, 0xe5);
        if (m_held.isValid()) {
            /// The page in the hand: the row stays where it is in the list until the drop, so the
            /// highlight is what says it is being moved.
            painter.fillRect(m_held, QColor(accent.red(), accent.green(), accent.blue(), 64));
            painter.setPen(QPen(accent, 1));
            painter.drawRect(m_held.adjusted(0, 0, -1, -1));
        }

        if (m_line >= 0) {
            painter.setPen(QPen(accent, 3));
            painter.drawLine(0, m_line, width(), m_line);
        }
    }

private:
    QRect m_held;
    int m_line = -1;
};

/**
 * Draws a page card from the pixels this screen prepared for it.
 *
 * The style's own icon drawing cannot be used for this, and that is measured rather than assumed
 * (Qt 5.15, an offscreen table at a 2.5 device ratio, the same one-pixel pattern in two cards):
 *
 *  - a card prepared at 115x155 DEVICE pixels and tagged 2.5 -- logical 46x62, exactly what the box
 *    is drawn at -- came out through the style as 42% mid-grey pixels: the delegate asks QIcon for
 *    the icon at the LOGICAL size, QIcon answers in logical pixels, and the ratio tag is lost on the
 *    way, so the picture was smoothed down to 46x62 and stretched again by the screen;
 *  - the same pixels drawn here came out with under 1% mid-grey: the whole source rect onto the
 *    logical card rect, with the painter already carrying the screen's ratio, maps one device pixel
 *    to one device pixel.
 *
 * The row itself -- background, selection, the struck-through words of a page marked for deletion --
 * is still the style's; only the picture is drawn here.
 */
class PdfPreviewDelegate : public QStyledItemDelegate
{
public:
    explicit PdfPreviewDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.column() != ThumbnailColumn) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem cell = option;
        initStyleOption(&cell, index);
        const QPixmap prepared = index.data(CardPixmapRole).value<QPixmap>();

        /// The picture is this delegate's business, not the style's.
        cell.icon = QIcon();
        QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &cell, painter, option.widget);

        if (prepared.isNull()) {
            return;
        }

        const qreal ratio = prepared.devicePixelRatio() > 0 ? prepared.devicePixelRatio() : 1.0;
        const QSize logical(qRound(prepared.width() / ratio), qRound(prepared.height() / ratio));
        QRect card(0, 0, logical.width(), logical.height());
        card.moveCenter(option.rect.center());
        painter->drawPixmap(card, prepared, QRectF(prepared.rect()));
    }
};

PdfNotebookOpsDialog::PdfNotebookOpsDialog(const QString &projectDir,
                                           const PdfSessionManifest &manifest, int anchorPage,
                                           QWidget *parent)
    : QDialog(parent)
    , m_projectDir(projectDir)
    , m_original(manifest)
    , m_anchor(anchorPage)
    , m_nextNumber(PdfNotebookOps::nextFreePageNumber(projectDir))
{
    setWindowTitle(i18n("Notebook ops"));

    for (const PdfPageRecord &page : manifest.pages) {
        Row row;
        row.record = page;
        row.fromDir = projectDir;
        m_rows.append(row);
    }

    buildUi();
    refresh();
}

void PdfNotebookOpsDialog::buildUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(
        i18n("Notebook \"%1\" has %2 page(s). Change the list here; nothing is written until you "
             "press Apply, and then the whole change is one step you can undo. Drag the page on "
             "the canvas to turn it by any angle, swipe a page left or right for a quarter turn, "
             "or press and hold a page to drag it somewhere else in the list.",
             m_original.displayName(), m_rows.size()),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *middle = new QHBoxLayout;

    m_table = new QTableWidget(m_rows.size(), ColumnCount, this);
    m_table->setHorizontalHeaderLabels({ i18n("Page"), i18n("#"), i18n("From"), i18n("Size"),
                                         i18n("Turn"), i18n("Ink") });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setIconSize(QSize(46, 62));
    m_table->verticalHeader()->setDefaultSectionSize(70);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setMinimumWidth(560);
    /// The card is drawn by a delegate of this screen's own, not by the style's icon drawing: see
    /// PdfPreviewDelegate for the measurement behind that.
    m_table->setItemDelegate(new PdfPreviewDelegate(m_table));
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() { refreshFooter(); });
    /// On the viewport rather than on the table: the viewport is the widget the press, the moves
    /// and the release actually arrive at, and it sees them before the view turns them into a
    /// selection.
    m_table->viewport()->installEventFilter(this);
    /// The hold that turns a press into a grab, and the visual a grab shows. Both belong to the
    /// viewport the gesture is made on.
    m_hold = new QTimer(this);
    m_hold->setSingleShot(true);
    m_hold->setInterval(HoldDelayMs);
    connect(m_hold, &QTimer::timeout, this, [this]() { startGrab(); });
    m_dropIndicator = new PdfRowDropIndicator(m_table->viewport());
    middle->addWidget(m_table, 1);

    /// The canvas: the selected page, large, where it can be turned to an angle no button can name.
    /// A turn made here is an edit of the working copy like any other -- it writes nothing.
    auto *preview = new QVBoxLayout;
    m_canvas = new PdfPageCanvas(this);
    m_canvas->setToolTip(i18n("Drag the page around its centre to turn it by any angle, or twist "
                              "two fingers on it."));
    /// While the hand is down the dialog moves only the readout: the record, the list and the
    /// pending change wait for the gesture to end, and then ONE report becomes one pending edit.
    m_canvas->previewed = [this](int degrees) {
        if (m_angle) {
            m_angle->setText(i18n("Turn: %1", turnLabel(degrees)));
        }
    };
    m_canvas->turned = [this](int degrees) { setSelectedTurn(degrees); };
    /// What the pane is about to draw, in device pixels. When the row's own preview has fewer pixels
    /// than that, the navigator is asked for a fresh one rather than the small one stretched.
    m_canvas->needsPreview = [this](const QSize &devicePixels) {
        const int row = m_table->currentRow();
        if (row >= 0 && row < m_rows.size()) {
            requestPreviewIfTooSmall(m_rows[row], devicePixels);
        }
    };
    preview->addWidget(m_canvas, 1);
    m_angle = new QLabel(this);
    m_angle->setObjectName(QStringLiteral("pdfio_ops_angle"));
    m_angle->setAlignment(Qt::AlignCenter);
    preview->addWidget(m_angle);
    middle->addLayout(preview, 2);

    auto *buttons = new QVBoxLayout;
    const auto addButton = [this, buttons](const QString &text, const std::function<void()> &action) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, action);
        buttons->addWidget(button);
        return button;
    };
    /// Every control is named, so a test -- and a screenshot probe looking for the screen -- can
    /// reach it without reading pixel positions.
    m_moveUp = addButton(i18n("Move up"), [this]() { moveSelected(-1); });
    m_moveUp->setObjectName(QStringLiteral("pdfio_ops_move_up"));
    m_moveDown = addButton(i18n("Move down"), [this]() { moveSelected(1); });
    m_moveDown->setObjectName(QStringLiteral("pdfio_ops_move_down"));
    buttons->addSpacing(10);
    m_duplicate = addButton(i18n("Duplicate"), [this]() { duplicateSelected(); });
    m_duplicate->setObjectName(QStringLiteral("pdfio_ops_duplicate"));
    m_delete = addButton(i18n("Delete page"), [this]() { deleteSelected(); });
    m_delete->setObjectName(QStringLiteral("pdfio_ops_delete_page"));
    m_keep = addButton(i18n("Keep page"), [this]() { keepSelected(); });
    m_keep->setObjectName(QStringLiteral("pdfio_ops_keep_page"));
    buttons->addSpacing(10);
    m_rotateLeft = addButton(i18n("Turn left"), [this]() { rotateSelected(-90); });
    m_rotateLeft->setObjectName(QStringLiteral("pdfio_ops_turn_left"));
    m_rotateRight = addButton(i18n("Turn right"), [this]() { rotateSelected(90); });
    m_rotateRight->setObjectName(QStringLiteral("pdfio_ops_turn_right"));
    buttons->addSpacing(18);

    /// Pages can be brought in from another PDF, and another notebook can be merged in: both are
    /// edits of THIS notebook's list, so they join the pending change like any other button.
    m_insert = addButton(i18n("Insert pages from a PDF..."), [this]() { insertPagesFromPdf(); });
    m_insert->setObjectName(QStringLiteral("pdfio_ops_insert"));
    m_merge = addButton(i18n("Merge a notebook in..."), [this]() { mergeNotebookIn(); });
    m_merge->setObjectName(QStringLiteral("pdfio_ops_merge_notebook"));
    /// Extract writes a notebook of its own, somewhere else: it cannot be part of this notebook's
    /// Apply, so it is the one entry that waits for the list to be settled.
    m_extract = addButton(i18n("Extract a page range..."), [this]() {
        m_requested = ExtractRangeAction;
        accept();
    });
    m_extract->setObjectName(QStringLiteral("pdfio_ops_extract"));

    buttons->addStretch(1);
    middle->addLayout(buttons);
    layout->addLayout(middle, 1);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("pdfio_ops_summary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("pdfio_ops_hint"));
    m_hint->setWordWrap(true);
    layout->addWidget(m_hint);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_apply = box->addButton(i18n("Apply"), QDialogButtonBox::AcceptRole);
    m_apply->setObjectName(QStringLiteral("pdfio_ops_apply"));
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_apply, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(box);

    /// The navigator writes a preview in the background and says so when it lands; that is when a
    /// row that asked for one takes the new pixels. Connected here, once everything the handler
    /// touches exists.
    connect(PdfPageNavigator::instance(), &PdfPageNavigator::thumbnailReady, this,
            [this](int index) { adoptFreshPreview(index); });

    resize(1180, 720);
}

void PdfNotebookOpsDialog::refresh()
{
    const int keepSelection = m_table->currentRow();

    m_table->setRowCount(m_rows.size());
    int position = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &row = m_rows[i];
        if (!row.removed) {
            ++position;
        }

        /// The row's card shows the page as the change would leave it, at the pixels the screen
        /// will draw it with. The pixels are kept in the row rather than read from the record's
        /// thumbFile because a turn drops that stale name, and the list must not go blank exactly
        /// where a page is turned.
        auto *thumbnail = new QTableWidgetItem;
        loadPreview(row);
        m_table->setItem(i, ThumbnailColumn, thumbnail);
        applyCardIcon(i);

        const auto cell = [this, i, &row](Column column, const QString &text) {
            auto *item = new QTableWidgetItem(text);
            if (row.removed) {
                /// Struck through rather than taken away: a page marked for deletion stays visible
                /// until Apply, so a mistake is one click from being undone.
                QFont font = item->font();
                font.setStrikeOut(true);
                item->setFont(font);
            }
            m_table->setItem(i, column, item);
        };
        cell(PositionColumn, row.removed ? i18n("(gone)") : QString::number(position));
        cell(SourceColumn, sourceLabel(row.record));
        cell(SizeColumn, sizeLabel(row.record));
        cell(TurnColumn, turnLabel(row.record.extraRotation));

        /// The ink column answers "has this page ever been drawn on": the artifact is the ink, and a
        /// page that was never drawn on has none.
        const bool inked = QFileInfo::exists(QDir(row.fromDir).filePath(row.record.kraFile));
        cell(InkColumn, row.fromMergedNotebook ? i18n("from the merged notebook")
                                               : (row.fromAddedPdf ? i18n("new page")
                                                                   : (row.isNew ? i18n("new copy")
                                                                                : (inked ? i18n("drawn on")
                                                                                         : i18n("blank")))));
    }

    if (keepSelection >= 0 && keepSelection < m_rows.size()) {
        m_table->selectRow(keepSelection);
    } else if (!m_rows.isEmpty()) {
        m_table->selectRow(0);
    }

    refreshFooter();
}

void PdfNotebookOpsDialog::refreshFooter()
{
    const QStringList pending = pendingDescriptions();
    const bool dirty = hasPendingEdits();

    /// Apply is off while nothing has changed: a no-op would still close and reopen the notebook,
    /// and a change here is meant to be a deliberate single act.
    m_apply->setEnabled(dirty);
    m_summary->setText(dirty
                           ? i18n("%1 change(s) pending: %2", pending.size(),
                                  pending.join(i18n(", ")))
                           : i18n("No changes yet: the notebook is as it is."));

    const int row = m_table->currentRow();
    const bool selected = row >= 0 && row < m_rows.size();
    const bool live = selected && !m_rows.at(row).removed;

    m_moveUp->setEnabled(live && keptAbove(row));
    m_moveDown->setEnabled(live && keptBelow(row));
    m_duplicate->setEnabled(live);
    m_delete->setEnabled(live && keptCount() > 1);
    m_keep->setEnabled(selected && m_rows.at(row).removed);
    m_rotateLeft->setEnabled(live);
    m_rotateRight->setEnabled(live);

    /// Insert and merge are edits of this notebook's page list, so they stay available with a change
    /// pending, like every other button. Extract writes a notebook of its own and waits, with the
    /// hint saying why.
    m_insert->setEnabled(true);
    m_merge->setEnabled(true);
    m_extract->setEnabled(!dirty);

    m_hint->setText(availabilityHint());

    /// The canvas follows the selection and the pending turn, and this is the one place both
    /// changes arrive: the header keeps the two from ever disagreeing.
    refreshCanvas();
}

void PdfNotebookOpsDialog::moveSelected(int delta)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    /// The nearest page that is still kept, in the direction asked for: a page marked for deletion
    /// is not somewhere a page can be moved to.
    int target = -1;
    for (int i = row + delta; i >= 0 && i < m_rows.size(); i += delta) {
        if (!m_rows.at(i).removed) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        return;
    }

    /// The same reorder a drop makes: a button and a drag differ only in where the row is put.
    moveRow(row, target);
}

void PdfNotebookOpsDialog::moveRow(int from, int to)
{
    if (from < 0 || from >= m_rows.size()) {
        return;
    }
    to = qBound(0, to, m_rows.size() - 1);
    if (to == from) {
        /// Dropped where it already is: nothing changes, so nothing becomes pending and Apply
        /// stays exactly where it was.
        return;
    }

    /// The row is taken out and put back at \a to, so the rows it passes move up or down by one --
    /// the usual drag-to-reorder, not a swap of two pages. Nothing is written: this edits the same
    /// working copy every button edits, and the whole move is one change when Apply runs.
    const Row moved = m_rows.takeAt(from);
    m_rows.insert(to, moved);

    refresh();
    m_table->selectRow(to);
}

void PdfNotebookOpsDialog::beginHold()
{
    m_holding = false;
    if (!m_hold || m_swipeRow < 0 || m_swipeRow >= m_rows.size()) {
        return;
    }
    if (m_rows.at(m_swipeRow).removed) {
        /// A page marked for deletion is on its way out: dragging it would only move something the
        /// change is about to take away, which is the same reason "Move up"/"Move down" are off for
        /// it. The press still selects it, so "Keep page" is one click away.
        return;
    }

    m_holding = true;
    m_hold->start(HoldDelayMs);
}

void PdfNotebookOpsDialog::startGrab()
{
    if (!m_holding) {
        return;
    }
    m_holding = false;
    if (m_swipeRow < 0 || m_swipeRow >= m_rows.size() || m_rows.at(m_swipeRow).removed) {
        return;
    }

    m_grabbing = true;
    m_grabRow = m_swipeRow;
    m_dropGap = dropGapAt(m_swipeFrom.y());
    showDropFeedback();
}

void PdfNotebookOpsDialog::updateGrab(const QPoint &at)
{
    m_dropGap = dropGapAt(at.y());
    showDropFeedback();
}

void PdfNotebookOpsDialog::finishGrab(bool dropped)
{
    const int from = m_grabRow;
    const int gap = m_dropGap;
    endGrab();

    /// Released outside the table: the drag is cancelled and the list is left exactly as it was.
    if (!dropped || from < 0 || gap < 0) {
        return;
    }

    /// Taking the row out first shifts everything after it up by one, so a gap below the row means
    /// the row's own final position is one less than that gap. A gap where the row already is comes
    /// out as \a from and moveRow() leaves it alone.
    moveRow(from, gap > from ? gap - 1 : gap);
}

void PdfNotebookOpsDialog::endGrab()
{
    m_grabbing = false;
    m_grabRow = -1;
    m_dropGap = -1;
    if (m_dropIndicator) {
        m_dropIndicator->clearDrop();
        m_dropIndicator->hide();
    }
}

int PdfNotebookOpsDialog::dropGapAt(int y) const
{
    if (!m_table || m_rows.isEmpty()) {
        return 0;
    }

    for (int row = 0; row < m_rows.size(); ++row) {
        const int top = m_table->rowViewportPosition(row);
        const int height = m_table->rowHeight(row);
        if (y < top + height) {
            /// The upper half of a row means "before it" and the lower half "after it", so the
            /// insertion line is always drawn where the pointer is and the drop is where the line is.
            return y < top + height / 2 ? row : row + 1;
        }
    }

    /// Below the last row is the end of the list.
    return m_rows.size();
}

void PdfNotebookOpsDialog::showDropFeedback()
{
    if (!m_dropIndicator || !m_table) {
        return;
    }

    /// The row in the hand, highlighted. The list's order does not change until the drop, so this is
    /// what says which page is being moved.
    QRect held;
    if (m_grabRow >= 0 && m_grabRow < m_rows.size()) {
        held = QRect(0, m_table->rowViewportPosition(m_grabRow), m_table->viewport()->width(),
                     m_table->rowHeight(m_grabRow));
    }

    /// And the line, at the top of the gap's row -- or along the bottom of the last row when the gap
    /// is the end of the list.
    int line = 0;
    if (m_dropGap >= 0 && m_dropGap < m_rows.size()) {
        line = m_table->rowViewportPosition(m_dropGap);
    } else if (!m_rows.isEmpty()) {
        const int last = m_rows.size() - 1;
        line = m_table->rowViewportPosition(last) + m_table->rowHeight(last);
    }

    m_dropIndicator->setGeometry(m_table->viewport()->rect());
    m_dropIndicator->setDrop(held, line);
    m_dropIndicator->show();
    m_dropIndicator->raise();
}

void PdfNotebookOpsDialog::duplicateSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    const Row source = m_rows.at(row);
    Row copy;
    copy.record = source.record;
    copy.record.generation = 0;
    copy.fromDir = m_projectDir;
    copy.isNew = true;
    /// The allocator is asked once, at the start, and each new page takes the next number: the
    /// engine refuses a destination the notebook already names, so a number is never guessed.
    const int number = m_nextNumber++;
    copy.record.kraFile = PdfSession::pageFileNameForNumber(number);
    copy.record.thumbFile = PdfSession::thumbFileNameForNumber(number);

    /// The files come from where the page is now, which may itself be a copy made a moment ago: the
    /// engine copies the list in order, so a chain of duplicates resolves.
    m_copies.append(qMakePair(QDir(source.fromDir).filePath(source.record.kraFile),
                              copy.record.kraFile));
    m_copies.append(qMakePair(QDir(source.fromDir).filePath(source.record.kraFile
                                                              + QStringLiteral(".layers.txt")),
                              copy.record.kraFile + QStringLiteral(".layers.txt")));
    if (!source.record.thumbFile.isEmpty()) {
        m_copies.append(qMakePair(QDir(source.fromDir).filePath(source.record.thumbFile),
                                  copy.record.thumbFile));
    }
    m_copyDirs.append(qMakePair(QDir(source.fromDir).filePath(source.record.kraFile
                                                             + QStringLiteral(".layers")),
                                copy.record.kraFile + QStringLiteral(".layers")));

    m_rows.insert(row + 1, copy);
    refresh();
    m_table->selectRow(row + 1);
}

void PdfNotebookOpsDialog::deleteSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }
    if (keptCount() <= 1) {
        return; /// the button is off, and the hint says why
    }

    if (m_rows.at(row).isNew) {
        /// It never existed: dropping it drops the files it would have brought, and there is
        /// nothing for the journal to take over.
        const Row dropped = m_rows.takeAt(row);
        const auto forget = [&dropped](QList<QPair<QString, QString>> *list) {
            for (int i = list->size() - 1; i >= 0; --i) {
                const QString destination = list->at(i).second;
                if (destination == dropped.record.kraFile
                    || destination == dropped.record.kraFile + QStringLiteral(".layers")
                    || destination == dropped.record.kraFile + QStringLiteral(".layers.txt")
                    || (!dropped.record.thumbFile.isEmpty()
                        && destination == dropped.record.thumbFile)) {
                    list->removeAt(i);
                }
            }
        };
        forget(&m_copies);
        forget(&m_copyDirs);
    } else {
        Row &gone = m_rows[row];
        gone.removed = true;
        m_removals << gone.record.kraFile
                   << gone.record.kraFile + QStringLiteral(".layers")
                   << gone.record.kraFile + QStringLiteral(".layers.txt");
        if (!gone.record.thumbFile.isEmpty()) {
            m_removals << gone.record.thumbFile;
        }
    }

    refresh();
    m_table->selectRow(qBound(0, row, m_rows.size() - 1));
}

void PdfNotebookOpsDialog::keepSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || !m_rows.at(row).removed) {
        return;
    }

    Row &back = m_rows[row];
    back.removed = false;
    m_removals.removeAll(back.record.kraFile);
    m_removals.removeAll(back.record.kraFile + QStringLiteral(".layers"));
    m_removals.removeAll(back.record.kraFile + QStringLiteral(".layers.txt"));
    if (!back.record.thumbFile.isEmpty()) {
        m_removals.removeAll(back.record.thumbFile);
    }

    refresh();
    m_table->selectRow(row);
}

void PdfNotebookOpsDialog::turnRow(int row, int degrees)
{
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    Row &turning = m_rows[row];
    turning.record.extraRotation = ((turning.record.extraRotation + degrees) % 360 + 360) % 360;
    /// The preview was made from the page before this turn, so it goes with the change: a stale
    /// preview would show the page the wrong way up, and the next save makes a new one.
    dropThumbnail(turning);

    refresh();
    m_table->selectRow(row);
}

void PdfNotebookOpsDialog::rotateSelected(int degrees)
{
    turnRow(m_table->currentRow(), degrees);
}

void PdfNotebookOpsDialog::setSelectedTurn(int degrees)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    const int angle = normalizedTurn(degrees);
    const int current = m_rows.at(row).record.extraRotation;
    if (angle == current) {
        /// The hand wobbled and came back to where it started: nothing changed, so nothing is
        /// dropped and Apply stays where it was.
        return;
    }

    /// A delta into the one turn path: the record the canvas leaves behind is the record a button
    /// leaves behind, which is what keeps the two from ever disagreeing.
    turnRow(row, angle - current);
}

void PdfNotebookOpsDialog::loadPreview(Row &row)
{
    if (!row.preview.isNull() || row.record.thumbFile.isEmpty()) {
        return;
    }

    const QString path = QDir(row.fromDir).filePath(row.record.thumbFile);
    if (!QFileInfo::exists(path)) {
        /// A preview is only a name until a save has drawn one. The canvas shows an empty sheet
        /// instead, and the page can still be turned.
        return;
    }

    const QPixmap preview(path);
    if (preview.isNull()) {
        return;
    }

    /// Kept together with the turn it was drawn at: everything painted from it is a difference from
    /// that turn.
    row.preview = preview;
    row.previewRotation = row.record.extraRotation;
}

void PdfNotebookOpsDialog::refreshCanvas()
{
    if (!m_canvas || !m_angle) {
        return;
    }

    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size()) {
        m_canvas->clearPage();
        m_angle->setText(i18n("Turn: select a page to turn it."));
        return;
    }

    Row &selected = m_rows[row];
    if (selected.removed) {
        /// Marked for deletion: the pending turn is still shown, but there is no page to turn --
        /// "Keep page" brings it back.
        m_canvas->setPage(QPixmap(), 0, selected.record.extraRotation, selected.record.sizePt);
        m_angle->setText(i18n("Turn: %1 (marked for deletion)",
                              turnLabel(selected.record.extraRotation)));
        return;
    }

    loadPreview(selected);
    m_canvas->setPage(selected.preview, selected.previewRotation, selected.record.extraRotation,
                      selected.record.sizePt);
    m_angle->setText(i18n("Turn: %1", turnLabel(selected.record.extraRotation)));
}

PdfNotebookOpsDialog::PreparedPreview PdfNotebookOpsDialog::cardPreview(int row) const
{
    PreparedPreview prepared;
    if (!m_table || row < 0 || row >= m_rows.size()) {
        return prepared;
    }

    const Row &card = m_rows.at(row);
    if (card.preview.isNull()) {
        return prepared;
    }

    /// Turned first, then fitted: a quarter turn swaps the page's sides, so the box the card is
    /// drawn in is the turned one and not the one it had before.
    const QPixmap turned = turnedPixmap(card.preview,
                                        card.record.extraRotation - card.previewRotation);
    const QSize fitted = turned.size().scaled(m_table->iconSize(), Qt::KeepAspectRatio);
    const qreal ratio = m_table->devicePixelRatioF() > 0 ? m_table->devicePixelRatioF() : 1.0;

    prepared.sourcePixels = card.preview.size();
    prepared.logicalSize = fitted;
    prepared.pixmap = pdfioPreviewForDisplay(turned, fitted, ratio);
    prepared.devicePixelRatio = ratio;
    return prepared;
}

PdfNotebookOpsDialog::PreparedPreview PdfNotebookOpsDialog::canvasPreview() const
{
    PreparedPreview prepared;
    if (!m_canvas) {
        return prepared;
    }

    prepared.pixmap = m_canvas->preparedPreview();
    prepared.sourcePixels = m_canvas->sourcePixels();
    prepared.devicePixelRatio = prepared.pixmap.isNull() ? 1.0 : prepared.pixmap.devicePixelRatio();
    if (!prepared.pixmap.isNull()) {
        const qreal ratio = prepared.devicePixelRatio > 0 ? prepared.devicePixelRatio : 1.0;
        prepared.logicalSize = QSize(qRound(prepared.pixmap.width() / ratio),
                                     qRound(prepared.pixmap.height() / ratio));
    }
    return prepared;
}

void PdfNotebookOpsDialog::applyCardIcon(int row)
{
    if (!m_table || row < 0 || row >= m_rows.size()) {
        return;
    }

    QTableWidgetItem *item = m_table->item(row, ThumbnailColumn);
    if (!item) {
        return;
    }

    const PreparedPreview card = cardPreview(row);
    /// The prepared pixels are what the delegate draws. The icon is kept as well, so the item still
    /// carries a decoration for anything that reads one -- the style never draws it, the delegate
    /// does.
    item->setData(CardPixmapRole, QVariant::fromValue(card.pixmap));
    item->setIcon(card.pixmap.isNull() ? QIcon() : QIcon(card.pixmap));
}

int PdfNotebookOpsDialog::navigatorIndexFor(const PdfPageRecord &record) const
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook() || record.kraFile.isEmpty()) {
        return -1;
    }

    /// The same project, or nothing: a preview is written into the notebook it belongs to, and a
    /// screen showing another one must not cause a write there.
    if (QFileInfo(navigator->projectDir()).absoluteFilePath()
        != QFileInfo(m_projectDir).absoluteFilePath()) {
        return -1;
    }

    const QList<PdfPageRecord> &pages = navigator->manifest().pages;
    for (int i = 0; i < pages.size(); ++i) {
        if (pages.at(i).kraFile == record.kraFile) {
            return i;
        }
    }
    return -1;
}

void PdfNotebookOpsDialog::requestPreviewIfTooSmall(Row &row, const QSize &devicePixels)
{
    if (row.previewRequested || row.record.thumbFile.isEmpty() || devicePixels.isEmpty()) {
        return;
    }

    const bool enough = !row.preview.isNull() && row.preview.width() >= devicePixels.width()
        && row.preview.height() >= devicePixels.height();
    if (enough) {
        return;
    }

    const int index = navigatorIndexFor(row.record);
    if (index < 0) {
        /// A page this change brought in -- an insertion, a merge, a duplicate -- has no page in the
        /// navigator's notebook to ask about. Its own preview is what it has.
        return;
    }

    /// Asked once per row: a preview that is still small after the ask would otherwise be asked for
    /// again on every repaint.
    row.previewRequested = true;
    PdfPageNavigator::instance()->ensureThumbnail(index);
}

void PdfNotebookOpsDialog::adoptFreshPreview(int navigatorIndex)
{
    PdfPageNavigator *navigator = PdfPageNavigator::instance();
    if (!navigator->hasNotebook() || navigatorIndex < 0
        || navigatorIndex >= m_original.pages.size()) {
        return;
    }
    if (QFileInfo(navigator->projectDir()).absoluteFilePath()
        != QFileInfo(m_projectDir).absoluteFilePath()) {
        return;
    }

    /// The file the navigator has just written, and the turn it was made at: the notebook's own
    /// committed turn, not the one this screen is holding on top of it.
    const PdfPageRecord &page = m_original.pages.at(navigatorIndex);

    for (int i = 0; i < m_rows.size(); ++i) {
        Row &row = m_rows[i];
        if (!row.previewRequested || row.record.kraFile != page.kraFile) {
            continue;
        }

        const QPixmap fresh(QDir(row.fromDir).filePath(page.thumbFile));
        if (fresh.isNull()) {
            continue;
        }

        row.preview = fresh;
        row.previewRotation = page.extraRotation;
        applyCardIcon(i);

        /// A preview that arrived while the hand is turning the page would move it under the hand;
        /// the pane takes the new pixels when the turn is done.
        if (m_canvas && m_table->currentRow() == i && !m_canvas->isTurning()) {
            m_canvas->setPage(row.preview, row.previewRotation, row.record.extraRotation,
                              row.record.sizePt);
        }
    }
}

bool PdfNotebookOpsDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (m_table && watched == m_table->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            const QPoint at = mousePosition(static_cast<QMouseEvent *>(event));
            m_swipeFrom = at;
            /// Selected on the press rather than on the release: the gesture is about the row it was
            /// made on, and every control on this screen acts on the selection.
            m_swipeRow = m_table->rowAt(at.y());
            if (m_swipeRow >= 0) {
                m_table->selectRow(m_swipeRow);
            }
            /// The reorder is armed on the SAME press the swipe is armed on -- one state machine,
            /// two outcomes. A press that moves more than a few pixels before the hold fires is the
            /// swipe; a press that stays put long enough becomes a grab.
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                beginHold();
            }
        } else if (event->type() == QEvent::MouseMove) {
            const QPoint at = mousePosition(static_cast<QMouseEvent *>(event));
            if (m_grabbing) {
                /// The hold has fired, so the drag owns the gesture: the insertion point follows the
                /// pointer, and the table must not scroll or select behind it.
                updateGrab(at);
                return true;
            }
            if (m_holding && (at - m_swipeFrom).manhattanLength() > HoldSlopPixels) {
                /// Moved before the hold fired: this is a swipe (or a slipped click), not a grab.
                m_holding = false;
                if (m_hold) {
                    m_hold->stop();
                }
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const QPoint at = mousePosition(static_cast<QMouseEvent *>(event));
            const int row = m_swipeRow;
            m_swipeRow = -1;
            m_holding = false;
            if (m_hold) {
                m_hold->stop();
            }

            if (m_grabbing) {
                /// The drag owns this release too: where it landed is a reorder, and the turn a
                /// sideways drag would otherwise have meant cannot happen at all. A release outside
                /// the table cancels instead.
                finishGrab(m_table->viewport()->rect().contains(at));
                return true;
            }

            const int dx = at.x() - m_swipeFrom.x();
            const int dy = at.y() - m_swipeFrom.y();

            /// Sideways and far enough, and not a drag down the list. A swipe to the right turns the
            /// page right, which is the way the paper goes; one swipe is one quarter turn, because a
            /// swipe carries a direction and no angle. The canvas is where any angle is asked for.
            if (row >= 0 && qAbs(dx) >= SwipeTurnPixels && qAbs(dx) > qAbs(dy)) {
                turnRow(row, dx > 0 ? 90 : -90);
            }
        }
    }

    return QDialog::eventFilter(watched, event);
}

void PdfNotebookOpsDialog::dropThumbnail(Row &row)
{
    if (row.record.thumbFile.isEmpty()) {
        return;
    }

    const QString thumb = row.record.thumbFile;
    for (int i = m_copies.size() - 1; i >= 0; --i) {
        if (m_copies.at(i).second == thumb) {
            m_copies.removeAt(i);
        }
    }
    if (!row.isNew && !m_removals.contains(thumb)) {
        m_removals << thumb;
    }
    row.record.thumbFile.clear();
}

bool PdfNotebookOpsDialog::hasPendingEdits() const
{
    if (!m_copies.isEmpty() || !m_copyDirs.isEmpty() || !m_removals.isEmpty()) {
        return true;
    }

    QList<PdfPageRecord> now;
    for (const Row &row : m_rows) {
        if (!row.removed) {
            now.append(row.record);
        }
    }
    if (now.size() != m_original.pages.size()) {
        return true;
    }
    for (int i = 0; i < now.size(); ++i) {
        const PdfPageRecord &a = now.at(i);
        const PdfPageRecord &b = m_original.pages.at(i);
        if (a.kraFile != b.kraFile || a.source != b.source || a.index != b.index
            || a.extraRotation != b.extraRotation || a.sizePt != b.sizePt
            || a.thumbFile != b.thumbFile) {
            return true;
        }
    }
    return false;
}

QStringList PdfNotebookOpsDialog::pendingDescriptions() const
{
    QStringList parts;

    int deleted = 0;
    int duplicated = 0;
    int inserted = 0;
    int mergedIn = 0;
    for (const Row &row : m_rows) {
        if (row.removed) {
            ++deleted;
        } else if (row.fromMergedNotebook) {
            ++mergedIn;
        } else if (row.fromAddedPdf) {
            ++inserted;
        } else if (row.isNew) {
            ++duplicated;
        }
    }
    if (deleted > 0) {
        parts << i18np("%1 page deleted", "%1 pages deleted", deleted);
    }
    if (inserted > 0) {
        parts << i18np("%1 page inserted from a PDF", "%1 pages inserted from a PDF", inserted);
    }
    if (mergedIn > 0) {
        parts << i18np("%1 page merged in from another notebook",
                       "%1 pages merged in from another notebook", mergedIn);
    }
    if (duplicated > 0) {
        parts << i18np("%1 page duplicated", "%1 pages duplicated", duplicated);
    }

    QHash<QString, int> turnBefore;
    for (const PdfPageRecord &page : m_original.pages) {
        turnBefore.insert(page.kraFile, page.extraRotation);
    }
    int turned = 0;
    for (const Row &row : m_rows) {
        if (row.removed || row.isNew) {
            continue;
        }
        if (turnBefore.value(row.record.kraFile, row.record.extraRotation)
            != row.record.extraRotation) {
            ++turned;
        }
    }
    if (turned > 0) {
        parts << i18np("%1 page turned", "%1 pages turned", turned);
    }

    /// The order: the pages that were here, in the order they are in now, against the order they
    /// had. A new page's place is not a move of anything, so it is left out of the comparison.
    QStringList were, are;
    for (const PdfPageRecord &page : m_original.pages) {
        were << page.kraFile;
    }
    for (const Row &row : m_rows) {
        if (!row.removed && !row.isNew) {
            are << row.record.kraFile;
        }
    }
    QStringList stillHere;
    for (const QString &name : were) {
        if (are.contains(name)) {
            stillHere << name;
        }
    }
    if (!are.isEmpty() && are != stillHere) {
        parts << i18n("the order changed");
    }

    if (parts.isEmpty() && hasPendingEdits()) {
        parts << i18n("the page list changed");
    }
    return parts;
}

QString PdfNotebookOpsDialog::availabilityHint() const
{
    if (m_rows.isEmpty()) {
        return i18n("There are no pages to list.");
    }
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size()) {
        return i18n("Select a page to move, duplicate, turn or delete it.");
    }
    if (m_rows.at(row).removed) {
        return i18n("This page is marked for deletion. \"Keep page\" brings it back; Cancel "
                    "leaves the notebook alone.");
    }

    QStringList notes;
    if (!hasPendingEdits()) {
        notes << i18n("Apply is off because nothing has changed yet.");
    } else {
        notes << i18n("Extract writes a notebook of its own, so finish this change first.");
    }
    if (keptCount() <= 1) {
        notes << i18n("A notebook keeps at least one page, so this page cannot be deleted.");
    }
    if (!keptAbove(row)) {
        notes << i18n("This is the first page, so it cannot move up.");
    }
    if (!keptBelow(row)) {
        notes << i18n("This is the last page, so it cannot move down.");
    }
    if (notes.isEmpty()) {
        return i18n("The whole change is applied in one step, and one undo takes it back.");
    }
    return notes.join(QStringLiteral(" "));
}

QString PdfNotebookOpsDialog::sourceLabel(const PdfPageRecord &record) const
{
    const int primary = m_original.sources.size();

    /// A page of a PDF this change is bringing in: that file's name, because that is where its
    /// background will come from once this is applied.
    if (record.source >= primary) {
        const int k = record.source - primary;
        if (k < m_additions.size()) {
            return i18n("%1, page %2", QFileInfo(m_additions.at(k).path).fileName(), record.index + 1);
        }
    }

    const QString file = record.source > 0 && record.source < primary
                             ? m_original.sourceAt(record.source).file
                             : m_original.sourceFile;
    return i18n("%1, page %2", QFileInfo(file).fileName(), record.index + 1);
}

QString PdfNotebookOpsDialog::summaryLine() const
{
    const QStringList parts = pendingDescriptions();
    if (parts.isEmpty()) {
        return i18n("the notebook's pages were changed");
    }
    return i18n("the notebook's pages were changed: %1", parts.join(i18n(", ")));
}

PdfNotebookOps::PageEdits PdfNotebookOpsDialog::edits() const
{
    PdfNotebookOps::PageEdits edits;
    edits.sources = m_original.sources;
    for (const Row &row : m_rows) {
        if (!row.removed) {
            edits.pages.append(row.record);
        }
    }
    edits.copyExternal = m_copies;
    edits.copyExternalDirs = m_copyDirs;
    edits.assetsToMerge = m_assets;
    edits.removeAfter = m_removals;

    /// The PDFs the kept pages really name are the ones that travel: a PDF whose pages were all
    /// dropped again is not copied anywhere. The indices are renumbered in order, so a page that
    /// named addition 2 now names whichever position it has among the kept ones.
    const int primary = m_original.sources.size();
    QHash<int, int> keptIndex;
    for (int k = 0; k < m_additions.size(); ++k) {
        for (const Row &row : m_rows) {
            if (!row.removed && row.record.source == primary + k) {
                keptIndex.insert(k, edits.additions.size());
                edits.additions.append(m_additions.at(k).path);
                break;
            }
        }
    }
    for (PdfPageRecord &page : edits.pages) {
        if (page.source >= primary) {
            page.source = primary + keptIndex.value(page.source - primary, 0);
        }
    }

    edits.summary = summaryLine();
    return edits;
}

void PdfNotebookOpsDialog::setSourceAdder(const SourceAdder &adder)
{
    m_adder = adder;
}

void PdfNotebookOpsDialog::setNotebookMerger(const NotebookMerger &merger)
{
    m_merger = merger;
}

int PdfNotebookOpsDialog::sourceIndexFor(const PdfSourceRecord &source, const QString &absoluteFile,
                                         QString *why)
{
    /// A PDF this notebook already has: its pages are drawn from that entry, and no second copy of
    /// the bytes is made.
    for (int i = 0; i < m_original.sources.size(); ++i) {
        if (!source.sha256.isEmpty() && m_original.sources.at(i).sha256 == source.sha256) {
            return i;
        }
    }

    /// A PDF this change is already bringing in.
    for (int i = 0; i < m_additions.size(); ++i) {
        if (!source.sha256.isEmpty() && m_additions.at(i).sha256 == source.sha256) {
            return m_original.sources.size() + i;
        }
    }

    if (!QFileInfo::exists(absoluteFile)) {
        if (why) {
            *why = i18n("there is no file at %1", absoluteFile);
        }
        return -1;
    }

    Addition addition;
    addition.path = absoluteFile;
    addition.sha256 = source.sha256.isEmpty() ? PdfSessionManifest::sha256OfFile(absoluteFile)
                                             : source.sha256;
    if (addition.sha256.isEmpty()) {
        if (why) {
            *why = i18n("%1 cannot be read", absoluteFile);
        }
        return -1;
    }

    m_additions.append(addition);
    return m_original.sources.size() + m_additions.size() - 1;
}

void PdfNotebookOpsDialog::insertPagesFromPdf()
{
    if (!m_adder.pickAndRead) {
        return;
    }

    PdfToAdd pdf;
    if (!m_adder.pickAndRead(&pdf) || pdf.path.isEmpty() || pdf.displayedSizes.isEmpty()) {
        return;
    }

    int first = 0;
    int count = pdf.displayedSizes.size();
    if (m_adder.askRange && !m_adder.askRange(pdf.displayedSizes.size(), &first, &count)) {
        return;
    }
    if (first < 0 || count < 1 || first + count > pdf.displayedSizes.size()) {
        return;
    }

    addPagesFromSource(pdf, first, count);
}

void PdfNotebookOpsDialog::addPagesFromSource(const PdfToAdd &pdf, int first, int count)
{
    PdfSourceRecord source;
    source.sha256 = PdfSessionManifest::sha256OfFile(pdf.path);
    source.byteSize = QFileInfo(pdf.path).size();

    QString why;
    const int sourceIndex = sourceIndexFor(source, pdf.path, &why);
    if (sourceIndex < 0) {
        qWarning("[pdfio] the screen could not add %s: %s", qPrintable(pdf.path), qPrintable(why));
        return;
    }

    /// After the page the user is on, which is where a reader expects new pages to land -- and it is
    /// the same place the menu's insert entry uses.
    const int at = qBound(0, m_table->currentRow() + 1, m_rows.size());
    for (int i = 0; i < count; ++i) {
        Row row;
        row.record.index = first + i;
        row.record.sizePt = pdf.displayedSizes.value(first + i);
        row.record.source = sourceIndex;
        const int number = m_nextNumber++;
        row.record.kraFile = PdfSession::pageFileNameForNumber(number);
        row.record.thumbFile = PdfSession::thumbFileNameForNumber(number);
        /// Nothing to copy: the page is new, and a page that has never been drawn on has no artifact.
        row.isNew = true;
        row.fromAddedPdf = true;
        m_rows.insert(at + i, row);
    }

    refresh();
    m_table->selectRow(at);
}

void PdfNotebookOpsDialog::mergeNotebookIn()
{
    if (!m_merger.pickNotebook) {
        return;
    }

    NotebookToMerge notebook;
    if (!m_merger.pickNotebook(&notebook) || !notebook.manifest.isValid()) {
        return;
    }

    addNotebook(notebook);
}

void PdfNotebookOpsDialog::addNotebook(const NotebookToMerge &notebook)
{
    const QDir from(notebook.dir);
    const int at = qBound(0, m_table->currentRow() + 1, m_rows.size());

    int placed = 0;
    for (const PdfPageRecord &page : notebook.manifest.pages) {
        const PdfSourceRecord incoming = notebook.manifest.sourceForPage(page);

        QString why;
        const int sourceIndex = sourceIndexFor(incoming, from.filePath(incoming.file), &why);
        if (sourceIndex < 0) {
            qWarning("[pdfio] the screen could not merge in %s: %s", qPrintable(notebook.dir),
                     qPrintable(why));
            return;
        }

        Row row;
        row.record = page;
        row.record.source = sourceIndex;
        row.record.generation = 0;
        const int number = m_nextNumber++;
        row.record.kraFile = PdfSession::pageFileNameForNumber(number);
        row.record.thumbFile = PdfSession::thumbFileNameForNumber(number);
        /// Its files are read from where the notebook being merged in lives, which is what lets the
        /// list show its previews before anything has been copied.
        row.fromDir = notebook.dir;
        row.isNew = true;
        row.fromMergedNotebook = true;

        m_copies.append(qMakePair(from.filePath(page.kraFile), row.record.kraFile));
        m_copies.append(qMakePair(from.filePath(page.kraFile + QStringLiteral(".layers.txt")),
                                  row.record.kraFile + QStringLiteral(".layers.txt")));
        if (!page.thumbFile.isEmpty()) {
            m_copies.append(qMakePair(from.filePath(page.thumbFile), row.record.thumbFile));
        }
        m_copyDirs.append(qMakePair(from.filePath(page.kraFile + QStringLiteral(".layers")),
                                    row.record.kraFile + QStringLiteral(".layers")));

        m_rows.insert(at + placed, row);
        ++placed;
    }

    /// And whatever the incoming notebook kept in assets/, because its pages' content layers point at
    /// those files rather than at the manifest.
    const QDir assets(from.filePath(QStringLiteral("assets")));
    for (const QString &name : assets.entryList(QDir::Files, QDir::Name)) {
        m_assets << assets.filePath(name);
    }

    refresh();
    m_table->selectRow(at);
}

int PdfNotebookOpsDialog::keptCount() const
{
    int kept = 0;
    for (const Row &row : m_rows) {
        if (!row.removed) {
            ++kept;
        }
    }
    return kept;
}

bool PdfNotebookOpsDialog::keptAbove(int row) const
{
    for (int i = row - 1; i >= 0; --i) {
        if (!m_rows.at(i).removed) {
            return true;
        }
    }
    return false;
}

bool PdfNotebookOpsDialog::keptBelow(int row) const
{
    for (int i = row + 1; i < m_rows.size(); ++i) {
        if (!m_rows.at(i).removed) {
            return true;
        }
    }
    return false;
}
