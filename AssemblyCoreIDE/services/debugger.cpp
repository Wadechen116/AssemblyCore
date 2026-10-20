#include "debugger.h"

#include <algorithm>
#include <sstream>

using namespace asmcore;

namespace acide {

Debugger::Debugger(SimSession& s, Workspace& w, Settings* settings, std::function<SnapshotPtr()> latest)
    : session_(s), ws_(w), settings_(settings), latest_(std::move(latest)) {
    conns_.add(session_.updated, [this](const SimUpdate& u) { onUpdate(u); });
}

SnapshotPtr Debugger::programForMapping() const {
    SnapshotPtr p = session_.program();
    if (p) return p;
    return latest_ ? latest_() : nullptr;
}

// ---- session -------------------------------------------------------------------------------------------------------------------------
bool Debugger::start(std::string& why) {
    SnapshotPtr prog = latest_ ? latest_() : nullptr;
    if (!prog) { why = "nothing has been built yet"; return false; }
    if (!prog->result.ok) { why = "the program has errors: fix them first"; return false; }
    const Project& p = ws_.project();
    std::string env;
    if (!p.environment.empty()) {
        if (!ws_.fileSystem()->read(p.absolute(p.environment), env)) { why = "cannot read the environment file " + p.environment; return false; }
        Document* d = ws_.find(p.absolute(p.environment));          // an unsaved edit of it counts
        if (d) env = d->text;
    }
    Profile pf;
    if (!p.profile.empty()) {
        std::string text;
        std::vector<std::string> errs;
        if (!ws_.fileSystem()->read(p.absolute(p.profile), text)) { why = "cannot read the profile " + p.profile; return false; }
        if (!pf.load(text, errs)) { why = "profile: " + (errs.empty() ? std::string("error") : errs[0]); return false; }
    }
    session_.load(prog, env, pf);
    // re-resolve every breakpoint against this program
    session_.setWatchExprs(watchExprs_);
    session_.setDataWatches(dataWatches_);
    pushBreakpoints();
    return true;
}

void Debugger::stop() {
    session_.unload();
    for (auto& b : bps_) b.hits = 0;
    breakpointsChanged.fire();
}

bool Debugger::stale() const {
    SnapshotPtr p = session_.program();
    return p && p->revision != ws_.revision();
}

void Debugger::run(const RunSpec& spec) { session_.run(spec); }

void Debugger::restart() { session_.reset(); }

bool Debugger::runToLine(const std::string& file, int line, std::string& why) {
    SnapshotPtr p = programForMapping();
    if (!p) { why = "no program"; return false; }
    auto v = p->map->instrsForLine(file, line);
    if (v.empty()) { why = "that line produces no code"; return false; }
    const asmcore::InstrInfo* pick = v.front();
    for (const auto* i : v) if (i->domain == focus_) { pick = i; break; }
    runTo_ = RunTo{true, file, line, pick->domain, pick->pc};
    session_.runToPc(pick->domain, pick->pc);
    return true;
}

// ---- breakpoints ---------------------------------------------------------------------------------------------------------------------
std::vector<BreakpointSpec> Debugger::specs(std::vector<size_t>* owner) const {
    std::vector<BreakpointSpec> out;
    if (owner) owner->clear();
    SnapshotPtr p = programForMapping();
    if (!p) return out;
    for (size_t i = 0; i < bps_.size(); i++) {
        if (!bps_[i].enabled) continue;
        bool seenSynthetic[2] = {false, false};          // a softret CALL line is several generated instructions: stop at the first only
        for (const InstrInfo* in : p->map->instrsForLine(bps_[i].file, bps_[i].line)) {
            if (in->synthetic) {
                bool& seen = seenSynthetic[in->domain == Domain::Main ? 0 : 1];
                if (seen) continue;
                seen = true;
            }
            out.push_back(BreakpointSpec{in->domain, in->pc, bps_[i].condition});
            if (owner) owner->push_back(i);
        }
    }
    return out;
}

void Debugger::pushBreakpoints() {
    std::vector<size_t> owner;
    specList_ = specs(&owner);
    session_.setBreakpoints(specList_);
    specOwner_ = owner;
    SnapshotPtr p = programForMapping();
    for (LineBreakpoint& b : bps_) b.resolved = p && !p->map->instrsForLine(b.file, b.line).empty();
}

int Debugger::toggleLineBreakpoint(const std::string& file, int line) {
    SnapshotPtr p = programForMapping();
    for (size_t i = 0; i < bps_.size(); i++)
        if (bps_[i].file == file && bps_[i].line == line) {            // removing works for any line, code or not
            bps_.erase(bps_.begin() + (long)i);
            pushBreakpoints();
            breakpointsChanged.fire();
            return line;
        }
    int use = p ? p->map->nearestCodeLine(file, line) : 0;
    if (!use) return 0;
    for (const LineBreakpoint& b : bps_) if (b.file == file && b.line == use) {         // the next code line already has one: that is the toggle
        removeLineBreakpoint(file, use);
        return use;
    }
    LineBreakpoint b;
    b.file = file;
    b.line = use;
    bps_.push_back(b);
    pushBreakpoints();
    breakpointsChanged.fire();
    return use;
}

bool Debugger::hasLineBreakpoint(const std::string& file, int line) const {
    for (const LineBreakpoint& b : bps_) if (b.file == file && b.line == line) return true;
    return false;
}

std::vector<LineBreakpoint> Debugger::lineBreakpoints() const {
    std::vector<LineBreakpoint> out = bps_;
    ViewPtr v = session_.view();
    if (!v) return out;
    // The simulator counts hits per address breakpoint; attribute each to the line whose spec has the same (domain, pc, condition).
    for (const sim::Breakpoint& b : v->breakpoints)
        for (size_t i = 0; i < specList_.size() && i < specOwner_.size(); i++)
            if (specList_[i].domain == b.domain && specList_[i].pc == b.pc && specList_[i].condition == b.condition) { out[specOwner_[i]].hits += b.hits; break; }
    return out;
}

void Debugger::setCondition(const std::string& file, int line, const std::string& cond) {
    for (LineBreakpoint& b : bps_) if (b.file == file && b.line == line) b.condition = cond;
    pushBreakpoints();
    breakpointsChanged.fire();
}
void Debugger::setEnabled(const std::string& file, int line, bool en) {
    for (LineBreakpoint& b : bps_) if (b.file == file && b.line == line) b.enabled = en;
    pushBreakpoints();
    breakpointsChanged.fire();
}
void Debugger::removeLineBreakpoint(const std::string& file, int line) {
    bps_.erase(std::remove_if(bps_.begin(), bps_.end(), [&](const LineBreakpoint& b) { return b.file == file && b.line == line; }), bps_.end());
    pushBreakpoints();
    breakpointsChanged.fire();
}
void Debugger::clearLineBreakpoints() {
    bps_.clear();
    pushBreakpoints();
    breakpointsChanged.fire();
}

void Debugger::linesShifted(const std::string& file, int fromLine, int delta) {
    if (!delta) return;
    bool moved = false;
    for (auto it = bps_.begin(); it != bps_.end();) {
        if (it->file == file && it->line > fromLine) {
            it->line += delta;
            moved = true;
            if (it->line < 1) { it = bps_.erase(it); continue; }
        }
        ++it;
    }
    if (moved) { pushBreakpoints(); breakpointsChanged.fire(); }
}

void Debugger::saveBreakpoints() {
    if (!settings_ || !ws_.hasProject()) return;
    std::vector<std::string> v;
    for (const LineBreakpoint& b : bps_) v.push_back(b.file + "|" + std::to_string(b.line) + "|" + (b.enabled ? "1" : "0") + "|" + b.condition);
    settings_->setList("breakpoints." + ws_.project().file, v);
    settings_->setList("watchexprs." + ws_.project().file, watchExprs_);
    settings_->setList("datawatches." + ws_.project().file, dataWatches_);
}

void Debugger::loadBreakpoints() {
    bps_.clear();
    if (!settings_ || !ws_.hasProject()) return;
    for (const std::string& s : settings_->getList("breakpoints." + ws_.project().file)) {
        std::vector<std::string> parts;
        std::stringstream ss(s);
        std::string part;
        while (parts.size() < 3 && std::getline(ss, part, '|')) parts.push_back(part);
        std::string cond;
        std::getline(ss, cond);
        if (parts.size() < 3) continue;
        LineBreakpoint b;
        b.file = parts[0];
        try { b.line = std::stoi(parts[1]); } catch (...) { continue; }
        b.enabled = parts[2] == "1";
        b.condition = cond;
        bps_.push_back(b);
    }
    watchExprs_ = settings_->getList("watchexprs." + ws_.project().file);
    dataWatches_ = settings_->getList("datawatches." + ws_.project().file);
    pushBreakpoints();
    breakpointsChanged.fire();
}

// ---- locations -------------------------------------------------------------------------------------------------------------------------
DebugLocation Debugger::locationOf(Domain d, uint32_t pc) const {
    DebugLocation l;
    SnapshotPtr p = session_.program();
    if (!p) return l;
    const InstrInfo* in = p->map->instrAt(d, pc);
    if (!in) return l;
    l.valid = true;
    l.domain = d;
    l.pc = pc;
    l.file = in->loc.file;
    l.line = in->loc.line;
    l.frames = in->frames;
    return l;
}

DebugLocation Debugger::location(Domain d) const {
    ViewPtr v = session_.view();
    if (!v) return DebugLocation();
    return locationOf(d, v->pc[d == Domain::Main ? 0 : 1]);
}

DebugLocation Debugger::stopLocation(const SimUpdate& u) const {
    if (!u.view) return DebugLocation();
    // "Run to line" reports the line that was asked for (an inline CALL line stops inside the procedure body).
    if (runTo_.active && u.view->pc[runTo_.domain == Domain::Main ? 0 : 1] == runTo_.pc && u.stop.reason == sim::StopReason::Breakpoint) {
        DebugLocation l = locationOf(runTo_.domain, runTo_.pc);
        if (l.valid) { l.file = runTo_.file; l.line = runTo_.line; }
        return l;
    }
    // A line breakpoint reports its own line (for an inline CALL line the stopped instruction belongs to the procedure body).
    if (u.stop.reason == sim::StopReason::Breakpoint)
        for (const sim::Breakpoint& b : u.view->breakpoints) {
            if (b.id != u.stop.id) continue;
            for (size_t k = 0; k < specList_.size() && k < specOwner_.size(); k++)
                if (specList_[k].domain == b.domain && specList_[k].pc == b.pc && specList_[k].condition == b.condition) {
                    DebugLocation l = locationOf(b.domain, b.pc);
                    if (l.valid) { l.file = bps_[specOwner_[k]].file; l.line = bps_[specOwner_[k]].line; }
                    return l;
                }
        }
    Domain d = focus_;
    if (u.stop.reason == sim::StopReason::Breakpoint || u.stop.reason == sim::StopReason::Watchpoint) {
        for (const sim::Breakpoint& b : u.view->breakpoints) if (b.id == u.stop.id) d = b.domain;
        if (u.stop.reason == sim::StopReason::Breakpoint && u.stop.detail.compare(0, 4, "peri") == 0) d = Domain::Peri;
        else if (u.stop.reason == sim::StopReason::Breakpoint && u.stop.detail.compare(0, 4, "main") == 0) d = Domain::Main;
    }
    for (int i = 0; i < 2; i++) if (u.view->hasFault[i] && u.stop.reason == sim::StopReason::Fault) d = i == 0 ? Domain::Main : Domain::Peri;
    return locationOf(d, u.view->pc[d == Domain::Main ? 0 : 1]);
}

void Debugger::onUpdate(const SimUpdate& u) {
    updated.fire(u);
    if (u.stop.valid) {
        DebugLocation where = stopLocation(u);
        runTo_.active = false;
        stopped.fire(u, where);
    }
}

}  // namespace acide
