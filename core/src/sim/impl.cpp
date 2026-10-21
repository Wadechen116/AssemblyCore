// Simulator construction, signals, expression binding, data regions, CSR bus, faults.
#include <algorithm>
#include <cstring>

#include "impl.h"

namespace asmcore {
namespace sim {

// ---- construction --------------------------------------------------------------------------------------------------------------
Simulator::Impl::Impl(const Result& r, const SimConfig& c, const Extensions* e) : cfg(c), ext(e ? *e : Extensions::defaults()), pf(c.profile), lat(c.profile.latency) {
    std::string err;
    if (!img[0].load(r.main, pf, err) && !err.empty()) errors.push_back(err);
    err.clear();
    if (!img[1].load(r.peri, pf, err) && !err.empty()) errors.push_back(err);
    if (!img[0].present && !img[1].present) errors.push_back("the assembler result holds no image");
    instrs = r.instrs;
    callSites = r.callSites;
    for (const InstrInfo& ii : instrs) instrAt[di(ii.domain)][ii.pc] = &ii;
    softret = r.callModel == "softret";
    for (auto& row : vecPc) for (auto& v : row) v = -1;
    for (const VectorInfo& v : r.vectors) if (v.id >= 0 && v.id < 64) vecPc[di(v.domain)][v.id] = v.pc;
    if (softret) for (const CallSiteInfo& cs : callSites) callByTransfer[di(cs.domain)][cs.transferPc] = &cs;
    for (const SymbolInfo& s : r.symbols) {
        if (s.kind == "label") labels[s.name] = (uint32_t)s.value;
        else if (s.kind == "csr") csrSymbols[s.name] = (uint32_t)s.value;
    }
    // MAIN_CONST_POOL size
    for (const SectionInfo& s : r.main.sections) if (s.name == "MAIN_CONST_POOL") constSize = s.size;
    registerOps();
    buildSignals();
    buildDefaultConditions();
    loadEnv(cfg.env);
    resetState();
}

void Simulator::Impl::registerOps() {
    // Main: spec 2 (opcode map)
    mainOps[0] = [this](OpCtx& c) { opMainCsr(c); };
    mainOps[1] = [this](OpCtx& c) { opMainBr(c); };
    mainOps[2] = [this](OpCtx& c) { opMainMov(c); };
    mainOps[3] = [this](OpCtx& c) { opMainAlu(c); };
    // Peri
    periOps[0] = [this](OpCtx& c) { opPeriCsr(c); };
    periOps[1] = [this](OpCtx& c) { opPeriBr(c); };
    periOps[2] = [this](OpCtx& c) { opPeriPub(c); };
    periOps[3] = [this](OpCtx& c) { opPeriXfer(c); };
}

void Simulator::Impl::defStored(const std::string& name, int64_t init) {
    SigDef d;
    d.name = name;
    d.stored = nStored++;
    d.init = init;
    sigIndex[name] = (int)sigs.size();
    sigs.push_back(d);
    getters.push_back(nullptr);
    setters.push_back(nullptr);
}
void Simulator::Impl::defComputed(const std::string& name, std::function<int64_t(const Impl&)> get, std::function<void(Impl&, int64_t)> set) {
    SigDef d;
    d.name = name;
    sigIndex[name] = (int)sigs.size();
    sigs.push_back(d);
    getters.push_back(std::move(get));
    setters.push_back(std::move(set));
}

void Simulator::Impl::buildSignals() {
    // latches and flags driven by the sequencers and by CSR actions
    for (const char* n : {"poll.ready", "poll.not_ready", "poll.timeout", "poll.arm", "timer.done", "peri.done", "peri.error", "main_timer.done", "frame_end_evt",
                          "pgl_pending", "ae_pending", "line_pending", "wcet_pgl", "wcet_ae", "wcet_line", "poll_mask", "timer_period", "main_timer_period", "timer.gen", "main_timer.gen"})
        defStored(n, 0);
    // the result slot (spec 12)
    defComputed("main.in_handler", [](const Impl& I) { return (int64_t)I.st.ev[0].active; });
    defComputed("peri.in_handler", [](const Impl& I) { return (int64_t)I.st.ev[1].active; });
    defComputed("main.events_pending", [](const Impl& I) { return (int64_t)I.st.ev[0].pending; });
    defComputed("peri.events_pending", [](const Impl& I) { return (int64_t)I.st.ev[1].pending; });
    defComputed("slot.valid", [](const Impl& I) { return (int64_t)I.st.slot.valid; }, [](Impl& I, int64_t v) { I.st.slot.valid = v != 0; });
    defComputed("slot.busy", [](const Impl& I) { return (int64_t)I.st.slot.busy; }, [](Impl& I, int64_t v) { I.st.slot.busy = v != 0; });
    defComputed("slot.overrun", [](const Impl& I) { return (int64_t)I.st.slot.overrun; }, [](Impl& I, int64_t v) { I.st.slot.overrun = v != 0; });
    defComputed("slot.service_id", [](const Impl& I) { return (int64_t)I.st.slot.svc; });
    // ALU_STATUS (spec 6.8)
    defComputed("alu.cmp_true", [](const Impl& I) { return (int64_t)I.st.m.cmp; });
    defComputed("alu.zero", [](const Impl& I) { return (int64_t)I.st.m.zero; });
    defComputed("alu.negative", [](const Impl& I) { return (int64_t)I.st.m.neg; });
    defComputed("alu.carry_u", [](const Impl& I) { return (int64_t)I.st.m.carry; });
    defComputed("alu.borrow_u", [](const Impl& I) { return (int64_t)I.st.m.borrow; });
    defComputed("alu.overflow_s", [](const Impl& I) { return (int64_t)I.st.m.ovs; });
    defComputed("alu.ov16", [](const Impl& I) { return (int64_t)I.st.m.ov16; });
    // video timing (spec 7.4, 14): derived from time, so nothing has to tick
    defComputed("vblank_open", [](const Impl& I) {
        const TimingCfg& t = I.timing;
        if (t.frameCycles <= 0 || t.vLen <= 0) return (int64_t)0;
        int64_t p = (int64_t)(I.st.now % (Cycle)t.frameCycles);
        return (int64_t)(p >= t.vStart && p < t.vStart + t.vLen);
    });
    defComputed("remaining_vblank_cycles", [](const Impl& I) {
        const TimingCfg& t = I.timing;
        if (t.frameCycles <= 0 || t.vLen <= 0) return (int64_t)0;
        int64_t p = (int64_t)(I.st.now % (Cycle)t.frameCycles);
        return (p >= t.vStart && p < t.vStart + t.vLen) ? t.vStart + t.vLen - p : (int64_t)0;
    });
    defComputed("hblank_open", [](const Impl& I) {
        const TimingCfg& t = I.timing;
        if (t.lineCycles <= 0 || t.hLen <= 0) return (int64_t)0;
        int64_t p = (int64_t)(I.st.now % (Cycle)t.lineCycles);
        return (int64_t)(p >= t.hStart && p < t.hStart + t.hLen);
    });
    defComputed("remaining_hblank_cycles", [](const Impl& I) {
        const TimingCfg& t = I.timing;
        if (t.lineCycles <= 0 || t.hLen <= 0) return (int64_t)0;
        int64_t p = (int64_t)(I.st.now % (Cycle)t.lineCycles);
        return (p >= t.hStart && p < t.hStart + t.hLen) ? t.hStart + t.hLen - p : (int64_t)0;
    });
}

void Simulator::Impl::buildDefaultConditions() {
    // Main ConditionMap (spec 7.3). Each entry is an expression over signals; [condition main N] in the environment file replaces it.
    static const struct { int code; const char* name; const char* expr; } kMain[] = {
        {0x0, "ALWAYS", "1"}, {0x1, "PERI_RESULT_VALID", "slot.valid"}, {0x2, "PERI_RESULT_OVERRUN", "slot.overrun"},
        {0x3, "PERI_ERROR", "peri.done && peri.error"}, {0x4, "PERI_NO_ERROR", "peri.done && !peri.error"},
        {0x5, "ALU_CMP_TRUE", "alu.cmp_true"}, {0x6, "ALU_OUT_OV16", "alu.ov16"}, {0x7, "MAIN_TIMER_DONE", "main_timer.done"},
        {0x8, "FRAME_END_EVT", "frame_end_evt"}, {0x9, "VBLANK_OPEN", "vblank_open"}, {0xA, "HBLANK_OPEN", "hblank_open"},
        {0xB, "PGL_COMMIT_ELIGIBLE", "pgl_pending && vblank_open && remaining_vblank_cycles >= wcet_pgl"},
        {0xC, "AE_COMMIT_ELIGIBLE", "ae_pending && vblank_open && remaining_vblank_cycles >= wcet_ae"},
        {0xD, "LINE_COMMIT_ELIGIBLE", "line_pending && hblank_open && remaining_hblank_cycles >= wcet_line"},
    };
    // Peri BR (spec 11.2)
    static const struct { int code; const char* name; const char* expr; } kPeri[] = {
        {0x0, "ALWAYS", "1"}, {0x1, "IF_READY", "poll.ready"}, {0x2, "IF_NOT_READY", "poll.not_ready"}, {0x3, "IF_TIMEOUT", "poll.timeout"},
        {0x4, "IF_ERROR", "peri.error"}, {0x5, "IF_NO_ERROR", "!peri.error"}, {0x6, "IF_TIMER_DONE", "timer.done"}, {0x7, "IF_TIMER_NOT_DONE", "!timer.done"},
    };
    std::string e;
    for (auto& c : kMain) { cond[0][c.code].defined = compile(c.expr, cond[0][c.code].e, e); cond[0][c.code].name = c.name; }
    for (auto& c : kPeri) { cond[1][c.code].defined = compile(c.expr, cond[1][c.code].e, e); cond[1][c.code].name = c.name; }
}

void Simulator::Impl::resetState() {
    State n;
    n.sig.assign((size_t)nStored, 0);
    for (const SigDef& d : sigs) if (d.stored >= 0) n.sig[(size_t)d.stored] = d.init;
    n.dev.resize(devs.size());
    st = n;
    st.m.bufOk = pf.csrBufResetDefined ? 0xFFFFFFFFu : 0u;
    for (auto& kv : csrDefs) if (kv.second.kind == "ram") st.csr[kv.first] = (uint8_t)kv.second.reset;
    for (size_t i = 0; i < devs.size(); i++) devs[i].dev->init(st.dev[i]);
    if (timing.frameCycles > 0) schedule((Cycle)timing.frameCycles, TimedEvent::FrameEnd);
    if (timing.frameCycles > 0 && timing.vLen > 0) { schedule((Cycle)timing.vStart, TimedEvent::WindowEdge, 0, 1); }
    if (timing.lineCycles > 0 && timing.hLen > 0) { schedule((Cycle)timing.hStart, TimedEvent::WindowEdge, 1, 1); }
    for (auto& ij : envInjects) schedule(ij.first, TimedEvent::Command, 0, 0, ij.second);
    if (!img[0].present) { st.m.halted = true; st.m.readyAt = kNever; }
    if (!img[1].present) { st.p.halted = true; st.p.readyAt = kNever; }
    nextSnap = 0;
    history.clear();
    lastWrites.clear();
}

// ---- signals -------------------------------------------------------------------------------------------------------------------
int64_t Simulator::Impl::sigGet(int id) const {
    const SigDef& d = sigs[(size_t)id];
    if (d.stored >= 0) return st.sig[(size_t)d.stored];
    return getters[(size_t)id] ? getters[(size_t)id](*this) : 0;
}
void Simulator::Impl::sigSet(int id, int64_t v) {
    const SigDef& d = sigs[(size_t)id];
    if (d.stored >= 0) {
        int64_t old = st.sig[(size_t)d.stored];
        if (old == v) return;
        st.sig[(size_t)d.stored] = v;
        emit([&](Event& e) { e.type = EventType::Signal; e.what = d.name; e.a = old; e.b = v; });
    } else if (setters[(size_t)id]) {
        int64_t old = getters[(size_t)id] ? getters[(size_t)id](*this) : 0;
        setters[(size_t)id](*this, v);
        if (old != v) emit([&](Event& e) { e.type = EventType::Signal; e.what = d.name; e.a = old; e.b = v; });
    }
}
int64_t Simulator::Impl::signal(const std::string& name) const {
    int id = sigId(name);
    return id < 0 ? 0 : sigGet(id);
}
void Simulator::Impl::setSignal(const std::string& name, int64_t value) {
    int id = sigId(name);
    if (id >= 0) sigSet(id, value);
    else emitNote("unknown signal '" + name + "'");
}
void Simulator::Impl::setSignalAfter(Cycle delay, const std::string& name, int64_t value) {
    int id = sigId(name);
    if (id < 0) { emitNote("unknown signal '" + name + "'"); return; }
    schedule(st.now + delay, TimedEvent::SetSignal, id, value, "");
}
void Simulator::Impl::setSignalAfterGuarded(Cycle delay, const std::string& name, int64_t value, const std::string& guardSignal, int64_t guardValue) {
    int id = sigId(name);
    if (id < 0 || sigId(guardSignal) < 0) { emitNote("unknown signal in guarded change"); return; }
    schedule(st.now + delay, TimedEvent::SetSignal, id, value, guardSignal + "=" + std::to_string(guardValue));
}
void Simulator::Impl::note(const std::string& text) { emitNote(text); }
void Simulator::Impl::emitNote(const std::string& text) {
    emit([&](Event& e) { e.type = EventType::Note; e.text = text; });
}

// ---- expressions -----------------------------------------------------------------------------------------------------------------
enum Special { S_NOW = 0, S_MPC, S_PPC, S_SCR, S_BUF, S_RX, S_SLOT, S_META, S_OPA, S_OPB, S_OUT, S_CONST, S_CSR, S_MHALT, S_PHALT, S_MFAULT, S_PFAULT, S_MINSTR, S_PINSTR, S_MCASC };
static const char* kSpecial[] = {"now", "main.pc", "peri.pc", "scratch", "csrbuf", "rx", "slot", "meta", "opa", "opb", "out", "const", "csr", "main.halted", "peri.halted", "main.faulted", "peri.faulted", "main.instrs", "peri.instrs", "main.cascade_active"};
static const int kSpecialBase = 100000;

int Simulator::Impl::bind(const std::string& name, bool indexed) {
    int id = sigId(name);
    if (id >= 0 && !indexed) return id;
    for (int i = 0; i < (int)(sizeof(kSpecial) / sizeof(kSpecial[0])); i++) if (name == kSpecial[i]) return kSpecialBase + i;
    return -1;
}

static int64_t le(const uint8_t* p, int n, int avail) {
    int64_t v = 0;
    for (int i = 0; i < n && i < 8; i++) v |= (int64_t)(i < avail ? p[i] : 0) << (8 * i);
    return v;
}

int64_t Simulator::Impl::get(int h, int64_t idx, int len) const {
    if (h < kSpecialBase) return sigGet(h);
    int n = len > 0 ? len : 1;
    auto arr = [&](const uint8_t* base, int size) -> int64_t {
        if (idx < 0 || idx >= size) return 0;
        return le(base + idx, n, size - (int)idx);
    };
    switch (h - kSpecialBase) {
        case S_NOW: return (int64_t)st.now;
        case S_MPC: return st.m.pc;
        case S_PPC: return st.p.pc;
        case S_SCR: return arr(st.m.scr, 32);
        case S_BUF: return arr(st.m.buf, 32);
        case S_RX: return arr(st.p.rx, 32);
        case S_SLOT: return arr(st.slot.payload, 32);
        case S_META: {
            uint8_t m[32] = {};
            m[0] = st.slot.svc; m[4] = st.slot.valid; m[5] = st.slot.overrun;
            return arr(m, 32);
        }
        case S_OPA: return st.m.opa;
        case S_OPB: return st.m.opb;
        case S_OUT: return (int64_t)st.m.out;
        case S_CONST: {
            if (idx < 0 || (size_t)(img[0].off[MT_CONST] + idx) >= img[0].bytes.size() || idx >= (int64_t)constSize) return 0;
            const uint8_t* b = img[0].bytes.data() + img[0].off[MT_CONST] + idx;
            return le(b, n, (int)constSize - (int)idx);
        }
        case S_CSR: {
            int64_t v = 0;
            for (int i = 0; i < n && i < 8; i++) { auto it = st.csr.find((uint32_t)(idx + i)); v |= (int64_t)(it == st.csr.end() ? 0 : it->second) << (8 * i); }
            return v;
        }
        case S_MHALT: return st.m.halted;
        case S_PHALT: return st.p.halted;
        case S_MFAULT: return st.m.faulted;
        case S_PFAULT: return st.p.faulted;
        case S_MINSTR: return (int64_t)st.m.instrs;
        case S_PINSTR: return (int64_t)st.p.instrs;
        case S_MCASC: return st.m.cascActive;
    }
    return 0;
}

bool Simulator::Impl::compile(const std::string& text, Expr& out, std::string& err) { return Expr::compile(text, *this, out, err); }

// ---- data regions ----------------------------------------------------------------------------------------------------------------
uint8_t Simulator::Impl::aluStatusByte() const {
    // ASSUMED (spec 6.8 lists the flags, not their byte layout): bit0 cmp_true, 1 zero, 2 negative, 3 carry_u, 4 borrow_u, 5 overflow_s, 6 ov16
    return (uint8_t)((st.m.cmp << 0) | (st.m.zero << 1) | (st.m.neg << 2) | (st.m.carry << 3) | (st.m.borrow << 4) | (st.m.ovs << 5) | (st.m.ov16 << 6));
}

bool Simulator::Impl::regionBytes(const std::string& region, int off, int len, std::vector<uint8_t>& out) const {
    out.clear();
    if (off < 0 || len < 1) return false;
    std::vector<uint8_t> src;
    if (region == "scratch") src.assign(st.m.scr, st.m.scr + 32);
    else if (region == "csrbuf") src.assign(st.m.buf, st.m.buf + 32);
    else if (region == "rx") src.assign(st.p.rx, st.p.rx + 32);
    else if (region == "slot") src.assign(st.slot.payload, st.slot.payload + 32);
    else if (region == "meta") { src.assign(32, 0); src[0] = st.slot.svc; src[4] = st.slot.valid; src[5] = st.slot.overrun; }
    else if (region == "opa") src = {(uint8_t)st.m.opa, (uint8_t)(st.m.opa >> 8)};
    else if (region == "opb") src = {(uint8_t)st.m.opb, (uint8_t)(st.m.opb >> 8)};
    else if (region == "out") src = {(uint8_t)st.m.out, (uint8_t)(st.m.out >> 8), (uint8_t)(st.m.out >> 16), (uint8_t)(st.m.out >> 24)};
    else if (region == "status") src = {aluStatusByte(), 0, 0, 0};
    else if (region == "const") { if (img[0].present) for (uint32_t i = 0; i < constSize; i++) src.push_back((uint8_t)img[0].u8(img[0].off[MT_CONST] + i)); }
    else return false;
    if ((size_t)off + (size_t)len > src.size()) return false;
    out.assign(src.begin() + off, src.begin() + off + len);
    return true;
}

void Simulator::Impl::noteWrite(const std::string& region, int off, int len) {
    lastWrites.push_back(WriteRec{region, off, len});
    emit([&](Event& e) { e.type = EventType::RegionWrite; e.what = region; e.a = off; e.b = len; });
}

// ---- CSR bus ---------------------------------------------------------------------------------------------------------------------
void Simulator::Impl::runAction(const std::string& spec) {
    if (spec.empty()) return;
    size_t c = spec.find(':');
    std::string name = c == std::string::npos ? spec : spec.substr(0, c);
    std::string arg = c == std::string::npos ? "" : spec.substr(c + 1);
    auto it = ext.actions().find(name);
    if (it == ext.actions().end()) { emitNote("unknown action '" + name + "'"); return; }
    it->second(*this, arg);
}

uint8_t Simulator::Impl::csrRead8(uint32_t addr, Domain by) {
    uint8_t v = 0;
    auto own = csrOwner.find(addr);
    if (own == csrOwner.end()) {
        if (unmappedFault) raise(by, di(by) == 0 ? st.m.pc : st.p.pc, "fault_csr_unmapped", "CSR " + std::to_string(addr) + " is not defined");
        auto it = st.csr.find(addr);
        v = it == st.csr.end() ? 0 : it->second;
    } else {
        const CsrDef& d = csrDefs[own->second];
        uint32_t k = addr - d.addr;
        if (d.kind == "ram") { auto it = st.csr.find(addr); v = it == st.csr.end() ? 0 : it->second; }
        else if (d.kind == "value") { int64_t sv = signal(d.bind); v = (uint8_t)((sv >> (8 * k)) & 0xFF); }
        else if (d.kind == "const") v = (uint8_t)((d.constVal >> (8 * k)) & 0xFF);
        else if (d.kind == "status") {
            for (int b = 0; b < 8; b++) if (!d.bitExpr[b].empty()) {
                Expr ex; std::string e;
                if (compile(d.bitExpr[b], ex, e) && ex.eval(*this)) v |= (uint8_t)(1u << b);
            }
        }
        // action registers read as 0
    }
    emit([&](Event& e) { e.type = EventType::CsrAccess; e.domain = di(by); e.what = "read"; e.a = addr; e.b = v; });
    return v;
}

void Simulator::Impl::csrWrite(uint32_t addr, uint8_t v, int mode, Domain by) {
    auto own = csrOwner.find(addr);
    static const char* mn[] = {"write", "set", "clr"};
    emit([&](Event& e) { e.type = EventType::CsrAccess; e.domain = di(by); e.what = mn[mode]; e.a = addr; e.b = v; });
    auto apply = [&](uint8_t cur) -> uint8_t { return mode == 0 ? v : mode == 1 ? (uint8_t)(cur | v) : (uint8_t)(cur & ~v); };
    if (own == csrOwner.end()) {
        if (unmappedFault) { raise(by, di(by) == 0 ? st.m.pc : st.p.pc, "fault_csr_unmapped", "CSR " + std::to_string(addr) + " is not defined"); return; }
        st.csr[addr] = apply(st.csr.count(addr) ? st.csr[addr] : 0);
        return;
    }
    const CsrDef& d = csrDefs[own->second];
    uint32_t k = addr - d.addr;
    if (d.kind == "ram") { st.csr[addr] = apply(st.csr.count(addr) ? st.csr[addr] : 0); }
    else if (d.kind == "value") {
        int64_t sv = signal(d.bind);
        uint8_t cur = (uint8_t)((sv >> (8 * k)) & 0xFF);
        int64_t nv = (sv & ~((int64_t)0xFF << (8 * k))) | ((int64_t)apply(cur) << (8 * k));
        setSignal(d.bind, nv);
    } else if (d.kind == "action") {
        uint8_t bits = mode == 2 ? 0 : v;       // write-1-to-pulse: clearing bits does nothing
        for (int b = 0; b < 8; b++) if (bits & (1u << b)) runAction(d.bitAction[b]);
    }
    // const / status: writes are ignored
}

// ---- faults ----------------------------------------------------------------------------------------------------------------------
void Simulator::Impl::raise(Domain d, uint32_t pc, const std::string& code, const std::string& msg) {
    int i = di(d);
    if (st.hasFault[i]) return;
    st.hasFault[i] = true;
    Fault f;
    f.domain = d;
    f.pc = pc;
    f.at = st.now;
    f.code = code;
    f.msg = msg;
    st.fault[i] = f;
    (i == 0 ? st.m.faulted : st.p.faulted) = true;
    (i == 0 ? st.m.readyAt : st.p.readyAt) = kNever;
    if (faultStopsAll) { st.m.readyAt = kNever; st.p.readyAt = kNever; }
    emit([&](Event& e) { e.type = EventType::Fault; e.domain = i; e.pc = pc; e.what = code; e.text = msg; });
    stopRequested = true;
    stopWhy = StopReason::Fault;
    stopDetail = std::string(domainName(d)) + " fault at pc " + std::to_string(pc) + ": " + code + " (" + msg + ")";
}

bool Simulator::Impl::fetchOk(Domain d, uint32_t pc) {
    if (pc >= img[di(d)].codeLimit) {
        raise(d, pc, "fault_pc_out_of_code", "pc " + std::to_string(pc) + " is outside the executable code (" + std::to_string(img[di(d)].codeLimit) + " bytes)");
        return false;
    }
    return true;
}

}  // namespace sim
}  // namespace asmcore
