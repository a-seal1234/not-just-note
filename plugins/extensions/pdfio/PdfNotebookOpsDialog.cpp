/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "PdfNotebookOpsDialog.h"

#include "PdfPageNavigator.h"
#include "session/PdfPageRotator.h"
#include "session/PdfSession.h"

#include <KLocalizedString>

#include <QAbstractItemView>
#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineF>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
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

/// Millimetres to points: the two units a person can be asked about a sheet of paper in. The
/// manifest, the renderer and the exporter all speak points, and this is the only place a typed
/// millimetre is turned into one.
qreal millimetresToPoints(qreal millimetres)
{
    return millimetres * 72.0 / 25.4;
}

/// A size as a person says it: "210 x 297 mm", rounded to the millimetre.
QString sizeInMillimetres(const QSizeF &sizePt)
{
    const auto mm = [](qreal points) { return qRound(points * 25.4 / 72.0); };
    return i18n("%1 x %2 mm", mm(sizePt.width()), mm(sizePt.height()));
}

/// The smallest and largest sheet a blank page may be typed as: below a centimetre there is nothing
/// to draw on, and above two metres it is not paper any more -- and both ends are far inside what
/// the page budget renders at a readable resolution.
constexpr qreal MinBlankPageMm = 10.0;
constexpr qreal MaxBlankPageMm = 2000.0;

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
/// goes next instead. Scale and Box reuse it as the screen size of a corner or edge grip.
constexpr qreal GripRadius = 24.0;

/// The shortest side a box may be dragged to, in points: half an inch, so a page can be cropped hard
/// without a slip of the hand turning it into a page of nothing.
constexpr qreal MinBoxSidePt = 36.0;

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

/// The size the READER sees, and which of the three modes produced it. The size is displaySizePt()'s
/// -- the one place the reader's size comes from -- so a scaled or cropped page says so here without
/// this function knowing how any of it is done.
QString sizeLabel(const PdfPageRecord &record)
{
    const QSizeF size = record.displaySizePt();
    QString label = QStringLiteral("%1 x %2 pt")
        .arg(QString::number(size.width(), 'f', 0), QString::number(size.height(), 'f', 0));
    QStringList modes;
    if (record.extraRotation != 0) {
        modes << i18n("turned");
    }
    if (record.extraScale != 1.0) {
        modes << i18n("scaled x%1", QString::number(record.extraScale, 'g', 3));
    }
    if (record.boxPt.isValid()) {
        modes << i18n("boxed");
    }
    if (!modes.isEmpty()) {
        label += QStringLiteral(" (%1)").arg(modes.join(QStringLiteral(" + ")));
    }
    return label;
}

/// A box in words, for the readout: its size, and what the drag did to the page.
QString boxLabel(const QRectF &box)
{
    if (!box.isValid()) {
        return i18n("the whole sheet");
    }
    return i18n("%1 x %2 pt", QString::number(qRound(box.width())),
                QString::number(qRound(box.height())));
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
    /// to the PAGE's own shape -- the canvas works it out from the turned page's bounding box, the
    /// card from PdfPageNavigator::previewBoxFor() and the page's displaySizePt(). A file still
    /// written at an older shape is drawn into that box rather than fitted by its own, so a turned,
    /// scaled or boxed page is never the wrong shape on screen. Smooth because this is a picture of
    /// text, and a nearest-neighbour scale of one is a page of broken letters.
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
 * When that still asks for more pixels than the row's preview holds -- a saved preview is fitted into
 * a 1152 box, and a 3K tablet's pane wants more than that -- the widget says so through \c
 * needsPreview rather than stretching: the dialog asks the navigator for a fresh one.
 */
class PdfPageCanvas : public QWidget
{
public:
    /**
     * What a drag does, one at a time and never two at once.
     *
     * Turn, Scale and Box are three different questions about a page -- which way up it is, how big
     * it is, and what of the source it is -- and they fail differently. A wrong turn is a page the
     * wrong way up; a wrong scale is the wrong size; a wrong crop is ink the user drew on that is
     * no longer on the page. One mode in force at a time, named on the pane before a drag starts, is
     * the whole defence against a gesture meaning something the hand did not intend.
     */
    enum Mode { TurnMode, ScaleMode, BoxMode };

    explicit PdfPageCanvas(QWidget *parent = nullptr);

    /// Shows the page: \a preview at \a previewRotation, the turn, scale and box this screen is
    /// holding on top of it, and the sheet the page is cut from. An empty \a preview draws an empty
    /// sheet of \a pageSizePt. \a previewBoxPt is the box the preview was made in, so the picture
    /// and the overlay are placed in one frame.
    void setPage(const QPixmap &preview, int previewRotation, int pendingRotation,
                 const QSizeF &pageSizePt, qreal extraScale, const QRectF &boxPt,
                 const QRectF &previewBoxPt);
    void clearPage();

    /// Which gesture a drag will perform. The pane says so before the drag begins.
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    /// The whole-degree angle being previewed while the hand is still down: the dialog moves the
    /// live readout and nothing else, so the list, the Turn column and the pending change stay
    /// exactly as they were until the hand comes up.
    std::function<void(int)> previewed;
    /// The whole-degree angle the gesture ended on. This is ONE pending edit, exactly as a button's
    /// turn is, so one drag from 0 to 37 degrees is one change when Apply runs.
    std::function<void(int)> turned;

    /// The scale being previewed while a corner is held, and the factor the hand ended on. One
    /// report per drag, like a turn: one drag is one pending edit and one undo.
    std::function<void(qreal)> scalePreviewed;
    std::function<void(qreal)> scaled;

    /// The box being previewed while an edge is held, and the box the hand ended on, both in source
    /// points with the origin at the page's top left. One report per drag.
    std::function<void(const QRectF &)> boxPreviewed;
    std::function<void(const QRectF &)> boxChanged;

    /// The DEVICE pixels the pane has just prepared its picture for, reported while painting. The
    /// dialog compares them with what the row's preview holds and asks the navigator for a fresh one
    /// when the picture would otherwise be stretched.
    std::function<void(const QSize &)> needsPreview;

    /// Whether a gesture is in flight. The dialog leaves the canvas alone meanwhile: a preview that
    /// arrived mid-gesture would move the page under the hand.
    bool isTurning() const { return m_gesture; }
    /// The pixels prepared for the last paint (device pixels), and what the row's preview held.
    QPixmap preparedPreview() const { return m_prepared; }
    QSize sourcePixels() const { return m_sourcePixels; }

    /// The factor a scale drag means: how far the hand is from the page's centre now, against how
    /// far it was when it took hold, applied to the factor the page had. Held between 10% and 800%,
    /// which is what the typed field offers too, so a drag can never ask for a page the field
    /// cannot show. Static and public because it is arithmetic a test can pin without a screen.
    static qreal scaleFromDrag(qreal heldPixels, qreal nowPixels, qreal fromScale);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool event(QEvent *event) override;

private:
    /// The grip a press took hold of: a corner for Scale, an edge for Box.
    enum Handle {
        NoHandle,
        TopLeftCorner,
        TopRightCorner,
        BottomRightCorner,
        BottomLeftCorner,
        LeftEdge,
        TopEdge,
        RightEdge,
        BottomEdge,
    };

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

    /// The frame the page is drawn in: the preview's own pixels when there is one, the page's points
    /// otherwise. Both frames carry the same shape, so one mapping of the box serves both.
    QSizeF sheetFrame() const;
    /// The box this screen is holding, in frame units.
    QRectF boxInFrame() const;
    /// The box the preview was made in, in frame units.
    QRectF previewBoxInFrame() const;
    /// The grip at \a at, or NoHandle. A grip is a screen length rather than a page length.
    Handle handleAt(const QPoint &at) const;
    /// The page's centre in widget coordinates: the point the turn measures from and the scale drag
    /// measures against.
    QPointF hub() const;
    /// Where a frame-space point lands on the screen, from the last paint.
    QPointF toWidget(const QPointF &inFrame) const;
    /// One gesture's worth of a scale drag: previews while the hand is down.
    void dragScale(const QPointF &at);
    /// One gesture's worth of a box drag: previews while the hand is down.
    void dragBox(const QPointF &at);

    /// The preview as it was read, and the turn it was drawn at: everything painted on top of it is
    /// a difference from that turn rather than the whole turn a second time.
    QPixmap m_preview;
    int m_previewRotation = 0;
    int m_rotation = 0;
    QSizeF m_pageSizePt;
    /// The scale and the box this screen is holding on top of the record: previews while a gesture
    /// is in flight, the pending edit once the hand is up.
    qreal m_scale = 1.0;
    QRectF m_box;
    QRectF m_previewBox;
    Mode m_mode = TurnMode;

    /// What the last paint prepared and drew, kept so the dialog can be told what was drawn: the
    /// pixels in DEVICE units, and the pixels the row's own preview had to make them from.
    QPixmap m_prepared;
    QSize m_sourcePixels;
    /// Where the page was drawn, and how big a frame unit came out: both gestures read them.
    QTransform m_frameToWidget;
    qreal m_fit = 1.0;

    /// A gesture in progress. While it is set the value it moves is a preview and no record has been
    /// touched: a turn's angle, a scale's factor or a box's edges.
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
    /// The scale and the box gesture: the grip, where the hand took hold, and what was there then.
    Handle m_handle = NoHandle;
    QPointF m_gestureStartAt;
    qreal m_gestureStartDistance = 0;
    qreal m_gestureStartScale = 1.0;
    QRectF m_gestureStartBox;
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
                            const QSizeF &pageSizePt, qreal extraScale, const QRectF &boxPt,
                            const QRectF &previewBoxPt)
{
    /// Never while the hand is down: a preview arriving mid-gesture would move the page under it.
    if (m_gesture) {
        return;
    }
    m_preview = preview;
    m_previewRotation = normalizedTurn(previewRotation);
    m_rotation = normalizedTurn(pendingRotation);
    m_pageSizePt = pageSizePt;
    m_scale = extraScale > 0 ? extraScale : 1.0;
    m_box = boxPt;
    m_previewBox = previewBoxPt;
    update();
}

void PdfPageCanvas::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    /// A mode change cancels whatever grip was in flight rather than letting a half-made gesture
    /// finish as a different one.
    m_gesture = false;
    m_twisting = false;
    m_handle = NoHandle;
    setCursor(mode == TurnMode ? Qt::OpenHandCursor
                               : (mode == ScaleMode ? Qt::SizeFDiagCursor : Qt::SizeHorCursor));
    update();
}

void PdfPageCanvas::clearPage()
{
    m_preview = QPixmap();
    m_previewRotation = 0;
    m_rotation = 0;
    m_pageSizePt = QSizeF();
    m_scale = 1.0;
    m_box = QRectF();
    m_previewBox = QRectF();
    m_prepared = QPixmap();
    m_sourcePixels = QSize();
    m_gesture = false;
    m_twisting = false;
    m_handle = NoHandle;
    update();
}

qreal PdfPageCanvas::scaleFromDrag(qreal heldPixels, qreal nowPixels, qreal fromScale)
{
    if (heldPixels <= 0.0 || !qIsFinite(heldPixels) || !qIsFinite(nowPixels)
        || !qIsFinite(fromScale) || fromScale <= 0.0) {
        return fromScale > 0.0 ? fromScale : 1.0;
    }
    /// The hand's distance from the page's centre is what the gesture carries: taking hold at the
    /// corner and pulling outwards is a bigger page, pushing in is a smaller one. Whole percent, so
    /// the number the readout shows is the number the record gets.
    const qreal factor = fromScale * (nowPixels / heldPixels);
    /// qBound needs one type on all three arguments, and qRound answers an int: the rounded value
    /// is what the record gets, so it is converted rather than the bound loosened to double.
    const qreal percent = qBound(qreal(10.0), qreal(qRound(factor * 100.0)), qreal(800.0));
    return percent / 100.0;
}

QSizeF PdfPageCanvas::sheetFrame() const
{
    /// The picture is the page as the row holds it, and its own pixels are the frame the box is drawn
    /// in. Without one the page's own points are the frame. Both are the same shape, so one box
    /// serves both and the overlay cannot drift from the picture it is drawn over.
    if (!m_preview.isNull()) {
        return QSizeF(m_preview.size());
    }
    return m_pageSizePt;
}

QRectF PdfPageCanvas::boxInFrame() const
{
    const QSizeF frame = sheetFrame();
    const QRectF box = m_box.isValid() ? m_box : QRectF(QPointF(0, 0), m_pageSizePt);
    if (m_pageSizePt.isEmpty() || frame.isEmpty()) {
        return box;
    }
    return QRectF(box.x() * frame.width() / m_pageSizePt.width(),
                  box.y() * frame.height() / m_pageSizePt.height(),
                  box.width() * frame.width() / m_pageSizePt.width(),
                  box.height() * frame.height() / m_pageSizePt.height());
}

QRectF PdfPageCanvas::previewBoxInFrame() const
{
    const QSizeF frame = sheetFrame();
    const QRectF box = m_previewBox.isValid() ? m_previewBox : QRectF(QPointF(0, 0), m_pageSizePt);
    if (m_pageSizePt.isEmpty() || frame.isEmpty()) {
        return box;
    }
    return QRectF(box.x() * frame.width() / m_pageSizePt.width(),
                  box.y() * frame.height() / m_pageSizePt.height(),
                  box.width() * frame.width() / m_pageSizePt.width(),
                  box.height() * frame.height() / m_pageSizePt.height());
}

QPointF PdfPageCanvas::hub() const
{
    const QRectF view = m_mode == ScaleMode ? boxInFrame()
                                            : QRectF(QPointF(0, 0), sheetFrame()).united(boxInFrame());
    return m_frameToWidget.map(view.center());
}

QPointF PdfPageCanvas::toWidget(const QPointF &inFrame) const
{
    return m_frameToWidget.map(inFrame);
}

PdfPageCanvas::Handle PdfPageCanvas::handleAt(const QPoint &at) const
{
    if (m_frameToWidget.isIdentity() || m_pageSizePt.isEmpty()) {
        return NoHandle;
    }
    const QTransform toFrame = m_frameToWidget.inverted();
    const QPointF point = toFrame.map(QPointF(at));
    const QRectF box = boxInFrame();
    if (box.isEmpty()) {
        return NoHandle;
    }

    /// A grip is a screen length, not a page length: on a page shown small a page-sized grip would
    /// be impossible to hit, and on a big one it would swallow the picture.
    const qreal reach = GripRadius / qMax(qreal(0.01), m_fit);
    const auto near = [&reach](const QPointF &a, const QPointF &b) {
        return (a - b).manhattanLength() <= reach;
    };

    if (m_mode == ScaleMode) {
        /// Scale is the whole page's size, so its grips are the corners: a corner drag carries the
        /// page's proportions and cannot make a page that is stretched.
        if (near(point, box.topLeft())) {
            return TopLeftCorner;
        }
        if (near(point, box.topRight())) {
            return TopRightCorner;
        }
        if (near(point, box.bottomRight())) {
            return BottomRightCorner;
        }
        if (near(point, box.bottomLeft())) {
            return BottomLeftCorner;
        }
        return NoHandle;
    }

    if (m_mode == BoxMode) {
        /// Box is what is inside the page, so its grips are the edges: inwards crops, outwards adds
        /// a margin, and a corner would have to mean two of them at once.
        const qreal slack = qMin(reach, qMax(qreal(1), box.height() / 2));
        const qreal slackX = qMin(reach, qMax(qreal(1), box.width() / 2));
        if (qAbs(point.x() - box.left()) <= slackX && point.y() >= box.top() - slack
            && point.y() <= box.bottom() + slack) {
            return LeftEdge;
        }
        if (qAbs(point.x() - box.right()) <= slackX && point.y() >= box.top() - slack
            && point.y() <= box.bottom() + slack) {
            return RightEdge;
        }
        if (qAbs(point.y() - box.top()) <= slack && point.x() >= box.left() - slackX
            && point.x() <= box.right() + slackX) {
            return TopEdge;
        }
        if (qAbs(point.y() - box.bottom()) <= slack && point.x() >= box.left() - slackX
            && point.x() <= box.right() + slackX) {
            return BottomEdge;
        }
        return NoHandle;
    }

    return NoHandle;
}

void PdfPageCanvas::dragScale(const QPointF &at)
{
    const qreal factor = scaleFromDrag(m_gestureStartDistance,
                                       QLineF(hub(), at).length(), m_gestureStartScale);
    if (qFuzzyCompare(factor, m_scale)) {
        return;
    }
    m_scale = factor;
    update();
    if (scalePreviewed) {
        scalePreviewed(factor);
    }
}

void PdfPageCanvas::dragBox(const QPointF &at)
{
    const QSizeF frame = sheetFrame();
    if (frame.isEmpty() || m_pageSizePt.isEmpty()) {
        return;
    }
    const QTransform toFrame = m_frameToWidget.inverted();
    const QPointF point = toFrame.map(at);
    const qreal perX = m_pageSizePt.width() / frame.width();
    const qreal perY = m_pageSizePt.height() / frame.height();

    /// In source points, whole ones: the page is a printed size, and a box that moves in hundredths
    /// of a point is a box nobody can type back.
    QRectF box = m_gestureStartBox;
    const qreal x = qRound(point.x() * perX);
    const qreal y = qRound(point.y() * perY);
    switch (m_handle) {
    case LeftEdge:
        box.setLeft(qMin(x, box.right() - MinBoxSidePt));
        break;
    case RightEdge:
        box.setRight(qMax(x, box.left() + MinBoxSidePt));
        break;
    case TopEdge:
        box.setTop(qMin(y, box.bottom() - MinBoxSidePt));
        break;
    case BottomEdge:
        box.setBottom(qMax(y, box.top() + MinBoxSidePt));
        break;
    default:
        return;
    }

    /// A margin is allowed to reach past the sheet, but not without bound: twice the sheet on any
    /// side is more page than anyone drags on purpose and keeps a stray gesture finite.
    const qreal limitX = m_pageSizePt.width() * 2.0;
    const qreal limitY = m_pageSizePt.height() * 2.0;
    box.setLeft(qMax(box.left(), -limitX));
    box.setTop(qMax(box.top(), -limitY));
    box.setRight(qMin(box.right(), m_pageSizePt.width() + limitX));
    box.setBottom(qMin(box.bottom(), m_pageSizePt.height() + limitY));

    if (box == m_box) {
        return;
    }
    m_box = box;
    update();
    if (boxPreviewed) {
        boxPreviewed(box);
    }
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
    const QSizeF frame = sheetFrame();
    if (area.width() < 2 || area.height() < 2 || frame.isEmpty()) {
        return;
    }

    /// The turn the picture still needs: the file was made with m_previewRotation already applied,
    /// so drawing the whole angle again would turn an already-sideways page twice.
    const qreal turn = normalizedTurn(m_rotation - m_previewRotation);

    /// What the pane has to fit. Turn and Box fit the sheet AND the box together, because a crop
    /// has to show what it is about to cut away -- that is the one mistake this mode can make that
    /// the others cannot. Scale fits the page the reader ends up with, so a page being made bigger
    /// stays on the pane where its corner grips are.
    const QRectF box = boxInFrame();
    QRectF view(0, 0, frame.width(), frame.height());
    view = (m_mode == ScaleMode) ? box : view.united(box);
    if (view.isEmpty()) {
        view = QRectF(0, 0, frame.width(), frame.height());
    }

    /// The sheet is scaled so its bounding box -- not the sheet itself -- fits the pane: a page set
    /// down at an angle takes more room than the same page upright, and the pane is what it has.
    const QSizeF boxed = PdfPageRecord::turnedSize(view.size(), int(turn));
    m_fit = qMin(area.width() / qMax(qreal(1), boxed.width()),
                 area.height() / qMax(qreal(1), boxed.height()));

    /// The frame the page is drawn in -> the pane. The view's centre goes to the pane's centre, which
    /// is the point the turn gesture has always measured its angles from.
    QTransform toWidget;
    toWidget.translate(area.center().x(), area.center().y());
    toWidget.rotate(turn);
    toWidget.scale(m_fit, m_fit);
    toWidget.translate(-view.center().x(), -view.center().y());
    m_frameToWidget = toWidget;

    /// Prepared at the size the pane is about to paint it at, in DEVICE pixels, and tagged with the
    /// ratio: Qt then puts those pixels on the glass one for one instead of stretching a small
    /// picture by the screen's ratio inside the painter.
    const QRectF previewRect = previewBoxInFrame();
    const QRectF previewOnScreen = toWidget.mapRect(previewRect);
    const qreal ratio = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    const QSize logical(qMax(1, qRound(previewOnScreen.width())),
                        qMax(1, qRound(previewOnScreen.height())));
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

    /// Everything below is drawn in the page's own frame, under one transform: the picture, the
    /// sheet's edge, the box and the grips cannot disagree about where the page is.
    painter.setTransform(toWidget, true);
    const qreal pen = 1.0 / qMax(qreal(0.01), m_fit);

    if (m_preview.isNull()) {
        /// No preview: the page has never been saved, or a turn dropped the stale one. An empty
        /// sheet of the page's own proportions still gives the hand something to work on.
        painter.setPen(QPen(QColor(0xb0, 0xb0, 0xb0), pen));
        painter.setBrush(QColor(0xff, 0xff, 0xff));
        painter.drawRect(previewRect);
    } else {
        /// The whole source onto the whole target, explicitly: the target is in frame coordinates
        /// and the pixmap holds the DEVICE pixels for it, so the painter's own device transform is
        /// what maps one to the other -- one source pixel per device pixel, and no second scaling by
        /// the pixmap's ratio tag. The three-argument form because QPainter has no (QRectF, QPixmap)
        /// overload, and toRect() would throw the pane's fractional position away.
        painter.drawPixmap(previewRect, m_prepared, QRectF(m_prepared.rect()));
    }
    painter.setBrush(Qt::NoBrush);

    if (m_mode == BoxMode) {
        /// What the crop is about to cut away, painted over the picture itself. The design record
        /// asks this mode for exactly one thing the others do not need: the user sees the ink that is
        /// about to go BEFORE Apply. A number in a table does not show it; this does.
        QPainterPath removed;
        removed.addRect(QRectF(0, 0, frame.width(), frame.height()));
        QPainterPath kept;
        kept.addRect(box);
        painter.fillPath(removed.subtracted(kept), QColor(0xd0, 0x20, 0x20, 0x80));

        painter.setPen(QPen(QColor(0xb0, 0xb0, 0xb0), pen, Qt::DashLine));
        painter.drawRect(QRectF(0, 0, frame.width(), frame.height()));
    }

    /// The box, when there is one: the page's own edge in blue, or the edge being dragged in red.
    if (m_mode == BoxMode || m_box.isValid()) {
        painter.setPen(QPen(m_mode == BoxMode ? QColor(0xff, 0x60, 0x60) : QColor(0x1e, 0x88, 0xe5),
                            pen * 2));
        painter.drawRect(box);
    }

    /// The grips, so the pane says where a drag can take hold before the hand goes there.
    const qreal grip = GripRadius / qMax(qreal(0.01), m_fit) / 2.5;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x1e, 0x88, 0xe5));
    const auto gripAt = [&painter, grip](const QPointF &at) {
        painter.drawRect(QRectF(at.x() - grip, at.y() - grip, grip * 2, grip * 2));
    };
    if (m_mode == ScaleMode) {
        gripAt(box.topLeft());
        gripAt(box.topRight());
        gripAt(box.bottomRight());
        gripAt(box.bottomLeft());
    } else if (m_mode == BoxMode) {
        gripAt(QPointF(box.left(), box.center().y()));
        gripAt(QPointF(box.right(), box.center().y()));
        gripAt(QPointF(box.center().x(), box.top()));
        gripAt(QPointF(box.center().x(), box.bottom()));
    }
    painter.resetTransform();
}

void PdfPageCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPointF at = QPointF(mousePosition(event));

    /// Scale and Box take hold of a GRIP, and only a grip: a press on the page itself must not resize
    /// or crop it by accident. Turn takes hold anywhere on the page, which is what it has always done
    /// and what the pane's users know.
    if (m_mode != TurnMode) {
        m_handle = handleAt(at.toPoint());
        if (m_handle == NoHandle) {
            event->accept();
            return;
        }
        m_gesture = true;
        m_gestureStartAt = at;
        m_gestureStartScale = m_scale;
        m_gestureStartBox = m_box.isValid() ? m_box : QRectF(QPointF(0, 0), m_pageSizePt);
        m_gestureStartDistance = QLineF(hub(), at).length();
        m_anchored = false;
        m_twisting = false;
        event->accept();
        return;
    }

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

    /// One mode in force, so a drag cannot be two things at once.
    if (m_mode == ScaleMode) {
        dragScale(QPointF(mousePosition(event)));
        event->accept();
        return;
    }
    if (m_mode == BoxMode) {
        dragBox(QPointF(mousePosition(event)));
        event->accept();
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

    /// The hand is up: whatever it landed on becomes the one pending edit of this gesture. Scale and
    /// Box report theirs the same way -- once, at the end -- so one drag is one change and one undo
    /// takes the whole gesture back.
    if (m_mode == ScaleMode) {
        const bool landed = m_gesture && !qFuzzyCompare(m_scale, m_gestureStartScale);
        m_gesture = false;
        m_handle = NoHandle;
        if (landed && scaled) {
            scaled(m_scale);
        }
        event->accept();
        return;
    }
    if (m_mode == BoxMode) {
        const bool landed = m_gesture && m_box != m_gestureStartBox;
        m_gesture = false;
        m_handle = NoHandle;
        if (landed && boxChanged) {
            boxChanged(m_box);
        }
        event->accept();
        return;
    }

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

    /// Scale and Box work the same way on glass as with a pen: the grip a finger takes hold of is the
    /// gesture. A two-finger twist is a TURN, so it belongs to Turn mode and means nothing here.
    if (m_mode != TurnMode) {
        if (positions.isEmpty()) {
            const bool landedScale = m_gesture && !qFuzzyCompare(m_scale, m_gestureStartScale);
            const bool landedBox = m_gesture && m_box != m_gestureStartBox;
            m_gesture = false;
            m_handle = NoHandle;
            if (landedScale && scaled) {
                scaled(m_scale);
            }
            if (landedBox && boxChanged) {
                boxChanged(m_box);
            }
            return;
        }
        const QPointF at = positions.first();
        if (!m_gesture) {
            m_handle = handleAt(at.toPoint());
            if (m_handle == NoHandle) {
                return;
            }
            m_gesture = true;
            m_gestureStartAt = at;
            m_gestureStartScale = m_scale;
            m_gestureStartBox = m_box.isValid() ? m_box : QRectF(QPointF(0, 0), m_pageSizePt);
            m_gestureStartDistance = QLineF(hub(), at).length();
            return;
        }
        if (m_mode == ScaleMode) {
            dragScale(at);
        } else {
            dragBox(at);
        }
        return;
    }

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

    /// The mode switch, above the page, exclusive, and always saying which gesture a drag will
    /// perform. Turn is checked at the start and behaves exactly as it always has: the pane has
    /// turned pages for weeks, and a regression in that gesture is the first thing a user notices.
    auto *modeRow = new QHBoxLayout;
    auto *modeCaption = new QLabel(i18n("Drag mode:"), this);
    modeRow->addWidget(modeCaption);
    const auto addMode = [this, modeRow](const QString &text, const QString &name, DragMode mode) {
        auto *button = new QPushButton(text, this);
        button->setCheckable(true);
        button->setObjectName(name);
        connect(button, &QPushButton::clicked, this, [this, mode]() { setDragMode(mode); });
        modeRow->addWidget(button);
        return button;
    };
    m_modeTurn = addMode(i18n("Turn"), QStringLiteral("pdfio_ops_mode_turn"), TurnDrag);
    m_modeScale = addMode(i18n("Scale"), QStringLiteral("pdfio_ops_mode_scale"), ScaleDrag);
    m_modeBox = addMode(i18n("Box"), QStringLiteral("pdfio_ops_mode_box"), BoxDrag);
    m_modeTurn->setChecked(true);
    modeRow->addStretch(1);
    preview->addLayout(modeRow);

    m_canvas = new PdfPageCanvas(this);
    m_canvas->setToolTip(i18n("Turn: drag the page around its centre, or twist two fingers on it. "
                              "Scale: drag a corner. Box: drag an edge inwards to crop or outwards "
                              "for a margin."));
    /// While the hand is down the dialog moves only the readout: the record, the list and the
    /// pending change wait for the gesture to end, and then ONE report becomes one pending edit.
    m_canvas->previewed = [this](int degrees) {
        if (m_angle) {
            m_angle->setText(i18n("Turn: %1", turnLabel(degrees)));
        }
    };
    m_canvas->turned = [this](int degrees) { setSelectedTurn(degrees); };
    m_canvas->scalePreviewed = [this](qreal factor) {
        if (m_scaleField) {
            m_updatingScale = true;
            m_scaleField->setValue(factor * 100.0);
            m_updatingScale = false;
        }
        if (m_angle) {
            m_angle->setText(i18n("Scale: %1%", qRound(factor * 100.0)));
        }
    };
    m_canvas->scaled = [this](qreal factor) { setSelectedScale(factor); };
    m_canvas->boxPreviewed = [this](const QRectF &box) {
        if (m_angle) {
            m_angle->setText(i18n("Box: %1", boxLabel(box)));
        }
    };
    m_canvas->boxChanged = [this](const QRectF &box) { setSelectedBox(box); };
    /// What the pane is about to draw, in device pixels. When the row's own preview has fewer pixels
    /// than that, the navigator is asked for a fresh one rather than the small one stretched.
    m_canvas->needsPreview = [this](const QSize &devicePixels) {
        const int row = m_table->currentRow();
        if (row >= 0 && row < m_rows.size()) {
            requestPreviewIfTooSmall(m_rows[row], devicePixels);
        }
    };
    preview->addWidget(m_canvas, 1);

    /// Scale as a number that can be typed or stepped, the same shape as the memory budget's field,
    /// because this is a tablet. It follows the corner drag and it drives it: one value, two ways in.
    auto *scaleRow = new QHBoxLayout;
    auto *scaleCaption = new QLabel(i18n("Scale"), this);
    m_scaleField = new QDoubleSpinBox(this);
    m_scaleField->setObjectName(QStringLiteral("pdfio_ops_scale_value"));
    m_scaleField->setRange(10.0, 800.0);
    m_scaleField->setDecimals(0);
    m_scaleField->setSingleStep(5.0);
    m_scaleField->setSuffix(i18n("%"));
    m_scaleField->setToolTip(i18n("How large the page is shown and exported. The source is "
                                  "unchanged, and a bigger page is rendered at a larger dpi rather "
                                  "than stretched."));
    connect(m_scaleField, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
            [this](double value) {
                if (m_updatingScale) {
                    return;
                }
                setSelectedScale(value / 100.0);
            });
    m_resetScale = new QPushButton(i18n("Reset scale"), this);
    m_resetScale->setObjectName(QStringLiteral("pdfio_ops_reset_scale"));
    connect(m_resetScale, &QPushButton::clicked, this, [this]() { setSelectedScale(1.0); });
    m_resetBox = new QPushButton(i18n("Reset box"), this);
    m_resetBox->setObjectName(QStringLiteral("pdfio_ops_reset_box"));
    connect(m_resetBox, &QPushButton::clicked, this, [this]() { setSelectedBox(QRectF()); });
    scaleRow->addWidget(scaleCaption);
    scaleRow->addWidget(m_scaleField);
    scaleRow->addWidget(m_resetScale);
    scaleRow->addWidget(m_resetBox);
    scaleRow->addStretch(1);
    preview->addLayout(scaleRow);

    /// What a crop is about to cut away, in words, beside the picture that shows it.
    m_boxWarning = new QLabel(this);
    m_boxWarning->setObjectName(QStringLiteral("pdfio_ops_box_warning"));
    m_boxWarning->setWordWrap(true);
    preview->addWidget(m_boxWarning);

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

    /// The blank page sits beside the PDF insert: both add pages to the list, and the difference is
    /// only where the paper comes from.
    m_insertBlank = addButton(i18n("Insert a blank page..."), [this]() { insertBlankPage(); });

    /// And a picture as a page of its own, beside it: one more way for paper to arrive, and the
    /// copy of the file is part of this screen's one Apply.
    m_insertImage = addButton(i18n("Insert a picture as a page..."), [this]() { insertImagePage(); });
    m_insertImage->setObjectName(QStringLiteral("pdfio_ops_insert_image"));
    m_insertImage->setToolTip(i18n("Adds a page whose paper is a picture -- a scan, a photo, a "
                                   "screenshot. The file is copied into the notebook when you "
                                   "apply."));
    m_insertBlank->setObjectName(QStringLiteral("pdfio_ops_insert_blank"));
    m_insertBlank->setToolTip(i18n("Adds an empty page you can draw on, at a size you choose."));
    m_insert->setObjectName(QStringLiteral("pdfio_ops_insert"));
    m_merge = addButton(i18n("Merge a notebook in..."), [this]() { mergeNotebookIn(); });
    m_merge->setObjectName(QStringLiteral("pdfio_ops_merge_notebook"));

    /// Pixel resize is a separate transaction, not an edit of this screen's working copy. Leave
    /// Manage Pages first so the existing resize path can save, change the manifest, and reopen the
    /// notebook without silently discarding pending moves/turns/deletes.
    m_batchResize = addButton(i18n("Batch resize pages..."), [this]() {
        if (hasPendingEdits()) {
            return;
        }
        m_requested = BatchPixelResizeAction;
        accept();
    });
    m_batchResize->setObjectName(QStringLiteral("pdfio_ops_batch_pixel_resize"));
    m_batchResize->setToolTip(i18n("Set pixel bounds and DPI for checked pages. Apply or cancel any "
                                   "pending page-list changes first."));

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
        /// thumbFile because a turn or a resize drops that stale picture (the name it goes by stays,
        /// which is where the new one is written), and the list must not go blank exactly where a
        /// page is turned.
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
                                               : (row.fromAddedPdf || row.fromAddedImage
                                                      ? i18n("new page")
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
    m_delete->setEnabled(live);
    m_keep->setEnabled(selected && m_rows.at(row).removed);
    m_rotateLeft->setEnabled(live);
    m_rotateRight->setEnabled(live);

    /// Insert and merge are edits of this notebook's page list, so they stay available with a change
    /// pending, like every other button. Extract writes a notebook of its own and waits, with the
    /// hint saying why.
    m_insert->setEnabled(true);
    m_insertBlank->setEnabled(true);
    /// Only where the platform's chooser is wired: an entry that cannot pick anything is not offered.
    m_insertImage->setEnabled(bool(m_adder.pickImage));
    m_merge->setEnabled(true);
    m_extract->setEnabled(!dirty);
    m_batchResize->setEnabled(!dirty);

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

/// The box a row's preview was made in: the box its page had when the preview was written, which
/// is the record the change started from. The picture and the crop overlay have to be placed in ONE
/// frame or the pane shows one page and warns about another.
QRectF PdfNotebookOpsDialog::previewBoxFor(const Row &row) const
{
    const QRectF sheet(0, 0, row.record.sizePt.width(), row.record.sizePt.height());
    for (const PdfPageRecord &page : m_original.pages) {
        if (!row.record.kraFile.isEmpty() && page.kraFile == row.record.kraFile) {
            return page.boxPt.isValid() ? page.boxPt : sheet;
        }
    }
    /// A page this change brought in has no record before it; its own box is the only frame there is.
    return row.record.boxPt.isValid() ? row.record.boxPt : sheet;
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
        if (m_boxWarning) {
            m_boxWarning->clear();
        }
        return;
    }

    Row &selected = m_rows[row];
    const QRectF previewBox = previewBoxFor(selected);
    if (selected.removed) {
        /// Marked for deletion: the pending size is still shown, but there is no page to work on --
        /// "Keep page" brings it back.
        m_canvas->setPage(QPixmap(), 0, selected.record.extraRotation, selected.record.sizePt,
                          selected.record.extraScale, selected.record.boxPt, previewBox);
        m_angle->setText(i18n("Turn: %1 (marked for deletion)",
                              turnLabel(selected.record.extraRotation)));
        if (m_boxWarning) {
            m_boxWarning->clear();
        }
        return;
    }

    loadPreview(selected);
    m_canvas->setPage(selected.preview, selected.previewRotation, selected.record.extraRotation,
                      selected.record.sizePt, selected.record.extraScale, selected.record.boxPt,
                      previewBox);

    /// The field follows the model without writing back to it.
    if (m_scaleField) {
        m_updatingScale = true;
        m_scaleField->setValue(selected.record.extraScale * 100.0);
        m_updatingScale = false;
    }

    /// The readout says WHICH MODE IS IN FORCE and what that mode currently has. The pane says what a
    /// drag will do before the drag starts, which is the whole reason the switch is explicit.
    switch (m_dragMode) {
    case ScaleDrag:
        m_angle->setText(i18n("Scale: %1%", qRound(selected.record.extraScale * 100.0)));
        break;
    case BoxDrag:
        m_angle->setText(i18n("Box: %1", boxLabel(selected.record.boxPt)));
        break;
    default:
        m_angle->setText(i18n("Turn: %1", turnLabel(selected.record.extraRotation)));
        break;
    }
    if (m_boxWarning) {
        m_boxWarning->setText(m_dragMode == BoxDrag ? boxCropWarning() : QString());
    }
}

void PdfNotebookOpsDialog::setDragMode(DragMode mode)
{
    m_dragMode = mode;
    if (m_modeTurn) {
        m_modeTurn->setChecked(mode == TurnDrag);
    }
    if (m_modeScale) {
        m_modeScale->setChecked(mode == ScaleDrag);
    }
    if (m_modeBox) {
        m_modeBox->setChecked(mode == BoxDrag);
    }
    if (m_canvas) {
        m_canvas->setMode(mode == ScaleDrag ? PdfPageCanvas::ScaleMode
                                            : (mode == BoxDrag ? PdfPageCanvas::BoxMode
                                                               : PdfPageCanvas::TurnMode));
    }
    refreshCanvas();
}

void PdfNotebookOpsDialog::setSelectedScale(qreal factor)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    /// The same range the field offers, so a drag and a typed number can never record different
    /// pages for the same value.
    const qreal wanted = qBound(qreal(0.1), factor, qreal(8.0));
    Row &scaling = m_rows[row];
    if (qFuzzyCompare(scaling.record.extraScale, wanted)) {
        return;
    }

    scaling.record.extraScale = wanted;
    /// The preview was made at the page's old size, so it goes with the change: a stale preview would
    /// show the old size and the next save makes a new one.
    dropThumbnail(scaling);
    restorePreviewIfUnchanged(scaling);

    refresh();
    m_table->selectRow(row);
}

void PdfNotebookOpsDialog::setSelectedBox(const QRectF &box)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return;
    }

    Row &cropping = m_rows[row];
    QRectF wanted = box;
    if (wanted.isValid()) {
        /// A box that covers the whole sheet IS the whole sheet: one spelling of "not cropped", so
        /// Apply and the table can tell a crop from a hand that came back to where it started.
        const QRectF sheet(0, 0, cropping.record.sizePt.width(), cropping.record.sizePt.height());
        const qreal slack = 1.0;
        if (qAbs(wanted.x() - sheet.x()) <= slack && qAbs(wanted.y() - sheet.y()) <= slack
            && qAbs(wanted.width() - sheet.width()) <= slack
            && qAbs(wanted.height() - sheet.height()) <= slack) {
            wanted = QRectF();
        }
    }
    if (cropping.record.boxPt == wanted) {
        return;
    }

    cropping.record.boxPt = wanted;
    /// The preview is a picture of the page in the box it had; the box just changed, so it goes with
    /// the change rather than showing the page the wrong size. The pane keeps drawing it in the
    /// frame it was made in (previewBoxFor) until a new one arrives.
    dropThumbnail(cropping);
    restorePreviewIfUnchanged(cropping);

    refresh();
    m_table->selectRow(row);
}

void PdfNotebookOpsDialog::restorePreviewIfUnchanged(Row &row)
{
    /// A page the user put back the way the notebook has it is not a change -- and the preview the
    /// edit dropped on the way is not one either. Without this, scaling a page and setting it back
    /// would leave Apply offering to remove the page's own preview: the same "nothing happened but
    /// the screen says it did" that the ops layer's own comparison exists to prevent.
    for (const PdfPageRecord &page : m_original.pages) {
        if (page.kraFile != row.record.kraFile) {
            continue;
        }
        const bool same = page.source == row.record.source && page.index == row.record.index
            && page.extraRotation == row.record.extraRotation
            && page.extraScale == row.record.extraScale && page.boxPt == row.record.boxPt
            && page.sizePt == row.record.sizePt;
        if (same) {
            /// The row is the record the notebook already has, so the picture the drop queued is
            /// the right one again: the removal is not part of any change and the name goes back
            /// with it. A notebook whose page records no preview keeps none -- there is nothing to
            /// restore it to.
            if (!page.thumbFile.isEmpty()) {
                row.record.thumbFile = page.thumbFile;
                m_removals.removeAll(page.thumbFile);
            }
        }
        return;
    }
}

QString PdfNotebookOpsDialog::boxCropWarning() const
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).removed) {
        return QString();
    }

    const Row &selected = m_rows.at(row);
    const PdfPageRecord &record = selected.record;
    if (!record.boxPt.isValid()) {
        return i18n("Box mode: no crop. Drag an edge inwards to cut the page down, or outwards to "
                    "add a margin.");
    }
    if (selected.preview.isNull()) {
        /// No picture to judge by. Saying "nothing will be lost" would be a guess, and this is the
        /// one mode where a guess costs the user their ink.
        return i18n("Box mode: this crop cannot be checked against the page's picture, because there "
                    "is no preview of it yet.");
    }

    /// The preview is in the frame of the box the page had when it was written. The part of it
    /// outside the pending box is exactly the ink this crop destroys, so the warning is counted from
    /// the picture the user is looking at rather than from a number in the manifest.
    const QRectF frame = previewBoxFor(selected);
    const QImage picture = selected.preview.toImage();
    if (picture.isNull() || frame.isEmpty()) {
        return QString();
    }

    const int step = qMax(1, qMax(picture.width(), picture.height()) / 256);
    int painted = 0;
    for (int y = 0; y < picture.height(); y += step) {
        for (int x = 0; x < picture.width(); x += step) {
            const QRgb pixel = picture.pixel(x, y);
            if (qAlpha(pixel) < 32) {
                continue;
            }
            /// Painted means ink, not paper: the preview is mostly white, and warning about white
            /// paper would make the warning meaningless.
            if (qRed(pixel) + qGreen(pixel) + qBlue(pixel) > 600) {
                continue;
            }
            const QPointF inPage(frame.x() + frame.width() * (qreal(x) + 0.5) / picture.width(),
                                 frame.y() + frame.height() * (qreal(y) + 0.5) / picture.height());
            if (!record.boxPt.contains(inPage)) {
                ++painted;
            }
        }
    }

    if (painted == 0) {
        return i18n("Box mode: nothing painted is in the part being cut away. What is outside the box "
                    "is gone from the page when you press Apply.");
    }
    return i18np("Box mode: this crop removes painted marks from the page -- %1 sampled pixel of ink "
                 "is inside the part being cut away, and it is destroyed when you press Apply.",
                 "Box mode: this crop removes painted marks from the page -- %1 sampled pixels of ink "
                 "are inside the part being cut away, and they are destroyed when you press Apply.",
                 painted);
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

    /// Turned first, then drawn: the file was written at previewRotation, so what it still needs
    /// on top of itself is the difference between that and the turn the screen is holding.
    const QPixmap turned = turnedPixmap(card.preview,
                                        card.record.extraRotation - card.previewRotation);
    /// The box comes from the page the screen is HOLDING, not from the file it happens to have:
    /// displaySizePt() is where the sheet, the box, the turn and the scale are already applied, so
    /// a page this screen has turned (sides swap), scaled (both change) or cropped (ratio changes)
    /// is prepared at the shape it is about to have. The file can still be the OLD shape -- it is
    /// regenerated at the new one -- and it is drawn into that box rather than fitted by its own,
    /// which is the difference between a preview of the page and a clipped picture of it.
    const QSize box = PdfPageNavigator::previewBoxFor(card.record, m_table->iconSize());
    if (box.isEmpty()) {
        return prepared;
    }
    const qreal ratio = m_table->devicePixelRatioF() > 0 ? m_table->devicePixelRatioF() : 1.0;

    prepared.sourcePixels = card.preview.size();
    prepared.logicalSize = box;
    prepared.pixmap = pdfioPreviewForDisplay(turned, box, ratio);
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
                              row.record.sizePt, row.record.extraScale, row.record.boxPt,
                              previewBoxFor(row));
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

    /// The stale PICTURE goes; the NAME it goes by stays.
    ///
    /// The name is the durable half of a preview and the only place a new one can land. Dropping it
    /// as well -- which is what this did -- left the record naming nothing, and every reader joins
    /// an empty name onto the project directory: ensureThumbnail() found the DIRECTORY, told the
    /// surfaces the preview was ready, and never generated one. A page that was turned or scaled
    /// here then had no picture at all on any surface, for good, which is the report: a page that
    /// was only MOVED keeps its picture, because a move drops nothing.
    ///
    /// So the file is what is dropped -- Apply moves it into the journal, the way
    /// PdfNotebookOps::rotatePages() has always dropped a turned page's preview -- and the ask that
    /// follows finds the name, finds no file, and makes a new picture at the page's CURRENT
    /// displaySizePt() (see PdfPageNavigator::makeOneThumbnail).
    const QString thumb = row.record.thumbFile;
    for (int i = m_copies.size() - 1; i >= 0; --i) {
        if (m_copies.at(i).second == thumb) {
            m_copies.removeAt(i);
        }
    }
    if (!row.isNew && !m_removals.contains(thumb)) {
        m_removals << thumb;
    }
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
        /// Every field that decides what the reader sees, so a scale or a box -- including one put
        /// BACK to its default -- is a change the screen offers to Apply instead of silently doing
        /// nothing.
        if (a.kraFile != b.kraFile || a.source != b.source || a.index != b.index
            || a.extraRotation != b.extraRotation || a.extraScale != b.extraScale
            || a.boxPt != b.boxPt || a.sizePt != b.sizePt || a.thumbFile != b.thumbFile) {
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
    int pictures = 0;
    for (const Row &row : m_rows) {
        if (row.removed) {
            ++deleted;
        } else if (row.fromMergedNotebook) {
            ++mergedIn;
        } else if (row.fromAddedImage) {
            ++pictures;
        } else if (row.fromAddedPdf) {
            ++inserted;
        } else if (row.isNew) {
            ++duplicated;
        }
    }
    if (pictures > 0) {
        parts << i18np("a picture brought in as a page", "%1 pictures brought in as pages", pictures);
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

    /// Scale and Box are named apart from a turn and apart from each other, because they are separate
    /// modes with separate mistakes: a wrong scale is the wrong size, a wrong crop is ink gone. A
    /// value put BACK to its default counts here too -- the Lead's own rule, and the reason the
    /// comparison is over the whole value rather than over "is it different from 1".
    int scaled = 0;
    int cropped = 0;
    int uncropped = 0;
    QHash<QString, PdfPageRecord> wasBefore;
    for (const PdfPageRecord &page : m_original.pages) {
        wasBefore.insert(page.kraFile, page);
    }
    for (const Row &row : m_rows) {
        if (row.removed || row.isNew) {
            continue;
        }
        const QHash<QString, PdfPageRecord>::const_iterator before =
            wasBefore.constFind(row.record.kraFile);
        if (before == wasBefore.constEnd()) {
            continue;
        }
        if (!qFuzzyCompare(before.value().extraScale, row.record.extraScale)) {
            ++scaled;
        }
        if (before.value().boxPt != row.record.boxPt) {
            if (row.record.boxPt.isValid()) {
                ++cropped;
            } else {
                ++uncropped;
            }
        }
    }
    if (scaled > 0) {
        parts << i18np("%1 page scaled", "%1 pages scaled", scaled);
    }
    if (cropped > 0) {
        parts << i18np("%1 page cropped", "%1 pages cropped", cropped);
    }
    if (uncropped > 0) {
        parts << i18np("%1 page un-cropped", "%1 pages un-cropped", uncropped);
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
        return i18n("This notebook has no pages. Insert pages from a PDF to continue.");
    }
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_rows.size()) {
        return i18n("Select a page to move, duplicate, turn or delete it.");
    }
    if (m_rows.at(row).removed) {
        if (keptCount() == 0) {
            return i18n("Applying this change leaves the notebook empty. Insert pages from a PDF to continue; "
                        "click Keep page to restore this page.");
        }
        return i18n("This page is marked for deletion. Click Keep page to restore it; Cancel "
                    "leaves the notebook alone.");
    }

    QStringList notes;
    if (!hasPendingEdits()) {
        notes << i18n("Apply is off because nothing has changed yet.");
    } else {
        notes << i18n("Extract and batch resize are separate operations; apply or cancel this page-list "
                       "change first.");
    }
    if (keptCount() == 1) {
        notes << i18n("Deleting this page leaves the notebook empty; insert pages from a PDF to continue.");
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
    /// A blank page comes from no file: naming one would point at a PDF that has nothing to do with
    /// it (and at "page 0" of it, the number a -1 index would print).
    if (record.isBlank()) {
        return i18n("Blank page, %1", sizeInMillimetres(record.sizePt));
    }

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
    /// The kind of each source this change brings in, in the order the pages name them: empty is a
    /// PDF, "image" is a picture the page is drawn from.
    for (const Addition &addition : m_additions) {
        edits.additionKinds.append(addition.kind);
    }
    edits.copyExternalDirs = m_copyDirs;
    edits.assetsToMerge = m_assets;
    edits.removeAfter = m_removals;
    /// Box mode writes the ink, not only the record: a crop that changed the manifest alone would
    /// leave the whole page's ink on disk for the roll to squeeze into the smaller rectangle. The
    /// screen built the page list, so it names the artifact step too, and applyPageEdits runs it
    /// inside the same journal entry and the same commit as everything else.
    edits.clipper = PdfPageRotator::clipInto;

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
    /// The picture entry is offered only where the platform's chooser is wired, and that is known
    /// when the adder arrives -- after the screen was built, so the footer is refreshed here.
    refreshFooter();
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
    addition.kind = source.kind;
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

QSizeF PdfNotebookOpsDialog::askBlankPageSize()
{
    const int row = m_table->currentRow();
    const auto sheetOf = [this](int at) -> QSizeF {
        if (at < 0 || at >= m_rows.size() || m_rows.at(at).removed) {
            return QSizeF();
        }
        return m_rows.at(at).record.displaySizePt();
    };
    const QSizeF before = sheetOf(row);
    const QSizeF after = sheetOf(row + 1);

    /// A neighbour's size is offered FIRST because it is what a reader wants nine times in ten: the
    /// blank page is a continuation of the page beside it, and asking for a measurement there would
    /// be asking the user to copy a number the screen already knows.
    QStringList labels;
    QList<QSizeF> sizes;
    const auto offer = [&labels, &sizes](const QSizeF &size, const QString &label) {
        if (!size.isValid() || size.isEmpty() || sizes.contains(size)) {
            return;
        }
        labels << label;
        sizes << size;
    };
    offer(before, i18n("Same size as the page before (%1)", sizeInMillimetres(before)));
    offer(after, i18n("Same size as the page after (%1)", sizeInMillimetres(after)));
    offer(QSizeF(millimetresToPoints(210), millimetresToPoints(297)), i18n("A4 portrait (210 x 297 mm)"));
    offer(QSizeF(millimetresToPoints(297), millimetresToPoints(210)), i18n("A4 landscape (297 x 210 mm)"));
    offer(QSizeF(millimetresToPoints(148), millimetresToPoints(210)), i18n("A5 portrait (148 x 210 mm)"));
    offer(QSizeF(millimetresToPoints(216), millimetresToPoints(279)), i18n("Letter portrait (216 x 279 mm)"));

    const QString typed = i18n("A size I type...");
    labels << typed;

    bool ok = false;
    const QString chosen = QInputDialog::getItem(this, i18n("Insert a blank page"),
                                                 i18n("The size of the blank page:"), labels, 0, false,
                                                 &ok);
    if (!ok) {
        return QSizeF();
    }
    const int at = labels.indexOf(chosen);
    if (at >= 0 && at < sizes.size()) {
        return sizes.at(at);
    }

    /// Typed: width then height in millimetres, with the last answer as the default, so a column of
    /// pages can be made without retyping the width.
    static qreal lastWidthMm = 210.0;
    static qreal lastHeightMm = 297.0;
    const qreal widthMm = QInputDialog::getDouble(this, i18n("Blank page width"),
                                                  i18n("Width in millimetres:"), lastWidthMm,
                                                  MinBlankPageMm, MaxBlankPageMm, 0, &ok);
    if (!ok) {
        return QSizeF();
    }
    const qreal heightMm = QInputDialog::getDouble(this, i18n("Blank page height"),
                                                   i18n("Height in millimetres:"), lastHeightMm,
                                                   MinBlankPageMm, MaxBlankPageMm, 0, &ok);
    if (!ok) {
        return QSizeF();
    }
    lastWidthMm = widthMm;
    lastHeightMm = heightMm;
    return QSizeF(millimetresToPoints(widthMm), millimetresToPoints(heightMm));
}

void PdfNotebookOpsDialog::insertBlankPage()
{
    const QSizeF size = askBlankPageSize();
    if (!size.isValid() || size.isEmpty()) {
        return;
    }

    /// After the page the user is on, which is where a reader expects new pages to land -- the same
    /// place the menu's own insert entry puts them.
    const int at = qBound(0, m_table->currentRow() + 1, m_rows.size());

    Row row;
    /// The record says "no source, and no page inside one". Both halves are needed: the manifest
    /// refuses a blank page that still names a page number, because that record can be read two ways
    /// and one of those ways is the user's PDF appearing where the blank page is.
    row.record.index = -1;
    row.record.source = -1;
    row.record.sizePt = size;
    row.record.rotation = 0;
    const int number = m_nextNumber++;
    row.record.kraFile = PdfSession::pageFileNameForNumber(number);
    row.record.thumbFile = PdfSession::thumbFileNameForNumber(number);
    /// Nothing to copy: the page has never been drawn on, so it has no artifact, and its paper is
    /// the renderer's business rather than a file's.
    row.isNew = true;

    m_rows.insert(at, row);
    refresh();
    m_table->selectRow(at);
}

void PdfNotebookOpsDialog::insertImagePage()
{
    if (!m_adder.pickImage) {
        return;
    }

    ImageToAdd picture;
    if (!m_adder.pickImage(&picture) || picture.path.isEmpty() || !picture.sizePt.isValid()) {
        return;
    }

    /// The source this page will draw from. A picture the change is already bringing in is SHARED:
    /// two pages of one scan is one copy of its bytes, which is the rule a PDF already follows.
    PdfSourceRecord source;
    source.file = QFileInfo(picture.path).fileName();
    source.sha256 = PdfSessionManifest::sha256OfFile(picture.path);
    source.byteSize = QFileInfo(picture.path).size();
    source.kind = QStringLiteral("image");

    QString why;
    const int sourceIndex = sourceIndexFor(source, picture.path, &why);
    if (sourceIndex < 0) {
        QMessageBox::warning(this, i18n("Insert a picture as a page"), why);
        return;
    }

    /// After the page the user is on, which is where every other insert entry puts new pages.
    const int at = qBound(0, m_table->currentRow() + 1, m_rows.size());

    Row row;
    /// One picture is one page, and the page of a picture source is always its first: an image has
    /// no second page to name.
    row.record.index = 0;
    row.record.source = sourceIndex;
    row.record.sizePt = picture.sizePt;
    /// The import's own answer about how big the picture is: the page's SCALE, which is the mode that
    /// already means "show this page at this share of its own size", and what the exporter applies.
    row.record.extraScale = picture.scale > 0.0 ? picture.scale : 1.0;
    const int number = m_nextNumber++;
    row.record.kraFile = PdfSession::pageFileNameForNumber(number);
    row.record.thumbFile = PdfSession::thumbFileNameForNumber(number);
    row.isNew = true;
    row.fromAddedImage = true;

    m_rows.insert(at, row);
    refresh();
    m_table->selectRow(at);
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
