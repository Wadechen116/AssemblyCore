// Editing block programs: connection rules, edit operations, history, clipboard. See include/asmcore/blocks_edit.h.
#include "asmcore/blocks_edit.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "asmcore/lang.h"
#include "blocks_internal.h"
#include "util.h"

namespace asmcore {
namespace blocks {

// ======================================================================================================================================
// connection rules
// ======================================================================================================================================
namespace {

std::string titleOf(const Registry& reg, const Block& b) {
    const BlockDef* d = reg.find(b.type);
    return d ? d->title : b.type;
}

// "main" / "peri" for a block that only exists in one domain, "" for one that exists in both
std::string exclusiveDomain(const BlockDef& d) { return d.domains == kMain ? "main" : d.domains == kPeri ? "peri" : ""; }

bool isTerminator(const Block& b) {
    if (b.type == "jmp" || b.type == "ret" || b.type == "br_eret") return true;
    if (b.type == "br_peri" && upper(b.get("cond")) == "ALWAYS") return true;
    if (b.type == "br_cond" && upper(b.get("cond")) == "ALWAYS") return true;
    return false;
}
bool isNoise(const Block& b) { return b.disabled || b.type == "comment" || b.type == "blank"; }
// blocks that do not execute, so "after a jump" does not make them suspicious
bool isTransparent(const Block& b) {
    static const std::set<std::string> t = {"label", "comment", "blank", "raw", "proc", "vector", "wcet", "assume", "directive"};
    return t.count(b.type) != 0;
}

struct Walk {
    const Registry& reg;
    const RuleOptions& opt;
    std::vector<Violation>& out;
    std::string stackDomain, stackTitle;             // floating stacks: the domain of the first Main-only / Peri-only block

    void add(Fit fit, const char* rule, const Block& b, const std::string& reason, std::vector<std::string> args = {}) {
        Violation v;
        v.fit = fit; v.rule = rule; v.blockId = b.id; v.reason = reason; v.args = std::move(args);
        out.push_back(std::move(v));
    }

    void seq(const std::vector<Block>& list, const StackContext& ctx, bool topLevelOfFloating) {
        const Block* prev = nullptr;
        for (const Block& b : list) {
            const BlockDef* d = reg.find(b.type);
            if (!d) continue;                                              // validate() reports unknown types
            if (!d->valueType.empty()) {
                if (ctx.domain != "floating" || list.size() != 1 || !topLevelOfFloating)
                    add(Fit::No, "H7", b, "'" + d->title + "' is a value: plug it into a socket of another block.", {d->title});
                continue;
            }
            // H1: Main-only and Peri-only blocks do not mix
            const std::string ex = exclusiveDomain(*d);
            if (!ex.empty()) {
                if (ctx.domain == "main" || ctx.domain == "peri") {
                    if (ex != ctx.domain)
                        add(Fit::No, "H1", b, "'" + d->title + "' is a " + (ex == "main" ? "Main" : "Peri") + " block, but this is a " + (ctx.domain == "main" ? "Main" : "Peri") + " section: Main and Peri are different sequencers.",
                            {d->title, ex, ctx.domain});
                } else if (ctx.domain == "floating") {
                    if (stackDomain.empty()) { stackDomain = ex; stackTitle = d->title; }
                    else if (stackDomain != ex)
                        add(Fit::No, "H1", b, "'" + d->title + "' is a " + (ex == "main" ? "Main" : "Peri") + " block, but this stack already holds a " + (stackDomain == "main" ? "Main" : "Peri") + " block ('" + stackTitle + "'): Main and Peri are different sequencers.",
                            {d->title, ex, stackDomain, stackTitle});
                }
            }
            if (b.type == "proc" && ctx.inProc) add(Fit::No, "H3", b, "A procedure cannot be inside another procedure.", {d->title});
            if (b.type == "ret" && (ctx.domain == "main" || ctx.domain == "peri") && !ctx.inProc)
                add(Fit::No, "H4", b, "RET only makes sense inside a procedure; here it would stand in the program flow.", {d->title});
            if (b.type == "vector" && ctx.inProc) add(Fit::No, "H5", b, "An event vector belongs to the program flow, not inside a procedure.", {d->title});
            if (ctx.domain.empty() && b.type != "directive" && b.type != "comment" && b.type != "blank" && b.type != "raw")
                add(Fit::No, "H6", b, "'" + d->title + "' needs a .main or .peri section: the definitions at the top of a file hold only definitions and text.", {d->title});
            // S1: code right after an unconditional jump / return that is not a label may never run
            if (prev && isTerminator(*prev) && !isTransparent(b) && !b.disabled)
                add(opt.strict ? Fit::No : Fit::Warn, "S1", b, "'" + d->title + "' comes after an unconditional jump or return with no label in between, so nothing may ever run it.", {d->title, titleOf(reg, *prev)});
            if (!isNoise(b)) prev = &b;
            // inside the slots
            for (const auto& st : b.stacks) {
                StackContext inner = ctx;
                inner.inProc = ctx.inProc || b.type == "proc";
                seq(st.second, inner, false);
            }
        }
    }
};

std::string key(const Violation& v) { return v.rule + "|" + v.blockId + "|" + v.reason; }

}  // namespace

std::vector<Violation> checkSequence(const std::vector<Block>& seq, const StackContext& ctx, const Registry& reg, const RuleOptions& opt) {
    std::vector<Violation> out;
    Walk w{reg, opt, out, "", ""};
    w.seq(seq, ctx, true);
    return out;
}

Violation checkSocket(const InputDef& in, const Block& child, const Registry& reg) {
    Violation v;
    v.fit = Fit::Ok;
    const BlockDef* cd = reg.find(child.type);
    if (!cd || cd->valueType.empty()) {
        v.fit = Fit::No; v.rule = "H7"; v.blockId = child.id;
        v.reason = "'" + (cd ? cd->title : child.type) + "' is not a value block, so it cannot be plugged into '" + in.label + "'.";
        return v;
    }
    if (!in.accepts.empty() && std::find(in.accepts.begin(), in.accepts.end(), cd->valueType) == in.accepts.end()) {
        std::string acc;
        for (const std::string& a : in.accepts) acc += (acc.empty() ? "" : ", ") + a;
        v.fit = Fit::No; v.rule = "H7"; v.blockId = child.id;
        v.reason = "'" + cd->title + "' (" + cd->valueType + ") does not fit into '" + in.label + "', which takes " + acc + ".";
        v.args = {cd->title, cd->valueType, in.label, acc};
    }
    return v;
}

Fit worst(const std::vector<Violation>& v) {
    Fit f = Fit::Ok;
    for (const Violation& x : v) if (x.fit == Fit::No) return Fit::No; else if (x.fit == Fit::Warn) f = Fit::Warn;
    return f;
}

std::string describe(const std::vector<Violation>& v) {
    std::string s;
    for (const Violation& x : v) s += (s.empty() ? "" : "\n") + x.reason;
    return s;
}

// ======================================================================================================================================
// positions
// ======================================================================================================================================
namespace {

struct Found {
    std::vector<Block>* list = nullptr;
    size_t index = 0;
    size_t section = 0;
    Block* container = nullptr;
    std::string slot;
    bool inProc = false;
};

bool findIn(std::vector<Block>& list, const std::string& id, Found& f, Block* container, const std::string& slot, bool inProc) {
    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].id == id) { f.list = &list; f.index = i; f.container = container; f.slot = slot; f.inProc = inProc; return true; }
        for (auto& st : list[i].stacks)
            if (findIn(st.second, id, f, &list[i], st.first, inProc || list[i].type == "proc")) return true;
    }
    return false;
}

bool findBlock(std::vector<Section>& secs, const std::string& id, Found& f) {
    for (size_t s = 0; s < secs.size(); s++)
        if (findIn(secs[s].blocks, id, f, nullptr, "", false)) { f.section = s; return true; }
    return false;
}

Block* findAny(std::vector<Block>& list, const std::string& id) {
    for (Block& b : list) {
        if (b.id == id) return &b;
        for (auto& st : b.stacks) if (Block* r = findAny(st.second, id)) return r;
        for (auto& vl : b.values) if (Block* r = findAny(vl.second, id)) return r;
    }
    return nullptr;
}
Block* findAnyIn(std::vector<Section>& secs, const std::string& id, size_t* section = nullptr) {
    for (size_t s = 0; s < secs.size(); s++) if (Block* b = findAny(secs[s].blocks, id)) { if (section) *section = s; return b; }
    return nullptr;
}

bool contains(const Block& b, const std::string& id) {
    if (b.id == id) return true;
    for (const auto& st : b.stacks) for (const Block& c : st.second) if (contains(c, id)) return true;
    for (const auto& vl : b.values) for (const Block& c : vl.second) if (contains(c, id)) return true;
    return false;
}

void collectIds(const Block& b, std::set<std::string>& ids) {
    ids.insert(b.id);
    for (const auto& st : b.stacks) for (const Block& c : st.second) collectIds(c, ids);
    for (const auto& vl : b.values) for (const Block& c : vl.second) collectIds(c, ids);
}
std::set<std::string> allIds(const std::vector<Section>& secs) {
    std::set<std::string> ids;
    for (const Section& s : secs) for (const Block& b : s.blocks) collectIds(b, ids);
    return ids;
}

void collectPoints(const std::vector<Block>& list, size_t section, const std::string& container, const std::string& slot, std::vector<Where>& out) {
    for (const Block& b : list) {
        Where w; w.section = section; w.container = container; w.slot = slot; w.before = b.id;
        out.push_back(w);
    }
    Where end; end.section = section; end.container = container; end.slot = slot;
    out.push_back(end);
    for (const Block& b : list)
        for (const auto& st : b.stacks) collectPoints(st.second, section, b.id, st.first, out);
}

}  // namespace

std::vector<Where> connectionPoints(const Document& doc) {
    std::vector<Where> out;
    for (size_t s = 0; s < doc.sections.size(); s++) collectPoints(doc.sections[s].blocks, s, "", "", out);
    return out;
}

Location locate(const Document& doc, const std::string& blockId) {
    Document copy = doc;
    Found f;
    Location l;
    if (!findBlock(copy.sections, blockId, f)) return l;
    l.found = true;
    l.section = f.section;
    l.container = f.container ? f.container->id : "";
    l.slot = f.slot;
    l.index = f.index;
    return l;
}

// ======================================================================================================================================
// history
// ======================================================================================================================================
EditHistory::EditHistory(Document& doc, size_t limit) : doc_(doc), limit_(limit < 2 ? 2 : limit) { reset(); }

void EditHistory::reset() {
    states_.clear();
    states_.push_back({doc_.sections, "", ""});
    pos_ = 0;
}

void EditHistory::record(const std::string& label, const std::string& mergeKey) {
    states_.resize(pos_ + 1);                                              // a new change drops the redo tail
    if (!mergeKey.empty() && pos_ > 0 && states_[pos_].mergeKey == mergeKey) {
        states_[pos_].sections = doc_.sections;                            // the same field again: still one step
        return;
    }
    states_.push_back({doc_.sections, label, mergeKey});
    pos_ = states_.size() - 1;
    if (states_.size() > limit_) {
        states_.erase(states_.begin());
        pos_ = states_.size() - 1;
    }
}

std::string EditHistory::undoLabel() const { return pos_ > 0 ? states_[pos_].label : std::string(); }
std::string EditHistory::redoLabel() const { return canRedo() ? states_[pos_ + 1].label : std::string(); }

bool EditHistory::undo() {
    if (!canUndo()) return false;
    pos_--;
    doc_.sections = states_[pos_].sections;
    return true;
}
bool EditHistory::redo() {
    if (!canRedo()) return false;
    pos_++;
    doc_.sections = states_[pos_].sections;
    return true;
}

// ======================================================================================================================================
// the editor
// ======================================================================================================================================
namespace {

using Work = std::vector<Section>;

std::vector<Block>* listFor(Work& work, const Where& at, std::string& err, bool& inProc) {
    inProc = false;
    if (at.section >= work.size()) { err = "there is no such stack"; return nullptr; }
    if (at.container.empty()) return &work[at.section].blocks;
    Found f;
    if (!findIn(work[at.section].blocks, at.container, f, nullptr, "", false)) { err = "the block " + at.container + " is not in this stack"; return nullptr; }
    Block& c = (*f.list)[f.index];
    inProc = f.inProc || c.type == "proc";
    const BlockDef* d = Registry::defaults().find(c.type);
    if (!d || std::find(d->stacks.begin(), d->stacks.end(), at.slot) == d->stacks.end()) { err = "'" + c.type + "' has no slot '" + at.slot + "'"; return nullptr; }
    return c.stack(at.slot, true);
}

bool insertAt(Work& work, const Where& at, std::vector<Block>& blocks, std::string& err) {
    bool inProc;
    std::vector<Block>* list = listFor(work, at, err, inProc);
    if (!list) return false;
    size_t pos = list->size();
    if (!at.before.empty()) {
        pos = list->size() + 1;
        for (size_t i = 0; i < list->size(); i++) if ((*list)[i].id == at.before) pos = i;
        if (pos > list->size()) { err = "the block " + at.before + " is not in that place"; return false; }
    }
    list->insert(list->begin() + (long)pos, blocks.begin(), blocks.end());
    return true;
}

void pruneEmptyFloating(Work& work, std::set<size_t>& touched) {
    for (size_t i = work.size(); i-- > 0;) {
        if (work[i].domain != "floating" || !work[i].blocks.empty()) continue;
        work.erase(work.begin() + (long)i);
        std::set<size_t> t2;
        for (size_t t : touched) if (t != i) t2.insert(t > i ? t - 1 : t);
        touched = t2;
    }
}

StackContext ctxOf(const Section& s) { return StackContext{s.domain, false}; }

}  // namespace

struct Editor::Impl {};

Editor::Editor(Document& doc, const Registry& reg, const RuleOptions& opt, EditHistory* history) : doc_(doc), reg_(reg), opt_(opt), history_(history) {}

namespace {

// The shared frame of the mutating operations: work on a copy of the sections, check the rules on every section that changed (only
// violations that are NEW count), and commit only if nothing is refused.
Outcome run(Editor& ed, Document& doc, const Registry& reg, const RuleOptions& opt, EditHistory* history, const std::string& label, const std::string& mergeKey, bool dryRun,
            const std::function<bool(Work&, std::set<size_t>&, Outcome&)>& mutate) {
    Outcome out;
    Work work = doc.sections;
    std::set<size_t> touched;
    const unsigned savedNextId = doc.nextId;                                // a dry run or a refused change must not use up ids
    std::vector<size_t> beforeIndex;                                        // not needed: before-state is read from doc.sections by index before pruning
    // violations before, per section index (those sections exist before the change)
    std::map<size_t, std::set<std::string>> before;
    for (size_t i = 0; i < doc.sections.size(); i++)
        for (const Violation& v : checkSequence(doc.sections[i].blocks, ctxOf(doc.sections[i]), reg, opt)) before[i].insert(key(v));
    if (!mutate(work, touched, out)) {
        out.ok = false;
        doc.nextId = savedNextId;
        return out;
    }
    // sections that were pruned shift indexes; the "before" violations of a section are matched by its position in the old document, which
    // only matters for sections that kept their index: compare by key regardless of section (keys contain the block id)
    std::set<std::string> allBefore;
    for (auto& kv : before) allBefore.insert(kv.second.begin(), kv.second.end());
    pruneEmptyFloating(work, touched);
    for (size_t i : touched) {
        if (i >= work.size()) continue;
        for (const Violation& v : checkSequence(work[i].blocks, ctxOf(work[i]), reg, opt))
            if (!allBefore.count(key(v))) out.violations.push_back(v);
    }
    out.fit = worst(out.violations);
    if (out.fit == Fit::No) { out.ok = false; doc.nextId = savedNextId; return out; }
    out.ok = true;
    if (dryRun) { doc.nextId = savedNextId; return out; }
    doc.sections = work;
    if (history) history->record(label, mergeKey);
    (void)ed;
    return out;
}

Outcome fail(const std::string& error) {
    Outcome o;
    o.ok = false;
    o.error = error;
    return o;
}

void refuse(Outcome& out, const char* rule, const std::string& blockId, const std::string& reason) {
    Violation v;
    v.fit = Fit::No; v.rule = rule; v.blockId = blockId; v.reason = reason;
    out.violations.push_back(v);
    out.fit = Fit::No;
}

}  // namespace

Outcome Editor::insertBlocks(const Where& at, std::vector<Block> blocks, bool dryRun) {
    if (blocks.empty()) return fail("nothing to insert");
    return run(*this, doc_, reg_, opt_, history_, "Insert blocks", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        const std::set<std::string> used = allIds(work);
        std::function<void(Block&)> fresh = [&](Block& b) {
            if (b.id.empty() || used.count(b.id)) b.id = doc_.newId();
            for (auto& st : b.stacks) for (Block& c : st.second) fresh(c);
            for (auto& vl : b.values) for (Block& c : vl.second) fresh(c);
        };
        for (Block& b : blocks) fresh(b);
        std::string err;
        if (!insertAt(work, at, blocks, err)) { out.error = err; return false; }
        touched.insert(at.section);
        for (const Block& b : blocks) out.ids.push_back(b.id);
        out.section = at.section;
        return true;
    });
}

Outcome Editor::removeBlocks(const std::vector<std::string>& ids, bool takeBelow) {
    if (ids.empty()) return fail("nothing to remove");
    return run(*this, doc_, reg_, opt_, history_, "Delete blocks", "", false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        bool any = false;
        for (const std::string& id : ids) {
            Found f;
            if (!findBlock(work, id, f)) continue;                          // already gone with its parent
            if (takeBelow) f.list->erase(f.list->begin() + (long)f.index, f.list->end());
            else f.list->erase(f.list->begin() + (long)f.index);
            touched.insert(f.section);
            any = true;
        }
        if (!any) { out.error = "those blocks do not exist"; return false; }
        return true;
    });
}

Outcome Editor::moveBlocks(const std::vector<std::string>& ids, const Where& to, bool takeBelow, bool dryRun) {
    if (ids.empty()) return fail("nothing to move");
    return run(*this, doc_, reg_, opt_, history_, "Move blocks", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        // collect (in document order) what moves
        std::vector<std::pair<std::pair<size_t, std::string>, Found>> order;
        std::set<std::string> moving;
        for (const std::string& id : ids) {
            Found f;
            if (!findBlock(work, id, f)) { out.error = "the block " + id + " does not exist"; return false; }
            std::vector<Block>& list = *f.list;
            for (size_t i = f.index; i < (takeBelow ? list.size() : f.index + 1); i++) collectIds(list[i], moving);
        }
        if (!to.before.empty() && moving.count(to.before)) { out.error = "a block cannot be moved in front of itself"; return false; }
        if (!to.container.empty() && moving.count(to.container)) { out.error = "a block cannot be moved into itself"; return false; }
        // take them out (the first id's list first; ids are processed in the order given, takeBelow takes siblings after)
        std::vector<Block> taken;
        std::set<std::string> takenIds;
        for (const std::string& id : ids) {
            if (takenIds.count(id)) continue;
            Found f;
            if (!findBlock(work, id, f)) continue;
            std::vector<Block>& list = *f.list;
            const size_t end = takeBelow ? list.size() : f.index + 1;
            for (size_t i = f.index; i < end; i++) { taken.push_back(list[i]); collectIds(list[i], takenIds); }
            list.erase(list.begin() + (long)f.index, list.begin() + (long)end);
            touched.insert(f.section);
        }
        std::string err;
        if (!insertAt(work, to, taken, err)) { out.error = err; return false; }
        touched.insert(to.section);
        for (const Block& b : taken) out.ids.push_back(b.id);
        out.section = to.section;
        return true;
    });
}

Outcome Editor::splitAt(const std::string& id, double x, double y, bool dryRun) {
    return run(*this, doc_, reg_, opt_, history_, "Detach blocks", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        Found f;
        if (!findBlock(work, id, f)) { out.error = "the block " + id + " does not exist"; return false; }
        if (!f.container && f.index == 0 && work[f.section].domain == "floating") { out.section = f.section; return true; }       // already a stack of its own
        std::vector<Block> list = *f.list;
        std::vector<Block> moved(list.begin() + (long)f.index, list.end());
        f.list->erase(f.list->begin() + (long)f.index, f.list->end());
        touched.insert(f.section);
        Section s;
        s.domain = "floating";
        s.x = x; s.y = y;
        s.blocks = moved;
        work.push_back(s);
        touched.insert(work.size() - 1);
        for (const Block& b : moved) out.ids.push_back(b.id);
        out.section = work.size() - 1;
        return true;
    });
}

Outcome Editor::joinSection(size_t source, const Where& to, bool dryRun) {
    return run(*this, doc_, reg_, opt_, history_, "Join stacks", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        if (source >= work.size()) { out.error = "there is no such stack"; return false; }
        if (source == to.section) { out.error = "a stack cannot be joined to itself"; return false; }
        if (work[source].domain != "floating") {
            refuse(out, "H2", "", "A section start (.main / .peri / definitions) can only be at the top: it cannot be placed under or between other blocks. Attach a floating stack to it instead.");
            return true;                                                    // violation recorded: run() reports the refusal
        }
        std::vector<Block> blocks = work[source].blocks;
        Where target = to;
        work.erase(work.begin() + (long)source);
        if (target.section > source) target.section--;
        std::string err;
        if (!insertAt(work, target, blocks, err)) { out.error = err; return false; }
        touched.insert(target.section);
        for (const Block& b : blocks) out.ids.push_back(b.id);
        out.section = target.section;
        return true;
    });
}

Outcome Editor::newFloating(std::vector<Block> blocks, double x, double y) {
    if (blocks.empty()) return fail("nothing to put on the canvas");
    return run(*this, doc_, reg_, opt_, history_, "New stack", "", false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        const std::set<std::string> used = allIds(work);
        std::function<void(Block&)> fresh = [&](Block& b) {
            if (b.id.empty() || used.count(b.id)) b.id = doc_.newId();
            for (auto& st : b.stacks) for (Block& c : st.second) fresh(c);
            for (auto& vl : b.values) for (Block& c : vl.second) fresh(c);
        };
        for (Block& b : blocks) fresh(b);
        Section s;
        s.domain = "floating";
        s.x = x; s.y = y;
        s.blocks = blocks;
        work.push_back(s);
        touched.insert(work.size() - 1);
        for (const Block& b : blocks) out.ids.push_back(b.id);
        out.section = work.size() - 1;
        return true;
    });
}

Outcome Editor::moveSection(size_t index, double x, double y) {
    return run(*this, doc_, reg_, opt_, history_, "Move stack", "stack:" + std::to_string(index), false, [&](Work& work, std::set<size_t>&, Outcome& out) {
        if (index >= work.size()) { out.error = "there is no such stack"; return false; }
        work[index].x = x;
        work[index].y = y;
        out.section = index;
        return true;
    });
}

Outcome Editor::attachToHat(size_t section, const std::string& domain, bool dryRun) {
    return run(*this, doc_, reg_, opt_, history_, "Attach to program", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        if (section >= work.size()) { out.error = "there is no such stack"; return false; }
        if (work[section].domain != "floating") { out.error = "this stack is already part of the program"; return false; }
        if (domain != "main" && domain != "peri" && !domain.empty()) { out.error = "a section is main, peri or the definitions (empty)"; return false; }
        work[section].domain = domain;
        touched.insert(section);
        out.section = section;
        return true;
    });
}

Outcome Editor::detachFromHat(size_t section) {
    return run(*this, doc_, reg_, opt_, history_, "Detach from program", "", false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        if (section >= work.size()) { out.error = "there is no such stack"; return false; }
        if (work[section].domain == "floating") { out.error = "this stack is not part of the program"; return false; }
        work[section].domain = "floating";
        touched.insert(section);
        out.section = section;
        return true;
    });
}

Outcome Editor::duplicate(const std::vector<std::string>& ids, bool dryRun) {
    if (ids.empty()) return fail("nothing to duplicate");
    return run(*this, doc_, reg_, opt_, history_, "Duplicate blocks", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        // the clones go right after their originals; later siblings first so earlier positions stay valid
        std::vector<std::pair<std::string, Found>> items;
        for (const std::string& id : ids) {
            Found f;
            if (!findBlock(work, id, f)) { out.error = "the block " + id + " does not exist"; return false; }
        }
        std::set<std::string> done;
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
            Found f;
            if (!findBlock(work, *it, f) || done.count(*it)) continue;
            Block clone = (*f.list)[f.index];
            detail::reid(clone, doc_);
            f.list->insert(f.list->begin() + (long)f.index + 1, clone);
            done.insert(*it);
            touched.insert(f.section);
            out.ids.insert(out.ids.begin(), clone.id);
        }
        return true;
    });
}

Outcome Editor::setField(const std::string& id, const std::string& name, const std::string& value) {
    return run(*this, doc_, reg_, opt_, history_, "Edit field", "field:" + id + ":" + name, false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        size_t sec = 0;
        Block* b = findAnyIn(work, id, &sec);
        if (!b) { out.error = "the block " + id + " does not exist"; return false; }
        const BlockDef* d = reg_.find(b->type);
        bool known = false;
        if (d) for (const FieldDef& f : d->fields) if (f.name == name) known = true;
        if (!known) { out.error = "'" + b->type + "' has no field '" + name + "'"; return false; }
        b->set(name, value);
        touched.insert(sec);
        out.ids.push_back(id);
        return true;
    });
}

Outcome Editor::setComment(const std::string& id, const std::string& comment) {
    return run(*this, doc_, reg_, opt_, history_, "Edit comment", "comment:" + id, false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        size_t sec = 0;
        Block* b = findAnyIn(work, id, &sec);
        if (!b) { out.error = "the block " + id + " does not exist"; return false; }
        b->comment = comment;
        touched.insert(sec);
        return true;
    });
}

Outcome Editor::setDisabled(const std::string& id, bool disabled) {
    return run(*this, doc_, reg_, opt_, history_, disabled ? "Disable block" : "Enable block", "", false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        size_t sec = 0;
        Block* b = findAnyIn(work, id, &sec);
        if (!b) { out.error = "the block " + id + " does not exist"; return false; }
        b->disabled = disabled;
        touched.insert(sec);
        return true;
    });
}

Outcome Editor::setCollapsed(const std::string& id, bool collapsed) {
    return run(*this, doc_, reg_, opt_, history_, collapsed ? "Collapse block" : "Expand block", "", false, [&](Work& work, std::set<size_t>&, Outcome& out) {
        Block* b = findAnyIn(work, id);
        if (!b) { out.error = "the block " + id + " does not exist"; return false; }
        b->collapsed = collapsed;
        return true;
    });
}

Outcome Editor::plugValue(const std::string& blockId, const std::string& socket, Block child, bool dryRun) {
    return run(*this, doc_, reg_, opt_, history_, "Plug value", "", dryRun, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        size_t sec = 0;
        Block* host = findAnyIn(work, blockId, &sec);
        if (!host) { out.error = "the block " + blockId + " does not exist"; return false; }
        const BlockDef* d = reg_.find(host->type);
        const InputDef* in = nullptr;
        if (d) for (const InputDef& i : d->inputs) if (i.name == socket) in = &i;
        if (!in) { out.error = "'" + host->type + "' has no value socket '" + socket + "'"; return false; }
        const Violation v = checkSocket(*in, child, reg_);
        if (v.fit == Fit::No) { out.violations.push_back(v); out.fit = Fit::No; return true; }
        if (child.id.empty()) child.id = doc_.newId();
        if (const Block* old = host->value(socket)) {                       // the block that was plugged in is not lost: it floats beside the host
            Section s;
            s.domain = "floating";
            s.x = work[sec].x + 24;
            s.y = work[sec].y + 24;
            s.blocks.push_back(*old);
            work.push_back(s);
            touched.insert(work.size() - 1);
        }
        host->setValue(socket, child);
        touched.insert(sec);
        out.ids.push_back(child.id);
        return true;
    });
}

Outcome Editor::unplugValue(const std::string& blockId, const std::string& socket, double x, double y) {
    return run(*this, doc_, reg_, opt_, history_, "Unplug value", "", false, [&](Work& work, std::set<size_t>& touched, Outcome& out) {
        size_t sec = 0;
        Block* host = findAnyIn(work, blockId, &sec);
        if (!host) { out.error = "the block " + blockId + " does not exist"; return false; }
        const Block* v = host->value(socket);
        if (!v) { out.error = "nothing is plugged into '" + socket + "'"; return false; }
        Section s;
        s.domain = "floating";
        s.x = x; s.y = y;
        s.blocks.push_back(*v);
        host->clearValue(socket);
        work.push_back(s);
        touched.insert(sec);
        touched.insert(work.size() - 1);
        out.ids.push_back(s.blocks[0].id);
        return true;
    });
}

Outcome Editor::setSectionComment(size_t section, const std::string& comment) {
    return run(*this, doc_, reg_, opt_, history_, "Edit comment", "seccomment:" + std::to_string(section), false, [&](Work& work, std::set<size_t>&, Outcome& out) {
        if (section >= work.size()) { out.error = "there is no such stack"; return false; }
        work[section].comment = comment;
        return true;
    });
}

Outcome Editor::removeSection(size_t section) {
    return run(*this, doc_, reg_, opt_, history_, "Delete stack", "", false, [&](Work& work, std::set<size_t>&, Outcome& out) {
        if (section >= work.size()) { out.error = "there is no such stack"; return false; }
        work.erase(work.begin() + (long)section);
        return true;
    });
}

// ======================================================================================================================================
// clipboard
// ======================================================================================================================================
namespace {

void topMost(const std::vector<Block>& list, const std::set<std::string>& want, std::vector<const Block*>& out) {
    for (const Block& b : list) {
        if (want.count(b.id)) { out.push_back(&b); continue; }              // its children come with it
        for (const auto& st : b.stacks) topMost(st.second, want, out);
    }
}

}  // namespace

std::string copyBlocks(const Document& doc, const std::vector<std::string>& ids) {
    const std::set<std::string> want(ids.begin(), ids.end());
    std::vector<const Block*> picked;
    for (const Section& s : doc.sections) topMost(s.blocks, want, picked);
    json::Value root = json::Value::makeObject();
    root.set("format", json::Value::makeString("acblk-clip"));
    root.set("version", json::Value::makeNumber(1));
    json::Value arr = json::Value::makeArray();
    for (const Block* b : picked) arr.push(detail::blockToJson(*b));
    root.set("blocks", arr);
    std::string out;
    json::write(root, out);
    return out + "\n";
}

bool pasteBlocks(Document& doc, const std::string& text, std::vector<Block>& out, std::string& note) {
    out.clear();
    note.clear();
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { note = "the clipboard is empty"; return false; }
    if (text[first] == '{') {
        json::Parser p(text);
        json::Value root;
        if (p.parse(root) && root.kind == json::Value::Object) {
            const json::Value* fmt = root.get("format");
            if (fmt && fmt->kind == json::Value::String && fmt->str == "acblk-clip") {
                const json::Value* arr = root.get("blocks");
                if (!arr || arr->kind != json::Value::Array) { note = "the copied blocks are damaged"; return false; }
                for (size_t i = 0; i < arr->arr.size(); i++) {
                    Block b;
                    std::string err;
                    if (!detail::blockFromJson(arr->arr[i], b, "blocks[" + std::to_string(i) + "]", err)) { note = err; return false; }
                    detail::reid(b, doc);
                    out.push_back(std::move(b));
                }
                if (out.empty()) { note = "no blocks were copied"; return false; }
                return true;
            }
        }
    }
    ImportStats st;
    out = detail::importFlat(doc, text, &st);
    while (!out.empty() && out.back().type == "blank") out.pop_back();
    if (out.empty()) { note = "nothing to paste"; return false; }
    if (st.raw) note = std::to_string(st.raw) + " line(s) kept as assembler text";
    return true;
}

std::string blocksToText(const Document& doc, const std::vector<std::string>& ids) {
    const std::set<std::string> want(ids.begin(), ids.end());
    std::vector<const Block*> picked;
    for (const Section& s : doc.sections) topMost(s.blocks, want, picked);
    Document tmp;
    tmp.sections.push_back(Section());
    for (const Block* b : picked) tmp.sections[0].blocks.push_back(*b);
    return generate(tmp).text;
}

std::vector<Block> blocksFromText(Document& doc, const std::string& text, bool* anyRaw) {
    ImportStats st;
    std::vector<Block> v = detail::importFlat(doc, text, &st);
    v.erase(std::remove_if(v.begin(), v.end(), [](const Block& b) { return b.type == "blank"; }), v.end());
    if (anyRaw) *anyRaw = st.raw > 0;
    return v;
}

// ======================================================================================================================================
// fields
// ======================================================================================================================================
std::string checkFieldValue(const BlockDef& def, const FieldDef& f, const std::string& value) {
    (void)def;
    const std::string v = trim(value);
    if (v.empty()) return f.optional || f.kind == FieldKind::Flag ? "" : "'" + f.label + "' needs a value";
    if (f.kind == FieldKind::Int && (f.lo != 0 || f.hi != 0)) {
        // only a plain number is range-checked; a name or an expression is the assembler's business
        char* end = nullptr;
        long long n = std::strtoll(v.c_str(), &end, 0);
        if (end && !*end && (n < f.lo || n > f.hi)) return "'" + f.label + "' is " + std::to_string(f.lo) + " to " + std::to_string(f.hi);
    }
    if (f.kind == FieldKind::Choice && !f.open && std::find(f.options.begin(), f.options.end(), v) == f.options.end()) return "'" + v + "' is not one of the choices";
    if (f.kind == FieldKind::Operand) {
        Operand op;
        if (parseOperand(v, op)) {
            static const lang::Language L = lang::Language::defaults();
            if (!L.find(lang::Tok::Region, op.region)) return "'" + op.region + "' is not a region name";
        }
    }
    return "";
}

}  // namespace blocks
}  // namespace asmcore
