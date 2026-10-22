#pragma once
// The canvas: one StackItem per stack (a section of the document). The scene follows a BlockSession -- it lays the document out again when
// the session says it changed and repaints when the selection changed -- and tells the outside what the person did only as signals
// (blockClicked, emptyClicked ...), so the same scene serves the read-only viewer of M2 and the editor of M3.
#include <map>
#include <memory>
#include <set>
#include <string>

#include <QCoreApplication>
#include <QFont>
#include <QGraphicsItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <vector>

#include "block_painter.h"
#include "blocks/block_layout.h"
#include "blocks/block_session.h"
#include "blocks/block_theme.h"
#include "signal.h"

namespace acide {

// Measures with Qt fonts.
class QtTextMetrics : public blocks::ITextMetrics {
public:
    QtTextMetrics(const QFont& ui, const QFont& mono) : ui_(ui), mono_(mono) {}
    double width(const std::string& s, bool mono) const override;
    double lineHeight(bool mono) const override;

private:
    QFont ui_, mono_;
};

// The words of the canvas, translated.
class QtTextProvider : public blocks::ITextProvider {
public:
    std::string blockTitle(const asmcore::blocks::BlockDef& d) const override;
    std::string fieldLabel(const asmcore::blocks::BlockDef&, const asmcore::blocks::FieldDef& f) const override;
    std::string inputLabel(const asmcore::blocks::BlockDef&, const asmcore::blocks::InputDef& i) const override;
    std::string hatTitle(const std::string& domain) const override;
    std::string floatingTag() const override;
    std::string collapsedSummary(size_t blocks) const override;
    std::string emptyValue() const override;
};

class BlockScene;

class StackItem : public QGraphicsItem {
public:
    StackItem(BlockScene& scene, size_t index);
    QRectF boundingRect() const override;
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;
    size_t index() const { return index_; }
    void relayout();

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* e) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* e) override;

private:
    const blocks::BlockBox* boxAt(const QPointF& itemPos) const;
    BlockScene& scene_;
    size_t index_;
};

class BlockScene : public QGraphicsScene {
public:
    BlockScene(blocks::BlockSession& session, const blocks::BlockTheme& theme, const QFont& ui, const QFont& mono, QObject* parent = nullptr);
    ~BlockScene() override;

    blocks::BlockSession& session() { return session_; }
    const blocks::DocumentLayout& layout() const { return layout_; }
    BlockPainter& painter() { return painter_; }
    const blocks::BlockTheme& theme() const { return painter_.theme(); }
    void setTheme(const blocks::BlockTheme& t);
    void setLayoutStyle(const blocks::LayoutStyle& s);

    // The blocks drawn with a badge: id -> the message shown as tooltip.
    const std::map<std::string, std::string>& problems() const { return problems_; }
    // Blocks marked as unable to nest (the diagonal lines); the editor sets them while a block is being dragged.
    void setHatched(const std::set<std::string>& ids);
    const std::set<std::string>& hatched() const { return hatched_; }
    std::string hovered() const { return hovered_; }
    void setHovered(const std::string& id);

    // ---- what the pointer does on the canvas (the scene owns the interaction; the view only zooms and pans)
    //   press on a block selects it (Ctrl toggles); press on empty canvas and drag draws a selection rectangle (Ctrl adds);
    //   dragging a selected block moves the whole stack it stands in (every stack that has a selected block), kept inside the canvas.
    std::string blockAt(const QPointF& scenePos) const;                 // innermost block, "" if none
    std::vector<std::string> blocksIn(const QRectF& r) const;           // blocks touched by a rectangle; a block inside a block that is also there is left out
    void selectAll();
    bool dragging() const { return mode_ == Mode::MoveStacks; }
    // Where the stacks being moved would end up; used by the drag and by tests.
    void moveSelectedStacksBy(const QPointF& delta, bool commit);

    // The rectangle of a block on the canvas (empty if unknown).
    QRectF blockRect(const std::string& id) const;
    // Where a stack lies; the sceneRect covers every stack with a wide margin so the canvas can be panned beyond them.
    void rebuild();

    // What happened on the canvas (the view owner connects):
    Signal<const std::string&, Qt::KeyboardModifiers> blockClicked;     // block id
    Signal<Qt::KeyboardModifiers> emptyClicked;
    Signal<> rebuilt;

    const std::set<std::string>& errorIds() const { return errorIds_; }
    const std::set<std::string>& warningIds() const { return warningIds_; }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* e) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* e) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* e) override;

private:
    friend class StackItem;
    void updateProblems();
    std::set<size_t> selectedStacks() const;
    QRectF canvasArea() const;

    enum class Mode { Idle, PressedBlock, MoveStacks, Band };
    Mode mode_ = Mode::Idle;
    QPointF pressPos_;
    std::string pressedId_;
    bool pressedWasSelected_ = false;
    Qt::KeyboardModifiers pressMods_;
    QGraphicsRectItem* band_ = nullptr;
    std::set<std::string> bandBase_;
    std::map<size_t, QPointF> dragOrigin_;                              // stack index -> its position when the drag began

    blocks::BlockSession& session_;
    QtTextMetrics metrics_;
    QtTextProvider text_;
    blocks::LayoutStyle style_;
    BlockPainter painter_;
    blocks::DocumentLayout layout_;
    std::map<std::string, std::string> problems_;
    std::set<std::string> errorIds_, warningIds_, hatched_;
    std::string hovered_;
    ConnectionBag conns_;
    bool rebuilding_ = false;
};

}  // namespace acide
