#include "blocks/block_layout.h"

#include <algorithm>
#include <cctype>
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

    // The comment of a block as lines (a comment may have several), each with its "; " and shortened to fit. h is the height of all lines.
    void measureComment(const std::string& comment, std::vector<std::string>& lines, double& wMax, double& h) const {
        lines.clear();
        wMax = h = 0;
        if (comment.empty()) return;
        std::string cur;
        std::vector<std::string> raw;
        for (char ch : comment) {
            if (ch == '\r') continue;
            if (ch == '\n') { raw.push_back(cur); cur.clear(); } else cur += ch;
        }
        raw.push_back(cur);
        while (raw.size() > 1 && raw.back().empty()) raw.pop_back();
        const double lh = e.m_.lineHeight(false);
        for (const std::string& r : raw) {
            bool el = false;
            lines.push_back(elide("; " + r, S.commentMaxWidth, false, el));
            wMax = std::max(wMax, w(lines.back(), false));
        }
        h = (double)lines.size() * lh;
    }

    // A component whose row got taller (a long comment on its host) grows to fill it, and what is plugged into it grows with it: every part of the
    // tree that is inside it is made to fit, and kept centred.
    void stretchTo(int idx, double newH) {
        BlockBox& b = out.blocks[(size_t)idx];
        const double oldH = b.rect.h;
        if (newH <= oldH + 0.01) return;
        const double delta = newH - oldH;
        b.rect.h = newH;
        b.head.h = newH;
        const double inner = newH - 2 * S.rowPadY;
        for (FieldBox& f : b.fields) { f.rect.y += delta / 2; f.labelRect.y += delta / 2; }
        b.titleRect.y += delta / 2;
        b.commentRect.y += delta / 2;
        for (ConnectionPoint& p : out.points) if (p.blockId == b.id && (p.kind == ConnectionPoint::Plug || p.kind == ConnectionPoint::Socket)) p.pos.y += delta / 2;
        const double centre = b.rect.y + newH / 2;
        for (SocketBox& sb : b.sockets) {
            sb.labelRect.y += delta / 2;
            if (sb.child < 0) { sb.rect.y += delta / 2; continue; }
            const int ci = sb.child;
            stretchTo(ci, inner);
            const BlockBox& cb = out.blocks[(size_t)ci];
            translate(ci, 0, centre - (cb.rect.y + cb.rect.h / 2));
            sb.rect = out.blocks[(size_t)ci].rect;
        }
    }

    // a cascade runs from its BEGIN to the last END before the next BEGIN (the numbering and the exits are checked by validate)
    void markCascades(const std::vector<int>& items) {
        for (size_t i = 0; i < items.size(); i++) {
            if (out.blocks[(size_t)items[i]].type != "cascade_begin") continue;
            size_t last = i;
            bool any = false;
            for (size_t j = i + 1; j < items.size(); j++) {
                const std::string& t = out.blocks[(size_t)items[j]].type;
                if (t == "cascade_begin") break;
                if (t == "cascade_end") { last = j; any = true; }
            }
            if (!any) continue;
            out.regions.push_back({"cascade", "cascade", unite(out.blocks[(size_t)items[i]].rect, out.blocks[(size_t)items[last]].rect)});
            i = last;
        }
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
        markCascades(items);
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
        if (def) { box.valueType = def->valueType; box.plug = plugShapeOf(def->valueType); box.plugDepth = box.plug == PlugShape::None ? 0 : S.plugDepth; }
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
                std::vector<std::string> cl;
                double cwid = 0, ch = 0;
                measureComment(b.comment, cl, cwid, ch);
                const double rowH = std::max(base * 0.8 + 4, ch + 2 * S.rowPadY);
                FieldBox f;
                f.name = "name"; f.label = ""; f.text = shown; f.fullText = b.get("name"); f.kind = FieldKind::Text; f.elided = el;
                const double fh = std::max(chipHeight(), base * 0.8);
                f.rect = {x + 4, y + (rowH - fh) / 2, cw, fh};
                self().fields.push_back(f);
                double right = x + 4 + cw + 4;
                self().commentLines = cl;
                if (!cl.empty()) {
                    self().commentRect = {right + S.gap, y + (rowH - ch) / 2, cwid, ch};
                    right = self().commentRect.right() + S.padX;
                    self().tailRect = {self().commentRect.x - 4, y, cwid + 4 + S.padX, rowH};
                } else {
                    self().tailRect = {right, y, S.tailStub, rowH};
                    right += S.tailStub;
                }
                self().rect = {x, y, right - x, rowH};
                self().head = self().rect;
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
                std::vector<std::string> cl;
                double cwid = 0, ch = 0;
                if (mono) measureComment(b.comment, cl, cwid, ch);                          // assembler text carries a comment; a comment block does not
                const double rowH = std::max(std::max(S.minRow * 0.8, (mono ? e.m_.lineHeight(true) : lh) + 2 * S.rowPadY), ch + 2 * S.rowPadY);
                const double fh = (mono ? e.m_.lineHeight(true) : lh);
                f.rect = {x + S.padX, y + (rowH - fh) / 2, tw, fh};
                self().fields.push_back(f);
                double right = x + S.padX + tw;
                self().commentLines = cl;
                if (mono) {
                    if (!cl.empty()) {
                        self().commentRect = {right + S.gap * 2, y + (rowH - ch) / 2, cwid, ch};
                        self().tailRect = {self().commentRect.x - 4, y, cwid + 4 + S.padX, rowH};
                        right = self().commentRect.right();
                    } else {
                        self().tailRect = {right + S.padX, y, S.tailStub, rowH};
                        right += S.padX + S.tailStub;
                    }
                }
                self().rect = {x, y, std::max(S.minWidth, right + S.padX - x), rowH};
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
        // a socket shows if its condition holds; the parts of a composite text ("dev=.., addr=..") show only while the text is split into them (or empty)
        auto inputVisible = [&](const InputDef& in) {
            if (!asmcore::blocks::partVisible(in.when, b)) return false;
            if (in.derive.compare(0, 4, "arg:") == 0) {
                bool any = false;
                for (const InputDef& o : def->inputs) if (o.derive == in.derive && b.value(o.name)) any = true;
                std::string text = b.get(in.derive.substr(4));
                while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
                if (!any && !text.empty()) return false;
            }
            return true;
        };
        if (def) {
            for (const InputDef& in : def->inputs) {
                const Block* child = inputVisible(in) ? b.value(in.name) : nullptr;
                if (!child) { plugged.push_back(-1); continue; }
                const int ci = block(*child, 0, 0, depth + 1, idx, in.name);
                plugged.push_back(ci);
                maxChildH = std::max(maxChildH, out.blocks[(size_t)ci].rect.h);
            }
        }
        std::vector<std::string> commentLines;
        double commentW = 0, commentH = 0;
        if (info.shape != Shape::Value) measureComment(b.comment, commentLines, commentW, commentH);       // (a component carries no comment of its own)
        const double rowH = std::max(std::max(base, maxChildH + 2 * S.rowPadY), commentH + 2 * S.rowPadY);
        // a row made taller by a long comment: what is plugged into it grows to fill it, with everything inside that
        for (int ci : plugged) if (ci >= 0) stretchTo(ci, rowH - 2 * S.rowPadY);
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
            auto derivedAway = [&](const std::string& n) {                  // ptr / len while a window is plugged in: the window says it; "args" while it is split
                for (const InputDef& in : def->inputs) {
                    if (in.derive.compare(0, 4, "arg:") == 0 && in.derive.substr(4) == n && b.value(in.name)) return true;
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
                if (si >= 0) { if (inputVisible(def->inputs[(size_t)si])) parts.push_back({true, (size_t)si}); continue; }
                if (derivedAway(f.name)) continue;
                parts.push_back({false, fi});
            }
            for (size_t i = 0; i < def->inputs.size(); i++) {
                bool mirrored = false;
                for (const FieldDef& f : def->fields) if (f.name == def->inputs[i].name) mirrored = true;
                if (!mirrored && inputVisible(def->inputs[i])) parts.push_back({true, i});
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
                sb.plug = plugShapeOfSocket(in.accepts);
                sb.plugDepth = sb.plug == PlugShape::None ? 0 : S.plugDepth;
                if (!sb.label.empty() && S.showFieldLabels) {
                    const double lw = w(sb.label, false);
                    sb.labelRect = {cx, centerY(lh), lw, lh};
                    cx += lw + S.gap / 2;
                }
                cx += sb.plugDepth;                                                  // room for the tab of what plugs in (or the notch of the dent)
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
        self().commentLines = commentLines;
        double tailX = cx;
        if (!commentLines.empty()) {
            self().commentRect = {cx, centerY(commentH), commentW, commentH};
            cx += commentW + S.gap;
        } else if (info.shape != Shape::Value) {
            cx += S.tailStub;                                                       // the zone where a double click starts a comment
        }
        double width = std::max(S.minWidth, cx - x - S.gap + S.padX);
        self().head = {x, y, width, rowH};
        if (info.shape != Shape::Value) self().tailRect = commentLines.empty() ? Rect{tailX - 2, y, x + width - tailX + 2, rowH} : Rect{tailX - 4, y, commentW + 4 + S.padX, rowH};
        double height = rowH;
        if (info.shape == Shape::Value) {
            ConnectionPoint plug;
            plug.kind = ConnectionPoint::Plug; plug.pos = {x - self().plugDepth, y + rowH / 2}; plug.blockId = b.id;
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
        out.header.comment = s.comment;
        double hw = std::max(style_.hatMinWidth, m_.width(out.header.text, false) + 2 * style_.hatPadX), hh = style_.hatHeight;
        {
            double cwid = 0, ch = 0;
            run.measureComment(s.comment, out.header.commentLines, cwid, ch);
            hh = std::max(hh, ch + 2 * style_.rowPadY + 6);
            const double tx = m_.width(out.header.text, false) + 2 * style_.hatPadX;
            if (!out.header.commentLines.empty()) {
                out.header.commentRect = {std::max(hw, tx) + style_.gap, (hh - ch) / 2, cwid, ch};
                hw = out.header.commentRect.right() + style_.padX;
                out.header.tailRect = {out.header.commentRect.x - 4, 0, hw - out.header.commentRect.x + 4, hh};
            } else {
                out.header.tailRect = {hw, 0, style_.tailStub, hh};
                hw += style_.tailStub;
            }
        }
        out.header.rect = {0, 0, hw, hh};
        top = hh;
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
    for (const RegionBox& r : out.regions) b = unite(b, Rect{r.rect.x - 6, r.rect.y - 14, r.rect.w + 12, r.rect.h + 20});
    for (const BlockBox& bb : out.blocks) {
        Rect r = bb.rect;
        if (bb.parent < 0 && bb.plugDepth > 0) { r.x -= bb.plugDepth; r.w += bb.plugDepth; }      // a component standing alone: its tab is part of it
        b = unite(b, r);
    }
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
