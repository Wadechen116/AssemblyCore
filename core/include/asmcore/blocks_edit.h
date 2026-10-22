#pragma once
// Editing block programs: which blocks may connect (the Assembly rules), the edit operations the canvas calls, undo / redo, the clipboard.
//
// Everything here works on the model of blocks.h and has no UI. The canvas only asks "may this go there?" (a dry run that returns the reasons,
// which it shows as hatching and a tooltip) and "do it" (which either applies the whole change or changes nothing).
#include <set>
#include <string>
#include <vector>

#include "asmcore/blocks.h"

namespace asmcore {
namespace blocks {

// ---- the connection rules ------------------------------------------------------------------------------------------------------------
enum class Fit { Ok, Warn, No };                    // No: may not connect (hard rule, or a soft rule in strict mode).  Warn: connects, the block gets a warning

struct Violation {
    Fit fit = Fit::No;
    std::string rule;                               // "H1" ... "S1": stable, the UI can translate by rule
    std::string blockId;                            // the block that does not fit
    std::string reason;                             // plain English; `args` are the pieces (block titles ...) for a translated sentence
    std::vector<std::string> args;
};

struct RuleOptions {
    bool strict = false;                            // soft rules (code after an unconditional jump that is not a label) also refuse
};

// Where a sequence of blocks stands: in a section ("main", "peri"), in the definitions at the top of the file (""), or on the canvas
// without a section ("floating"); inProc: inside a .proc body.
struct StackContext {
    std::string domain;
    bool inProc = false;
};

// All the ways `seq` breaks the rules where it stands (nested slots are checked too). Hard rules:
//   H1 a Main-only and a Peri-only block in one stack      H2 (structural: nothing goes above a section start; see Editor::join)
//   H3 a procedure inside a procedure                      H4 RET in the program flow (outside a procedure)
//   H5 an event vector inside a procedure                  H6 anything but definitions and text in the definitions
//   H7 a value block standing in a stack / the wrong type in a socket (checkSocket)
// Soft rule: S1 a block after an unconditional jump / return that is not a label (it may never run).
std::vector<Violation> checkSequence(const std::vector<Block>& seq, const StackContext& ctx, const Registry& reg = Registry::defaults(), const RuleOptions& opt = RuleOptions());
Violation checkSocket(const InputDef& in, const Block& child, const Registry& reg = Registry::defaults());
Fit worst(const std::vector<Violation>& v);
std::string describe(const std::vector<Violation>& v);                     // the reasons, one per line (for a tooltip)

// ---- positions in a document ---------------------------------------------------------------------------------------------------------
// A place blocks can be inserted: in the top-level list of a section (container empty) or in a slot of a container block, before the block
// `before` (empty: at the end).
struct Where {
    size_t section = 0;
    std::string container;                          // id of the container block, "" = the section itself
    std::string slot = "BODY";                      // which slot of the container
    std::string before;                             // id of the sibling to insert in front of, "" = append
};
std::vector<Where> connectionPoints(const Document& doc);                 // every place something could be inserted (before each block, at the end of each list)

struct Location {                                   // where a block is
    bool found = false;
    size_t section = 0;
    std::string container, slot;                    // "" for the top level
    size_t index = 0;
};
Location locate(const Document& doc, const std::string& blockId);

// ---- history (undo / redo) -----------------------------------------------------------------------------------------------------------
// Snapshots of the sections. `record` is called after a change; consecutive records with the same non-empty mergeKey (typing in one field)
// are one step. Undo / redo restore the sections; the id counter is never rewound.
class EditHistory {
public:
    explicit EditHistory(Document& doc, size_t limit = 200);
    void reset();                                                          // forget the history; the document as it is now is the start
    void record(const std::string& label, const std::string& mergeKey = "");
    bool canUndo() const { return pos_ > 0; }
    bool canRedo() const { return pos_ + 1 < states_.size(); }
    std::string undoLabel() const;                                         // what undo would undo ("" if nothing)
    std::string redoLabel() const;
    bool undo();
    bool redo();
    size_t size() const { return states_.size(); }

private:
    struct State { std::vector<Section> sections; std::string label, mergeKey; };
    Document& doc_;
    std::vector<State> states_;
    size_t pos_ = 0, limit_;
};

// ---- the edit operations -------------------------------------------------------------------------------------------------------------
struct Outcome {
    bool ok = false;                                // done (or, for a dry run, would be done)
    Fit fit = Fit::Ok;                              // Warn: done, with warnings in `violations`
    std::vector<Violation> violations;
    std::string error;                              // not a rule problem: a block / section that does not exist, nothing to do ...
    std::vector<std::string> ids;                   // the blocks that were inserted / moved (the new ones, for a paste)
    size_t section = 0;                             // the section an operation created or changed
};

class Editor {
public:
    Editor(Document& doc, const Registry& reg = Registry::defaults(), const RuleOptions& opt = RuleOptions(), EditHistory* history = nullptr);

    // Every mutating call checks the rules first; with dryRun nothing is changed. A refused change leaves the document exactly as it was.
    Outcome insertBlocks(const Where& at, std::vector<Block> blocks, bool dryRun = false);
    Outcome removeBlocks(const std::vector<std::string>& ids, bool takeBelow = false);
    // Move blocks (each with its inner slots) to `to`. takeBelow: also the siblings after each of them (what dragging a block does).
    Outcome moveBlocks(const std::vector<std::string>& ids, const Where& to, bool takeBelow = false, bool dryRun = false);
    // Cut the stack at `id`: that block and its following siblings become a new floating stack at (x, y).
    Outcome splitAt(const std::string& id, double x, double y, bool dryRun = false);
    // Put all blocks of a floating stack into another place. A stack that starts with a .main / .peri / definitions header cannot move (H2).
    Outcome joinSection(size_t source, const Where& to, bool dryRun = false);
    Outcome newFloating(std::vector<Block> blocks, double x, double y);
    Outcome moveSection(size_t index, double x, double y);
    // A floating stack becomes part of the program as a section of `domain` ("main", "peri", or "" for definitions) / the other way round.
    Outcome attachToHat(size_t section, const std::string& domain, bool dryRun = false);
    Outcome detachFromHat(size_t section);
    Outcome duplicate(const std::vector<std::string>& ids, bool dryRun = false);
    Outcome setField(const std::string& id, const std::string& name, const std::string& value);
    Outcome setComment(const std::string& id, const std::string& comment);
    Outcome setDisabled(const std::string& id, bool disabled);
    Outcome setCollapsed(const std::string& id, bool collapsed);
    Outcome plugValue(const std::string& blockId, const std::string& socket, Block child, bool dryRun = false);
    // The component becomes a floating block at (x, y). The socket it leaves is a HOLE (its text mirror is emptied too, so the instruction is incomplete
    // until something else is plugged in); keepText leaves the text where it was.
    Outcome unplugValue(const std::string& blockId, const std::string& socket, double x, double y, bool keepText = false);
    // Drag support. liftBlocks takes a block (and, with takeBelow, everything after it in its stack) out onto the canvas as a new floating stack at (x, y);
    // plugSection plugs the lone component of a floating stack into a socket (the stack goes away). Both can run as a dry run for feedback.
    Outcome liftBlocks(const std::string& id, bool takeBelow, double x, double y, bool dryRun = false);
    Outcome plugSection(size_t source, const std::string& blockId, const std::string& socket, bool dryRun = false);
    // While a group is open, everything the editor records in the history is one step (a drag: lift, move, drop).
    void beginGroup(const std::string& key) { group_ = key; }
    void endGroup() { group_.clear(); }
    const std::string& groupKey() const { return group_; }
    Outcome setSectionComment(size_t section, const std::string& comment);
    Outcome removeSection(size_t section);                                                                  // a floating stack, with everything on it

    Document& document() { return doc_; }
    const RuleOptions& options() const { return opt_; }
    void setOptions(const RuleOptions& o) { opt_ = o; }

private:
    Document& doc_;
    const Registry& reg_;
    RuleOptions opt_;
    EditHistory* history_;
    std::string group_;
    bool plugInto(std::vector<Section>& work, std::set<size_t>& touched, Outcome& out, const std::string& blockId, const std::string& socket, Block child);
    struct Impl;
};

// ---- the clipboard -------------------------------------------------------------------------------------------------------------------
// A selection as text that can be pasted back (JSON, format "acblk-clip"). `ids` are blocks of any depth; a block whose parent is also
// selected is not copied twice; the order is the order in the document.
std::string copyBlocks(const Document& doc, const std::vector<std::string>& ids);
// Blocks from clipboard text: a copyBlocks() string (the blocks get fresh ids), or assembler text (imported; the lines of all sections are
// put together, section headers are dropped). `note` says what happened ("3 lines kept as text"); false if there is nothing to paste.
bool pasteBlocks(Document& doc, const std::string& text, std::vector<Block>& out, std::string& note);
// The assembler text of some blocks (what a plain-text copy puts on the clipboard).
std::string blocksToText(const Document& doc, const std::vector<std::string>& ids);

// Blocks from typed assembler text (one or more lines): "MOV CONST(1,2) -> MAIN_SCRATCHPAD[0..1]", "start: JMP start". anyRaw: some line was not understood
// and became a Raw block.
std::vector<Block> blocksFromText(Document& doc, const std::string& text, bool* anyRaw = nullptr);

// ---- what an editor needs to know about a field ---------------------------------------------------------------------------------------
// "" if the value is fine, otherwise a hint ("shift amount is 1 to 8"). Only plain numbers are range-checked; names and expressions pass.
std::string checkFieldValue(const BlockDef& def, const FieldDef& field, const std::string& value);

}  // namespace blocks
}  // namespace asmcore
