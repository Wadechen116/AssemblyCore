#pragma once
// The window onto the canvas: zoom (Ctrl + wheel, buttons), panning (middle button, space + drag, or the RIGHT button dragged on the empty canvas), a grid,
// "fit everything". The right button on the empty canvas: dragged, the whole picture moves with the pointer (nothing in the document changes); pressed and
// released without moving, the context menu of the canvas opens as before. On a block the right button is the block's menu, as it was.
#include <QElapsedTimer>
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
    // The zoom a canvas opens with: 66 % shows about twice as much of a program as 100 % and the text is still read with ease. The setting blocks.zoom
    // (per cent, 25 .. 300) changes it; "1:1" and Ctrl+0 still go to 100 %.
    static constexpr int kDefaultZoomPercent = 66;
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
    void contextMenuEvent(QContextMenuEvent* e) override;
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
    bool rightDown_ = false, rightMoved_ = false, forwardingMenu_ = false;      // the right button went down on the empty canvas / has been dragged since
    QPoint rightPress_;
    QElapsedTimer rightReleased_;                                              // when it came up (the platform's own menu event of that click is not wanted twice)
    QPoint last_;
    QString dragEntry_;
    blocks::BlockTheme theme_ = blocks::BlockTheme::light();
};

}  // namespace acide
