#include "blocks/block_session.h"

#include <algorithm>
#include <functional>

namespace acide {
namespace blocks {

using namespace asmcore::blocks;

BlockSession::BlockSession(const Registry& reg) : reg_(reg), history_(doc_), editor_(doc_, reg, RuleOptions(), &history_) {}

bool BlockSession::load(const std::string& text, std::string& error) {
    Document d;
    if (!parseDocument(text, d, error, reg_, &external_)) return false;
    setDocument(d);
    return true;
}

void BlockSession::setDocument(const Document& doc) {
    doc_ = doc;
    history_.reset();
    selection_.clear();
    invalidate();
    changed.fire();
    selectionChanged.fire();
}

void BlockSession::invalidate() {
    haveGenerated_ = false;
    haveProblems_ = false;
    haveSymbols_ = false;
}

void BlockSession::setExternalSymbols(const SymbolTable& t) {
    external_ = t;
    invalidate();
    retypeReferences(doc_, reg_, symbols());                       // names that the outside now defines become references (nothing in the text changes)
    invalidate();
    changed.fire();
}

const SymbolTable& BlockSession::symbols() const {
    if (!haveSymbols_) {
        symbols_ = SymbolTable();
        symbols_.addDocument(doc_);
        symbols_.addTable(external_);
        haveSymbols_ = true;
    }
    return symbols_;
}

namespace {
void collect(const Block& b, std::set<std::string>& ids) {
    ids.insert(b.id);
    for (const auto& st : b.stacks) for (const Block& c : st.second) collect(c, ids);
    for (const auto& vl : b.values) for (const Block& c : vl.second) collect(c, ids);
}
std::set<std::string> allIds(const Document& d) {
    std::set<std::string> ids;
    for (const Section& s : d.sections) for (const Block& b : s.blocks) collect(b, ids);
    return ids;
}
const Block* findBlock(const std::vector<Block>& v, const std::string& id) {
    for (const Block& b : v) {
        if (b.id == id) return &b;
        for (const auto& st : b.stacks) if (const Block* r = findBlock(st.second, id)) return r;
        for (const auto& vl : b.values) if (const Block* r = findBlock(vl.second, id)) return r;
    }
    return nullptr;
}
}  // namespace

void BlockSession::pruneSelection() {
    const std::set<std::string> ids = allIds(doc_);
    bool removed = false;
    for (auto it = selection_.begin(); it != selection_.end();) {
        if (!ids.count(*it)) { it = selection_.erase(it); removed = true; }
        else ++it;
    }
    if (removed) selectionChanged.fire();
}

Outcome BlockSession::apply(const std::function<Outcome(Editor&)>& op) {
    Outcome o = op(editor_);
    if (o.ok) {
        invalidate();
        pruneSelection();
        changed.fire();
    }
    return o;
}

namespace {
// the block and the socket a component is plugged into
bool findHost(const std::vector<Block>& v, const std::string& id, std::string& host, std::string& socket) {
    for (const Block& b : v) {
        for (const auto& vl : b.values)
            for (const Block& c : vl.second) {
                if (c.id == id) { host = b.id; socket = vl.first; return true; }
                std::vector<Block> one{c};
                if (findHost(one, id, host, socket)) return true;
            }
        for (const auto& st : b.stacks) if (findHost(st.second, id, host, socket)) return true;
    }
    return false;
}
}  // namespace

size_t BlockSession::deleteSelection() {
    const std::vector<std::string> ids(selection_.begin(), selection_.end());
    size_t n = 0;
    beginGroup("delete:" + std::to_string(++groupSerial_));
    for (const std::string& id : ids) {
        if (!doc_.find(id)) continue;                                     // gone already with the block that held it
        std::string host, socket;
        bool nested = false;
        for (const Section& s : doc_.sections) if (!nested && findHost(s.blocks, id, host, socket)) nested = true;
        asmcore::blocks::Outcome o;
        if (nested) {
            o = apply([&](Editor& e) { return e.unplugValue(host, socket, 0, 0); });
            if (o.ok) o = apply([&](Editor& e) { return e.removeSection(o.section); });
        } else {
            o = apply([&](Editor& e) { return e.removeBlocks({id}, false); });
        }
        if (o.ok) n++;
    }
    endGroup();
    return n;
}

bool BlockSession::undo() {
    if (!history_.undo()) return false;
    invalidate();
    pruneSelection();
    changed.fire();
    return true;
}

bool BlockSession::redo() {
    if (!history_.redo()) return false;
    invalidate();
    pruneSelection();
    changed.fire();
    return true;
}

void BlockSession::setRuleOptions(const RuleOptions& o) {
    rules_ = o;
    editor_.setOptions(o);
}

void BlockSession::setGenerateOptions(const GenerateOptions& o) {
    genOptions_ = o;
    invalidate();
    changed.fire();
}

const Generated& BlockSession::generated() const {
    if (!haveGenerated_) {
        generated_ = generate(doc_, reg_, genOptions_);
        haveGenerated_ = true;
    }
    return generated_;
}

const std::vector<BlockProblem>& BlockSession::problems() const {
    if (!haveProblems_) {
        problems_ = validate(doc_, reg_, &external_);
        haveProblems_ = true;
    }
    return problems_;
}

BlockSession::Stats BlockSession::stats() const {
    Stats s;
    s.stacks = doc_.sections.size();
    for (const Section& sec : doc_.sections) {
        (sec.domain == "floating" ? s.floatingStacks : s.programStacks)++;
        std::function<void(const std::vector<Block>&)> walk = [&](const std::vector<Block>& v) {
            for (const Block& b : v) {
                s.blocks++;
                if (b.type == "raw") s.rawBlocks++;
                for (const auto& st : b.stacks) walk(st.second);               // the blocks of the program; the components inside them are not counted
            }
        };
        walk(sec.blocks);
    }
    s.problems = problems().size();
    return s;
}

void BlockSession::select(const std::vector<std::string>& ids) {
    std::set<std::string> next(ids.begin(), ids.end());
    const std::set<std::string> existing = allIds(doc_);
    for (auto it = next.begin(); it != next.end();) it = existing.count(*it) ? std::next(it) : next.erase(it);
    if (next == selection_) return;
    selection_ = next;
    selectionChanged.fire();
}

void BlockSession::toggle(const std::string& id) {
    if (selection_.count(id)) selection_.erase(id);
    else if (allIds(doc_).count(id)) selection_.insert(id);
    else return;
    selectionChanged.fire();
}

void BlockSession::clearSelection() {
    if (selection_.empty()) return;
    selection_.clear();
    selectionChanged.fire();
}

std::vector<int> BlockSession::selectedLines() const {
    std::set<std::string> ids;
    for (const std::string& id : selection_) {
        for (const Section& s : doc_.sections)
            if (const Block* b = findBlock(s.blocks, id)) collect(*b, ids);
    }
    std::vector<int> lines;
    for (const GenLine& l : generated().lines) if (!l.blockId.empty() && ids.count(l.blockId)) lines.push_back(l.line);
    return lines;
}

std::string BlockSession::blockAtLine(int line) const {
    const GenLine* l = generated().atLine(line);
    return l ? l->blockId : std::string();
}

}  // namespace blocks
}  // namespace acide
