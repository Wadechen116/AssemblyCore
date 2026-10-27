#pragma once
// The window onto the canvas: zoom (Ctrl + wheel, buttons), panning (middle button, or space + drag), a grid, "fit everything".
#include <QGraphicsView>
#include <QString>

#include "blocks/block_theme.h"

namespace acide {

class BlockView : public QGraphicsView {
    Q_OBJECT
public:
    explicit BlockView(QGraphicsScene* scene, QWidget* parent = nullptr);
    double zoom() const { return zoom_; }
    void setTheme(const blocks::BlockTheme& t);
    static constexpr double kMinZoom = 0.25, kMaxZoom = 3.0;
    static constexpr const char* kSampleMime = "application/x-acblk-sample";

public slots:
    void setZoom(double z);                      // 1.0 = 100 %
    void zoomIn() { setZoom(zoom_ * 1.2); }
    void zoomOut() { setZoom(zoom_ / 1.2); }
    void resetZoom() { setZoom(1.0); }
    void fitAll();                               // all stacks in view (never above 100 %)
    void showBlock(const QRectF& r);             // scroll so that a rectangle is visible
    void showContent();                          // at 100 %: the program centred if it fits, else its top left corner

signals:
    void zoomChanged(double z);
    // a sample from the toolbox is dragged over the canvas: phase 0 it came in, 1 it moves, 2 it was dropped, 3 it left without being dropped
    void paletteDrag(const QString& entryId, const QPointF& scenePos, const QPoint& screenPos, int phase);

protected:
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dragLeaveEvent(QDragLeaveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void drawBackground(QPainter* p, const QRectF& rect) override;

private:
    double zoom_ = 1.0;
    bool panning_ = false, space_ = false;
    QPoint last_;
    QString dragEntry_;
    blocks::BlockTheme theme_ = blocks::BlockTheme::light();
};

}  // namespace acide
