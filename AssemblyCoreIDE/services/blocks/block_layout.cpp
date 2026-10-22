#include "blocks/block_layout.h"

#include <algorithm>
#include <cmath>

namespace acide {
namespace blocks {

using namespace asmcore::blocks;

Rect unite(const Rect& a, const Rect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const double x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.right(), b.right()) - x, std::max(a.bottom(), b.bottom()) - y};
}

std::string ITextProvider::hatTitle(const std::string& domain) const {
    if (domain == "main") return "Main";
    if (domain == "peri") return "Peri";
    return "Definitions";
}

const BlockBox* StackLayout::find(const std::string& id) const {
    for (const BlockBox& b : blocks) if (b.id == id) return &b;
    return nullptr;
}

const BlockBox* StackLayout::blockAt(Pt p) const {
    for (size_t i = blocks.size(); i-- > 0;)
        if (blocks[i].rect.contains(p)) return &blocks[i];
    return nullptr;
}

const StackLayout* DocumentLayout::stackOf(const std::string& blockId, const BlockBox** box) const {
    for (const StackLayout& s : stacks)
        if (const BlockBox* b = s.find(blockId)) { if (box) *box = b; return &s; }
    return nullptr;
}

namespace {
const ITextProvider& defaultText() { static const ITextProvider t; return t; }

// the end of the UTF-8 sequence that starts at i
size_t nextCodePoint(const std::string& s, size_t i) {
    size_t j = i + 1;
    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) j++;
    return j;
}

size_t countBlocks(const std::vector<Block>& v) {
    size_t n = 0;
    for (const Block& b : v) { n++; for (const auto& st : b.stacks) n += countBlocks(st.second); }
    return n;
}
}  // namespace

LayoutEngine::LayoutEngine(const ITextMetrics& metrics, const LayoutStyle& style, const Registry& reg, const ShapeRegistry& shapes, const ITextProvider* text)
    : m_(metrics), style_(style), reg_(reg), shapes_(shapes), text_(text ? text : &defaultText()) {}

struct LayoutEngine::Run {
    const LayoutEngine& e;
    StackLayout& out;
    const ITextProvider& T;
    const LayoutStyle& S;

    Run(const LayoutEngine& en, StackLayout& o) : e(en), out(o), T(*en.text_), S(en.style_) {}

    double w(const std::string& s, bool mono) const { return e.m_.width(s, mono); }
    double chipHeight() const { return e.m_.lineHeight(true) + 2 * S.chipPadY; }
    double baseRow() const { return std::max(S.minRow, chipHeight() + 2 * S.rowPadY); }

    // `s` shortened with an ellipsis so that it fits `maxW`
    std::string elide(const std::string& s, double maxW, bool mono, bool& elided) const {
        elided = false;
        if (w(s, mono) <= maxW) return s;
        elided = true;
        const std::string dots = "\xE2\x80\xA6";
        std::string cut;
        size_t i = 0;
        while (i < s.size()) {
            size_t j = nextCodePoint(s, i);
            if (w(cut + s.substr(i, j - i) + dots, mono) > maxW) break;
            cut += s.substr(i, j - i);
            i = j;
        }
        return cut + dots;
    }

    bool isDescendant(int j, int ancestor) const {
        for (int p = out.blocks[(size_t)j].parent; p >= 0; p = out.blocks[(size_t)p].parent) if (p == ancestor) return true;
        return false;
    }
    static void shift(Rect& r, double dx, double dy) { r.x += dx; r.y += dy; }
    void shiftBox(BlockBox& b, double dx, double dy) const {
        shift(b.rect, dx, dy); shift(b.head, dx, dy); shift(b.titleRect, dx, dy); shift(b.commentRect, dx, dy);
        for (FieldBox& f : b.fields) { shift(f.rect, dx, dy); shift(f.labelRect, dx, dy); }
        for (SocketBox& s : b.sockets) { shift(s.rect, dx, dy); shift(s.labelRect, dx, dy); }
        for (SlotBox& s : b.slotBoxes) shift(s.rect, dx, dy);
    }
    // a block and everything inside it (and the connection points that belong to them)
    void translate(int idx, double dx, double dy) {
        if (dx == 0 && dy == 0) return;
        std::vector<char> moved(out.blocks.size(), 0);
        moved[(size_t)idx] = 1;
        shiftBox(out.blocks[(size_t)idx], dx, dy);
        std::vector<std::string> ids{out.blocks[(size_t)idx].id};
        for (size_t j = (size_t)idx + 1; j < out.blocks.size(); j++)
            if (isDescendant((int)j, idx)) { moved[j] = 1; shiftBox(out.blocks[j], dx, dy); ids.push_back(out.blocks[j].id); }
        for (ConnectionPoint& p : out.points)
            if (std::find(ids.begin(), ids.end(), p.blockId) != ids.end()) { p.pos.x += dx; p.pos.y += dy; }
    }

    double list(const std::vector<Block>& v, double x, double y, int depth, int parent, const std::string& slot, std::vector<int>& items, double& width) {
        double cy = y;
        width = 0;
        for (const Block& b : v) {
            const int idx = block(b, x, cy, depth, parent, slot);
            items.push_back(idx);
            const BlockBox& bb = out.blocks[(size_t)idx];
            ConnectionPoint prev, next;
            prev.kind = ConnectionPoint::Previous; prev.pos = {x, cy}; prev.blockId = bb.id;
            next.kind = ConnectionPoint::Next; next.pos = {x, cy + bb.rect.h}; next.blockId = bb.id;
            out.points.push_back(prev);
            out.points.push_back(next);
            cy += bb.rect.h;
            width = std::max(width, bb.rect.w);
        }
        return cy - y;
    }

    int block(const Block& b, double x, double y, int depth, int parent, const std::string& slot) {
        const BlockDef* def = e.reg_.find(b.type);
        BlockBox box;
        box.id = b.id;
        box.type = b.type;
        box.depth = depth;
        box.parent = parent;
        box.parentSlot = slot;
        box.comment = b.comment;
        box.disabled = b.disabled;
        box.collapsed = b.collapsed;
        box.known = def != nullptr;
        box.category = def ? def->category : "Text";
        ShapeInfo info;
        if (def) info = e.shapes_.infoFor(*def);
        else info = ShapeInfo{Shape::Text, false};
        box.shape = info.shape;
        const int idx = (int)out.blocks.size();
        out.blocks.push_back(box);
        auto self = [&]() -> BlockBox& { return out.blocks[(size_t)idx]; };
        const double lh = e.m_.lineHeight(false), base = baseRow();

        switch (info.shape) {
            case Shape::Spacer:
                self().rect = {x, y, S.spacerWidth, S.spacerHeight};
                self().head = self().rect;
                return idx;
            case Shape::Tag: {
                const std::string name = b.get("name", "?") + ":";
                bool el = false;
                const std::string shown = elide(name, S.maxChipWidth, true, el);
                const double cw = w(shown, true) + 2 * S.chipPadX + 8;
                FieldBox f;
                f.name = "name"; f.label = ""; f.text = shown; f.fullText = b.get("name"); f.kind = FieldKind::Text; f.elided = el;
                f.rect = {x + 4, y + (base * 0.8 < chipHeight() ? 0 : (base - base * 0.8) / 2), cw, std::max(chipHeight(), base * 0.8)};
                self().fields.push_back(f);
                self().rect = {x, y, cw + 8, base * 0.8 + 4};
                self().head = self().rect;
                self().fields[0].rect.y = y + (self().rect.h - self().fields[0].rect.h) / 2;
                return idx;
            }
            case Shape::Note:
            case Shape::Text: {
                const bool mono = info.shape == Shape::Text;
                const std::string full = b.get("text");
                bool el = false;
                const std::string shown = elide(full, S.maxNoteWidth, mono, el);
                const double tw = w(shown.empty() ? std::string(" ") : shown, mono);
                FieldBox f;
                f.name = "text"; f.text = shown; f.fullText = full; f.kind = FieldKind::Text; f.elided = el; f.empty = full.empty();
                const double rowH = std::max(S.minRow * 0.8, (mono ? e.m_.lineHeight(true) : lh) + 2 * S.rowPadY);
                f.rect = {x + S.padX, y + S.rowPadY, tw, rowH - 2 * S.rowPadY};
                self().fields.push_back(f);
                self().rect = {x, y, std::max(S.minWidth, tw + 2 * S.padX), rowH};
                self().head = self().rect;
                return idx;
            }
            default: break;
        }

        // ---- Statement / Container / Value: a row of title, fields and sockets
        const std::string title = def && info.showTitle ? T.blockTitle(*def) : std::string();
        self().title = title;
        // the blocks plugged into value sockets are laid out first (their size decides the height of the row)
        std::vector<int> plugged;                       // parallel to def->inputs, -1 = empty
        double maxChildH = 0;
        if (def) {
            for (const InputDef& in : def->inputs) {
                const Block* child = asmcore::blocks::partVisible(in.when, b) ? b.value(in.name) : nullptr;
                if (!child) { plugged.push_back(-1); continue; }
                const int ci = block(*child, 0, 0, depth + 1, idx, in.name);
                plugged.push_back(ci);
                maxChildH = std::max(maxChildH, out.blocks[(size_t)ci].rect.h);
            }
        }
        const double rowH = std::max(base, maxChildH + 2 * S.rowPadY);
        double cx = x + S.padX;
        auto centerY = [&](double h) { return y + (rowH - h) / 2; };
        if (!title.empty()) {
            const double tw = w(title, false);
            self().titleRect = {cx, centerY(lh), tw, lh};
            cx += tw + S.gap;
        }
        if (def) {
            // The parts of the row in the order the definition gives them. A field that has a socket of the same name (its text mirror) is drawn AS that
            // socket -- the component plugged in, or, when nothing is, the text typed -- so an operand is one thing on the canvas, not two.
            struct Part { bool socket; size_t index; };
            std::vector<Part> parts;
            auto socketNamed = [&](const std::string& n) -> int {
                for (size_t i = 0; i < def->inputs.size(); i++) if (def->inputs[i].name == n) return (int)i;
                return -1;
            };
            auto derivedAway = [&](const std::string& n) {                  // ptr / len while a window is plugged in: the window says it
                for (const InputDef& in : def->inputs) {
                    if (in.derive.compare(0, 6, "range:") != 0 || !b.value(in.name)) continue;
                    const std::string spec = in.derive.substr(6);
                    const size_t comma = spec.find(',');
                    if (n == spec.substr(0, comma) || n == spec.substr(comma + 1)) return true;
                }
                return false;
            };
            for (size_t fi = 0; fi < def->fields.size(); fi++) {
                const FieldDef& f = def->fields[fi];
                if (!asmcore::blocks::partVisible(f.when, b)) continue;
                const int si = socketNamed(f.name);
                if (si >= 0) { if (asmcore::blocks::partVisible(def->inputs[(size_t)si].when, b)) parts.push_back({true, (size_t)si}); continue; }
                if (derivedAway(f.name)) continue;
                parts.push_back({false, fi});
            }
            for (size_t i = 0; i < def->inputs.size(); i++) {
                bool mirrored = false;
                for (const FieldDef& f : def->fields) if (f.name == def->inputs[i].name) mirrored = true;
                if (!mirrored && asmcore::blocks::partVisible(def->inputs[i].when, b)) parts.push_back({true, i});
            }
            auto fieldOf = [&](const std::string& n) -> const FieldDef* { for (const FieldDef& f : def->fields) if (f.name == n) return &f; return nullptr; };
            for (const Part& part : parts) {
                if (!part.socket) {
                    const FieldDef& f = def->fields[part.index];
                    const std::string value = b.get(f.name);
                    if (f.kind == FieldKind::Flag) {
                        if (value != "1" && (!S.showEmptyOptional || info.shape == Shape::Value)) continue;
                    } else if (value.empty() && f.optional && (!S.showEmptyOptional || info.shape == Shape::Value)) continue;
                    FieldBox fb;
                    fb.name = f.name;
                    fb.kind = f.kind;
                    fb.fullText = value;
                    const std::string label = T.fieldLabel(*def, f);
                    if (f.kind == FieldKind::Flag) {
                        fb.text = label;
                        fb.checked = value == "1";
                        fb.label = "";
                        const double cw = w(label, false) + 2 * S.chipPadX + 18;           // the check mark
                        fb.rect = {cx, centerY(chipHeight()), cw, chipHeight()};
                        cx += cw + S.gap;
                    } else {
                        if (S.showFieldLabels && !label.empty()) {
                            fb.label = label;
                            const double lw = w(label, false);
                            fb.labelRect = {cx, centerY(lh), lw, lh};
                            cx += lw + S.gap / 2;
                        }
                        fb.empty = value.empty();
                        const std::string shown = fb.empty ? T.emptyValue() : elide(value, S.maxChipWidth - 2 * S.chipPadX, true, fb.elided);
                        fb.text = shown;
                        const double cw = std::max(w(shown, true) + 2 * S.chipPadX, 22.0);
                        fb.rect = {cx, centerY(chipHeight()), cw, chipHeight()};
                        cx += cw + S.gap;
                    }
                    self().fields.push_back(fb);
                    continue;
                }
                const size_t i = part.index;
                const InputDef& in = def->inputs[i];
                const FieldDef* mirror = fieldOf(in.name);
                const std::string typed = mirror ? b.get(in.name) : std::string();
                if (plugged[i] < 0 && typed.empty() && mirror && mirror->optional && (!S.showEmptyOptional || info.shape == Shape::Value)) continue;   // an optional operand left out
                SocketBox sb;
                sb.name = in.name;
                sb.label = T.inputLabel(*def, in);
                if (sb.label.empty() && mirror) sb.label = T.fieldLabel(*def, *mirror);
                sb.types = in.accepts;
                if (!sb.label.empty() && S.showFieldLabels) {
                    const double lw = w(sb.label, false);
                    sb.labelRect = {cx, centerY(lh), lw, lh};
                    cx += lw + S.gap / 2;
                }
                if (plugged[i] >= 0) {
                    const BlockBox cb = out.blocks[(size_t)plugged[i]];
                    translate(plugged[i], cx - cb.rect.x, centerY(cb.rect.h) - cb.rect.y);
                    sb.rect = out.blocks[(size_t)plugged[i]].rect;
                    sb.child = plugged[i];
                    cx = sb.rect.right() + S.gap;
                } else {
                    // nothing plugged in: the text typed (the mirror) stands in, or an empty dent
                    sb.fullText = typed;
                    sb.empty = typed.empty();
                    sb.text = sb.empty ? std::string() : elide(typed, S.maxChipWidth - 2 * S.chipPadX, true, sb.elided);
                    const double sw = sb.empty ? S.socketMinWidth : std::max(S.socketMinWidth, w(sb.text, true) + 2 * S.chipPadX);
                    sb.rect = {cx, centerY(chipHeight()), sw, chipHeight()};
                    cx += sw + S.gap;
                }
                ConnectionPoint sp;
                sp.kind = ConnectionPoint::Socket; sp.pos = {sb.rect.x, sb.rect.center().y}; sp.blockId = b.id; sp.slot = in.name;
                out.points.push_back(sp);
                self().sockets.push_back(sb);
            }
        }
        if (!b.comment.empty()) {
            bool el = false;
            const std::string shown = elide("; " + b.comment, S.commentMaxWidth, false, el);
            const double cw = w(shown, false);
            self().commentRect = {cx, centerY(lh), cw, lh};
            cx += cw + S.gap;
        }
        double width = std::max(S.minWidth, cx - x - S.gap + S.padX);
        self().head = {x, y, width, rowH};
        double height = rowH;
        if (info.shape == Shape::Value) {
            ConnectionPoint plug;
            plug.kind = ConnectionPoint::Plug; plug.pos = {x, y + rowH / 2}; plug.blockId = b.id;
            out.points.push_back(plug);
        }
        // ---- containers
        if (info.shape == Shape::Container && def) {
            if (b.collapsed) {
                size_t n = 0;
                for (const auto& st : b.stacks) n += countBlocks(st.second);
                self().hiddenBlocks = n;
                FieldBox fb;
                fb.name = ""; fb.kind = FieldKind::Text; fb.empty = false;
                fb.text = fb.fullText = T.collapsedSummary(n);
                const double cw = w(fb.text, true) + 2 * S.chipPadX;        // drawn like every chip: monospace
                fb.rect = {x + width - S.padX + S.gap, y + (rowH - chipHeight()) / 2, cw, chipHeight()};
                // the summary sits at the end of the head row
                width = fb.rect.right() - x + S.padX;
                self().head.w = width;
                self().fields.push_back(fb);
            } else {
                double cy = y + rowH, maxInner = 0;
                for (const std::string& slotName : def->stacks) {
                    SlotBox sb;
                    sb.name = slotName;
                    const std::vector<Block>* content = b.stack(slotName);
                    static const std::vector<Block> none;
                    double iw = 0;
                    const double sx = x + S.slotIndent;
                    ConnectionPoint sp;
                    sp.kind = ConnectionPoint::Slot; sp.pos = {sx, cy}; sp.blockId = b.id; sp.slot = slotName;
                    out.points.push_back(sp);
                    const double h = list(content ? *content : none, sx, cy, depth + 1, idx, slotName, sb.items, iw);
                    const double sh = std::max(h, S.emptySlot);
                    sb.rect = {sx, cy, std::max(iw, S.minWidth), sh};
                    maxInner = std::max(maxInner, iw);
                    cy += sh;
                    self().slotBoxes.push_back(sb);
                }
                height = cy - y + S.containerFoot;
                width = std::max(width, S.slotIndent + maxInner + S.slotPad);
                for (SlotBox& sb : self().slotBoxes) sb.rect.w = width - S.slotIndent - S.slotPad;
            }
        }
        self().rect = {x, y, width, height};
        return idx;
    }
};

StackLayout LayoutEngine::layoutStack(const Section& s, size_t index) const {
    StackLayout out;
    out.section = index;
    out.domain = s.domain;
    out.origin = {s.x, s.y};
    Run run(*this, out);
    double top = 0;
    if (s.domain == "main" || s.domain == "peri" || s.domain.empty()) {
        out.header.kind = HeaderBox::Hat;
        out.header.domain = s.domain;
        out.header.text = text_->hatTitle(s.domain);
        out.header.rect = {0, 0, std::max(style_.hatMinWidth, m_.width(out.header.text, false) + 2 * style_.hatPadX), style_.hatHeight};
        top = style_.hatHeight;
        ConnectionPoint under;
        under.kind = ConnectionPoint::Next; under.pos = {0, top}; under.blockId = ""; under.slot = "";
        out.points.push_back(under);
    } else {
        out.header.kind = HeaderBox::Tag;
        out.header.domain = s.domain;
        out.header.text = text_->floatingTag();
        out.header.rect = {0, 0, m_.width(out.header.text, false) + 2 * style_.hatPadX, style_.tagHeight};
        top = style_.tagHeight;
    }
    std::vector<int> items;
    double width = 0;
    run.list(s.blocks, 0, top, 0, -1, "", items, width);
    out.top = items;
    Rect b = out.header.rect;
    for (const BlockBox& bb : out.blocks) b = unite(b, bb.rect);
    out.bounds = b;
    return out;
}

DocumentLayout LayoutEngine::layoutDocument(const Document& d) const {
    DocumentLayout out;
    for (size_t i = 0; i < d.sections.size(); i++) {
        out.stacks.push_back(layoutStack(d.sections[i], i));
        const StackLayout& s = out.stacks.back();
        out.bounds = unite(out.bounds, s.bounds.translated(s.origin.x, s.origin.y));
    }
    return out;
}

}  // namespace blocks
}  // namespace acide
