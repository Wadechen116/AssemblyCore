#pragma once
// Block programs (the model behind the block editor): a program as a tree of blocks instead of lines of text.
//
//   BlockDef      what a kind of block is: its fields, the text it generates, which domain it may appear in (data, built from the
//                 vocabulary of the language, so a new instruction cannot be forgotten without a test failing)
//   Document      one source file as blocks: sections (.main / .peri), each a list of blocks; blocks may hold statement stacks (.proc)
//   importAsm     text -> blocks. Never fails: a line it does not understand becomes a Raw block that keeps the text verbatim
//   generate      blocks -> assembler text, with the block that produced every line (for diagnostics, breakpoints, "show me the text")
//   serialize     the .acblk file (JSON)
//
// "Level 1" blocks are one per instruction / directive and mirror the text exactly: assemble(generate(importAsm(t))) gives the image of t.
// Higher-level blocks (if / while / variables...) are added as further BlockDefs that lower to Level 1 text; the model does not change.
// Standard C++17, no Qt, no file I/O.
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace asmcore {
namespace blocks {

enum class FieldKind { Text, Int, Choice, Flag, Operand };      // Operand: a region with an optional byte range ("MAIN_SCRATCHPAD[0..1]"), edited as region + numbers

struct FieldDef {
    std::string name;
    std::string label;                       // shown on the block
    FieldKind kind = FieldKind::Text;
    std::vector<std::string> options;        // Choice: the usual values (a drop-down)
    bool open = false;                       // Choice: other values are allowed (they are kept; validation only notes them)
    bool optional = false;                   // may be empty
    std::string def;                         // value of a new block
    std::string help;
    long long lo = 0, hi = 0;                // Int: the range that makes sense (checked only for plain number literals; names and expressions are fine); lo == hi == 0: no range
    std::string when;                        // shown only while the condition holds: "op=SHL,SHR" or "op!=AND,OR" (a field of the same block); "" = always
};

// A value socket: another block plugs into it (an expression). `accepts` are the value types the socket takes.
//
// An input is a *component socket*. If the block has a field of the same name, that field is the socket's text MIRROR: the text of what is
// plugged in (or the text typed when nothing is plugged, which is also what a program from before components exists as). The text is what an
// older tool reads; the components are an addition (see docs/BLOCKS_COMPOSITION.md, "compatibility").
// `derive` makes a socket the source of other fields: "range:ptr,len" -- a plugged region range [first..last] gives ptr = first and
// len = last - first + 1 (so ptr + len <= size holds by construction).
struct InputDef {
    std::string name, label;
    std::vector<std::string> accepts;        // value types: "int", "region:MAIN_SCRATCHPAD", "op.alu", "sign", "cond.main", "cond.peri", "label.flow", "label.proc"
    bool optional = false;
    std::string derive;                      // "range:<ptr field>,<len field>" or ""
    std::string when;                        // as FieldDef::when
    InputDef() = default;
    InputDef(std::string n, std::string l, std::vector<std::string> a = {}, bool opt = false) : name(std::move(n)), label(std::move(l)), accepts(std::move(a)), optional(opt) {}
};

constexpr int kMain = 1, kPeri = 2, kBoth = 3;       // same bits as lang::DomMask

struct BlockDef {
    std::string type;                        // "mov", "alu", "csr_write8", ... (stable: stored in files)
    std::string title;                       // "move"
    std::string category;                    // toolbox drawer: "Data", "Math", "Control", "CSR", "Peripheral", "Structure", "Definitions", "Text"
    int domains = kBoth;
    std::vector<FieldDef> fields;
    std::vector<std::string> stacks;         // names of statement slots (".proc" has "BODY")
    std::vector<InputDef> inputs;            // value sockets
    std::string semantic;                    // how a component block is read: "number", "binop", "unop", "paren", "region" ("" = by its template)
    std::string valueType;                   // non-empty: this block is a VALUE (it plugs into a socket and generates an expression, not a line)
    // Text this block generates. {field} is replaced by the field; {?flag:text} is text only when the Flag field is set;
    // [ ... ] is dropped unless every {field} inside it is non-empty.
    std::string tmpl;
    std::string help, spec;
    bool structural = false;                 // generates more than one line / has its own code in generate()
};

class Registry {
public:
    static const Registry& defaults();       // the Level 1 blocks, options taken from the language vocabulary
    const BlockDef* find(const std::string& type) const;
    BlockDef* edit(const std::string& type);                  // for building a registry
    const std::vector<BlockDef>& all() const { return defs_; }
    void add(const BlockDef& d);
private:
    std::vector<BlockDef> defs_;
};

struct Block {
    std::string id;                          // unique in the document, stable across saves
    std::string type;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<std::pair<std::string, std::vector<Block>>> stacks;
    std::vector<std::pair<std::string, std::vector<Block>>> values;     // value sockets: name -> zero or one block
    std::string comment;                     // end-of-line comment
    bool disabled = false;                   // generated as a comment
    bool collapsed = false;                  // editor state

    const std::string* field(const std::string& name) const;
    std::string get(const std::string& name, const std::string& fallback = "") const;
    void set(const std::string& name, const std::string& value);
    std::vector<Block>* stack(const std::string& name, bool create = false);
    const std::vector<Block>* stack(const std::string& name) const;
    const Block* value(const std::string& socket) const;                 // the block plugged into a socket, or nullptr
    void setValue(const std::string& socket, const Block& b);
    void clearValue(const std::string& socket);
};

struct Section {
    // "main" / "peri": a program section (starts with .main / .peri); "": the definitions at the top of a file (before the first .main / .peri);
    // "floating": a stack lying on the canvas that is NOT part of the program (nothing is generated for it).
    std::string domain;
    std::vector<Block> blocks;
    std::string comment;                     // comment after the .main / .peri directive
    double x = 0, y = 0;                     // where the stack lies on the canvas
};

struct Document {
    int version = 1;
    std::string name;
    std::string hardware;                    // the .achw this program uses (relative path), "" = none
    std::string output;                      // the generated assembler file (relative path), "" = NAME.generated.asm
    std::vector<Section> sections;
    unsigned nextId = 1;

    std::string newId() { return "b" + std::to_string(nextId++); }
    Block make(const std::string& type);                       // a block with a fresh id and the default field values of its definition
    Block* find(const std::string& id);
    const Block* find(const std::string& id) const;
    size_t blockCount() const;
    void assignMissingIds();
};

// ---- text -> blocks ------------------------------------------------------------------------------------------------------------------
struct ImportStats {
    int lines = 0, blocks = 0, raw = 0;
    std::vector<std::string> rawLines;       // the text of the Raw blocks (what the block set does not cover yet)
};
Document importAsm(const std::string& text, const std::string& name = "", ImportStats* stats = nullptr, const Registry& reg = Registry::defaults());

// ---- blocks -> text ------------------------------------------------------------------------------------------------------------------
struct GenLine {
    int line = 0;                            // 1-based line in `text`
    std::string blockId;                     // "" for generated structure (a .main line)
};
struct Generated {
    std::string text;
    std::vector<GenLine> lines;
    const GenLine* atLine(int line) const;                      // which block produced a line
    std::vector<int> linesOf(const std::string& blockId) const;
};
struct GenerateOptions {
    std::string header;                      // comment lines at the top of the file (without the "; "), e.g. "Generated from X.acblk - edit the blocks, not this file"
};
// Program sections are written in canvas order: the definitions first, then the .main / .peri sections from top to bottom (left to right at the
// same height). Floating stacks are not written.
Generated generate(const Document& doc, const Registry& reg = Registry::defaults(), const GenerateOptions& opt = GenerateOptions());
// The sections that generate() writes, in the order it writes them (indexes into doc.sections).
std::vector<size_t> programOrder(const Document& doc);

// ---- checking ------------------------------------------------------------------------------------------------------------------------
struct BlockProblem {
    bool error = true;
    std::string blockId, message;
};
// Structure only (known type, required fields, a block in a domain it exists in, choices). Whether the program is right is the assembler's
// job: assemble the generated text.
std::vector<BlockProblem> validate(const Document& doc, const Registry& reg = Registry::defaults());

// ---- the .acblk file -----------------------------------------------------------------------------------------------------------------
bool parseDocument(const std::string& text, Document& out, std::string& error, const Registry& reg = Registry::defaults());
std::string serializeDocument(const Document& doc, const Registry& reg = Registry::defaults());

// ---- helpers for editors: take an operand apart and put it together -----------------------------------------------------------------------
// "MAIN_SCRATCHPAD[0..1]" -> region "MAIN_SCRATCHPAD", first "0", last "1";  "ALU_OPA" -> region only;  "CONST(5, 2)" is not a range (false).
struct Operand {
    std::string region, first, last;
    bool isRange = false;                    // has [a..b] or [a]
};
bool parseOperand(const std::string& text, Operand& out);
std::string formatOperand(const Operand& op);

// ---- components (stage M2b) ------------------------------------------------------------------------------------------------------------
// Value types a socket can take and a value block gives (BlockDef::valueType, InputDef::accepts).
constexpr const char* kTypeInt = "int";
std::string regionType(const std::string& canonicalRegionName);         // "region:MAIN_SCRATCHPAD"
bool isRegionType(const std::string& type);

// Is a field / socket shown, given the other fields of the block ("when" conditions)?
bool partVisible(const std::string& when, const Block& b);

// The text of a field, or of the component plugged into the socket of the same name, or derived from a socket that has `derive`.
std::string partText(const Block& b, const BlockDef& d, const std::string& name, const Registry& reg = Registry::defaults());

// What a value block stands for in assembler text; `into` is the socket it is plugged into (it decides, e.g., that a number there is a constant).
std::string valueText(const Block& v, const Registry& reg = Registry::defaults(), const InputDef* into = nullptr);

// Text -> components. Splits the text of every operand that has a socket (numbers, expressions, regions with their ranges, constants)
// into blocks, but only where the blocks give back exactly the same text (ignoring spaces); anything else stays as text in the mirror field.
// Returns how many component blocks were made. A damaged / older file never fails: its text is what counts.
int explodeBlock(Block& b, Document& doc, const Registry& reg = Registry::defaults());
int explodeDocument(Document& doc, const Registry& reg = Registry::defaults());
// Brings the components in line with the text mirrors after a file was read: a component whose text differs from its mirror is dropped
// (the text wins -- an older tool may have edited it), then the text is split again.
void reconcileDocument(Document& doc, const Registry& reg = Registry::defaults());

// A number literal as the assembler reads it ("123", "0x7F", "7Fh", "0b101", "101b", "'A'"); false if the text is not one.
bool parseNumberLiteral(const std::string& text, long long& out);

}  // namespace blocks
}  // namespace asmcore
