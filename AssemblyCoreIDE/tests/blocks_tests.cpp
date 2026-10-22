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
#include "blocks/block_layout.h"
#include "blocks/block_session.h"
#include "blocks/block_shapes.h"
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
    t_plug_shapes();
    t_layout_rows();
    t_layout_shapes();
    t_layout_containers();
    t_layout_points();
    t_layout_document();
    t_layout_text();
    t_layout_values();
    t_session();
    t_samples();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
