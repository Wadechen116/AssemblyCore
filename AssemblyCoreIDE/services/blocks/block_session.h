#pragma once
// One block program being edited: the document, its edit history, what it generates, what is selected. This is the "presenter" between the
// model (asmcore blocks) and any view: it has no Qt, tells the views what changed through two signals, and every change to the document goes
// through apply(), so a view never has to guess when to redraw and the caches (generated text, structure check) are never stale.
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "asmcore/blocks.h"
#include "asmcore/blocks_edit.h"
#include "signal.h"

namespace acide {
namespace blocks {

class BlockSession {
public:
    explicit BlockSession(const asmcore::blocks::Registry& reg = asmcore::blocks::Registry::defaults());
    BlockSession(const BlockSession&) = delete;
    BlockSession& operator=(const BlockSession&) = delete;

    // ---- the document
    // Replaces the document with the .acblk text. On a damaged file `error` says why and the session is left as it was.
    bool load(const std::string& acblkText, std::string& error);
    void setDocument(const asmcore::blocks::Document& doc);         // e.g. after an import; resets the history and the selection
    const asmcore::blocks::Document& document() const { return doc_; }
    std::string serialize() const { return asmcore::blocks::serializeDocument(doc_, reg_); }
    const asmcore::blocks::Registry& registry() const { return reg_; }

    // ---- editing
    // A drag is one undo step however many edits it makes (lift, move, drop): everything between beginGroup and endGroup is merged in the history.
    void beginGroup(const std::string& key) { editor_.beginGroup(key); }
    void endGroup() { editor_.endGroup(); }
    // What an operation WOULD do (pass dryRun = true to it): never changes anything, never fires a signal. For the feedback while dragging.
    asmcore::blocks::Outcome probe(const std::function<asmcore::blocks::Outcome(asmcore::blocks::Editor&)>& op) { return op(editor_); }
    // Runs an edit operation on the document; if it was done (ok) the caches are dropped, the selection is pruned of blocks that no longer exist
    // and `changed` fires. A refused operation changes nothing and fires nothing.
    asmcore::blocks::Outcome apply(const std::function<asmcore::blocks::Outcome(asmcore::blocks::Editor&)>& op);
    // Deletes the selected blocks: an instruction (with what it holds) is removed from its stack; a component is taken out of its socket and thrown away,
    // which leaves a hole. One undo step. Returns how many were deleted.
    size_t deleteSelection();
    // ---- the clipboard. A copy is the blocks as JSON (to paste back as blocks) and as assembler text (to paste into a text editor).
    struct Clip { std::string blocks, text; };
    Clip copySelection() const;
    size_t cutSelection(Clip& out);                                       // copy, then delete
    // Pastes blocks (a copy of ours, or assembler text). Instructions go under the selected instruction; a lone component goes into an empty socket of the
    // selected block that takes it; otherwise (or with nothing selected) they lie on the canvas at (x, y). The pasted blocks are selected. Returns how many; `note` says what was kept as text.
    size_t paste(const std::string& text, double x, double y, std::string& note);
    size_t duplicateSelection();                                          // each selected instruction twice, the copy right after it; a component: a copy beside it
    size_t toggleDisabled();                                              // the selection is switched off, or on if everything in it was off
    size_t toggleCollapsed();                                             // containers in the selection fold / unfold
    bool undo();
    bool redo();
    std::string undoLabel() const { return history_.undoLabel(); }
    std::string redoLabel() const { return history_.redoLabel(); }
    void setRuleOptions(const asmcore::blocks::RuleOptions& o);

    // ---- what the document generates and how it checks out (cached until the next change)
    void setGenerateOptions(const asmcore::blocks::GenerateOptions& o);
    const asmcore::blocks::Generated& generated() const;
    const std::vector<asmcore::blocks::BlockProblem>& problems() const;
    // Symbols defined outside the document (the hardware description): the names components may use, and the "not defined" checks know them.
    void setExternalSymbols(const asmcore::blocks::SymbolTable& t);
    // The symbols of the document and the outside together (cached until the next change): what a name drop-down offers.
    const asmcore::blocks::SymbolTable& symbols() const;
    struct Stats {
        size_t blocks = 0, stacks = 0, programStacks = 0, floatingStacks = 0, rawBlocks = 0, problems = 0;
    };
    Stats stats() const;

    // ---- selection (block ids)
    const std::set<std::string>& selection() const { return selection_; }
    bool isSelected(const std::string& id) const { return selection_.count(id) != 0; }
    void select(const std::vector<std::string>& ids);              // replaces the selection
    void toggle(const std::string& id);
    void clearSelection();
    // The generated lines of the selected blocks, including everything inside them (the text view highlights them).
    std::vector<int> selectedLines() const;
    // The block that produced a generated line ("" if none).
    std::string blockAtLine(int line) const;

    Signal<> changed;                                              // the document was replaced or edited
    Signal<> selectionChanged;

private:
    void invalidate();
    void pruneSelection();

    const asmcore::blocks::Registry& reg_;
    asmcore::blocks::Document doc_;
    asmcore::blocks::EditHistory history_;
    asmcore::blocks::Editor editor_;
    asmcore::blocks::RuleOptions rules_;
    asmcore::blocks::GenerateOptions genOptions_;
    std::set<std::string> selection_;
    unsigned groupSerial_ = 0;
    asmcore::blocks::SymbolTable external_;
    mutable asmcore::blocks::SymbolTable symbols_;
    mutable bool haveGenerated_ = false, haveProblems_ = false, haveSymbols_ = false;
    mutable asmcore::blocks::Generated generated_;
    mutable std::vector<asmcore::blocks::BlockProblem> problems_;
};

}  // namespace blocks
}  // namespace acide
