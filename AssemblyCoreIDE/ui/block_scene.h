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
#include <QGraphicsScene>

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
    void mousePressEvent(QGraphicsSceneMouseEvent* e) override;

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

private:
    friend class StackItem;
    void updateProblems();

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
