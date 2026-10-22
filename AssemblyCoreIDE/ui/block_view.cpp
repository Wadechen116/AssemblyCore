#include "block_view.h"

#include <QGraphicsItem>
#include <QKeyEvent>
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

void BlockView::wheelEvent(QWheelEvent* e) {
    if (e->modifiers() & Qt::ControlModifier) {
        const double steps = e->angleDelta().y() / 120.0;
        setZoom(zoom_ * std::pow(1.15, steps));
        e->accept();
        return;
    }
    QGraphicsView::wheelEvent(e);
}

void BlockView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton || (space_ && e->button() == Qt::LeftButton)) {
        panning_ = true;
        last_ = e->pos();
        viewport()->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QGraphicsView::mousePressEvent(e);
}

void BlockView::mouseMoveEvent(QMouseEvent* e) {
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
    if (panning_) {
        panning_ = false;
        viewport()->setCursor(space_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
        e->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
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
