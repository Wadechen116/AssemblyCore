#pragma once
// The geometry of a block program: where every block, field, slot and connection point lies. No Qt and no drawing: text is measured through
// ITextMetrics and every word shown to the person comes from ITextProvider, so a renderer (Qt today, anything tomorrow) draws what this
// computes, tests run with fixed-width fake metrics, and translation never touches the layout code.
//
// Coordinates are in "units" (pixels at 100 % zoom). Inside a StackLayout they are relative to the stack's own origin; DocumentLayout
// places the stacks on the canvas with the position stored in the document.
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "asmcore/blocks.h"
#include "blocks/block_shapes.h"

namespace acide {
namespace blocks {

struct Pt {
    double x = 0, y = 0;
};
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    Pt center() const { return {x + w / 2, y + h / 2}; }
    bool contains(Pt p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    bool intersects(const Rect& o) const { return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
    bool empty() const { return w <= 0 || h <= 0; }
    Rect translated(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
};
Rect unite(const Rect& a, const Rect& b);

// ---- what the layout needs from the outside --------------------------------------------------------------------------------------------
class ITextMetrics {
public:
    virtual ~ITextMetrics() = default;
    virtual double width(const std::string& utf8, bool mono) const = 0;
    virtual double lineHeight(bool mono) const = 0;
};

// The words on the canvas. The default returns the English text of the definitions; the IDE supplies a translating one.
class ITextProvider {
public:
    virtual ~ITextProvider() = default;
    virtual std::string blockTitle(const asmcore::blocks::BlockDef& d) const { return d.title; }
    virtual std::string fieldLabel(const asmcore::blocks::BlockDef&, const asmcore::blocks::FieldDef& f) const { return f.label; }
    virtual std::string inputLabel(const asmcore::blocks::BlockDef&, const asmcore::blocks::InputDef& i) const { return i.label; }
    virtual std::string hatTitle(const std::string& domain) const;                     // "Main", "Peri", "Definitions"
    virtual std::string floatingTag() const { return "not part of the program"; }
    virtual std::string collapsedSummary(size_t blocks) const { return std::to_string(blocks) + (blocks == 1 ? " block" : " blocks"); }
    virtual std::string emptyValue() const { return "..."; }                          // what an empty field shows
};

struct LayoutStyle {
    double padX = 10, rowPadY = 5;               // inside a block row
    double gap = 6;                              // between the parts of a row
    double chipPadX = 6, chipPadY = 2;           // inside a field chip
    double minRow = 28;
    double slotIndent = 16, slotPad = 4, emptySlot = 18, containerFoot = 12;
    double slotLabel = 18;                       // the row that names a slot ("then", "else") in a block with more than one
    double hatHeight = 30, hatMinWidth = 110, hatPadX = 14;
    double tagHeight = 18;                       // the strip above a floating stack
    double minWidth = 56;
    double maxChipWidth = 240, maxNoteWidth = 420;
    double spacerWidth = 48, spacerHeight = 10;
    double socketMinWidth = 34;
    double tailStub = 22;                        // the empty zone at the end of a block row where a double click edits the end-of-line comment
    double plugDepth = 7;                        // how far the tab of a component reaches into the dent of its socket (to the left of the component)
    double commentMaxWidth = 220;
    bool showEmptyOptional = false;              // an editor shows the empty optional fields (to be filled in); a viewer leaves them out
    bool showFieldLabels = true;
};

// ---- the result ----------------------------------------------------------------------------------------------------------------------------
struct FieldBox {
    std::string name, label, text, fullText;      // text: what is drawn (shortened if needed); fullText: the value
    asmcore::blocks::FieldKind kind = asmcore::blocks::FieldKind::Text;
    Rect rect, labelRect;                         // the chip, and the label before it (empty if none)
    bool elided = false, empty = false, checked = false;
};
struct SocketBox {
    std::string name, label;
    Rect rect, labelRect;
    int child = -1;                               // index into StackLayout::blocks of the block plugged in, -1: empty
    std::string text, fullText;                   // nothing plugged in: the text typed (what the block says when it holds no component), shortened / whole
    bool empty = true, elided = false;
    std::vector<std::string> types;               // what the socket takes (value types): a renderer may shape the dent after it
    PlugShape plug = PlugShape::None;             // the shape of the dent (from the types)
    double plugDepth = 0;
};
struct SlotBox {
    std::string name;
    std::string label;                            // "" for a block with one slot; else the name of the slot in lower case ("then")
    Rect labelRect;                               // the row above the slot that carries the label
    Rect rect;                                    // where the stack inside lies (at least a minimum height, even when empty)
    std::vector<int> items;                       // top-level blocks of the slot, indexes into StackLayout::blocks
};
struct BlockBox {
    std::string id, type, title, comment, category, valueType;
    std::vector<std::string> commentLines;        // the comment as drawn: one string per line, each shortened to fit
    Rect tailRect;                                // the end of the block: the comment, or the empty zone where a double click starts one
    PlugShape plug = PlugShape::None;             // a component's plug: the tab on its left edge
    double plugDepth = 0;
    Shape shape = Shape::Statement;
    Rect rect;                                    // the whole block, slotBoxes included
    Rect head;                                    // its top row
    Rect titleRect, commentRect;
    std::vector<FieldBox> fields;
    std::vector<SocketBox> sockets;
    std::vector<SlotBox> slotBoxes;
    bool disabled = false, collapsed = false, known = true;
    int parent = -1;                              // the block this one stands in (slot or socket), -1: top level of the stack
    std::string parentSlot;                       // the slot / socket name
    int depth = 0;
    size_t hiddenBlocks = 0;                      // collapsed: how many blocks are folded away
};

struct ConnectionPoint {
    enum Kind { Previous, Next, Slot, Plug, Socket } kind = Previous;
    Pt pos;
    std::string blockId;                          // the block it belongs to ("" for the point under the stack's header)
    std::string slot;
};

struct HeaderBox {
    enum Kind { None, Hat, Tag } kind = None;
    Rect rect;
    std::string text, domain;
    int order = 0;                                // a hat of the program: 1, 2, 3 ... the order the text is written in (0: not part of the program)
    std::string comment;                          // a hat carries the comment after .main / .peri
    std::vector<std::string> commentLines;
    Rect commentRect, tailRect;
};

// A group of blocks that belong together without being one block: a cascade (CASCADE_BEGIN ... CASCADE_END n). The canvas frames it.
struct RegionBox {
    std::string kind, label;
    Rect rect;
};

struct StackLayout {
    size_t section = 0;
    std::string domain;
    Pt origin;                                    // where the stack lies on the canvas
    HeaderBox header;
    Rect bounds;                                  // header and all blocks (stack coordinates)
    std::vector<BlockBox> blocks;                 // a block comes before everything inside it
    std::vector<int> top;                         // the top-level blocks in order
    std::vector<ConnectionPoint> points;
    std::vector<RegionBox> regions;               // frames around cascades

    const BlockBox* find(const std::string& id) const;
    // The innermost block under a point (stack coordinates), or nullptr; parents are found only where their children are not.
    const BlockBox* blockAt(Pt p) const;
};

struct DocumentLayout {
    std::vector<StackLayout> stacks;              // one per section, same order as Document::sections
    Rect bounds;                                  // all stacks, canvas coordinates
    const StackLayout* stackOf(const std::string& blockId, const BlockBox** box = nullptr) const;
};

// ---- the engine --------------------------------------------------------------------------------------------------------------------------
class LayoutEngine {
public:
    LayoutEngine(const ITextMetrics& metrics, const LayoutStyle& style = LayoutStyle(), const asmcore::blocks::Registry& reg = asmcore::blocks::Registry::defaults(),
                 const ShapeRegistry& shapes = ShapeRegistry::defaults(), const ITextProvider* text = nullptr);

    StackLayout layoutStack(const asmcore::blocks::Section& s, size_t index) const;
    DocumentLayout layoutDocument(const asmcore::blocks::Document& d) const;
    const LayoutStyle& style() const { return style_; }

private:
    const ITextMetrics& m_;
    LayoutStyle style_;
    const asmcore::blocks::Registry& reg_;
    const ShapeRegistry& shapes_;
    const ITextProvider* text_;
    struct Run;
};

}  // namespace blocks
}  // namespace acide
