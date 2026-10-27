#include "blocks/block_session.h"
#include "blocks/block_drop.h"

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

BlockSession::Clip BlockSession::copySelection() const {
    const std::vector<std::string> ids(selection_.begin(), selection_.end());
    return Clip{copyBlocks(doc_, ids), blocksToText(doc_, ids)};
}

size_t BlockSession::cutSelection(Clip& out) {
    out = copySelection();
    return deleteSelection();
}

size_t BlockSession::paste(const std::string& text, double x, double y, std::string& note) {
    std::vector<Block> blocks;
    if (!pasteBlocks(doc_, text, blocks, note)) return 0;
    return place(blocks, x, y);
}

size_t BlockSession::place(std::vector<Block> blocks, double x, double y) {
    if (blocks.empty()) return 0;
    const BlockDef* first = reg_.find(blocks[0].type);
    const bool component = first && !first->valueType.empty();
    std::vector<std::string> ids;
    asmcore::blocks::Outcome o;
    beginGroup("paste:" + std::to_string(++groupSerial_));
    if (component && blocks.size() == 1) {
        // into an empty socket of a selected block that takes it
        for (const std::string& sid : selection_) {
            const Block* host = doc_.find(sid);
            const BlockDef* hd = host ? reg_.find(host->type) : nullptr;
            if (!hd) continue;
            for (const InputDef& in : hd->inputs) {
                if (host->value(in.name)) continue;
                if (checkSocket(in, blocks[0], reg_).fit == Fit::No) continue;
                o = apply([&](Editor& e) { return e.plugValue(sid, in.name, blocks[0]); });
                if (o.ok) { ids = o.ids; break; }
            }
            if (!ids.empty()) break;
        }
    } else if (!component) {
        // under the last selected instruction
        const Block* lastSel = nullptr;
        size_t lastKey = 0;
        for (const std::string& sid : selection_) {
            const Location loc = locate(doc_, sid);
            if (!loc.found) continue;
            const BlockDef* d = reg_.find(doc_.find(sid)->type);
            if (!d || !d->valueType.empty()) continue;
            const size_t key = loc.section * 100000 + loc.index;
            if (!lastSel || key >= lastKey) { lastSel = doc_.find(sid); lastKey = key; }
        }
        if (!lastSel) {
            // nothing selected: an EMPTY part of the program is clearly where to type (a new file starts with an empty .main)
            for (size_t i = doc_.sections.size(); i-- > 0;) {
                const Section& sec = doc_.sections[i];
                if (!sec.blocks.empty() || (sec.domain != "main" && sec.domain != "peri")) continue;
                const int need = first ? first->domains : kBoth;
                if (!(need & (sec.domain == "main" ? kMain : kPeri))) continue;
                Where w;
                w.section = i;
                o = apply([&](Editor& e) { return e.insertBlocks(w, blocks, false); });
                if (o.ok) { ids = o.ids; break; }
            }
        }
        if (lastSel) {
            const Location loc = locate(doc_, lastSel->id);
            ConnectionPoint cp;
            cp.kind = ConnectionPoint::Next;
            cp.blockId = lastSel->id;
            const Where w = whereOf(doc_, loc.section, cp);
            o = apply([&](Editor& e) { return e.insertBlocks(w, blocks, false); });
            if (o.ok) ids = o.ids;
        }
    }
    if (ids.empty()) {                                                    // on the canvas
        o = apply([&](Editor& e) { return e.newFloating(blocks, x, y); });
        if (o.ok) ids = o.ids;
    }
    endGroup();
    if (!ids.empty()) select(ids);
    return ids.size();
}

size_t BlockSession::duplicateSelection() {
    std::vector<std::string> stmts, comps;
    for (const std::string& id : selection_) {
        const Block* b = doc_.find(id);
        const BlockDef* d = b ? reg_.find(b->type) : nullptr;
        (d && !d->valueType.empty() ? comps : stmts).push_back(id);
    }
    size_t n = 0;
    std::vector<std::string> now;
    beginGroup("duplicate:" + std::to_string(++groupSerial_));
    if (!stmts.empty()) {
        const asmcore::blocks::Outcome o = apply([&](Editor& e) { return e.duplicate(stmts); });
        if (o.ok) { n += o.ids.size(); now = o.ids; }
    }
    for (const std::string& id : comps) {                                // a socket holds one component: the copy lies beside it
        const std::vector<std::string> one{id};
        std::vector<Block> clone;
        std::string note;
        if (!pasteBlocks(doc_, copyBlocks(doc_, one), clone, note)) continue;
        const Location loc = locate(doc_, id);
        const asmcore::blocks::Outcome o = apply([&](Editor& e) { return e.newFloating(clone, 40 + 24.0 * (double)n, 40 + 24.0 * (double)n); });
        if (o.ok) { n += o.ids.size(); for (const std::string& i : o.ids) now.push_back(i); }
        (void)loc;
    }
    endGroup();
    if (!now.empty()) select(now);
    return n;
}

size_t BlockSession::toggleDisabled() {
    size_t on = 0;
    std::vector<std::string> ids;
    for (const std::string& id : selection_) {
        const Block* b = doc_.find(id);
        const BlockDef* d = b ? reg_.find(b->type) : nullptr;
        if (!b || !d || !d->valueType.empty()) continue;                  // components cannot be switched off (an operand is not a line)
        ids.push_back(id);
        if (!b->disabled) on++;
    }
    if (ids.empty()) return 0;
    const bool disable = on > 0;                                          // anything still on: switch everything off; else switch on
    beginGroup("disable:" + std::to_string(++groupSerial_));
    size_t n = 0;
    for (const std::string& id : ids) if (apply([&](Editor& e) { return e.setDisabled(id, disable); }).ok) n++;
    endGroup();
    return n;
}

size_t BlockSession::toggleCollapsed() {
    std::vector<std::string> ids;
    size_t open = 0;
    for (const std::string& id : selection_) {
        const Block* b = doc_.find(id);
        const BlockDef* d = b ? reg_.find(b->type) : nullptr;
        if (!b || !d || d->stacks.empty()) continue;
        ids.push_back(id);
        if (!b->collapsed) open++;
    }
    if (ids.empty()) return 0;
    const bool fold = open > 0;
    beginGroup("collapse:" + std::to_string(++groupSerial_));
    size_t n = 0;
    for (const std::string& id : ids) if (apply([&](Editor& e) { return e.setCollapsed(id, fold); }).ok) n++;
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
        problems_.insert(problems_.end(), build_.begin(), build_.end());
        haveProblems_ = true;
    }
    return problems_;
}

void BlockSession::setOutput(const std::string& output) {
    if (doc_.output == output) return;
    doc_.output = output;
    invalidate();
    changed.fire();
}

void BlockSession::setBuildProblems(std::vector<BlockProblem> p) {
    auto same = [](const BlockProblem& a, const BlockProblem& b) { return a.blockId == b.blockId && a.message == b.message && a.error == b.error; };
    if (p.size() == build_.size() && std::equal(p.begin(), p.end(), build_.begin(), same)) return;
    build_ = std::move(p);
    haveProblems_ = false;
    changed.fire();
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
