#pragma once
// Draws the geometry that LayoutEngine computed. Stateless apart from the theme and the fonts: what to draw comes in as a StackLayout and a
// BlockPaintState per block. Each shape (Statement, Container ...) and each kind of field is painted by a small strategy object kept in a
// table, so a new shape or a different look for a kind of field is one class and one registration -- nothing else changes.
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPainterPath>

#include "blocks/block_layout.h"
#include "blocks/block_theme.h"

namespace acide {

struct BlockPaintState {
    bool selected = false, hovered = false;
    bool hatched = false;                 // cannot be nested here (the diagonal lines)
    bool error = false, warning = false;  // a badge
    QColor tint;                          // valid: the block is part of the line a sequencer is executing: drawn in this colour instead of its category's
    // the simulator on the block
    double heat = -1;                     // 0..1: how often it was executed compared with the busiest block; < 0: not run (or no run to show)
    uint64_t hits = 0;
    bool dim = false;                     // an instruction that has not run, while others have
    double flash = 0;                     // 1 just executed ... 0 long ago
    bool flashPeri = false;
};

// What the simulator tells the blocks, for painting (block_scene keeps it up to date).
struct ExecPaint {
    std::map<std::string, uint64_t> hits;
    std::set<std::string> executable;                                  // the blocks that make instructions
    uint64_t maxHits = 0;
    std::map<std::string, std::pair<qint64, bool>> flash;              // block -> (when, Peri's?)
    qint64 now = 0;
    bool showFlash = true, showHeat = true;
};

// What the blocks of one stack are drawn with.
struct StackPaintInfo {
    const std::set<std::string>* selected = nullptr;
    std::string hovered;
    const std::set<std::string>* hatched = nullptr;
    const std::set<std::string>* errors = nullptr;
    const std::set<std::string>* warnings = nullptr;
    const ExecPaint* exec = nullptr;
    const std::map<std::string, QColor>* tint = nullptr;               // the blocks of the line each sequencer is executing (simulator running or stopped)
    bool floating = false;
    // A soft effect on a block (and everything inside it): scale around an anchor. Returns false if the block has none. anchor: 0 = its centre, 1 = the left edge.
    std::function<bool(const std::string& id, double& sx, double& sy, int& anchor)> fx;
    BlockPaintState stateOf(const std::string& id) const;
};

QColor toQColor(const blocks::Rgba& c);

class BlockPainter;
struct PaintContext {
    QPainter& p;
    const blocks::BlockTheme& theme;
    const QFont &ui, &mono;
    BlockPainter& owner;
};

class IShapePainter {
public:
    virtual ~IShapePainter() = default;
    virtual void paint(PaintContext& c, const blocks::BlockBox& b, const BlockPaintState& st) = 0;
};

class IFieldPainter {
public:
    virtual ~IFieldPainter() = default;
    virtual void paint(PaintContext& c, const blocks::FieldBox& f, const blocks::BlockBox& b, const QColor& textColor) = 0;
};

class BlockPainter {
public:
    BlockPainter(const blocks::BlockTheme& theme, const QFont& ui, const QFont& mono);
    void setTheme(const blocks::BlockTheme& t) { theme_ = t; }
    const blocks::BlockTheme& theme() const { return theme_; }
    void setShapePainter(blocks::Shape s, std::unique_ptr<IShapePainter> p) { shapes_[s] = std::move(p); }
    void setFieldPainter(asmcore::blocks::FieldKind k, std::unique_ptr<IFieldPainter> p) { fields_[k] = std::move(p); }

    void paintStack(QPainter& p, const blocks::StackLayout& s, const StackPaintInfo& info);
    void paintBlock(QPainter& p, const blocks::BlockBox& b, const BlockPaintState& st);
    void paintField(PaintContext& c, const blocks::FieldBox& f, const blocks::BlockBox& b, const QColor& text);

    // The outline of a block as a path (used for painting and for the selection / hatch clip).
    static QPainterPath outline(const blocks::BlockBox& b);
    QColor fillOf(const blocks::BlockBox& b) const;
    QColor fillFor(const blocks::BlockBox& b, const BlockPaintState& st) const { return st.tint.isValid() ? st.tint : fillOf(b); }

private:
    blocks::BlockTheme theme_;
    QFont ui_, mono_;
    std::map<blocks::Shape, std::unique_ptr<IShapePainter>> shapes_;
    std::map<asmcore::blocks::FieldKind, std::unique_ptr<IFieldPainter>> fields_;
};

}  // namespace acide
