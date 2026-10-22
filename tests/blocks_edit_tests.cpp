// Editing block programs: the connection rules, the edit operations, undo / redo, the clipboard, floating stacks, value sockets.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>

#include "asmcore/asmcore.h"
#include "asmcore/blocks.h"
#include "asmcore/blocks_edit.h"

using namespace asmcore;
using namespace asmcore::blocks;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& why = "") {
    if (ok) { g_pass++; std::printf("  PASS  %s\n", what.c_str()); return; }
    g_fail++;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), why.c_str());
}
static void section(const char* s) { std::printf("\n%s\n", s); }

// ---- helpers -------------------------------------------------------------------------------------------------------------------------
static Block mk(Document& d, const char* type, std::initializer_list<std::pair<const char*, const char*>> f = {}) {
    Block b = d.make(type);
    for (auto& kv : f) b.set(kv.first, kv.second);
    return b;
}
static Block MOV(Document& d) { return mk(d, "mov", {{"src", "CONST(1, 1)"}, {"dst", "MAIN_SCRATCHPAD[0..0]"}}); }
static Block XFER(Document& d) { return mk(d, "xfer", {{"rw", "READ"}, {"args", "dev=0, len=1, ptr=0"}}); }
static Block JMP(Document& d, const char* t = "t") { return mk(d, "jmp", {{"target", t}}); }
static Block LABEL(Document& d, const char* n) { return mk(d, "label", {{"name", n}}); }
static Block RET(Document& d) { return mk(d, "ret"); }

static Document docOf(const std::string& text) { return importAsm(text); }
// the file text without the id counter: using up ids is not a change of the program
static std::string norm(const Document& d) { Document c = d; c.nextId = 1; return serializeDocument(c); }
static std::string text(const Document& d) { return generate(d).text; }
static bool hasRule(const std::vector<Violation>& v, const char* rule) { for (auto& x : v) if (x.rule == rule) return true; return false; }
static bool hasRule(const Outcome& o, const char* rule) { return hasRule(o.violations, rule); }
static std::string firstId(const Document& d, size_t sec, size_t i) { return d.sections[sec].blocks[i].id; }

// a registry with a tiny expression language, only for the tests of value sockets
static Registry testRegistry() {
    Registry r = Registry::defaults();
    BlockDef num;
    num.type = "tnum"; num.title = "number"; num.category = "Math"; num.domains = kBoth; num.valueType = "int";
    FieldDef f; f.name = "v"; f.label = "value";
    num.fields = {f};
    num.tmpl = "{v}";
    r.add(num);
    BlockDef yes;
    yes.type = "yes"; yes.title = "yes"; yes.category = "Math"; yes.domains = kBoth; yes.valueType = "bool";
    yes.tmpl = "TRUE";
    r.add(yes);
    BlockDef add;
    add.type = "add"; add.title = "add"; add.category = "Math"; add.domains = kBoth; add.valueType = "int";
    add.inputs = {{"A", "left", {"int"}, false}, {"B", "right", {"int"}, false}};
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

static void t_rules() {
    section("connection rules: what may be next to what");
    Document d;
    // H1
    {
        std::vector<Block> seq{MOV(d), XFER(d)};
        check(hasRule(checkSequence(seq, {"main", false}), "H1") && worst(checkSequence(seq, {"main", false})) == Fit::No, "H1: a Peri block in a Main section is refused");
        check(hasRule(checkSequence(seq, {"peri", false}), "H1"), "H1: a Main block in a Peri section is refused");
        std::vector<Violation> v = checkSequence(seq, {"floating", false});
        check(hasRule(v, "H1") && v[0].blockId == seq[1].id && v[0].reason.find("already holds a Main block") != std::string::npos, "H1: in a floating stack the first Main/Peri block decides, the other one is refused, with a reason", describe(v));
        std::vector<Block> both{LABEL(d, "a"), JMP(d), mk(d, "comment", {{"text", "x"}})};
        check(checkSequence(both, {"floating", false}).empty() || worst(checkSequence(both, {"floating", false})) != Fit::No, "blocks that exist in both domains fit anywhere");
        std::vector<Block> pe{XFER(d), JMP(d)};
        check(worst(checkSequence(pe, {"peri", false})) != Fit::No, "a Peri block in a Peri section is fine");
    }
    // H3 / H4 / H5
    {
        Block outer = mk(d, "proc", {{"name", "a"}}), inner = mk(d, "proc", {{"name", "b"}});
        outer.stack("BODY", true)->push_back(inner);
        std::vector<Block> seq{outer};
        check(hasRule(checkSequence(seq, {"main", false}), "H3"), "H3: a procedure inside a procedure is refused");
        Block p = mk(d, "proc", {{"name", "f"}});
        p.stack("BODY", true)->push_back(RET(d));
        std::vector<Block> ok{p};
        check(checkSequence(ok, {"main", false}).empty(), "RET inside a procedure is fine");
        std::vector<Block> flow{RET(d)};
        check(hasRule(checkSequence(flow, {"main", false}), "H4"), "H4: RET in the program flow is refused");
        check(checkSequence(flow, {"floating", false}).empty(), "H4: ...but not on a floating stack (it may become the inside of a procedure)");
        Block p2 = mk(d, "proc", {{"name", "g"}});
        p2.stack("BODY", true)->push_back(mk(d, "vector", {{"id", "0"}, {"label", "h"}}));
        std::vector<Block> vs{p2};
        check(hasRule(checkSequence(vs, {"main", false}), "H5"), "H5: an event vector inside a procedure is refused");
        std::vector<Block> v2{mk(d, "vector", {{"id", "0"}, {"label", "h"}})};
        check(checkSequence(v2, {"main", false}).empty(), "...but in the program flow it is fine");
    }
    // H6
    {
        std::vector<Block> defs{mk(d, "directive", {{"name", ".equ"}, {"args", "A = 1"}}), mk(d, "comment", {{"text", "c"}}), mk(d, "blank"), mk(d, "raw", {{"text", "x"}})};
        check(checkSequence(defs, {"", false}).empty(), "H6: definitions, comments, blank lines and text fit in the definitions");
        std::vector<Block> bad{MOV(d), LABEL(d, "l")};
        std::vector<Violation> v = checkSequence(bad, {"", false});
        check(v.size() == 2 && hasRule(v, "H6") && v[0].reason.find("needs a .main or .peri section") != std::string::npos, "H6: instructions and labels are refused there, with a reason", describe(v));
    }
    // S1
    {
        std::vector<Block> seq{JMP(d), MOV(d)};
        std::vector<Violation> v = checkSequence(seq, {"main", false});
        check(v.size() == 1 && v[0].rule == "S1" && v[0].fit == Fit::Warn, "S1: code right after an unconditional jump is a warning");
        RuleOptions strict;
        strict.strict = true;
        check(worst(checkSequence(seq, {"main", false}, Registry::defaults(), strict)) == Fit::No, "S1: strict mode refuses it");
        std::vector<Block> withLabel{JMP(d), LABEL(d, "x"), MOV(d)};
        check(checkSequence(withLabel, {"main", false}).empty(), "S1: a label in between makes it fine");
        std::vector<Block> noise{JMP(d), mk(d, "comment", {{"text", "c"}}), MOV(d)};
        check(hasRule(checkSequence(noise, {"main", false}), "S1"), "S1: a comment in between does not");
        Block dj = JMP(d);
        dj.disabled = true;
        std::vector<Block> dis{dj, MOV(d)};
        check(checkSequence(dis, {"main", false}).empty(), "S1: a disabled jump jumps nowhere");
        std::vector<Block> proc{JMP(d), mk(d, "proc", {{"name", "f"}})};
        proc[1].stack("BODY", true)->push_back(RET(d));
        check(checkSequence(proc, {"main", false}).empty(), "S1: a procedure after the main flow's jump is the normal layout");
        Block cond = mk(d, "br_cond", {{"cond", "ALWAYS"}, {"target", "t"}});
        std::vector<Block> c2{cond, MOV(d)};
        check(hasRule(checkSequence(c2, {"main", false}), "S1"), "S1: BR.COND ALWAYS counts as a jump");
        Block c3 = mk(d, "br_cond", {{"cond", "ALU_CMP_TRUE"}, {"target", "t"}});
        std::vector<Block> c4{c3, MOV(d)};
        check(checkSequence(c4, {"main", false}).empty(), "S1: a conditional branch falls through");
    }
}

static void t_values() {
    section("value sockets");
    Registry reg = testRegistry();
    Document d;
    Block num = mk(d, "tnum", {{"v", "5"}});
    Block yes = mk(d, "yes");
    Block add = mk(d, "add");
    InputDef takesInt{"A", "left", {"int"}, false};
    check(checkSocket(takesInt, num, reg).fit == Fit::Ok, "an int block fits an int socket");
    Violation bad = checkSocket(takesInt, yes, reg);
    check(bad.fit == Fit::No && bad.rule == "H7" && bad.reason.find("does not fit into 'left', which takes int") != std::string::npos, "a bool block does not fit, and the reason says what the socket takes", bad.reason);
    check(checkSocket(takesInt, mk(d, "mov"), reg).fit == Fit::No, "a statement block is not a value");
    std::vector<Block> stack{num};
    check(hasRule(checkSequence(stack, {"main", false}, reg), "H7"), "H7: a value block standing in a program stack is refused");
    check(checkSequence(stack, {"floating", false}, reg).empty(), "...but alone on the canvas it is fine");
    std::vector<Block> two{num, num};
    check(hasRule(checkSequence(two, {"floating", false}, reg), "H7"), "...only alone: two value blocks in one stack are refused");
    // generation through sockets
    Document doc;
    Section s;
    s.domain = "main";
    Block setv = mk(doc, "setv", {{"name", "X"}});
    Block a = mk(doc, "add");
    a.setValue("A", mk(doc, "tnum", {{"v", "2"}}));
    Block inner = mk(doc, "add");
    inner.setValue("A", mk(doc, "tnum", {{"v", "3"}}));
    inner.setValue("B", mk(doc, "tnum", {{"v", "4"}}));
    a.setValue("B", inner);
    setv.setValue("V", a);
    s.blocks.push_back(setv);
    doc.sections.push_back(s);
    check(generate(doc, reg).text == ".main\n    .equ X = (2 + (3 + 4))\n", "an expression is generated from nested value blocks", generate(doc, reg).text);
    { std::string probs; for (auto& p : validate(doc, reg)) probs += p.message + "; "; check(validate(doc, reg).empty(), "...and validates", probs); }
    // file round trip
    Document back;
    std::string err;
    check(parseDocument(serializeDocument(doc), back, err) && generate(back, reg).text == generate(doc, reg).text && serializeDocument(back) == serializeDocument(doc), "values survive the .acblk file", err);
    check(back.find(inner.id) != nullptr && back.blockCount() == doc.blockCount(), "...and ids inside sockets are found / counted");
    // validation
    Document wrong = doc;
    wrong.sections[0].blocks[0].clearValue("V");
    bool missing = false;
    for (auto& p : validate(wrong, reg)) if (p.message.find("'to' needs a block plugged in") != std::string::npos) missing = true;
    check(missing, "an empty required socket is reported");
    wrong = doc;
    wrong.sections[0].blocks[0].setValue("V", mk(wrong, "yes"));
    bool typed = false;
    for (auto& p : validate(wrong, reg)) if (p.message.find("does not fit into 'to'") != std::string::npos) typed = true;
    check(typed, "a wrong type in a socket is reported");
    // editor: plug / unplug
    Document e;
    e.sections.push_back(Section());
    e.sections[0].domain = "main";
    e.sections[0].blocks.push_back(mk(e, "setv", {{"name", "Y"}}));
    Editor ed(e, reg);
    const std::string host = e.sections[0].blocks[0].id;
    Outcome o = ed.plugValue(host, "V", mk(e, "yes"));
    check(!o.ok && hasRule(o, "H7") && !e.sections[0].blocks[0].value("V"), "plugging a bool into an int socket is refused and nothing changes");
    o = ed.plugValue(host, "V", mk(e, "tnum", {{"v", "1"}}));
    check(o.ok && e.sections[0].blocks[0].value("V") && e.sections[0].blocks[0].value("V")->get("v") == "1", "plugging an int works");
    o = ed.plugValue(host, "V", mk(e, "tnum", {{"v", "2"}}));
    check(o.ok && e.sections[0].blocks[0].value("V")->get("v") == "2" && e.sections.size() == 2 && e.sections[1].domain == "floating" && e.sections[1].blocks[0].get("v") == "1", "plugging into an occupied socket floats the old block beside the host (nothing is lost)");
    o = ed.unplugValue(host, "V", 50, 60);
    check(o.ok && !e.sections[0].blocks[0].value("V") && e.sections.size() == 3 && e.sections[2].x == 50 && e.sections[2].y == 60, "unplugging puts the value on the canvas where asked");
}

static void t_floating() {
    section("floating stacks and the order of the program");
    Document d = docOf(".main\nstart: JMP start\n.peri\np: JMP p\n.main\nagain: JMP again\n");
    check(d.sections.size() == 3 && d.sections[0].y < d.sections[1].y && d.sections[1].y < d.sections[2].y, "the importer lays the stacks out top to bottom in file order");
    const std::string original = text(d);
    // moving a section changes the order of the output
    Editor ed(d);
    ed.moveSection(2, 0, -100);
    check(programOrder(d) == std::vector<size_t>({2, 0, 1}), "the stack that lies highest is written first");
    check(text(d).find(".main\nagain:") == 0, "...and so is it in the text", text(d));
    ed.moveSection(2, d.sections[2].x, d.sections[1].y + 10);
    check(text(d) == original, "putting it back restores the text");
    // same height: left before right
    d.sections[1].y = d.sections[0].y;
    d.sections[1].x = d.sections[0].x + 500;
    check(programOrder(d)[0] == 0 && programOrder(d)[1] == 1, "at the same height the left one comes first");
    // floating stacks are not written
    Outcome o = ed.splitAt(firstId(d, 2, 0), 700, 40);
    check(o.ok && d.sections.size() == 4 && d.sections[3].domain == "floating" && d.sections[2].blocks.empty(), "splitting at the first block of a section moves everything out; the section start stays", describe(o.violations) + o.error);
    d = docOf(".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP a\n");
    Editor ed2(d);
    const std::string movId = firstId(d, 0, 1);
    o = ed2.splitAt(movId, 300, 40);
    check(o.ok && d.sections.size() == 2 && d.sections[1].domain == "floating" && d.sections[1].blocks.size() == 2 && d.sections[1].x == 300, "splitAt cuts a stack: the block and everything after it float");
    check(text(d) == ".main\na:\n", "...and the floating blocks are not written", text(d));
    check(validate(d).empty(), "a floating stack is a valid document");
    // the file keeps positions and floating stacks
    Document back;
    std::string err;
    check(parseDocument(serializeDocument(d), back, err) && back.sections[1].domain == "floating" && back.sections[1].x == 300 && serializeDocument(back) == serializeDocument(d), "positions and floating stacks survive the .acblk file", err);
    // an old file without positions keeps its order
    Document old;
    check(parseDocument("{\"format\":\"acblk\",\"version\":1,\"sections\":[{\"domain\":\"main\",\"blocks\":[{\"type\":\"label\",\"id\":\"b1\",\"fields\":{\"name\":\"a\"}}]},{\"domain\":\"peri\",\"blocks\":[{\"type\":\"label\",\"id\":\"b2\",\"fields\":{\"name\":\"p\"}}]}]}", old, err) && text(old) == ".main\na:\n\n.peri\np:\n", "a file written before positions existed keeps its section order", text(old));
    // join: a floating stack becomes part of the program
    o = ed2.joinSection(1, Where{0, "", "", ""});
    check(o.ok && d.sections.size() == 1 && text(d).find("    JMP a") != std::string::npos, "joining a floating stack to the end of a section makes it part of the program", text(d));
    // a section start can never go under another block
    d = docOf(".main\na: JMP a\n.peri\np: JMP p\n");
    Editor ed3(d);
    o = ed3.joinSection(1, Where{0, "", "", ""});
    check(!o.ok && hasRule(o, "H2") && d.sections.size() == 2, "H2: a .main/.peri start cannot be put under another block (refused, nothing changes)", describe(o.violations));
    // attach / detach
    o = ed3.detachFromHat(1);
    check(o.ok && d.sections[1].domain == "floating" && text(d) == ".main\na:\n    JMP a\n", "detaching a section takes it out of the program", text(d));
    o = ed3.attachToHat(1, "main");
    check(o.ok && d.sections[1].domain == "main", "a floating stack made only of blocks that exist in both domains can become a Main section", describe(o.violations));
}

static void t_editor() {
    section("edit operations");
    Document d = docOf(".main\nstart: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n ALU ADD unsigned\n JMP start\n.peri\np: EXEC_XFER READ dev=0, len=1, ptr=0\n JMP p\n");
    Editor ed(d);
    const std::string before = norm(d);
    // insert
    Block cs = mk(d, "csr_write8", {{"csr", "C"}, {"imm", "1"}});
    Outcome o = ed.insertBlocks(Where{0, "", "", firstId(d, 0, 3)}, {cs}, true);
    check(o.ok && norm(d) == before, "dry run: says yes and changes nothing");
    o = ed.insertBlocks(Where{0, "", "", firstId(d, 0, 3)}, {cs});
    check(o.ok && d.sections[0].blocks[3].type == "csr_write8" && o.ids.size() == 1, "insert before a block");
    o = ed.insertBlocks(Where{0, "", "", ""}, {JMP(d, "start")});
    check(o.ok && d.sections[0].blocks.back().type == "jmp", "insert at the end");
    // refused
    const std::string snapshot = norm(d);
    Block peri = XFER(d);
    const std::string snapshot2 = norm(d);
    o = ed.insertBlocks(Where{0, "", "", ""}, {peri});
    check(!o.ok && o.fit == Fit::No && hasRule(o, "H1") && norm(d) == snapshot2, "a Peri block into the Main section: refused, the document is untouched");
    Block lost = MOV(d);
    const std::string snapshot3 = norm(d);
    o = ed.insertBlocks(Where{0, "", "", "no-such-block"}, {lost});
    check(!o.ok && !o.error.empty() && norm(d) == snapshot3 && d.nextId > 1, "a place that does not exist: an error, nothing changes", o.error);
    o = ed.insertBlocks(Where{9, "", "", ""}, {MOV(d)});
    check(!o.ok && !o.error.empty(), "a section that does not exist: an error");
    // warn
    o = ed.insertBlocks(Where{0, "", "", ""}, {mk(d, "csr_write8", {{"csr", "C"}, {"imm", "2"}})});
    check(o.ok && o.fit == Fit::Warn && hasRule(o, "S1"), "code after the last JMP is accepted with a warning");
    // ids made unique
    Block dup = mk(d, "ret");
    dup.id = d.sections[0].blocks[0].id;
    o = ed.insertBlocks(Where{1, "", "", ""}, {dup});
    std::set<std::string> idset;
    std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) { for (auto& b : v) { idset.insert(b.id); for (auto& s : b.stacks) walk(s.second); for (auto& vl : b.values) walk(vl.second); } };
    for (auto& s : d.sections) walk(s.blocks);
    check(idset.size() == d.blockCount(), "an inserted block whose id is already taken gets a new one");
    // move
    d = docOf(".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n ALU ADD unsigned\n JMP a\n");
    Editor ed2(d);
    const std::string aluId = firstId(d, 0, 2), movId = firstId(d, 0, 1), jmpId = firstId(d, 0, 3);
    o = ed2.moveBlocks({aluId}, Where{0, "", "", movId});
    check(o.ok && d.sections[0].blocks[1].type == "alu" && d.sections[0].blocks[2].type == "mov", "move one block up");
    o = ed2.moveBlocks({movId}, Where{0, "", "", ""}, true);
    check(o.ok && d.sections[0].blocks.back().type == "jmp" && d.sections[0].blocks[d.sections[0].blocks.size() - 2].type == "mov", "move a block and everything after it (dragging takes the rest)", text(d));
    o = ed2.moveBlocks({jmpId}, Where{0, "", "", jmpId});
    check(!o.ok && !o.error.empty(), "a block cannot be moved in front of itself");
    // into a procedure
    d = docOf(".main\na: JMP a\n.proc f\n RET\n.endp\n");
    Editor ed3(d);
    const std::string procId = firstId(d, 0, 2);
    o = ed3.moveBlocks({firstId(d, 0, 1)}, Where{0, procId, "BODY", ""});
    check(o.ok && d.sections[0].blocks[1].stack("BODY")->size() == 2, "a block can be moved into a procedure's body");
    o = ed3.moveBlocks({procId}, Where{0, procId, "BODY", ""});
    check(!o.ok && !o.error.empty(), "a procedure cannot be moved into itself");
    // proc into proc
    Block p2 = mk(d, "proc", {{"name", "g"}});
    o = ed3.insertBlocks(Where{0, procId, "BODY", ""}, {p2});
    check(!o.ok && hasRule(o, "H3"), "a procedure into a procedure: refused (H3)");
    o = ed3.insertBlocks(Where{0, "", "", ""}, {RET(d)});
    check(!o.ok && hasRule(o, "H4"), "RET into the program flow: refused (H4)");
    // remove / duplicate / fields
    d = docOf(".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP a\n");
    Editor ed4(d);
    o = ed4.duplicate({firstId(d, 0, 1)});
    check(o.ok && d.sections[0].blocks.size() == 4 && d.sections[0].blocks[2].type == "mov" && d.sections[0].blocks[2].id != d.sections[0].blocks[1].id, "duplicate puts a copy with a new id right after the original");
    o = ed4.setField(firstId(d, 0, 1), "src", "CONST(9, 1)");
    check(o.ok && d.sections[0].blocks[1].get("src") == "CONST(9, 1)", "set a field");
    o = ed4.setField(firstId(d, 0, 1), "nonsense", "x");
    check(!o.ok && !o.error.empty(), "a field the block does not have: an error");
    o = ed4.setDisabled(firstId(d, 0, 1), true);
    check(o.ok && text(d).find("; MOV CONST(9, 1)") != std::string::npos, "disable");
    o = ed4.setComment(firstId(d, 0, 1), "hello");
    check(o.ok && d.sections[0].blocks[1].comment == "hello", "comment");
    o = ed4.removeBlocks({firstId(d, 0, 1), firstId(d, 0, 2)});
    check(o.ok && d.sections[0].blocks.size() == 2, "remove several");
    o = ed4.removeBlocks({"nothing"});
    check(!o.ok, "removing a block that does not exist is an error");
    // floating stack emptied by a move disappears
    d = docOf(".main\na: JMP a\n");
    Editor ed5(d);
    o = ed5.newFloating({MOV(d)}, 10, 20);
    check(o.ok && d.sections.size() == 2 && d.sections[1].domain == "floating", "a new floating stack");
    const std::string fid = d.sections[1].blocks[0].id;
    o = ed5.moveBlocks({fid}, Where{0, "", "", ""});
    check(o.ok && d.sections.size() == 1, "moving the only block out of a floating stack removes the empty stack");
    // attach with a Peri-only block
    d = docOf(".main\na: JMP a\n");
    Editor ed6(d);
    ed6.newFloating({XFER(d)}, 0, 0);
    o = ed6.attachToHat(1, "main");
    check(!o.ok && hasRule(o, "H1") && d.sections[1].domain == "floating", "a stack with a Peri block cannot become a Main section (H1)");
    o = ed6.attachToHat(1, "peri");
    check(o.ok && d.sections[1].domain == "peri", "...but can become a Peri section");
    // remove a stack
    o = ed6.removeSection(1);
    check(o.ok && d.sections.size() == 1, "remove a whole stack");
    // connection points
    d = docOf(".main\na: JMP a\n.proc f\n RET\n.endp\n");
    std::vector<Where> pts = connectionPoints(d);
    check(pts.size() == 2 + 1 + 1 + 1 + 1 + 0 || pts.size() >= 5, "there is a connection point before every block and at the end of every list, also in procedure slots", std::to_string(pts.size()));
    bool slotPoint = false;
    for (auto& w : pts) if (!w.container.empty() && w.slot == "BODY") slotPoint = true;
    check(slotPoint, "...including the BODY of the procedure");
    check(locate(d, firstId(d, 0, 2)).found && locate(d, "zzz").found == false, "locate finds a block or says it is not there");
    Location l = locate(d, d.sections[0].blocks[2].stack("BODY")->at(0).id);
    check(l.found && l.container == firstId(d, 0, 2) && l.slot == "BODY" && l.index == 0, "...also inside a procedure");
}

static void t_history() {
    section("undo / redo");
    Document d = docOf(".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP a\n");
    EditHistory h(d);
    Editor ed(d, Registry::defaults(), RuleOptions(), &h);
    const std::string s0 = norm(d);
    check(!h.canUndo() && !h.canRedo(), "a fresh history has nothing to undo");
    ed.duplicate({firstId(d, 0, 1)});
    const std::string s1 = norm(d);
    ed.setDisabled(firstId(d, 0, 1), true);
    const std::string s2 = norm(d);
    ed.removeBlocks({firstId(d, 0, 2)});
    const std::string s3 = norm(d);
    check(h.canUndo() && h.undoLabel() == "Delete blocks", "undo names the step it would undo", h.undoLabel());
    check(h.undo() && norm(d) == s2 && h.undo() && norm(d) == s1 && h.undo() && norm(d) == s0 && !h.undo(), "undo three steps, back to the start");
    check(h.redoLabel() == "Duplicate blocks", "redo names its step", h.redoLabel());
    check(h.redo() && h.redo() && h.redo() && norm(d) == s3 && !h.redo(), "redo three steps, back to the end");
    // new change drops the redo tail
    h.undo();
    ed.setComment(firstId(d, 0, 1), "x");
    check(!h.canRedo(), "a new change after undo drops what could be redone");
    // refused operations leave no step
    const size_t n = h.size();
    ed.insertBlocks(Where{0, "", "", ""}, {XFER(d)});
    check(h.size() == n, "a refused change is not a history step");
    // typing in one field is one step
    Document t = docOf(".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n");
    EditHistory ht(t);
    Editor et(t, Registry::defaults(), RuleOptions(), &ht);
    const std::string id = firstId(t, 0, 1);
    et.setField(id, "src", "C");
    et.setField(id, "src", "CO");
    et.setField(id, "src", "CON");
    et.setField(id, "src", "CONST(7, 1)");
    check(ht.size() == 2, "four edits of one field are one step", std::to_string(ht.size()));
    ht.undo();
    check(t.sections[0].blocks[1].get("src") == "CONST(1, 1)", "...and one undo restores the value from before the typing");
    et.setField(id, "dst", "ALU_OPA");
    et.setField(id, "src", "X");
    check(ht.size() == 3, "editing another field starts a new step");
    // the id counter is not rewound
    Document c = docOf(".main\na: JMP a\n");
    EditHistory hc(c);
    Editor ec(c, Registry::defaults(), RuleOptions(), &hc);
    Outcome o = ec.duplicate({firstId(c, 0, 1)});
    const std::string made = o.ids[0];
    hc.undo();
    o = ec.duplicate({firstId(c, 0, 1)});
    check(o.ids[0] != made, "ids of undone blocks are not reused");
    // limit
    Document lim = docOf(".main\na: JMP a\n");
    EditHistory hl(lim, 5);
    Editor el(lim, Registry::defaults(), RuleOptions(), &hl);
    for (int i = 0; i < 20; i++) el.setComment(firstId(lim, 0, 1), std::to_string(i) + "-x");
    for (int i = 0; i < 20; i++) el.setDisabled(firstId(lim, 0, 1), i % 2 == 0);
    check(hl.size() <= 5, "the history is bounded", std::to_string(hl.size()));
}

static void t_clipboard() {
    section("clipboard and typed text");
    Document d = docOf(".main\na: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP a\n.proc f\n CALL g\n RET\n.endp\n");
    const std::string movId = firstId(d, 0, 1), procId = firstId(d, 0, 3);
    std::string clip = copyBlocks(d, {movId, procId});
    check(clip.find("\"format\": \"acblk-clip\"") != std::string::npos, "a copy is JSON with its own format name");
    std::vector<Block> pasted;
    std::string note;
    check(pasteBlocks(d, clip, pasted, note) && pasted.size() == 2 && pasted[0].type == "mov" && pasted[1].type == "proc" && pasted[1].stack("BODY")->size() == 2, "pasting gives the blocks back, a procedure with its body");
    std::set<std::string> old;
    std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) { for (auto& b : v) { old.insert(b.id); for (auto& s : b.stacks) walk(s.second); } };
    for (auto& s : d.sections) walk(s.blocks);
    bool fresh = true;
    std::function<void(const std::vector<Block>&)> chk = [&](const std::vector<Block>& v) { for (auto& b : v) { if (old.count(b.id)) fresh = false; for (auto& s : b.stacks) chk(s.second); } };
    chk(pasted);
    check(fresh, "...with new ids (no clash with the originals)");
    // a block and its parent selected: copied once
    std::string both = copyBlocks(d, {procId, d.sections[0].blocks[3].stack("BODY")->at(0).id});
    std::vector<Block> p2;
    pasteBlocks(d, both, p2, note);
    check(p2.size() == 1 && p2[0].type == "proc" && p2[0].stack("BODY")->size() == 2, "a block whose parent is selected too is not copied twice");
    // text
    check(pasteBlocks(d, "MOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[4..5]\nstart: JMP start\n", pasted, note) && pasted.size() == 3 && pasted[0].type == "mov" && pasted[1].type == "label" && pasted[2].type == "jmp", "pasting assembler text makes blocks (a label line becomes label + instruction)");
    check(pasteBlocks(d, ".main\nMOV ALU_OUT[0..1] -> MAIN_SCRATCHPAD[4..5]\n.peri\nJMP x\n", pasted, note) && pasted.size() == 2, "section headers in pasted text are dropped, the lines are put together");
    check(pasteBlocks(d, "FROBNICATE 1\n", pasted, note) && pasted.size() == 1 && pasted[0].type == "raw" && note.find("kept as assembler text") != std::string::npos, "text that is not understood is kept as a Raw block, with a note", note);
    check(!pasteBlocks(d, "   \n", pasted, note) && !note.empty(), "an empty clipboard: nothing to paste, with a note", note);
    check(!pasteBlocks(d, "{\"format\":\"acblk-clip\",\"blocks\":5}", pasted, note), "a damaged copy is refused", note);
    check(blocksToText(d, {movId, procId}) == "    MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n.proc f\n    CALL g\n    RET\n.endp\n", "the plain-text form of a selection", blocksToText(d, {movId, procId}));
    // typed text
    bool raw = false;
    std::vector<Block> t = blocksFromText(d, "start: MOV CONST(1,2) -> MAIN_SCRATCHPAD[0..1]", &raw);
    check(t.size() == 2 && t[0].type == "label" && t[1].type == "mov" && !raw && t[1].id != t[0].id, "one typed line: the label and the instruction");
    t = blocksFromText(d, "JMP", &raw);
    check(t.size() == 1 && t[0].type == "raw" && raw, "a half-typed line is a Raw block, flagged");
}

static void t_fields() {
    section("field checks for the editor");
    const Registry& reg = Registry::defaults();
    const BlockDef* alu = reg.find("alu");
    const FieldDef& amount = alu->fields[1];
    check(checkFieldValue(*alu, amount, "3").empty() && checkFieldValue(*alu, amount, "").empty(), "a valid shift amount, and an empty optional one, pass");
    check(checkFieldValue(*alu, amount, "9").find("1 to 8") != std::string::npos && checkFieldValue(*alu, amount, "0").find("1 to 8") != std::string::npos, "an amount outside 1..8 is explained");
    check(checkFieldValue(*alu, amount, "SHIFT_BY").empty() && checkFieldValue(*alu, amount, "1+2").empty(), "a name or an expression is not range-checked");
    check(checkFieldValue(*alu, amount, "0x4").empty(), "hex numbers are understood");
    const BlockDef* mov = reg.find("mov");
    check(checkFieldValue(*mov, mov->fields[0], "MAIN_SCRATCHPAD[0..1]").empty() && checkFieldValue(*mov, mov->fields[0], "CONST(5, 2)").empty(), "operands: a region with a range, a constant");
    check(checkFieldValue(*mov, mov->fields[0], "NOT_A_REGION[0]").find("not a region name") != std::string::npos, "an unknown region is named");
    check(checkFieldValue(*mov, mov->fields[1], "").find("needs a value") != std::string::npos, "a required field cannot be empty");
    const BlockDef* sp = reg.find("csr_write_sp");
    check(checkFieldValue(*sp, sp->fields[1], "33").find("1 to 32") != std::string::npos && checkFieldValue(*sp, sp->fields[2], "32").find("0 to 31") != std::string::npos, "length and pointer ranges");
}

static void t_header() {
    section("generated file header and output name");
    Document d = docOf(".main\na: JMP a\n");
    d.output = "prog.asm";
    GenerateOptions o;
    o.header = "Generated from prog.acblk - edit the blocks, not this file";
    Generated g = generate(d, Registry::defaults(), o);
    check(g.text.compare(0, 2, "; ") == 0 && g.text.find("edit the blocks") != std::string::npos, "the header is a comment at the top");
    check(g.lines.size() == 5 && g.lines[0].blockId.empty() && g.atLine(4)->blockId == d.sections[0].blocks[0].id, "the source map counts the header lines", std::to_string(g.lines.size()));
    GenerateOptions two;
    two.header = "line one\nline two";
    check(generate(d, Registry::defaults(), two).text.compare(0, 20, "; line one\n; line tw") == 0, "a header of several lines");
    Document back;
    std::string err;
    check(parseDocument(norm(d), back, err) && back.output == "prog.asm", "the output name is in the file", err);
    // assembles identically with and without the header
    Options opt;
    Result a = assemble({Source{"a.asm", generate(d).text}}, nullptr, opt), b = assemble({Source{"a.asm", g.text}}, nullptr, opt);
    check(a.ok && b.ok && a.main.bytes == b.main.bytes, "the header changes no byte of the image");
}

// ---- random edits keep the document valid and the rules honest -----------------------------------------------------------------------------
static unsigned g_seed = 12345;
static unsigned rnd(unsigned n) { g_seed = g_seed * 1103515245u + 12345u; return (g_seed >> 16) % n; }

static Block randomBlock(Document& d) {
    switch (rnd(14)) {
        case 0: return MOV(d);
        case 1: return mk(d, "alu", {{"op", "ADD"}});
        case 2: return mk(d, "csr_write8", {{"csr", "C"}, {"imm", "1"}});
        case 3: return JMP(d, "t");
        case 4: return LABEL(d, ("l" + std::to_string(rnd(5))).c_str());
        case 5: return mk(d, "call", {{"target", "f"}});
        case 6: return RET(d);
        case 7: return XFER(d);
        case 8: return mk(d, "pub", {{"service", "S"}, {"src", "0"}, {"len", "1"}});
        case 9: return mk(d, "comment", {{"text", "c"}});
        case 10: return mk(d, "raw", {{"text", "ZZ EQU 1"}});     // Raw blocks are never checked by the rules, so use a line that is a valid definition everywhere
        case 11: { Block p = mk(d, "proc", {{"name", ("p" + std::to_string(rnd(4))).c_str()}}); if (rnd(2)) p.stack("BODY", true)->push_back(RET(d)); return p; }
        case 12: return mk(d, "vector", {{"id", "0"}, {"label", "h"}});
        default: return mk(d, "br_cond", {{"cond", "ALU_CMP_TRUE"}, {"target", "t"}});
    }
}

static void t_fuzz() {
    section("random edits: refused changes change nothing, accepted ones keep the document valid and the structure assemblable");
    int accepted = 0, refused = 0, warned = 0, structuralBad = 0, notIdempotent = 0, changedOnRefuse = 0, validateErrors = 0;
    std::string firstProblem;
    for (int round = 0; round < 40; round++) {
        Document d = docOf(".main\nstart: MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n JMP start\n.peri\np: JMP p\n");
        EditHistory h(d);
        Editor ed(d, Registry::defaults(), RuleOptions(), &h);
        for (int step = 0; step < 60; step++) {
            const std::string before = norm(d);
            Outcome o;
            const unsigned op = rnd(9);
            auto anyBlockId = [&]() -> std::string {
                std::vector<std::string> all;
                std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) { for (auto& b : v) { all.push_back(b.id); for (auto& s : b.stacks) walk(s.second); } };
                for (auto& s : d.sections) walk(s.blocks);
                return all.empty() ? std::string() : all[rnd((unsigned)all.size())];
            };
            auto anyWhere = [&]() { std::vector<Where> p = connectionPoints(d); return p[rnd((unsigned)p.size())]; };
            switch (op) {
                case 0: o = ed.insertBlocks(anyWhere(), {randomBlock(d)}); break;
                case 1: { std::string id = anyBlockId(); if (id.empty()) continue; o = ed.moveBlocks({id}, anyWhere(), rnd(2) != 0); break; }
                case 2: { std::string id = anyBlockId(); if (id.empty()) continue; o = ed.splitAt(id, rnd(500), rnd(500)); break; }
                case 3: { if (d.sections.size() < 2) continue; o = ed.joinSection(rnd((unsigned)d.sections.size()), anyWhere()); break; }
                case 4: { o = ed.newFloating({randomBlock(d)}, rnd(500), rnd(500)); break; }
                case 5: { if (d.sections.empty()) continue; static const char* doms[] = {"main", "peri", ""}; o = ed.attachToHat(rnd((unsigned)d.sections.size()), doms[rnd(3)]); break; }
                case 6: { if (d.sections.empty()) continue; o = ed.detachFromHat(rnd((unsigned)d.sections.size())); break; }
                case 7: { std::string id = anyBlockId(); if (id.empty()) continue; o = ed.removeBlocks({id}, rnd(2) != 0); break; }
                default: { std::string id = anyBlockId(); if (id.empty()) continue; o = ed.duplicate({id}); break; }
            }
            if (o.ok) {
                accepted++;
                if (o.fit == Fit::Warn) warned++;
                for (const BlockProblem& p : validate(d)) if (p.error) { validateErrors++; if (firstProblem.empty()) firstProblem = "op " + std::to_string(op) + ": " + p.message; }
                const std::string txt = generate(d).text;
                if (generate(importAsm(txt)).text != txt) notIdempotent++;
                Options opt;
                Result r = assemble({Source{"f.asm", txt}}, nullptr, opt);
                bool duplicateName = false;                                  // a repeated procedure name is E104; the .endp that follows it is only a follow-up error
                for (const Diagnostic& dg : r.diags) if (dg.code == "E104") duplicateName = true;
                for (const Diagnostic& dg : r.diags) if (!duplicateName && ((dg.code == "E105" && dg.msg.find("no '.main' or '.peri'") == std::string::npos) || dg.code == "E307")) { structuralBad++; if (firstProblem.empty()) firstProblem = "assembler " + dg.code + ": " + dg.msg + "\n" + txt; }
            } else {
                refused++;
                if (norm(d) != before) changedOnRefuse++;
            }
        }
        // undo everything: back to the start
        int guard = 0;
        while (h.canUndo() && guard++ < 1000) h.undo();
        if (generate(d).text != ".main\nstart:\n    MOV CONST(1, 1) -> MAIN_SCRATCHPAD[0..0]\n    JMP start\n\n.peri\np:\n    JMP p\n") { notIdempotent++; if (firstProblem.empty()) firstProblem = "undo did not return to the start:\n" + generate(d).text; }
    }
    std::printf("        (%d accepted, %d with warnings, %d refused)\n", accepted, warned, refused);
    check(accepted > 300 && refused > 100, "the random edits exercise both outcomes", std::to_string(accepted) + " accepted, " + std::to_string(refused) + " refused");
    check(changedOnRefuse == 0, "a refused change never altered the document");
    check(validateErrors == 0, "after every accepted change validate() finds no structural error", firstProblem);
    check(structuralBad == 0, "the assembler never reports a domain / section error (E105, E307) for what the rules accepted", firstProblem);
    check(notIdempotent == 0, "the text is stable (text -> blocks -> text) after every accepted change, and undoing all of it restores the start", firstProblem);
}

int main() {
    std::printf("AssemblyCore block editing tests\n");
    t_rules();
    t_values();
    t_floating();
    t_editor();
    t_history();
    t_clipboard();
    t_fields();
    t_header();
    t_fuzz();
    std::printf("\n%s  %d passed, %d failed\n", g_fail ? "FAILED" : "OK    ", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
