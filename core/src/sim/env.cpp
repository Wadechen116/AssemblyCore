// Environment file: [sim] [timing] [latency] [bus i2c] [signal] [csr] [device] [condition] [inject]  (docs/SIM.md)
#include "impl.h"

namespace asmcore {
namespace sim {

namespace {
bool numberOf(Simulator::Impl& I, const std::string& text, int64_t& v) {
    std::string err;
    auto look = [&](const std::string& n, int64_t& out) {
        auto it = I.csrSymbols.find(n);
        if (it == I.csrSymbols.end()) return false;
        out = it->second;
        return true;
    };
    return evalExpr(text, look, v, err);
}
}  // namespace

bool Simulator::Impl::buildCsr(const IniSection& s) {
    CsrDef d;
    d.name = s.name;
    auto line = [&](const std::string& m) { errors.push_back("line " + std::to_string(s.line) + " [csr " + s.name + "]: " + m); };
    int64_t v = 0;
    if (const std::string* a = s.get("addr")) {
        if (!numberOf(*this, *a, v)) { line("bad addr '" + *a + "'"); return false; }
    } else {
        auto it = csrSymbols.find(s.name);
        if (it == csrSymbols.end()) { line("no addr, and the program defines no .csr named '" + s.name + "'"); return false; }
        v = it->second;
    }
    if (v < 0 || v > 0xFFF) { line("addr out of range"); return false; }
    d.addr = (uint32_t)v;
    if (const std::string* k = s.get("kind")) d.kind = lower(*k);
    if (d.kind != "ram" && d.kind != "value" && d.kind != "action" && d.kind != "status" && d.kind != "const") { line("kind must be ram | value | action | status | const"); return false; }
    if (const std::string* r = s.get("reset")) { if (!numberOf(*this, *r, v)) { line("bad reset"); return false; } d.reset = (int)v; }
    if (const std::string* b = s.get("bind")) d.bind = *b;
    if (const std::string* b = s.get("bytes")) { if (!numberOf(*this, *b, v) || v < 1 || v > 4) { line("bytes must be 1..4"); return false; } d.bytes = (int)v; }
    if (const std::string* c = s.get("const")) { if (!numberOf(*this, *c, v)) { line("bad const"); return false; } d.constVal = v; }
    for (int b = 0; b < 8; b++) {
        if (const std::string* x = s.get("bit" + std::to_string(b))) { if (d.kind == "status") d.bitExpr[b] = *x; else d.bitAction[b] = *x; }
    }
    if (d.kind == "value") {
        if (d.bind.empty() || sigId(d.bind) < 0) { line("kind=value needs bind = <existing signal>"); return false; }
    }
    if (d.kind == "action") {
        for (int b = 0; b < 8; b++) if (!d.bitAction[b].empty()) {
            std::string nm = d.bitAction[b].substr(0, d.bitAction[b].find(':'));
            if (!ext.actions().count(nm)) { line("bit" + std::to_string(b) + ": unknown action '" + nm + "'"); return false; }
        }
    }
    int span = d.kind == "value" ? d.bytes : 1;
    for (int k = 0; k < span; k++) {
        if (csrOwner.count(d.addr + k)) { line("address " + std::to_string(d.addr + k) + " is already defined"); return false; }
    }
    for (int k = 0; k < span; k++) csrOwner[d.addr + k] = d.addr;
    csrDefs[d.addr] = d;
    return true;
}

bool Simulator::Impl::buildDevice(const IniSection& s) {
    DeviceSpec ds;
    ds.name = s.name;
    for (auto& kv : s.kv) ds.keys[kv.first].push_back(kv.second);
    auto line = [&](const std::string& m) { errors.push_back("line " + std::to_string(s.line) + " [device " + s.name + "]: " + m); };
    int64_t v = 0;
    const std::string* a = s.get("addr");
    if (!a || !numberOf(*this, *a, v) || v < 0 || v > 0x7F) { line("addr = <7-bit address> is required"); return false; }
    ds.addr = (int)v;
    std::string kind = s.get("kind") ? lower(*s.get("kind")) : "scripted";
    auto f = ext.devices().find(kind);
    if (f == ext.devices().end()) { line("unknown device kind '" + kind + "'"); return false; }
    if (devIndexByAddr.count(ds.addr)) { line("I2C address " + std::to_string(ds.addr) + " is used twice"); return false; }
    std::string err;
    std::unique_ptr<I2cDevice> dev = f->second(ds, err);
    if (!dev) { line(err.empty() ? "device could not be created" : err); return false; }
    DevInst di_;
    di_.spec = ds;
    di_.dev = std::move(dev);
    devIndexByAddr[ds.addr] = (int)devs.size();
    devs.push_back(std::move(di_));
    return true;
}

void Simulator::Impl::loadEnv(const std::string& text) {
    std::vector<IniSection> secs = parseIni(text, errors);
    // pass 1: things the others refer to (signals, latency)
    for (const IniSection& s : secs) {
        if (s.kind == "signal") {
            int64_t v = 0;
            if (const std::string* i = s.get("init")) numberOf(*this, *i, v);
            if (sigId(s.name) >= 0) errors.push_back("line " + std::to_string(s.line) + ": signal '" + s.name + "' already exists");
            else defStored(s.name, v);
        } else if (s.kind == "latency") {
            std::string t;
            for (auto& kv : s.kv) t += kv.first + " = " + kv.second + "\n";
            std::vector<std::string> pe;
            if (!pf.load(t, pe)) for (auto& e : pe) errors.push_back("[latency] " + e);
            lat = pf.latency;
        }
    }
    for (const IniSection& s : secs) {
        auto line = [&](const std::string& m) { errors.push_back("line " + std::to_string(s.line) + " [" + s.kind + (s.name.empty() ? "" : " " + s.name) + "]: " + m); };
        int64_t v = 0;
        if (s.kind == "sim") {
            for (auto& kv : s.kv) {
                if (kv.first == "tie_order") tieMainFirst = lower(kv.second).compare(0, 4, "peri") != 0;
                else if (kv.first == "unmapped_csr") unmappedFault = lower(kv.second) == "fault";
                else if (kv.first == "fault_stops") faultStopsAll = lower(kv.second) == "all";
                else line("unknown key '" + kv.first + "'");
            }
        } else if (s.kind == "timing") {
            struct K { const char* k; int64_t TimingCfg::*m; };
            const K ks[] = {{"frame_cycles", &TimingCfg::frameCycles}, {"vblank_start", &TimingCfg::vStart}, {"vblank_len", &TimingCfg::vLen},
                            {"line_cycles", &TimingCfg::lineCycles}, {"hblank_start", &TimingCfg::hStart}, {"hblank_len", &TimingCfg::hLen}};
            for (auto& kv : s.kv) {
                bool hit = false;
                for (const K& k : ks) if (kv.first == k.k) {
                    hit = true;
                    if (numberOf(*this, kv.second, v)) timing.*k.m = v; else line("bad number for " + kv.first);
                }
                if (!hit) line("unknown key '" + kv.first + "'");
            }
        } else if (s.kind == "bus") {
            if (lower(s.name) != "i2c") { line("only 'bus i2c' is supported"); continue; }
            for (auto& kv : s.kv) {
                if (kv.first == "bus_hz" && numberOf(*this, kv.second, v)) { pf.latency.i2cHz = (int)v; lat = pf.latency; }
                else if (kv.first == "cpu_hz" && numberOf(*this, kv.second, v)) { pf.latency.cpuHz = (int)v; lat = pf.latency; }
                else line("unknown key '" + kv.first + "'");
            }
        } else if (s.kind == "csr") {
            buildCsr(s);
        } else if (s.kind == "device") {
            buildDevice(s);
        } else if (s.kind == "condition") {
            std::string dom = lower(s.name.substr(0, s.name.find(' ')));
            std::string code = trim(s.name.substr(s.name.find(' ') == std::string::npos ? s.name.size() : s.name.find(' ')));
            int d = dom == "main" ? 0 : dom == "peri" ? 1 : -1;
            if (d < 0 || !numberOf(*this, code, v) || v < 0 || v > 15) { line("expected [condition main|peri <0..15>]"); continue; }
            const std::string* ex = s.get("expr");
            if (!ex) { line("expr = ... is required"); continue; }
            std::string err;
            Cond c;
            if (!compile(*ex, c.e, err)) { line(err); continue; }
            c.defined = true;
            c.name = s.get("name") ? *s.get("name") : "";
            cond[d][v] = c;
        } else if (s.kind == "inject") {
            for (auto& kv : s.kv) {
                if (kv.first.compare(0, 3, "at ") != 0 || !numberOf(*this, kv.first.substr(3), v) || v < 0) { line("expected 'at <cycle> = <command>'"); continue; }
                envInjects.push_back({(Cycle)v, kv.second});
            }
        } else if (s.kind == "params") {
            std::string t;
            for (auto& kv : s.kv) t += kv.first + " = " + kv.second + "\n";
            std::vector<std::string> pe;
            if (!params.load(t, pe)) for (auto& m : pe) line(m);
        } else if (s.kind != "signal" && s.kind != "latency") {
            line("unknown section kind");
        }
    }
}

}  // namespace sim
}  // namespace asmcore
