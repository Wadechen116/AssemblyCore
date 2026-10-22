#pragma once
// The window onto the canvas: zoom (Ctrl + wheel, buttons), panning (middle button, or space + drag), a grid, "fit everything".
#include <QGraphicsView>

#include "blocks/block_theme.h"

namespace acide {

class BlockView : public QGraphicsView {
    Q_OBJECT
public:
    explicit BlockView(QGraphicsScene* scene, QWidget* parent = nullptr);
    double zoom() const { return zoom_; }
    void setTheme(const blocks::BlockTheme& t);
    static constexpr double kMinZoom = 0.25, kMaxZoom = 3.0;

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

protected:
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void drawBackground(QPainter* p, const QRectF& rect) override;

private:
    double zoom_ = 1.0;
    bool panning_ = false, space_ = false;
    QPoint last_;
    blocks::BlockTheme theme_ = blocks::BlockTheme::light();
};

}  // namespace acide
