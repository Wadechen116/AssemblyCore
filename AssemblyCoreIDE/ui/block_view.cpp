#include "block_view.h"

#include <QGraphicsItem>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <cmath>

#include "block_painter.h"

namespace acide {

BlockView::BlockView(QGraphicsScene* scene, QWidget* parent) : QGraphicsView(scene, parent) {
    setRenderHint(QPainter::Antialiasing, true);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setViewportUpdateMode(SmartViewportUpdate);
    setDragMode(NoDrag);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
}

void BlockView::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasFormat(kSampleMime)) { e->ignore(); return; }
    dragEntry_ = QString::fromUtf8(e->mimeData()->data(kSampleMime));
    e->setDropAction(Qt::CopyAction);
    e->accept();
    emit paletteDrag(dragEntry_, mapToScene(e->position().toPoint()), viewport()->mapToGlobal(e->position().toPoint()), 0);
}

void BlockView::dragMoveEvent(QDragMoveEvent* e) {
    if (dragEntry_.isEmpty()) { e->ignore(); return; }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    emit paletteDrag(dragEntry_, mapToScene(e->position().toPoint()), viewport()->mapToGlobal(e->position().toPoint()), 1);
}

void BlockView::dragLeaveEvent(QDragLeaveEvent* e) {
    if (!dragEntry_.isEmpty()) emit paletteDrag(dragEntry_, QPointF(), QPoint(), 3);
    dragEntry_.clear();
    e->accept();
}

void BlockView::dropEvent(QDropEvent* e) {
    if (dragEntry_.isEmpty()) { e->ignore(); return; }
    e->setDropAction(Qt::CopyAction);
    e->accept();
    const QString id = dragEntry_;
    dragEntry_.clear();
    emit paletteDrag(id, mapToScene(e->position().toPoint()), viewport()->mapToGlobal(e->position().toPoint()), 2);
}

void BlockView::setTheme(const blocks::BlockTheme& t) {
    theme_ = t;
    viewport()->update();
}

void BlockView::setZoom(double z) {
    z = std::max(kMinZoom, std::min(kMaxZoom, z));
    if (std::fabs(z - zoom_) < 1e-9) return;
    zoom_ = z;
    QTransform t;
    t.scale(z, z);
    setTransform(t);
    emit zoomChanged(zoom_);
}

void BlockView::fitAll() {
    if (!scene()) return;
    QRectF r;
    for (QGraphicsItem* it : scene()->items()) r |= it->sceneBoundingRect();
    if (r.isEmpty()) { resetZoom(); return; }
    r.adjust(-20, -20, 20, 20);
    const double z = std::min(1.0, std::min(viewport()->width() / r.width(), viewport()->height() / r.height()));
    setZoom(z);
    centerOn(r.center());
}

void BlockView::showBlock(const QRectF& r) { ensureVisible(r, 40, 40); }

void BlockView::showContent() {
    if (!scene()) return;
    QRectF r;
    for (QGraphicsItem* it : scene()->items()) r |= it->sceneBoundingRect();
    if (r.isEmpty()) return;
    const QSizeF vp = mapToScene(viewport()->rect()).boundingRect().size();
    if (r.width() <= vp.width() && r.height() <= vp.height()) centerOn(r.center());
    else centerOn(r.left() + vp.width() / 2 - 20, r.top() + vp.height() / 2 - 20);
}

void BlockView::wheelEvent(QWheelEvent* e) {
    // the wheel zooms (around the pointer); Shift + wheel scrolls sideways, Alt + wheel scrolls up and down; panning is the middle button / space + drag
    if (e->modifiers() & Qt::ShiftModifier) { horizontalScrollBar()->setValue(horizontalScrollBar()->value() - e->angleDelta().y() / 2); e->accept(); return; }
    if (e->modifiers() & Qt::AltModifier) { verticalScrollBar()->setValue(verticalScrollBar()->value() - e->angleDelta().y() / 2); e->accept(); return; }
    const double steps = e->angleDelta().y() / 120.0;
    if (steps != 0) setZoom(zoom_ * std::pow(1.15, steps));
    e->accept();
}

void BlockView::mousePressEvent(QMouseEvent* e) {
    rightReleased_.invalidate();                                 // (a new press: whatever menu event comes now belongs to it, not to the last right click)
    if (e->button() == Qt::MiddleButton || (space_ && e->button() == Qt::LeftButton)) {
        panning_ = true;
        last_ = e->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    // the right button on the empty canvas (the grid: no stack under the pointer): the start of a pan, or of a click that opens the menu
    if (e->button() == Qt::RightButton && scene() && !scene()->itemAt(mapToScene(e->pos()), transform())) {
        rightDown_ = true;
        rightMoved_ = false;
        rightPress_ = last_ = e->pos();
        e->accept();
        return;
    }
    QGraphicsView::mousePressEvent(e);
}

void BlockView::mouseMoveEvent(QMouseEvent* e) {
    if (rightDown_) {
        if (!rightMoved_ && (e->pos() - rightPress_).manhattanLength() > 4) {          // (a hand that trembles is still a click)
            rightMoved_ = true;
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
        if (rightMoved_) {
            const QPoint d = e->pos() - last_;
            last_ = e->pos();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        }
        e->accept();
        return;
    }
    if (panning_) {
        const QPoint d = e->pos() - last_;
        last_ = e->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        e->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
}

void BlockView::mouseReleaseEvent(QMouseEvent* e) {
    if (rightDown_ && e->button() == Qt::RightButton) {
        const bool click = !rightMoved_;
        rightDown_ = rightMoved_ = false;
        rightReleased_.start();
        viewport()->setCursor(space_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
        e->accept();
        if (click) {                                                                  // not dragged: the menu of the canvas, as a right click always gave
            QContextMenuEvent menu(QContextMenuEvent::Mouse, e->pos(), e->globalPosition().toPoint(), e->modifiers());
            forwardingMenu_ = true;
            QGraphicsView::contextMenuEvent(&menu);
            forwardingMenu_ = false;
        }
        return;
    }
    if (panning_) {
        panning_ = false;
        viewport()->setCursor(space_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
        e->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
}

// The platform sends its own "context menu" event for a right click: when the button goes down (X11) or after it has come up (Windows). For a right button
// that this view is handling (the empty canvas) that event is not the one to act on -- the menu opens from the release above, and never after a drag.
void BlockView::contextMenuEvent(QContextMenuEvent* e) {
    if (!forwardingMenu_ && e->reason() == QContextMenuEvent::Mouse && (rightDown_ || (rightReleased_.isValid() && rightReleased_.elapsed() < 150))) { e->accept(); return; }
    QGraphicsView::contextMenuEvent(e);
}

void BlockView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { space_ = true; viewport()->setCursor(Qt::OpenHandCursor); e->accept(); return; }
    if (e->modifiers() & Qt::ControlModifier) {
        if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) { zoomIn(); e->accept(); return; }
        if (e->key() == Qt::Key_Minus) { zoomOut(); e->accept(); return; }
        if (e->key() == Qt::Key_0) { resetZoom(); e->accept(); return; }
    }
    QGraphicsView::keyPressEvent(e);
}

void BlockView::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { space_ = false; if (!panning_) viewport()->setCursor(Qt::ArrowCursor); e->accept(); return; }
    QGraphicsView::keyReleaseEvent(e);
}

void BlockView::drawBackground(QPainter* p, const QRectF& rect) {
    p->fillRect(rect, toQColor(theme_.canvas));
    const double step = 24;
    if (zoom_ < 0.4) return;                                     // too dense to be of use
    const double x0 = std::floor(rect.left() / step) * step, y0 = std::floor(rect.top() / step) * step;
    p->setRenderHint(QPainter::Antialiasing, false);
    for (double x = x0; x < rect.right(); x += step) {
        p->setPen(QPen(toQColor(std::fmod(std::fabs(x), step * 5) < 0.5 ? theme_.gridMajor : theme_.grid), 0));
        p->drawLine(QPointF(x, rect.top()), QPointF(x, rect.bottom()));
    }
    for (double y = y0; y < rect.bottom(); y += step) {
        p->setPen(QPen(toQColor(std::fmod(std::fabs(y), step * 5) < 0.5 ? theme_.gridMajor : theme_.grid), 0));
        p->drawLine(QPointF(rect.left(), y), QPointF(rect.right(), y));
    }
}

}  // namespace acide
