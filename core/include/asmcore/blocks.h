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

enum class FieldKind { Text, Int, Choice, Flag };

struct FieldDef {
    std::string name;
    std::string label;                       // shown on the block
    FieldKind kind = FieldKind::Text;
    std::vector<std::string> options;        // Choice: the usual values (a drop-down)
    bool open = false;                       // Choice: other values are allowed (they are kept; validation only notes them)
    bool optional = false;                   // may be empty
    std::string def;                         // value of a new block
    std::string help;
};

constexpr int kMain = 1, kPeri = 2, kBoth = 3;       // same bits as lang::DomMask

struct BlockDef {
    std::string type;                        // "mov", "alu", "csr_write8", ... (stable: stored in files)
    std::string title;                       // "move"
    std::string category;                    // toolbox drawer: "Data", "Math", "Control", "CSR", "Peripheral", "Structure", "Definitions", "Text"
    int domains = kBoth;
    std::vector<FieldDef> fields;
    std::vector<std::string> stacks;         // names of statement slots (".proc" has "BODY")
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
    std::string comment;                     // end-of-line comment
    bool disabled = false;                   // generated as a comment
    bool collapsed = false;                  // editor state

    const std::string* field(const std::string& name) const;
    std::string get(const std::string& name, const std::string& fallback = "") const;
    void set(const std::string& name, const std::string& value);
    std::vector<Block>* stack(const std::string& name, bool create = false);
    const std::vector<Block>* stack(const std::string& name) const;
};

struct Section {
    std::string domain;                      // "main", "peri", or "" for lines before the first .main / .peri
    std::vector<Block> blocks;
    std::string comment;                     // comment after the .main / .peri directive
};

struct Document {
    int version = 1;
    std::string name;
    std::string hardware;                    // the .achw this program uses (relative path), "" = none
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
Generated generate(const Document& doc, const Registry& reg = Registry::defaults());

// ---- checking ------------------------------------------------------------------------------------------------------------------------
struct BlockProblem {
    bool error = true;
    std::string blockId, message;
};
// Structure only (known type, required fields, a block in a domain it exists in, choices). Whether the program is right is the assembler's
// job: assemble the generated text.
std::vector<BlockProblem> validate(const Document& doc, const Registry& reg = Registry::defaults());

// ---- the .acblk file -----------------------------------------------------------------------------------------------------------------
bool parseDocument(const std::string& text, Document& out, std::string& error);
std::string serializeDocument(const Document& doc);

// ---- helpers for editors: take an operand apart and put it together -----------------------------------------------------------------------
// "MAIN_SCRATCHPAD[0..1]" -> region "MAIN_SCRATCHPAD", first "0", last "1";  "ALU_OPA" -> region only;  "CONST(5, 2)" is not a range (false).
struct Operand {
    std::string region, first, last;
    bool isRange = false;                    // has [a..b] or [a]
};
bool parseOperand(const std::string& text, Operand& out);
std::string formatOperand(const Operand& op);

}  // namespace blocks
}  // namespace asmcore
