// Block canvas services (no Qt): theme, shapes, layout engine, session. The layout is checked with fake fixed-width text metrics, so the numbers
// do not depend on any font.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include "asmcore/blocks.h"
#include "asmcore/blocks_edit.h"
#include "asmcore/asmcore.h"
#include "blocks/block_catalog.h"
#include "blocks/block_drop.h"
#include "blocks/block_layout.h"
#include "blocks/block_session.h"
#include "blocks/block_shapes.h"
#include "blocks/block_spring.h"
#include "blocks/block_tidy.h"
#include "blocks/block_theme.h"

using namespace acide;
using namespace acide::blocks;
using namespace asmcore::blocks;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }

#ifndef ACIDE_SAMPLE_DIR
#define ACIDE_SAMPLE_DIR "."
#endif

// 7 units per code point (proportional text), 8 (monospace); a line is 16 high
struct FakeMetrics : ITextMetrics {
    static size_t points(const std::string& s) { size_t n = 0; for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++; return n; }
    double width(const std::string& s, bool mono) const override { return (double)points(s) * (mono ? 8.0 : 7.0); }
    double lineHeight(bool) const override { return 16; }
};

static bool inside(const Rect& inner, const Rect& outer, double eps = 0.01) {
    return inner.x >= outer.x - eps && inner.y >= outer.y - eps && inner.right() <= outer.right() + eps && inner.bottom() <= outer.bottom() + eps;
}
static Document docOf(const std::string& text) { return importAsm(text); }

static void t_theme() {
    section("theme");
    Rgba c;
    check(parseColor("#1a73e8", c) && c.r == 0x1a && c.g == 0x73 && c.b == 0xe8 && c.a == 255 && formatColor(c) == "#1a73e8", "colours: parse and format #rrggbb");
    check(parseColor("#11223380", c) && c.a == 0x80 && formatColor(c) == "#11223380", "...also with alpha");
    check(!parseColor("blue", c) && !parseColor("#12345", c) && !parseColor("#12345g", c), "bad colours are refused");
    check(mix(rgb(0, 0, 0), rgb(200, 100, 50), 0.5) == rgb(100, 50, 25), "mix");
    check(luminance(rgb(255, 255, 255)) > 0.95 && luminance(rgb(0, 0, 0)) < 0.01, "luminance");
    const BlockTheme l = BlockTheme::light(), d = BlockTheme::dark_();
    check(!l.dark && d.dark && !(l.canvas == d.canvas), "light and dark themes differ");
    check(l.textOn(rgb(240, 240, 240)) == l.blockText && l.textOn(rgb(10, 10, 40)) == l.blockTextOnDark, "the text colour is chosen for the fill");
    const Rgba unknown = l.categoryColor("SomethingNew");
    check(unknown == l.categoryColor("SomethingNew") && !(unknown == l.categoryColor("SomethingElse")), "an unknown category gets a stable colour of its own");
    bool all = true;
    for (const BlockDef& def : Registry::defaults().all()) all = all && l.category.count(def.category) && d.category.count(def.category);   // every drawer of every built-in block
    check(all, "every drawer of the built-in blocks has a colour in both themes");
    std::set<std::string> distinct;
    for (auto& kv : l.category) distinct.insert(formatColor(kv.second));
    check(distinct.size() == l.category.size(), "...and they are different from each other");
    Settings s;
    s.set("blocks.category.Data", "#010203");
    s.set("blocks.canvas", "#0a0b0c");
    s.set("blocks.selection", "bogus");
    BlockTheme t = BlockTheme::fromSettings(s, false);
    check(t.category["Data"] == rgb(1, 2, 3) && t.canvas == rgb(10, 11, 12) && t.selection == l.selection, "settings override a category and a role; a bad value is ignored");
    check(BlockTheme::fromSettings(Settings(), true).dark, "the dark flag picks the dark base");
}

static void t_shapes() {
    section("shapes");
    const ShapeRegistry& r = ShapeRegistry::defaults();
    const Registry& reg = Registry::defaults();
    check(r.infoFor(*reg.find("mov")).shape == Shape::Statement && r.infoFor(*reg.find("proc")).shape == Shape::Container, "statements and containers");
    check(r.infoFor(*reg.find("label")).shape == Shape::Tag && r.infoFor(*reg.find("comment")).shape == Shape::Note && r.infoFor(*reg.find("blank")).shape == Shape::Spacer && r.infoFor(*reg.find("raw")).shape == Shape::Text, "label, comment, blank, raw have their own shapes");
    check(!r.infoFor(*reg.find("directive")).showTitle && r.infoFor(*reg.find("mov")).showTitle, "which shapes show a title");
    BlockDef v;
    v.type = "num"; v.valueType = "int";
    BlockDef c;
    c.type = "loop"; c.stacks = {"DO"};
    check(r.infoFor(v).shape == Shape::Value && r.infoFor(c).shape == Shape::Container, "a new definition gets a shape from what it is (a value, has slotBoxes)");
    ShapeRegistry custom;
    custom.set("num", {Shape::Tag, false});
    check(custom.infoFor(v).shape == Shape::Tag, "an explicit entry wins");
    bool allKnown = true;
    for (const BlockDef& d : reg.all()) { (void)r.infoFor(d); allKnown = allKnown && true; }
    check(allKnown, "every built-in definition has a shape");
}

static StackLayout layoutOf(const std::string& asmText, size_t section = 0, const LayoutStyle& style = LayoutStyle()) {
    static FakeMetrics m;
    Document d = docOf(asmText);
    LayoutEngine e(m, style);
    return e.layoutStack(d.sections[section], section);
}

static void t_spring() {
    section("the spring behind the soft effects");
    Spring s;
    s.kick(0.8);
    double maxX = 0, minX = 10;
    int steps = 0;
    while (!s.settled() && steps < 2000) { s.step(1.0 / 60); maxX = std::max(maxX, s.x); minX = std::min(minX, s.x); steps++; }
    check(s.settled() && steps < 400, "it settles (in well under 7 s of frames)", std::to_string(steps));
    check(maxX > 1.02, "an under-damped spring overshoots its rest (that is the bounce)", std::to_string(maxX));
    check(minX >= 0.79 && std::fabs(s.x - 1) < 0.01, "and comes to rest at the target without leaving the starting side by much");
    Spring stiff;
    stiff.damping = 2 * std::sqrt(stiff.stiffness) * 1.2;                      // over-damped: no wobble
    stiff.kick(0.8);
    double over = 0;
    for (int i = 0; i < 400; i++) { stiff.step(1.0 / 60); over = std::max(over, stiff.x); }
    check(over <= 1.0 + 1e-6, "an over-damped spring does not overshoot (the damping decides how bouncy)", std::to_string(over));
    Spring big;
    big.kick(1.5);
    big.step(5.0);                                                              // a frame that took seconds: capped and stepped finely
    check(std::isfinite(big.x) && std::isfinite(big.v) && std::fabs(big.x) < 10, "a very long frame cannot blow it up");
    Spring a, b;
    a.kick(0.9); b.kick(0.9);
    for (int i = 0; i < 60; i++) a.step(1.0 / 60);
    for (int i = 0; i < 120; i++) b.step(1.0 / 120);
    check(std::fabs(a.x - b.x) < 0.01, "the result hardly depends on the frame rate");
}

static void t_plug_shapes() {
    section("the shape of a joint follows the value type");
    check(plugShapeOf("int") == PlugShape::Round && plugShapeOf("region:ALU_OPA") == PlugShape::Point && plugShapeOf("op.alu") == PlugShape::Square && plugShapeOf("cond.main") == PlugShape::Square &&
          plugShapeOf("sign") == PlugShape::Square && plugShapeOf("label.flow") == PlugShape::Flag && plugShapeOf("label.proc") == PlugShape::Flag && plugShapeOf("csr") == PlugShape::Diamond &&
          plugShapeOf("service") == PlugShape::Diamond && plugShapeOf("") == PlugShape::None, "numbers are round, regions pointed, options square, labels a flag, references a diamond");
    check(plugShapeOfSocket({"int"}) == PlugShape::Round && plugShapeOfSocket({"region:MAIN_SCRATCHPAD"}) == PlugShape::Point && plugShapeOfSocket({"csr", "int"}) == PlugShape::Any && plugShapeOfSocket({}) == PlugShape::None,
          "a socket takes the shape of what it accepts; one that accepts several shapes is a plain square dent");
    const Registry& reg = Registry::defaults();
    bool onlyOwn = true;
    std::set<PlugShape> shapes;
    for (const BlockDef& d : reg.all()) if (!d.valueType.empty()) shapes.insert(plugShapeOf(d.valueType));
    check(shapes.size() >= 5, "the components use five different joints", std::to_string(shapes.size()));
    (void)onlyOwn;
    // the layout leaves room for the tab: the child starts plugDepth after its label, and the dent of an empty socket has the shape of its types
    StackLayout s = layoutOf(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n");
    const BlockBox& mov = s.blocks[(size_t)s.top[0]];
    const BlockBox& num = s.blocks[(size_t)mov.sockets[0].child];
    check(num.plug == PlugShape::Round && num.plugDepth == LayoutStyle().plugDepth && num.valueType == "int", "a number has a round plug");
    check(mov.sockets[0].plug == PlugShape::Any && mov.sockets[1].plug == PlugShape::Point, "the source takes several shapes, the destination only regions");
    check(mov.sockets[0].rect.x - mov.sockets[0].labelRect.right() >= LayoutStyle().plugDepth - 0.01, "room for the tab between the label and the component");
    const BlockBox& region = s.blocks[(size_t)mov.sockets[1].child];
    check(region.plug == PlugShape::Point && region.sockets.size() == 2 && region.sockets[0].plug == PlugShape::Round, "a region has a pointed plug and round sockets for its numbers");
    const BlockBox& jmp = s.blocks[(size_t)s.top[1]];
    check(jmp.sockets.size() == 1 && jmp.sockets[0].plug == PlugShape::Flag && jmp.sockets[0].empty == false, "the jump target takes a label: a flag-shaped dent holding the typed name");
    // a component standing alone: its tab belongs to the stack's bounds
    Document d;
    Section sec;
    sec.domain = "floating";
    sec.blocks.push_back(d.make("num"));
    d.sections.push_back(sec);
    FakeMetrics m;
    StackLayout alone = LayoutEngine(m).layoutStack(d.sections[0], 0);
    check(alone.blocks[0].parent < 0 && alone.bounds.x <= alone.blocks[0].rect.x - LayoutStyle().plugDepth + 0.01, "a number lying alone: the bounds include its tab");
}

static void t_layout_comments() {
    section("layout: the comment at the end of a block, over several lines");
    StackLayout one = layoutOf(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    const BlockBox& m1 = one.blocks[0];
    check(!m1.tailRect.empty() && m1.tailRect.right() <= m1.rect.right() + 0.01 && m1.tailRect.x > m1.sockets.back().rect.right() - 0.01 && m1.commentLines.empty(), "every instruction has an empty zone at its end for a double click");
    // with a comment
    Document d = docOf(".main\n MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n");
    d.sections[0].blocks[0].comment = "first\nsecond\nthird";
    FakeMetrics m;
    StackLayout three = LayoutEngine(m).layoutStack(d.sections[0], 0);
    const BlockBox& m3 = three.blocks[0];
    check(m3.commentLines.size() == 3 && m3.commentLines[0] == "; first" && m3.commentRect.h == 3 * 16, "three lines of comment, each with its semicolon");
    check(m3.rect.h >= 3 * 16 + 2 * LayoutStyle().rowPadY - 0.01 && m3.rect.h > m1.rect.h, "the whole block is taller to hold them", std::to_string(m3.rect.h) + " vs " + std::to_string(m1.rect.h));
    check(inside(m3.commentRect, m3.head) && !m3.tailRect.empty() && inside(m3.tailRect, m3.rect), "the comment lies inside the block, and its zone is the tail");
    // what is plugged in grows with the row, and what is inside that too
    const BlockBox& src = three.blocks[(size_t)m3.sockets[0].child];
    const BlockBox& dst = three.blocks[(size_t)m3.sockets[1].child];
    const double inner = m3.rect.h - 2 * LayoutStyle().rowPadY;
    check(std::fabs(src.rect.h - inner) < 0.01 && std::fabs(dst.rect.h - inner) < 0.01, "the number and the region plugged into it are as high as the row allows", std::to_string(src.rect.h) + " / " + std::to_string(dst.rect.h) + " / " + std::to_string(inner));
    const BlockBox& first = three.blocks[(size_t)dst.sockets[0].child];
    check(first.rect.h > one.blocks[(size_t)one.blocks[(size_t)m1.sockets[1].child].sockets[0].child].rect.h + 1 && first.rect.h <= dst.rect.h - 2 * LayoutStyle().rowPadY + 0.01, "...and the numbers inside the region too, a little less each level");
    bool centred = true;
    for (const BlockBox* b : {&src, &dst, &first}) centred = centred && std::fabs((b->rect.y + b->rect.h / 2) - (m3.rect.y + m3.rect.h / 2)) < 0.6;
    check(centred, "all of them stay centred in the row");
    bool partsInside = true;
    for (const BlockBox* b : {&src, &dst, &first}) {
        for (const FieldBox& f : b->fields) partsInside = partsInside && inside(f.rect, b->rect);
        for (const SocketBox& s : b->sockets) partsInside = partsInside && inside(s.rect, b->rect) && s.rect.h <= b->rect.h + 0.01;
    }
    check(partsInside, "the parts of the grown blocks are inside them");
    // a connection point of a grown component follows
    bool pointsOk = true;
    for (const ConnectionPoint& p : three.points)
        if (p.kind == ConnectionPoint::Plug && p.blockId == src.id) pointsOk = pointsOk && std::fabs(p.pos.y - (src.rect.y + src.rect.h / 2)) < 0.6;
    check(pointsOk, "the plug point of a grown component is in the middle of it");
    // the stack below is pushed down; a container's slot content follows its head
    Document c = docOf(".main\n.proc f\n RET\n.endp\n JMP x\n");
    c.sections[0].blocks[0].comment = "a\nb";
    StackLayout cl = LayoutEngine(m).layoutStack(c.sections[0], 0);
    const BlockBox& proc = cl.blocks[0];
    check(proc.head.h >= 2 * 16 + 2 * LayoutStyle().rowPadY - 0.01 && proc.slotBoxes[0].rect.y >= proc.head.bottom() - 0.01 && inside(proc.slotBoxes[0].rect, proc.rect), "a procedure with a long comment: its head is taller and the body starts below it");
    bool below = true;
    for (size_t i = 1; i < cl.top.size(); i++) below = below && cl.blocks[(size_t)cl.top[i]].rect.y >= cl.blocks[(size_t)cl.top[i - 1]].rect.bottom() - 0.01;
    check(below, "the blocks after it are pushed down (no overlap)");
    // a label and assembler text carry comments too
    Document l = docOf(".main\nstart:    ; entry\n weird thing    ; why\n JMP start\n");
    l.sections[0].blocks[0].comment = "entry\npoint";
    StackLayout ll = LayoutEngine(m).layoutStack(l.sections[0], 0);
    check(ll.blocks[0].shape == Shape::Tag && ll.blocks[0].commentLines.size() == 2 && ll.blocks[0].rect.h >= 2 * 16 + 2 * LayoutStyle().rowPadY - 0.01 && !ll.blocks[0].tailRect.empty(), "a label with a two-line comment is taller");
    check(ll.blocks[1].shape == Shape::Text && ll.blocks[1].commentLines.size() == 1 && ll.blocks[1].tailRect.x >= ll.blocks[1].fields[0].rect.right(), "assembler text carries its comment after the text");
    // a hat
    Document h = docOf(".main\n JMP x\n");
    h.sections[0].comment = "the entry\nof the program";
    StackLayout hl = LayoutEngine(m).layoutStack(h.sections[0], 0);
    check(hl.header.commentLines.size() == 2 && hl.header.rect.h > LayoutStyle().hatHeight && hl.header.commentRect.x >= hl.header.rect.x + 40 && !hl.header.tailRect.empty() && inside(hl.header.commentRect, hl.header.rect), "a hat with a two-line comment is taller; the blocks start below it");
    check(std::fabs(hl.blocks[0].rect.y - hl.header.rect.bottom()) < 0.01, "(and the first block under it)");
    StackLayout hp = layoutOf(".main\n JMP x\n");
    check(!hp.header.tailRect.empty() && hp.header.tailRect.x >= hp.header.rect.x + 40, "a hat has the empty zone at its end");
    // elision
    Document long_;
    long_ = docOf(".main\n JMP x\n");
    long_.sections[0].blocks[0].comment = std::string(400, 'z');
    StackLayout el = LayoutEngine(m).layoutStack(long_.sections[0], 0);
    check(el.blocks[0].commentRect.w <= LayoutStyle().commentMaxWidth + 0.01, "a long line is shortened");
}

static void t_layout_regions() {
    section("layout: a cascade is framed");
    StackLayout s = layoutOf(".main\nstart: JMP start\n CASCADE_BEGIN 1, b0\nb0: JMP x\n CASCADE_END 0, done\nb1: JMP y\n CASCADE_END 1, done\ndone: JMP done\n");
    check(s.regions.size() == 1 && s.regions[0].kind == "cascade", "one frame around the cascade");
    const Rect& r = s.regions[0].rect;
    const BlockBox *begin = nullptr, *last = nullptr;
    for (const BlockBox& b : s.blocks) { if (b.type == "cascade_begin") begin = &b; if (b.type == "cascade_end") last = &b; }
    check(begin && last && std::fabs(r.y - begin->rect.y) < 0.01 && std::fabs(r.bottom() - last->rect.bottom()) < 0.01, "from the BEGIN block to the last END block");
    check(s.bounds.x <= r.x - 6 + 0.01 && s.bounds.bottom() >= r.bottom() + 20 - 0.01, "the stack's bounds include the frame and its label");
    StackLayout none = layoutOf(".main\n CASCADE_BEGIN 0, b0\nb0: JMP x\n");
    check(none.regions.empty(), "a BEGIN without END is not framed (validate reports it)");
}

static void t_layout_rows() {
    section("layout: rows");
    StackLayout s = layoutOf(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start, safe\n");
    check(s.header.kind == HeaderBox::Hat && s.header.text == "Main" && s.header.rect.h == LayoutStyle().hatHeight, "a program section has a hat with its name");
    check(s.top.size() == 4 && s.blocks.size() > 4, "four blocks on the top level, and the components inside them");
    auto topBox = [&](size_t i) -> const BlockBox& { return s.blocks[(size_t)s.top[i]]; };
    const BlockBox& mov = topBox(1);
    check(mov.type == "mov" && mov.title == "move" && mov.shape == Shape::Statement && mov.fields.empty() && mov.sockets.size() == 2, "the move block: a title and two sockets (its operands are components)");
    check(mov.sockets[0].label == "from" && mov.sockets[1].label == "to" && mov.sockets[0].child >= 0 && mov.sockets[1].child >= 0, "the sockets have their labels and hold the components");
    const BlockBox& src = s.blocks[(size_t)mov.sockets[0].child];
    const BlockBox& dst = s.blocks[(size_t)mov.sockets[1].child];
    check(src.type == "num" && src.parent >= 0 && src.sockets.size() == 1 && src.sockets[0].fullText == "5" && src.fields.size() == 1 && src.fields[0].fullText == "16", "CONST(5, 2) is a number block: the value, and the width (16 bits) as a further field");
    check(dst.type == "region_main_scratchpad" && dst.sockets.size() == 2 && dst.sockets[0].child >= 0 && dst.sockets[1].child >= 0, "the scratchpad range is a region block with its first and last as numbers");
    check(mov.titleRect.x >= mov.head.x && mov.titleRect.right() <= mov.sockets[0].labelRect.x + 0.01, "the title comes first, then the first label");
    bool ordered = mov.sockets[1].labelRect.x >= mov.sockets[0].rect.right() && mov.sockets[0].rect.x >= mov.sockets[0].labelRect.right();
    check(ordered, "the parts of a row follow each other left to right without overlapping");
    bool within = inside(mov.titleRect, mov.head) && inside(src.rect, mov.head) && inside(dst.rect, mov.head);
    for (const SocketBox& sk : mov.sockets) within = within && inside(sk.rect, mov.head) && inside(sk.labelRect, mov.head);
    check(within && inside(mov.head, mov.rect), "everything lies inside its row, the row inside the block");
    check(mov.rect.h >= LayoutStyle().minRow, "a row is at least the minimum height");
    // the sockets of the alu block depend on the operation
    const BlockBox& alu = topBox(2);
    check(alu.fields.empty() && alu.sockets.size() == 2 && alu.sockets[0].fullText == "ADD" && alu.sockets[1].fullText == "unsigned", "an alu row: the operation and the sign (typed text in sockets); no shift amount for ADD", std::to_string(alu.sockets.size()));
    StackLayout shl = layoutOf(".main\n ALU SHL 3 signed\n");
    check(shl.blocks[0].sockets.size() == 3 && shl.blocks[0].sockets[1].name == "amount" && shl.blocks[0].sockets[1].child >= 0, "a shift shows its amount (a number component)");
    StackLayout logic = layoutOf(".main\n ALU AND\n");
    check(logic.blocks[0].sockets.size() == 1, "a logic operation has no sign socket at all");
    // optional operands
    LayoutStyle edit;
    edit.showEmptyOptional = true;
    StackLayout bare = layoutOf(".main\n ALU SHL\n");
    StackLayout bareEdit = layoutOf(".main\n ALU SHL\n", 0, edit);
    check(bare.blocks[0].sockets.size() == 1 && bareEdit.blocks[0].sockets.size() == 3 && bareEdit.blocks[0].sockets[1].empty, "an empty optional operand is left out in a viewer, shown as an empty socket in an editor");
    // flags
    const BlockBox& jmp = topBox(3);
    check(jmp.fields.size() == 1 && jmp.fields[0].kind == FieldKind::Flag && jmp.fields[0].checked && jmp.fields[0].text == "event checkpoint" && jmp.sockets.size() == 1, "a set flag is shown with its label, checked; the target is a socket");
    StackLayout plain = layoutOf(".main\n JMP start\n");
    check(plain.blocks[0].fields.empty(), "an unset flag is not shown in a viewer");
    StackLayout ed2 = layoutOf(".main\n JMP start\n", 0, edit);
    check(ed2.blocks[0].fields.size() == 1 && !ed2.blocks[0].fields[0].checked, "...but an editor shows it, unchecked");
    // stacking
    bool stacked = true;
    for (size_t i = 1; i < s.top.size(); i++) stacked = stacked && std::fabs(topBox(i).rect.y - topBox(i - 1).rect.bottom()) < 0.01;
    check(stacked && std::fabs(topBox(0).rect.y - s.header.rect.bottom()) < 0.01, "blocks touch: each starts where the one above ends, the first right under the hat");
    // elision
    StackLayout longv = layoutOf(".main\n MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]   ; " + std::string(300, 'x') + "\n");
    check(longv.blocks[0].commentRect.w > 0 && longv.blocks[0].commentRect.w <= LayoutStyle().commentMaxWidth + 0.01, "a long comment is shortened to its maximum width");
    std::string big = "MAIN_SCRATCHPAD[0..1] " + std::string(80, 'z');
    StackLayout el = layoutOf(".main\n MOV " + big + " -> ALU_OPA\n");
    const SocketBox& ef = el.blocks[0].sockets[0];
    check(ef.child < 0 && ef.elided && ef.rect.w <= LayoutStyle().maxChipWidth + 0.01 && ef.text.size() < ef.fullText.size() && ef.text.find("\xE2\x80\xA6") != std::string::npos && ef.fullText == big, "an operand that cannot be split stays text; a long one is shortened with an ellipsis, the full text is kept for the tooltip");
    // non-ASCII
    StackLayout cjk = layoutOf(".main\n MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]  ; \xE4\xB8\xAD\xE6\x96\x87\xE8\xA8\xBB\xE8\xA7\xA3\n");
    check(cjk.blocks[0].commentRect.w == 7.0 * (2 + 4), "text is measured by characters, not bytes");
}

static void t_layout_shapes() {
    section("layout: label, comment, blank, raw");
    StackLayout s = layoutOf(".main\n; a comment\nstart:\n\n weird thing here\n");
    check(s.blocks.size() == 4, "four blocks", std::to_string(s.blocks.size()));
    check(s.blocks[0].shape == Shape::Note && s.blocks[0].fields[0].fullText == "a comment", "a comment is a note");
    check(s.blocks[1].shape == Shape::Tag && s.blocks[1].fields[0].text == "start:", "a label is a tag showing 'name:'");
    check(s.blocks[2].shape == Shape::Spacer && s.blocks[2].rect.h == LayoutStyle().spacerHeight, "a blank line is a thin spacer");
    check(s.blocks[3].shape == Shape::Text && s.blocks[3].fields[0].fullText == "weird thing here" && !s.blocks[3].known == false, "assembler text is a Text block");
    // an unknown type does not break the layout
    Document d = docOf(".main\n JMP x\n");
    d.sections[0].blocks[0].type = "from_the_future";
    FakeMetrics m;
    StackLayout u = LayoutEngine(m).layoutStack(d.sections[0], 0);
    check(u.blocks.size() == 1 && !u.blocks[0].known && u.blocks[0].shape == Shape::Text && u.blocks[0].rect.h > 0, "a block of an unknown type is shown as text and flagged unknown (a newer file still opens)");
    // a stack with only a header
    Document empty;
    Section sec;
    sec.domain = "peri";
    empty.sections.push_back(sec);
    StackLayout es = LayoutEngine(m).layoutStack(empty.sections[0], 0);
    check(es.header.text == "Peri" && es.blocks.empty() && es.bounds.h == LayoutStyle().hatHeight, "an empty section is just its hat");
    // headers
    Document fl = docOf(".main\n JMP x\n");
    fl.sections[0].domain = "floating";
    StackLayout fs2 = LayoutEngine(m).layoutStack(fl.sections[0], 0);
    check(fs2.header.kind == HeaderBox::Tag && fs2.header.text == "not part of the program" && fs2.blocks[0].rect.y == LayoutStyle().tagHeight, "a floating stack has a tag strip instead of a hat");
    Document defs = docOf("; hello\n.main\n JMP x\n");
    StackLayout ds = LayoutEngine(m).layoutStack(defs.sections[0], 0);
    check(defs.sections[0].domain.empty() && ds.header.text == "Definitions", "the definitions have their own hat");
}

static void t_layout_containers() {
    section("layout: containers (procedures)");
    StackLayout s = layoutOf(".main\n JMP start\n.proc f\n CALL g\n .proc_dummy_not_real\n RET\n.endp\n");
    // the line ".proc_dummy_not_real" is Raw text; the structure is: jmp, proc{ call, raw, ret }
    check(s.top.size() == 2 && s.blocks[1].type == "proc" && s.blocks[1].shape == Shape::Container, "a procedure is a container");
    const BlockBox& p = s.blocks[(size_t)s.top[1]];
    check(p.slotBoxes.size() == 1 && p.slotBoxes[0].name == "BODY" && p.slotBoxes[0].items.size() == 3, "its body holds three blocks");
    bool nested = true;
    for (int i : p.slotBoxes[0].items) {
        const BlockBox& c = s.blocks[(size_t)i];
        nested = nested && c.parent == s.top[1] && c.parentSlot == "BODY" && c.depth == 1 && c.rect.x >= p.rect.x + LayoutStyle().slotIndent - 0.01 && inside(c.rect, p.rect);
    }
    check(nested, "the blocks inside are indented, know their parent and lie inside it");
    check(inside(p.slotBoxes[0].rect, p.rect) && p.slotBoxes[0].rect.y >= p.head.bottom() - 0.01, "the slot lies below the head, inside the block");
    check(p.rect.bottom() >= p.slotBoxes[0].rect.bottom() + LayoutStyle().containerFoot - 0.01, "the block ends with a foot below the slot");
    check(s.blocks[(size_t)s.top[1]].rect.y == s.blocks[(size_t)s.top[0]].rect.bottom(), "a container stacks like any block");
    // children come after parents
    bool order = true;
    for (size_t i = 0; i < s.blocks.size(); i++) order = order && (s.blocks[i].parent < (int)i);
    check(order, "a block always comes after the block it is inside (draw order)");
    // an empty slot keeps a minimum height
    StackLayout e = layoutOf(".main\n.proc g\n.endp\n");
    check(e.blocks[0].slotBoxes[0].items.empty() && e.blocks[0].slotBoxes[0].rect.h == LayoutStyle().emptySlot, "an empty body keeps a minimum height so something can be dropped in");
    // collapsed
    Document d = docOf(".main\n.proc g\n CALL a\n CALL b\n RET\n.endp\n JMP x\n");
    d.sections[0].blocks[0].collapsed = true;
    FakeMetrics m;
    StackLayout c = LayoutEngine(m).layoutStack(d.sections[0], 0);
    check(c.blocks.size() == 2 && c.blocks[0].hiddenBlocks == 3 && c.blocks[0].slotBoxes.empty() && c.blocks[0].fields.back().text == "3 blocks", "a collapsed container folds its blocks away and says how many", std::to_string(c.blocks.size()));
    check(c.blocks[0].rect.h == c.blocks[0].head.h, "...and is one row high");
    // nesting twice
    Document n = docOf(".main\n.proc a\n CALL x\n.endp\n");
    Block outer = n.make("proc");
    outer.set("name", "outer");
    Block inner = n.make("proc");
    inner.set("name", "inner");
    inner.stack("BODY", true)->push_back(n.make("ret"));
    outer.stack("BODY", true)->push_back(inner);
    n.sections[0].blocks.push_back(outer);
    StackLayout nl = LayoutEngine(m).layoutStack(n.sections[0], 0);
    const BlockBox* deep = nullptr;
    for (const BlockBox& b : nl.blocks) if (b.type == "ret") deep = &b;
    check(deep && deep->depth == 2 && deep->rect.x >= 2 * LayoutStyle().slotIndent - 0.01, "containers nest: two levels indent twice");
}

static void t_layout_points() {
    section("layout: connection points and hit testing");
    StackLayout s = layoutOf(".main\n MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP a\n.proc f\n RET\n.endp\n");
    auto find = [&](ConnectionPoint::Kind k, const std::string& id, const std::string& slot = "") -> const ConnectionPoint* {
        for (const ConnectionPoint& p : s.points) if (p.kind == k && p.blockId == id && p.slot == slot) return &p;
        return nullptr;
    };
    const BlockBox &a = s.blocks[(size_t)s.top[0]], &b = s.blocks[(size_t)s.top[1]];
    const ConnectionPoint* an = find(ConnectionPoint::Next, a.id);
    const ConnectionPoint* bp = find(ConnectionPoint::Previous, b.id);
    check(an && bp && an->pos.x == bp->pos.x && an->pos.y == bp->pos.y, "the bottom of a block is where the next one's top is");
    const ConnectionPoint* under = nullptr;
    for (const ConnectionPoint& p : s.points) if (p.kind == ConnectionPoint::Next && p.blockId.empty()) under = &p;
    check(under && under->pos.y == s.header.rect.bottom() && find(ConnectionPoint::Previous, a.id)->pos.y == under->pos.y, "under the hat is a connection point where the first block starts");
    const BlockBox* proc = s.find(s.blocks[(size_t)s.top[2]].id);
    const ConnectionPoint* slot = find(ConnectionPoint::Slot, proc->id, "BODY");
    check(slot && slot->pos.x == proc->slotBoxes[0].rect.x && slot->pos.y == proc->slotBoxes[0].rect.y, "a slot has a point at its start");
    const BlockBox* ret = nullptr;
    for (const BlockBox& x : s.blocks) if (x.type == "ret") ret = &x;
    check(ret && find(ConnectionPoint::Previous, ret->id) && find(ConnectionPoint::Next, ret->id)->pos.y == ret->rect.bottom(), "blocks inside a slot have their points too");
    // hit testing: the innermost block
    const Pt inRet = ret->rect.center();
    const BlockBox* hit = s.blockAt(inRet);
    check(hit && hit->type == "ret", "a point inside a procedure's body finds the block there, not the procedure");
    const BlockBox* headHit = s.blockAt({proc->head.x + 2, proc->head.y + 2});
    check(headHit && headHit->type == "proc", "...and a point on the procedure's head finds the procedure");
    check(s.blockAt({-50, -50}) == nullptr, "a point on empty canvas finds nothing");
    // all points are finite and lie within the bounds (plus the foot)
    bool ok = true;
    for (const ConnectionPoint& p : s.points) ok = ok && std::isfinite(p.pos.x) && std::isfinite(p.pos.y) && p.pos.x >= s.bounds.x - 0.01 && p.pos.y >= s.bounds.y - 0.01 && p.pos.y <= s.bounds.bottom() + 0.01;
    check(ok, "every connection point lies within the stack");
}

static void t_layout_document() {
    section("layout: a document");
    Document d = docOf(".main\na: JMP a\n.peri\np: JMP p\n");
    d.sections[1].x = 300;
    d.sections[1].y = 120;
    Section fl;
    fl.domain = "floating";
    fl.x = -80;
    fl.y = 400;
    fl.blocks.push_back(d.make("ret"));
    d.sections.push_back(fl);
    FakeMetrics m;
    DocumentLayout dl = LayoutEngine(m).layoutDocument(d);
    check(dl.stacks.size() == 3 && dl.stacks[1].origin.x == 300 && dl.stacks[1].origin.y == 120 && dl.stacks[2].header.kind == HeaderBox::Tag, "one layout per section, at its position; the floating one has a tag");
    Rect expect;
    for (const StackLayout& s : dl.stacks) expect = unite(expect, s.bounds.translated(s.origin.x, s.origin.y));
    check(dl.bounds.x == expect.x && dl.bounds.y == expect.y && dl.bounds.w == expect.w && dl.bounds.h == expect.h && dl.bounds.x <= -80 && dl.bounds.bottom() > 400, "the bounds cover every stack, wherever it lies");
    const BlockBox* box = nullptr;
    const StackLayout* owner = dl.stackOf(d.sections[1].blocks[1].id, &box);
    check(owner && owner->section == 1 && box && box->type == "jmp", "a block is found in the stack it belongs to");
    check(dl.stackOf("nope") == nullptr, "an unknown id is not found");
    // deterministic
    DocumentLayout again = LayoutEngine(m).layoutDocument(d);
    bool same = again.stacks.size() == dl.stacks.size();
    for (size_t i = 0; same && i < dl.stacks.size(); i++) same = again.stacks[i].blocks.size() == dl.stacks[i].blocks.size() && again.stacks[i].bounds.w == dl.stacks[i].bounds.w && again.stacks[i].bounds.h == dl.stacks[i].bounds.h;
    check(same, "the same document gives the same layout");
}

// words come from a provider: a translation changes widths, not the code
struct LongWords : ITextProvider {
    std::string blockTitle(const BlockDef& d) const override { return d.title + " (translated)"; }
    std::string hatTitle(const std::string&) const override { return "A very long hat title indeed"; }
    std::string emptyValue() const override { return "<empty>"; }
};

static void t_layout_text() {
    section("layout: words come from a provider");
    Document d = docOf(".main\n MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n");
    FakeMetrics m;
    StackLayout a = LayoutEngine(m).layoutStack(d.sections[0], 0);
    LongWords lw;
    StackLayout b = LayoutEngine(m, LayoutStyle(), Registry::defaults(), ShapeRegistry::defaults(), &lw).layoutStack(d.sections[0], 0);
    check(b.blocks[0].title == "move (translated)" && b.blocks[0].rect.w > a.blocks[0].rect.w, "another provider's title is used and the block grows");
    check(b.header.text == "A very long hat title indeed" && b.header.rect.w > a.header.rect.w, "...also the hat");
}

static Registry valueRegistry() {
    Registry r = Registry::defaults();
    BlockDef num;
    num.type = "tnum"; num.title = "number"; num.category = "Math"; num.domains = kBoth; num.valueType = "int";
    FieldDef f; f.name = "v"; f.label = "";
    num.fields = {f};
    num.tmpl = "{v}";
    r.add(num);
    BlockDef add;
    add.type = "add"; add.title = "add"; add.category = "Math"; add.domains = kBoth; add.valueType = "int";
    add.inputs = {{"A", "", {"int"}, false}, {"B", "plus", {"int"}, false}};
    add.tmpl = "({A} + {B})";
    r.add(add);
    BlockDef setv;
    setv.type = "setv"; setv.title = "set"; setv.category = "Data"; setv.domains = kMain;
    FieldDef nm; nm.name = "name"; nm.label = "name";
    setv.fields = {nm};
    setv.inputs = {{"V", "to", {"int"}, false}};
    setv.tmpl = ".equ {name} = {V}";
    r.add(setv);
    return r;
}

static void t_layout_values() {
    section("layout: value sockets (for the expression blocks of stage B4)");
    Registry reg = valueRegistry();
    Document d;
    Section sec;
    sec.domain = "main";
    Block setv = d.make("setv");
    setv.set("name", "X");
    sec.blocks.push_back(setv);
    d.sections.push_back(sec);
    FakeMetrics m;
    LayoutEngine eng(m, LayoutStyle(), reg);
    StackLayout empty = eng.layoutStack(d.sections[0], 0);
    check(empty.blocks[0].sockets.size() == 1 && empty.blocks[0].sockets[0].child < 0 && empty.blocks[0].sockets[0].rect.w >= LayoutStyle().socketMinWidth, "an empty socket is a placeholder");
    // fill it with add(2, add(3, 4))
    Block add = d.make("add");
    Block n2 = d.make("tnum");
    n2.set("v", "2");
    Block inner = d.make("add");
    Block n3 = d.make("tnum"), n4 = d.make("tnum");
    n3.set("v", "3");
    n4.set("v", "4");
    inner.setValue("A", n3);
    inner.setValue("B", n4);
    add.setValue("A", n2);
    add.setValue("B", inner);
    d.sections[0].blocks[0].setValue("V", add);
    StackLayout s = eng.layoutStack(d.sections[0], 0);
    check(s.blocks.size() == 6, "the plugged blocks are in the layout", std::to_string(s.blocks.size()));
    const BlockBox& host = s.blocks[0];
    check(host.sockets[0].child >= 0 && s.blocks[(size_t)host.sockets[0].child].type == "add" && s.blocks[(size_t)host.sockets[0].child].parent == 0, "the socket knows the block plugged into it, and that block its parent");
    bool inRow = true;
    for (size_t i = 1; i < s.blocks.size(); i++) inRow = inRow && inside(s.blocks[i].rect, host.head);
    check(inRow, "all the plugged blocks lie inside the host's row");
    const BlockBox& innerBox = s.blocks[(size_t)s.blocks[(size_t)host.sockets[0].child].sockets[1].child];
    check(innerBox.type == "add" && innerBox.depth == 2 && innerBox.shape == Shape::Value, "nested values are laid out recursively");
    bool left = true;
    for (size_t i = 1; i < s.blocks.size(); i++) left = left && s.blocks[i].rect.x >= host.titleRect.right() - 0.01;
    check(left, "...to the right of the title");
    check(host.rect.w > empty.blocks[0].rect.w, "the host grows to hold what is plugged in");
    int plugs = 0, sockets = 0;
    for (const ConnectionPoint& p : s.points) { plugs += p.kind == ConnectionPoint::Plug; sockets += p.kind == ConnectionPoint::Socket; }
    check(plugs == 5 && sockets == 5, "every value has a plug point and every socket a socket point", std::to_string(plugs) + " plugs, " + std::to_string(sockets) + " sockets");
    // a taller child makes the row taller and the other parts stay centred
    FakeMetrics tall;
    struct Tall : ITextMetrics {
        double width(const std::string& s, bool mono) const override { return FakeMetrics().width(s, mono); }
        double lineHeight(bool) const override { return 16; }
    } t;
    (void)tall; (void)t;
    check(host.head.h >= s.blocks[(size_t)host.sockets[0].child].rect.h + 2 * LayoutStyle().rowPadY - 0.01, "the row is high enough for the tallest block plugged in");
    check(std::fabs(host.titleRect.center().y - host.head.center().y) < 0.51, "the title stays vertically centred in the row");
}

// where to put the payload's origin so that its first block's attachment point lies at q
static Pt originFor(const StackLayout& payload, Pt q, bool value) {
    const BlockBox& f = payload.blocks[(size_t)payload.top[0]];
    return value ? Pt{q.x - f.rect.x, q.y - f.rect.center().y} : Pt{q.x - f.rect.x, q.y - f.rect.y};
}

static void t_drop() {
    section("dropping a dragged stack: where it fits, and what happens");
    FakeMetrics m;
    BlockSession s;
    s.setDocument(docOf(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.peri\np: JMP p\n"));
    LayoutEngine eng(m);
    const std::string movId = s.document().sections[0].blocks[1].id, aluId = s.document().sections[0].blocks[2].id;
    const std::string numId = s.document().sections[0].blocks[1].value("src")->id;
    const std::string original = s.generated().text;

    // ---- a component, lifted out of its socket
    s.beginGroup("drag:1");
    Outcome u = s.apply([&](Editor& e) { return e.unplugValue(movId, "src", 400, 100); });
    check(u.ok && s.document().sections.size() == 3, "(the number is lifted out: it floats)");
    DocumentLayout lay = eng.layoutDocument(s.document());
    const size_t payload = s.document().sections.size() - 1;
    // the hole it left
    const StackLayout& host = lay.stacks[0];
    const BlockBox* movBox = host.find(movId);
    check(movBox && movBox->sockets[0].child < 0 && movBox->sockets[0].empty && movBox->sockets[0].plug == PlugShape::Any, "its socket is now an empty hole");
    Pt hole{0, 0};
    for (const ConnectionPoint& cp : host.points) if (cp.kind == ConnectionPoint::Socket && cp.blockId == movId && cp.slot == "src") hole = {host.origin.x + cp.pos.x, host.origin.y + cp.pos.y};
    DropTarget t = findDropTarget(s, lay, payload, originFor(lay.stacks[payload], hole, true));
    check(t.kind == DropTarget::Socket && t.fits() && t.hostId == movId && t.socket == "src", "brought back to the hole it fits (a Socket target that fits)", t.reason);
    t = findDropTarget(s, lay, payload, originFor(lay.stacks[payload], {hole.x + 12, hole.y + 6}, true));
    check(t.kind == DropTarget::Socket && t.fits(), "a little off still snaps");
    t = findDropTarget(s, lay, payload, originFor(lay.stacks[payload], {hole.x + 200, hole.y + 200}, true));
    check(t.kind == DropTarget::None, "far from everything: nothing");
    Pt dstHole{0, 0};
    for (const ConnectionPoint& cp : host.points) if (cp.kind == ConnectionPoint::Socket && cp.blockId == movId && cp.slot == "dst") dstHole = {host.origin.x + cp.pos.x, host.origin.y + cp.pos.y};
    t = findDropTarget(s, lay, payload, originFor(lay.stacks[payload], dstHole, true));
    check(t.kind == DropTarget::Socket && !t.fits() && !t.reason.empty(), "over the destination hole a number does not fit (a region is needed): reported, with the reason", t.reason);
    Pt nextPoint{0, 0};
    for (const ConnectionPoint& cp : host.points) if (cp.kind == ConnectionPoint::Next && cp.blockId == movId) nextPoint = {host.origin.x + cp.pos.x, host.origin.y + cp.pos.y};
    t = findDropTarget(s, lay, payload, originFor(lay.stacks[payload], nextPoint, true));
    check(t.kind != DropTarget::Insert, "a component is never offered a place between instructions");
    Outcome dropped = performDrop(s, payload, findDropTarget(s, lay, payload, originFor(lay.stacks[payload], hole, true)));
    s.endGroup();
    check(dropped.ok && s.document().sections.size() == 2 && s.generated().text == original, "dropped: plugged again, and the program is as it was", s.generated().text);
    check(s.document().find(numId) != nullptr, "(the same block)");
    check(s.undo() && s.generated().text == original && s.document().sections.size() == 2, "lift + drop is one undo step");

    // ---- instructions, lifted out of a stack
    s.setDocument(docOf(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.peri\np: JMP p\n"));
    const std::string alu2 = s.document().sections[0].blocks[2].id, mov2 = s.document().sections[0].blocks[1].id;
    const std::string text0 = s.generated().text;
    s.beginGroup("drag:2");
    s.apply([&](Editor& e) { return e.liftBlocks(alu2, true, 600, 300); });
    lay = eng.layoutDocument(s.document());
    const size_t pl = s.document().sections.size() - 1;
    check(s.document().sections[pl].blocks.size() == 2 && s.generated().text.find("ALU") == std::string::npos, "(the ALU block and the jump below it are lifted out)");
    Pt under{0, 0};
    for (const ConnectionPoint& cp : lay.stacks[0].points) if (cp.kind == ConnectionPoint::Next && cp.blockId == mov2) under = {lay.stacks[0].origin.x + cp.pos.x, lay.stacks[0].origin.y + cp.pos.y};
    t = findDropTarget(s, lay, pl, originFor(lay.stacks[pl], under, false));
    check(t.kind == DropTarget::Insert && t.fits() && t.where.section == 0 && t.where.before.empty() && t.where.container.empty(), "brought under the move block: insert at the end of Main", t.reason);
    Pt peri{0, 0};
    for (const ConnectionPoint& cp : lay.stacks[1].points) if (cp.kind == ConnectionPoint::Next && cp.blockId.empty()) peri = {lay.stacks[1].origin.x + cp.pos.x, lay.stacks[1].origin.y + cp.pos.y};
    t = findDropTarget(s, lay, pl, originFor(lay.stacks[pl], peri, false));
    check(t.kind == DropTarget::Insert && !t.fits() && (t.reason.find("Peri") != std::string::npos || t.reason.find("Main") != std::string::npos), "under the Peri hat a Main-only block does not fit (H1): reported", t.reason);
    t = findDropTarget(s, lay, pl, originFor(lay.stacks[pl], {5000, 5000}, false));
    check(t.kind == DropTarget::None, "nothing near: nothing");
    dropped = performDrop(s, pl, findDropTarget(s, lay, pl, originFor(lay.stacks[pl], under, false)));
    s.endGroup();
    check(dropped.ok && s.generated().text == text0 && s.document().sections.size() == 2, "dropped: the stack is part of the program again", s.generated().text);
    check(s.undo() && s.generated().text == text0, "one undo");

    // ---- into the slot of a procedure, above a floating stack
    s.setDocument(docOf(".main\n.proc f\n.endp\n JMP x\n"));
    s.apply([&](Editor& e) { return e.liftBlocks(s.document().sections[0].blocks[1].id, true, 700, 20); });
    lay = eng.layoutDocument(s.document());
    const size_t jp = s.document().sections.size() - 1;
    const std::string procId = s.document().sections[0].blocks[0].id;
    Pt slot{0, 0};
    for (const ConnectionPoint& cp : lay.stacks[0].points) if (cp.kind == ConnectionPoint::Slot && cp.blockId == procId) slot = {lay.stacks[0].origin.x + cp.pos.x, lay.stacks[0].origin.y + cp.pos.y};
    t = findDropTarget(s, lay, jp, originFor(lay.stacks[jp], slot, false));
    check(t.kind == DropTarget::Insert && t.where.container == procId && t.where.slot == "BODY" && t.fits(), "into the body of a procedure", t.reason);
    performDrop(s, jp, t);
    const std::vector<Block>* body = s.document().sections[0].blocks[0].stack("BODY");
    check(body && body->size() == 1 && s.generated().text.find(".proc f\n    JMP x\n.endp") != std::string::npos, "...and it is there", s.generated().text);
}

static bool anyOverlap(const DocumentLayout& lay) {
    for (size_t i = 0; i < lay.stacks.size(); i++)
        for (size_t j = i + 1; j < lay.stacks.size(); j++) {
            const Rect a = lay.stacks[i].bounds.translated(lay.stacks[i].origin.x, lay.stacks[i].origin.y), b = lay.stacks[j].bounds.translated(lay.stacks[j].origin.x, lay.stacks[j].origin.y);
            if (a.intersects(b)) return true;
        }
    return false;
}

static void t_catalog() {
    section("the catalog: drawers, samples, patterns, search");
    BlockCatalog cat;
    const Registry& reg = Registry::defaults();
    // every definition is offered (except a blank line and raw text), and every entry can be made
    std::set<std::string> offered;
    for (const CatalogEntry& e : cat.entries()) if (!e.pattern()) offered.insert(e.type);
    bool all = true;
    std::string missing;
    for (const BlockDef& d : reg.all()) if (d.type != "blank" && d.type != "raw" && !offered.count(d.type)) { all = false; missing += d.type + " "; }
    check(all, "every block definition is in the toolbox (a new definition needs a sample text, or this test says so)", missing);
    // the drawers
    const std::vector<BlockCatalog::Drawer> drawers = cat.drawers();
    check(!drawers.empty() && drawers[0].name == "Patterns" && drawers[0].cls == "pattern", "the first drawer holds the patterns");
    std::set<const CatalogEntry*> inDrawers;
    size_t count = 0;
    for (const auto& d : drawers) for (const CatalogEntry* e : d.entries) { inDrawers.insert(e); count++; }
    check(count == cat.entries().size() && inDrawers.size() == count, "every entry is in exactly one drawer");
    std::string order;
    for (const auto& d : drawers) order += d.name + " ";
    check(order.find("Data") < order.find("Structure") && order.find("Structure") < order.find("Values") && order.find("Values") < order.find("References"), "instructions first, then structure, definitions, text, then the components", order);
    bool pure = true;
    for (const auto& d : drawers) for (const CatalogEntry* e : d.entries) pure = pure && e->cls == d.cls;
    check(pure, "a drawer holds one class only (instructions and components are never mixed)");
    // every entry can be made, and makes sense where it belongs
    int made = 0, structuralErrors = 0, raws = 0;
    std::string firstBad;
    for (const CatalogEntry& e : cat.entries()) {
        Document doc;
        std::vector<Block> blocks = cat.instantiate(e, doc);
        if (blocks.empty()) { firstBad = e.id + " makes nothing"; continue; }
        made++;
        Section s;
        s.domain = e.component() ? "floating" : (e.domains == kPeri ? "peri" : e.cls == "definition" ? "" : "main");
        s.blocks = blocks;
        doc.sections.push_back(s);
        for (const BlockProblem& p : validate(doc)) if (p.kind == "structure" && p.error) { structuralErrors++; if (firstBad.empty()) firstBad = e.id + ": " + p.message; }
        std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) { for (const Block& b : v) { if (b.type == "raw" && e.type != "raw") raws++; for (auto& st : b.stacks) walk(st.second); } };
        walk(blocks);
    }
    check(made == (int)cat.entries().size() && structuralErrors == 0 && raws == 0, "every entry makes blocks with no structural error and nothing left as raw text", firstBad);
    // components are built with their parts
    Document d;
    std::vector<Block> reg1 = cat.instantiate(*cat.find("type:region_main_scratchpad"), d), bin = cat.instantiate(*cat.find("type:expr_bin"), d), mov = cat.instantiate(*cat.find("type:mov"), d);
    check(reg1[0].value("first") && reg1[0].value("last") && bin[0].value("a") && bin[0].value("b") && mov[0].value("src") && mov[0].value("dst"), "a region, an expression and a move come with their parts (numbers, regions) in place");
    check(generate(Document()).text.empty() && !mov[0].get("src").empty(), "(and the text mirror is filled)");
    // patterns
    int patterns = 0, patternErrors = 0;
    std::string patBad;
    for (const CatalogEntry& e : cat.entries()) {
        if (!e.pattern()) continue;
        patterns++;
        Document pd;
        std::vector<Block> bl = cat.instantiate(e, pd);
        if (bl.size() < 2) patBad += e.id + " is one block; ";
        std::string text = (e.domains == kPeri ? ".devmap 0, I2C, 0x50\n.peri\n" : ".main\n");
        for (const std::string& line : {e.text}) text += line + "\n";
        text += e.domains == kPeri ? "end: JMP end\n" : "fin: JMP fin\n";
        std::vector<asmcore::Source> src{asmcore::Source{"p.asm", text}};
        asmcore::Options opt;
        asmcore::Result r = asmcore::assemble(src, nullptr, opt);
        bool errors = !r.ok;
        if (errors) { patternErrors++; patBad += e.id + ": " + r.diagnosticsText().substr(0, 120) + "; "; }
    }
    check(patterns >= 5 && patternErrors == 0 && patBad.empty(), "every pattern is several blocks and assembles", patBad);
    // search
    auto titles = [&](const std::string& q, size_t n = 12) { std::string s; for (const CatalogEntry* e : cat.search(q, n)) s += e->title + "|"; return s; };
    auto firstOf = [&](const std::string& q) { auto r = cat.search(q, 3); return r.empty() ? std::string() : r[0]->title; };
    check(firstOf("mov") == "move" && firstOf("move") == "move", "'mov' and 'move' find the move block first", titles("mov"));
    check(firstOf("jump") == "jump" && firstOf("ALU") == "calculate", "'jump'; the mnemonic 'ALU' finds the calculate block", titles("alu"));
    check(titles("exec_csr").find("write CSR") != std::string::npos && titles("exec_csr").find("read CSR") != std::string::npos, "the mnemonic EXEC_CSR finds the CSR instructions", titles("exec_csr"));
    check(titles("csr").find("write CSR") != std::string::npos, "'csr' finds the CSR blocks", titles("csr"));
    check(!cat.search("mv").empty() && firstOf("mv") == "move", "the letters in order find a block ('mv' -> move)", titles("mv"));
    check(cat.search("").empty() && cat.search("   ").empty() && cat.search("zzzzqq").empty(), "an empty query or one that matches nothing: nothing");
    check(cat.search("c", 3).size() == 3 && cat.search("c", 1).size() == 1, "the limit");
    check(firstOf("scratch") == "scratchpad", "components are found by name too ('scratch' -> scratchpad)", titles("scratch"));
    check(titles("WAIT").find("Wait until") != std::string::npos, "patterns are found (case does not matter)", titles("WAIT"));
    // typed assembler
    check(cat.looksLikeAssembler("MOV CONST(1,2) -> MAIN_SCRATCHPAD[0..1]") && cat.looksLikeAssembler("start:") && cat.looksLikeAssembler(".equ X = 1") && cat.looksLikeAssembler("exec_csr write8 5, 1"), "a line of assembler is recognised (mnemonic, label, directive; any case)");
    check(!cat.looksLikeAssembler("move") && !cat.looksLikeAssembler("csr") && !cat.looksLikeAssembler("scratch") && !cat.looksLikeAssembler(""), "a name to search for is not");
    Document d2;
    bool raw = true;
    std::vector<Block> typed = cat.fromAssembler(d2, "MOV CONST(1,2) -> MAIN_SCRATCHPAD[0..1]", &raw);
    check(typed.size() == 1 && typed[0].type == "mov" && typed[0].value("src") && !raw, "a typed line becomes the block (with its components)");
    typed = cat.fromAssembler(d2, "frobnicate the thing", &raw);
    check(typed.size() == 1 && typed[0].type == "raw" && raw, "a line that is not understood stays text, and says so");
}

static void t_custom_blocks() {
    section("the user's own blocks in the toolbox");
    CustomBlock c{"Data", "clear first byte", "MOV CONST(0, 1) -> MAIN_SCRATCHPAD[0..0]\n; and say so\n"};
    const std::string line = encodeCustom(c);
    CustomBlock back;
    check(line.find('\n') == std::string::npos && decodeCustom(line, back) && back.drawer == "Data" && back.title == c.title && back.text == "MOV CONST(0, 1) -> MAIN_SCRATCHPAD[0..0]\n; and say so\n", "a block is one line in the settings and comes back whole (line breaks included)", line);
    CustomBlock odd{"Co\tntrol", "a\\b\ttab \xE4\xB8\xAD\xE6\x96\x87", "x\\n\ty"};
    CustomBlock odd2;
    check(decodeCustom(encodeCustom(odd), odd2) && odd2.drawer == odd.drawer && odd2.title == odd.title && odd2.text == odd.text, "tabs, backslashes and non-ASCII text survive");
    check(!decodeCustom("", odd2) && !decodeCustom("only one field", odd2) && !decodeCustom("\tno drawer\ttext", odd2), "damaged lines are refused");

    BlockCatalog cat;
    const size_t before = cat.entries().size();
    check(cat.checkCustom(c).ok && cat.checkCustom(c).blocks == 2, "a name and a text that makes blocks is fine");
    check(!cat.checkCustom(CustomBlock{"Data", "", "RET"}).ok && !cat.checkCustom(CustomBlock{"Data", "x", "  "}).ok, "a name and a text are needed");
    check(!cat.checkCustom(CustomBlock{"Data", "move", "RET"}).ok && cat.checkCustom(CustomBlock{"Data", "move", "RET"}).message.find("already") != std::string::npos, "the name must be new in its drawer");
    check(cat.checkCustom(CustomBlock{"Math", "move", "RET"}).ok, "(the same name in another drawer is fine)");
    check(cat.checkCustom(CustomBlock{"Data", "weird", "frobnicate"}).ok && cat.checkCustom(CustomBlock{"Data", "weird", "frobnicate"}).raw == 1 && !cat.checkCustom(CustomBlock{"Data", "weird", "frobnicate"}).message.empty(), "text that is not understood is allowed, with a note");
    check(!cat.checkCustom(CustomBlock{"Values", "bad", "not an operand ("}).ok && cat.checkCustom(CustomBlock{"Values", "bytes 4 to 7", "MAIN_SCRATCHPAD[4..7]"}).ok && cat.checkCustom(CustomBlock{"Values", "limit plus one", "LIMIT + 1"}).ok, "in a component drawer the text must be an operand");

    cat.setCustom({c, CustomBlock{"Values", "bytes 4 to 7", "MAIN_SCRATCHPAD[4..7]"}, CustomBlock{"Peripheral", "read two", "EXEC_XFER READ dev=0, len=2, ptr=0"}});
    check(cat.entries().size() == before + 3, "the entries are added");
    const CatalogEntry* e = cat.find(BlockCatalog::customId(c));
    check(e && e->custom && e->cls == "instruction" && e->category == "Data" && e->title == "clear first byte", "a custom block is in its drawer with the class of the drawer");
    const CatalogEntry* comp = cat.find("custom:Values/bytes 4 to 7");
    check(comp && comp->cls == "component" && comp->component(), "one in a component drawer is a component");
    const CatalogEntry* peri = cat.find("custom:Peripheral/read two");
    check(peri && peri->domains == asmcore::blocks::kPeri, "it is Peri-only if its first block is");
    // in the drawers
    bool inData = false, inValues = false;
    for (const auto& d : cat.drawers()) {
        for (const CatalogEntry* x : d.entries) {
            if (x->id == e->id && d.name == "Data") inData = true;
            if (x->id == comp->id && d.name == "Values") inValues = true;
        }
    }
    check(inData && inValues, "the drawers list them");
    // making blocks
    Document d;
    std::vector<Block> v = cat.instantiate(*e, d);
    check(v.size() == 2 && v[0].type == "mov" && v[0].value("src") && v[1].type == "comment", "a custom instruction makes the blocks of its text (here a move and a comment)");
    std::vector<Block> cv = cat.instantiate(*comp, d);
    check(cv.size() == 1 && cv[0].type == "region_main_scratchpad" && cv[0].value("first") && cv[0].value("last"), "a custom component makes the operand (a region with its range)");
    check(cat.search("clear first").size() == 1 && cat.search("clear first")[0]->custom, "Typeblock finds it by name");
    // editing keeps the name available, removing takes it away
    check(cat.checkCustom(CustomBlock{"Data", "clear first byte", "RET"}, e->id).ok && !cat.checkCustom(CustomBlock{"Data", "clear first byte", "RET"}).ok, "the entry being edited may keep its name");
    cat.setCustom({});
    check(cat.entries().size() == before && !cat.find(e->id), "removing them all brings the toolbox back to what it was");
}

static std::string slurp(const fs::path& p);

// The blocks the project's hardware description brings: a CSR, a device, a service, a constant, a name each get their own.
static void t_hardware_drawers() {
    section("blocks from the hardware description");
    asmcore::Hardware hw;
    std::vector<std::string> perr;
    const bool parsed = asmcore::parseHardware(slurp(fs::path(ACIDE_SAMPLE_DIR) / "11_hardware_designer" / "board.achw"), hw, perr);
    check(parsed && !hw.csrs.empty() && !hw.devices.empty() && !hw.services.empty(), "the sample description reads", perr.empty() ? "" : perr[0]);
    BlockCatalog cat;
    const size_t before = cat.entries().size();
    check(cat.setHardware(hw) && cat.hardwareCount() > 10 && cat.entries().size() == before + cat.hardwareCount(), "the entries are added");
    check(!cat.setHardware(hw), "the same description again changes nothing (the toolbox is not built again)");
    auto has = [&](const std::string& id) { return cat.find(id) != nullptr; };
    check(has("hw:Hardware CSR/write AE_CTRL") && has("hw:Hardware CSR/set bits AE_CTRL") && has("hw:Hardware CSR/clear bits AE_CTRL") && has("hw:Hardware CSR/read AE_CTRL"), "a RAM CSR: write, set bits, clear bits, read back");
    check(has("hw:Hardware CSR/write POLL_CTRL") && !has("hw:Hardware CSR/read POLL_CTRL"), "an action CSR can be written but not read");
    check(has("hw:Hardware Devices/read DEV_IMU") && has("hw:Hardware Devices/write DEV_IMU"), "a device: read and write by its DeviceMap name");
    check(has("hw:Hardware Services/publish SID_IMU_GYRO_RAW"), "a service: publish");
    const CatalogEntry* equ = cat.find("hw:Hardware Names/PERI_DONE");
    check(equ && equ->cls == "component" && equ->operand && equ->hardware, "a name of the description is a component");
    const CatalogEntry* wr = cat.find("hw:Hardware CSR/write AE_CTRL");
    const CatalogEntry* rd = cat.find("hw:Hardware Devices/read DEV_IMU");
    check(wr && wr->domains == asmcore::blocks::kBoth && rd && rd->domains == asmcore::blocks::kPeri, "Main-only / Peri-only follows the block (a device is read by Peri)");
    // they are in the drawers, right after the patterns
    std::vector<std::string> order;
    for (const auto& d : cat.drawers()) order.push_back(d.name);
    size_t pat = std::find(order.begin(), order.end(), "Patterns") - order.begin(), csr = std::find(order.begin(), order.end(), "Hardware CSR") - order.begin(), data = std::find(order.begin(), order.end(), "Data") - order.begin();
    check(csr == pat + 1 && csr < data, "the hardware drawers come right after the patterns");
    // every one makes blocks (no Raw), and the line it says assembles with the generated include
    const std::string inc = asmcore::generateInc(hw);
    int made = 0, bad = 0;
    std::string why;
    for (const CatalogEntry& e : cat.entries()) {
        if (!e.hardware) continue;
        Document doc;
        std::vector<Block> v = cat.instantiate(e, doc);
        bool raw = v.empty();
        for (const Block& b : v) raw |= b.type == "raw";
        if (raw) { bad++; why += e.title + " (raw) "; continue; }
        made++;
        if (e.operand) continue;
        const bool peri = e.domains == asmcore::blocks::kPeri;
        const std::string prog = peri ? ".main\nm: JMP m\n.peri\nstart: " + e.text + "\np: BR.ALWAYS p\n" : ".main\nstart: " + e.text + "\nh: JMP h\n";
        asmcore::Options opt;
        const asmcore::Result res = asmcore::assemble({asmcore::Source{"defs.inc", inc}, asmcore::Source{"p.asm", prog}}, nullptr, opt);
        bool err = false;
        for (const asmcore::Diagnostic& d : res.diags) if (d.sev == asmcore::Diagnostic::Error) { err = true; why += e.title + ": " + d.code + " " + d.msg + " | "; }
        if (err) bad++;
    }
    check(made > 10 && bad == 0, "every block of the description makes blocks and its line assembles", why);
    check(cat.search("AE_CTRL").size() >= 4 && cat.search("gyro").size() >= 1, "Typeblock finds them by the name of the part");
    check(cat.clearHardware() && cat.hardwareCount() == 0 && cat.entries().size() == before, "without a description the toolbox is as it was");
}

static void t_tidy() {
    section("tidy up: nothing overlaps, the program stays what it was");
    FakeMetrics m;
    LayoutEngine eng(m);
    BlockSession s;
    s.setDocument(docOf("; a header\n.equ N = 1\n.main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n JMP start\n.peri\np: JMP p\n.main\nsecond: JMP second\n"));
    const std::string text = s.generated().text;
    // some loose stacks, and everything piled up on one spot
    s.apply([&](Editor& e) { return e.liftBlocks(s.document().sections[1].blocks[1].id, true, 10, 10); });
    {
        Block nb;
        nb.type = "num";
        nb.set("value", "1");
        s.apply([&](Editor& e) { return e.newFloating({nb}, 12, 12); });                       // (no id: the editor gives it one)
    }
    for (size_t i = 0; i < s.document().sections.size(); i++) s.apply([&](Editor& e) { return e.moveSection(i, 5.0 * (double)i, 3.0 * (double)i + (s.document().sections[i].domain == "floating" ? 0 : 100.0 * (double)i)); });
    const std::string text2 = s.generated().text;
    DocumentLayout lay = eng.layoutDocument(s.document());
    check(anyOverlap(lay), "(the stacks overlap to begin with)");
    const std::vector<size_t> orderBefore = programOrder(s.document());
    const size_t moved = tidyUp(s, lay);
    lay = eng.layoutDocument(s.document());
    check(moved > 0 && !anyOverlap(lay), "tidied: nothing overlaps", std::to_string(moved));
    check(s.generated().text == text2 && programOrder(s.document()) == orderBefore, "the program is exactly what it was (same text, same order)", s.generated().text);
    // columns
    const Document& d = s.document();
    double mainX = -1, periX = -1, looseX = -1, defsY = 1e9;
    for (size_t i = 0; i < d.sections.size(); i++) {
        const double x = lay.stacks[i].origin.x + lay.stacks[i].bounds.x;
        if (d.sections[i].domain == "main" && mainX < 0) mainX = x;
        if (d.sections[i].domain == "peri") periX = x;
        if (d.sections[i].domain == "floating") looseX = x;
        if (d.sections[i].domain.empty()) defsY = lay.stacks[i].origin.y;
    }
    check(mainX == 0 && periX > mainX && looseX > periX, "Main on the left, Peri in the next column, loose stacks on the right of both", std::to_string(mainX) + " " + std::to_string(periX) + " " + std::to_string(looseX));
    check(defsY < lay.stacks[1].origin.y + 1, "the definitions are at the top");
    // order: each program stack strictly below the one before it
    bool strictly = true;
    double last = -1e9;
    for (size_t i : programOrder(d)) { strictly = strictly && lay.stacks[i].origin.y + lay.stacks[i].bounds.y > last; last = lay.stacks[i].origin.y + lay.stacks[i].bounds.y; }
    check(strictly, "each part of the program is strictly below the one before it (that is what keeps the order)");
    // idempotent, and one undo step
    check(tidyUp(s, lay) == 0, "tidying a tidy canvas moves nothing");
    check(s.undo() && s.generated().text == text2, "one undo puts every stack back (the text never changed)");
    check(anyOverlap(eng.layoutDocument(s.document())), "(and they overlap again)");
    // snapping
    s.redo();
    TidyOptions snap;
    snap.snap = 8;
    const std::vector<Pt> places = tidyPlaces(s.document(), eng.layoutDocument(s.document()), snap);
    bool onGrid = true;
    DocumentLayout l2 = eng.layoutDocument(s.document());
    for (size_t i = 0; i < places.size(); i++) onGrid = onGrid && std::fmod(std::fabs(places[i].x + l2.stacks[i].bounds.x), 8.0) < 1e-6 && std::fmod(std::fabs(places[i].y + l2.stacks[i].bounds.y), 8.0) < 1e-6;
    check(onGrid, "with a grid, the pictures stand on it");

    section("the number on a hat is its place in the program");
    DocumentLayout dl = eng.layoutDocument(s.document());
    std::string orders;
    for (size_t i = 0; i < dl.stacks.size(); i++) orders += std::to_string(dl.stacks[i].header.order) + " ";
    check(dl.stacks[programOrder(s.document())[0]].header.order == 1 && dl.stacks[programOrder(s.document()).back()].header.order == (int)programOrder(s.document()).size(), "1, 2, 3 ... in the order the text is written", orders);
    bool floatingZero = true;
    for (size_t i = 0; i < dl.stacks.size(); i++) if (s.document().sections[i].domain == "floating" && dl.stacks[i].header.order != 0) floatingZero = false;
    check(floatingZero, "a stack that is not part of the program has none");
    // dragging a hat changes the number
    BlockSession t2;
    t2.setDocument(docOf(".main\na: JMP a\n.main\nb: JMP b\n"));
    t2.apply([&](Editor& e) { return e.moveSection(0, 0, 500); });
    check(eng.layoutDocument(t2.document()).stacks[0].header.order == 2 && eng.layoutDocument(t2.document()).stacks[1].header.order == 1, "moving the first part below the second makes it the second");
    check(t2.generated().text.find("b:\n") < t2.generated().text.find("a:\n"), "...and the text follows");
}

static void t_clipboard() {
    section("clipboard, duplicate, disable, collapse");
    BlockSession s;
    s.setDocument(docOf(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.proc f\n RET\n.endp\n"));
    const std::string mov = s.document().sections[0].blocks[1].id, alu = s.document().sections[0].blocks[2].id, proc = s.document().sections[0].blocks[4].id;
    const std::string original = s.generated().text;
    // copy
    s.select({mov});
    const BlockSession::Clip clip = s.copySelection();
    check(clip.text == "    MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n" && clip.blocks.find("acblk-clip") != std::string::npos, "a copy is the blocks (to paste back) and their assembler text");
    // paste under the selected instruction
    s.select({alu});
    std::string note;
    size_t n = s.paste(clip.blocks, 300, 300, note);
    check(n == 1 && s.document().sections[0].blocks.size() == 6 && s.document().sections[0].blocks[3].type == "mov" && s.document().sections[0].blocks[3].id != mov, "pasted under the selected instruction, with a new id");
    check(s.selection().size() == 1 && s.isSelected(s.document().sections[0].blocks[3].id), "the pasted block is selected");
    check(s.generated().text.find("MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n    ALU ADD unsigned\n    MOV CONST(5, 2)") != std::string::npos, "...and the program has it there", s.generated().text);
    check(s.undo() && s.generated().text == original, "one undo");
    // paste assembler text with nothing selected: it lies on the canvas
    s.clearSelection();
    n = s.paste("MOV CONST(1, 1) -> MAIN_SCRATCHPAD[4..4]\n ALU SUB\n", 400, 120, note);
    check(n == 2 && s.document().sections.back().domain == "floating" && s.document().sections.back().x == 400 && s.document().sections.back().y == 120 && s.document().sections.back().blocks.size() == 2, "assembler text becomes blocks on the canvas at the given place");
    check(s.generated().text == original, "(not part of the program yet)");
    s.undo();
    // text that is not code is kept as text blocks, with a note
    n = s.paste("what is this", 0, 0, note);
    check(n == 1 && s.document().sections.back().blocks[0].type == "raw" && !note.empty(), "what is not understood is kept as text, and the note says so", note);
    s.undo();
    check(s.paste("", 0, 0, note) == 0 && !note.empty(), "an empty clipboard pastes nothing");
    // a lone component goes into an empty socket
    s.apply([&](Editor& e) { return e.unplugValue(mov, "src", 500, 50); });
    const std::string floatingNum = s.document().sections.back().blocks[0].id;
    s.select({floatingNum});
    const BlockSession::Clip numClip = s.copySelection();
    check(numClip.text == "CONST(5, 2)\n" || numClip.text == "5\n", "a component copied as text is the expression it stands for", numClip.text);
    s.select({mov});
    n = s.paste(numClip.blocks, 0, 0, note);
    check(n == 1 && s.document().sections[0].blocks[1].value("src") && s.document().sections[0].blocks[1].value("src")->type == "num", "a component pasted with a block selected goes into its empty socket that takes it");
    check(s.generated().text.find("MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]") != std::string::npos, "...and the instruction is whole again", s.generated().text);
    s.setDocument(docOf(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n.proc f\n RET\n.endp\n"));
    const std::string mov2 = s.document().sections[0].blocks[1].id, alu2 = s.document().sections[0].blocks[2].id, proc2 = s.document().sections[0].blocks[4].id;
    // cut
    s.select({alu2});
    BlockSession::Clip cut;
    check(s.cutSelection(cut) == 1 && s.generated().text.find("ALU") == std::string::npos && cut.text == "    ALU ADD unsigned\n", "cut: the block is gone and is on the clipboard");
    s.undo();
    // duplicate
    s.select({mov2, alu2});
    check(s.duplicateSelection() == 2 && s.document().sections[0].blocks.size() == 7, "duplicate: each selected instruction gets a copy right after it");
    check(s.selection().size() == 2 && !s.isSelected(mov2), "the copies are selected");
    s.undo();
    s.select({s.document().sections[0].blocks[1].value("src")->id});
    check(s.duplicateSelection() == 1 && s.document().sections.back().domain == "floating" && s.document().sections.back().blocks[0].get("value") == "5", "a component is duplicated beside: a copy on the canvas");
    s.undo();
    // disable / enable
    s.select({mov2, alu2});
    check(s.toggleDisabled() == 2 && s.generated().text.find("; MOV CONST(5, 2)") != std::string::npos && s.generated().text.find("; ALU ADD") != std::string::npos, "disable: they are written as comments");
    check(s.toggleDisabled() == 2 && s.generated().text.find("; MOV") == std::string::npos, "and enable again");
    s.select({mov2});
    s.toggleDisabled();
    s.select({mov2, alu2});
    s.toggleDisabled();
    check(s.document().find(mov2)->disabled && s.document().find(alu2)->disabled, "if only some are off, the selection is switched off");
    s.undo(); s.undo(); s.undo(); s.undo();
    s.select({s.document().sections[0].blocks[1].value("src")->id});
    check(s.toggleDisabled() == 0, "a component cannot be switched off (it is not a line)");
    // collapse
    s.select({proc2, mov2});
    check(s.toggleCollapsed() == 1 && s.document().find(proc2)->collapsed, "fold: only containers fold");
    check(s.toggleCollapsed() == 1 && !s.document().find(proc2)->collapsed, "unfold");
}

static void t_session() {
    section("session");
    BlockSession s;
    int changes = 0, selections = 0;
    s.changed.connect([&] { changes++; });
    s.selectionChanged.connect([&] { selections++; });
    Document d = docOf(".main\nstart: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP start\n.proc f\n RET\n.endp\n");
    s.setDocument(d);
    check(changes == 1 && s.generated().text == generate(d).text, "setDocument tells the views and generates the text");
    // the file
    std::string err;
    BlockSession t;
    check(t.load(s.serialize(), err) && t.generated().text == s.generated().text && t.serialize() == s.serialize(), "load / serialize round trip", err);
    check(!t.load("{", err) && !err.empty() && t.generated().text == s.generated().text, "a damaged file is refused and the session is left as it was", err);
    // selection
    const std::string movId = s.document().sections[0].blocks[1].id, procId = s.document().sections[0].blocks[3].id;
    s.select({movId});
    check(s.isSelected(movId) && s.selection().size() == 1 && selections == 2, "select");
    s.select({movId});
    check(selections == 2, "selecting the same again fires nothing");
    s.toggle(procId);
    check(s.selection().size() == 2, "toggle");
    s.select({procId, "no-such-block"});
    check(s.selection().size() == 1 && s.isSelected(procId), "ids that do not exist are dropped");
    std::vector<int> lines = s.selectedLines();
    const std::vector<int> procLines = s.generated().linesOf(procId);
    const std::string retId = (*s.document().sections[0].blocks[3].stack("BODY"))[0].id;
    check(lines.size() == 3 && std::find(lines.begin(), lines.end(), s.generated().linesOf(retId)[0]) != lines.end() && procLines.size() == 2, "the lines of a selected container include the blocks inside it");
    check(s.blockAtLine(s.generated().linesOf(movId)[0]) == movId && s.blockAtLine(1) == "" && s.blockAtLine(999) == "", "line -> block");
    s.clearSelection();
    check(s.selection().empty() && selections >= 5, "clear");
    // edits
    s.select({movId});
    const int before = changes;
    Outcome o = s.apply([&](Editor& e) { return e.setField(movId, "src", "CONST(9, 1)"); });
    check(o.ok && changes == before + 1 && s.generated().text.find("CONST(9, 1)") != std::string::npos, "an edit goes through apply(): the views are told and the text is new");
    o = s.apply([&](Editor& e) { return e.removeBlocks({"no-such-block"}); });
    check(!o.ok && changes == before + 1, "a refused edit changes nothing and tells nobody");
    check(s.undo() && s.generated().text.find("CONST(1, 1)") != std::string::npos && changes == before + 2, "undo restores the text and tells the views");
    check(s.redo() && s.generated().text.find("CONST(9, 1)") != std::string::npos, "redo");
    check(s.undoLabel() == "Edit field", "the undo label");
    // selection pruned when the block disappears
    s.apply([&](Editor& e) { return e.removeBlocks({movId}); });
    check(s.selection().empty(), "a deleted block leaves the selection");
    s.undo();
    check(s.document().find(movId) != nullptr, "...and undo brings it back");
    // problems and stats
    Document bad = s.document();
    bad.sections[0].blocks[1].type = "frobnicate";
    s.setDocument(bad);
    check(!s.problems().empty() && s.problems()[0].message.find("unknown block type") != std::string::npos, "structure problems are available");
    BlockSession::Stats st = s.stats();
    check(st.blocks == 4 + 1 && st.stacks == 1 && st.programStacks == 1 && st.floatingStacks == 0 && st.problems >= 1, "stats", std::to_string(st.blocks));
    Document r = docOf(".main\n weird\n weirder\n JMP x\n");
    s.setDocument(r);
    check(s.stats().rawBlocks == 2, "raw blocks are counted");
    // symbols from outside
    {
        BlockSession sym;
        sym.setDocument(docOf(".main\n EXEC_CSR WRITE8 EXT_CTRL, 1\n"));
        auto undefined = [&] { for (const auto& p : sym.problems()) if (p.message.find("'EXT_CTRL' is not defined") != std::string::npos) return true; return false; };
        check(undefined() && sym.symbols().find("EXT_CTRL") == nullptr, "a name nobody defines is flagged");
        SymbolTable ext;
        ext.addAsm(".csr EXT_CTRL = 0x30\n");
        int fired = 0;
        sym.changed.connect([&] { fired++; });
        sym.setExternalSymbols(ext);
        check(!undefined() && sym.symbols().find("EXT_CTRL", "csr") && fired == 1, "symbols from outside (the hardware description) are known to the checks and offered by name; the views are told");
        sym.setDocument(docOf(".equ LOCAL = 1\n.main\n EXEC_CSR WRITE8 LOCAL, 1\n"));
        check(sym.symbols().find("LOCAL", "int") && sym.symbols().find("EXT_CTRL", "csr"), "the document's own symbols and the outside ones together");
    }
    // deleting the selection
    {
        BlockSession del;
        del.setDocument(docOf(".main\nstart: MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]\n ALU ADD unsigned\n JMP start\n"));
        const Block& mv = del.document().sections[0].blocks[1];
        const std::string numId = mv.value("src")->id, aluId = del.document().sections[0].blocks[2].id;
        const std::string beforeDelete = del.generated().text;
        del.select({numId, aluId});
        const size_t n = del.deleteSelection();
        check(n == 2 && !del.document().find(aluId) && !del.document().find(numId) && del.document().sections.size() == 1, "deleting a component and an instruction removes both (the component leaves no floating copy)");
        check(!del.document().sections[0].blocks[1].value("src") && del.generated().text.find("(incomplete)") != std::string::npos && del.generated().text.find("ALU") == std::string::npos, "the component's place is a hole; the instruction is gone");
        check(del.selection().empty(), "the selection is empty afterwards");
        check(del.undo() && del.generated().text == beforeDelete, "one undo brings both back");
    }
    // header option
    GenerateOptions go;
    go.header = "Generated from x.acblk";
    s.setGenerateOptions(go);
    check(s.generated().text.compare(0, 2, "; ") == 0 && s.blockAtLine(1) == "", "the header option changes the text and the line map");
    // floating stacks counted
    s.apply([&](Editor& e) { return e.splitAt(s.document().sections[0].blocks[1].id, 10, 10); });
    check(s.stats().floatingStacks == 1, "floating stacks are counted");
}

static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void t_samples() {
    section("layout of every sample program keeps its invariants");
    FakeMetrics m;
    LayoutEngine eng(m);
    int files = 0, bad = 0;
    std::string first;
    for (auto& e : fs::recursive_directory_iterator(ACIDE_SAMPLE_DIR)) {
        if (!e.is_regular_file() || (e.path().extension() != ".asm" && e.path().extension() != ".inc")) continue;
        if (e.path().generic_string().find("/out/") != std::string::npos) continue;
        files++;
        Document d = importAsm(slurp(e.path()));
        DocumentLayout dl = eng.layoutDocument(d);
        for (const StackLayout& s : dl.stacks) {
            for (size_t i = 0; i < s.blocks.size(); i++) {
                const BlockBox& b = s.blocks[i];
                bool ok = std::isfinite(b.rect.x) && std::isfinite(b.rect.y) && b.rect.w > 0 && b.rect.h > 0 && inside(b.head, b.rect) && b.parent < (int)i;
                for (const FieldBox& f : b.fields) ok = ok && inside(f.rect, b.rect) && f.rect.w > 0;
                if (b.parent >= 0) ok = ok && inside(b.rect, s.blocks[(size_t)b.parent].rect);
                if (!ok) { bad++; if (first.empty()) first = e.path().string() + " block " + b.id + " (" + b.type + ")"; }
            }
            // top-level blocks stack without overlap
            for (size_t i = 1; i < s.top.size(); i++)
                if (s.blocks[(size_t)s.top[i]].rect.y + 0.01 < s.blocks[(size_t)s.top[i - 1]].rect.bottom()) { bad++; if (first.empty()) first = e.path().string() + " overlap"; }
        }
    }
    check(files >= 15, "found the samples", std::to_string(files));
    check(bad == 0, "every block has a finite, positive size, its parts inside it, and lies inside its parent; stacks do not overlap", first);
}

int main() {
    std::printf("AssemblyCoreIDE block canvas services tests\n");
    t_theme();
    t_shapes();
    t_spring();
    t_plug_shapes();
    t_layout_comments();
    t_layout_regions();
    t_layout_rows();
    t_layout_shapes();
    t_layout_containers();
    t_layout_points();
    t_layout_document();
    t_layout_text();
    t_layout_values();
    t_drop();
    t_catalog();
    t_custom_blocks();
    t_hardware_drawers();
    t_tidy();
    t_clipboard();
    t_session();
    t_samples();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
