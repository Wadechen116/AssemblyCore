// Hardware description: file format, generators, importer, validation. See include/asmcore/hardware.h.
#include "asmcore/hardware.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

#include "asmcore/asmcore.h"
#include "asmcore/sim.h"
#include "sim/ini.h"
#include "util.h"

namespace asmcore {

namespace {

bool parseNum(const std::string& text, long long& out) {
    std::string s = trim(text);
    if (s.empty()) return false;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    int base = 10;
    if (s.size() > i + 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
    else if (s.size() > i + 2 && s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; i += 2; }
    if (i >= s.size()) return false;
    long long v = 0;
    for (; i < s.size(); i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        v = v * base + d;
        if (v > (1LL << 40)) return false;
    }
    out = neg ? -v : v;
    return true;
}

std::string hex(long long v, int digits) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%0*llX", digits, (unsigned long long)v);
    return b;
}

bool identOk(const std::string& s) {
    if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

const char* const kRoles[] = {"timer_start", "timer_clear", "poll_arm", "poll_clear"};
bool roleOk(const std::string& r) { for (const char* x : kRoles) if (r == x) return true; return false; }

const char* const kEnvKinds[] = {"sim", "timing", "latency", "bus", "signal", "condition", "inject"};
bool envKind(const std::string& k) { for (const char* x : kEnvKinds) if (k == x) return true; return false; }

// "; text" at the end of a line -> the text; the line without it
std::string splitComment(const std::string& line, std::string& comment) {
    size_t at = line.find(';');
    if (at == std::string::npos) { comment.clear(); return line; }
    comment = trim(line.substr(at + 1));
    return line.substr(0, at);
}

std::string withNote(const std::string& line, const std::string& description) {
    return description.empty() ? line : line + "    ; " + description;
}

}  // namespace

HwCsr* Hardware::findCsr(const std::string& n) { for (auto& c : csrs) if (c.name == n) return &c; return nullptr; }
const HwCsr* Hardware::findCsr(const std::string& n) const { for (auto& c : csrs) if (c.name == n) return &c; return nullptr; }
HwDevice* Hardware::findDevice(const std::string& n) { for (auto& d : devices) if (d.name == n) return &d; return nullptr; }
HwService* Hardware::findService(const std::string& n) { for (auto& s : services) if (s.name == n) return &s; return nullptr; }

// ---- parse ---------------------------------------------------------------------------------------------------------------------------
bool parseHardware(const std::string& text, Hardware& out, std::vector<std::string>& errors) {
    out = Hardware();
    std::vector<std::string> perr;
    std::vector<sim::IniSection> secs = sim::parseIni(text, perr);
    bool ok = perr.empty();
    for (auto& e : perr) errors.push_back(e);
    auto err = [&](const sim::IniSection& s, const std::string& m) {
        errors.push_back("line " + std::to_string(s.line) + " [" + s.kind + (s.name.empty() ? "" : " " + s.name) + "]: " + m);
        ok = false;
    };
    for (const sim::IniSection& s : secs) {
        if (s.kind == "hardware") {
            for (auto& kv : s.kv) {
                if (kv.first == "name") out.name = kv.second;
                else if (kv.first == "description") out.description = kv.second;
                else if (kv.first == "include_file") out.includeFile = kv.second;
                else if (kv.first == "env_file") out.envFile = kv.second;
                else err(s, "unknown key '" + kv.first + "'");
            }
        } else if (s.kind == "equ") {
            HwEqu e;
            e.name = s.name;
            for (auto& kv : s.kv) {
                if (kv.first == "value") e.value = kv.second;
                else if (kv.first == "description") e.description = kv.second;
                else err(s, "unknown key '" + kv.first + "'");
            }
            out.equs.push_back(e);
        } else if (s.kind == "const") {
            HwConst c;
            c.name = s.name;
            for (auto& kv : s.kv) {
                long long v;
                if (kv.first == "width") { if (!parseNum(kv.second, v) || (v != 1 && v != 2)) err(s, "width must be 1 or 2"); else c.width = (int)v; }
                else if (kv.first == "values") for (const std::string& x : splitTop(kv.second)) c.values.push_back(trim(x));
                else if (kv.first == "description") c.description = kv.second;
                else err(s, "unknown key '" + kv.first + "'");
            }
            out.consts.push_back(c);
        } else if (s.kind == "csr") {
            HwCsr c;
            c.name = s.name;
            for (auto& kv : s.kv) {
                long long v;
                const std::string& k = kv.first;
                if (k == "addr") { if (!parseNum(kv.second, v)) err(s, "addr must be a number"); else c.addr = (int)v; }
                else if (k == "kind") c.kind = lower(kv.second);
                else if (k == "reset") { if (!parseNum(kv.second, v)) err(s, "reset must be a number"); else c.reset = (int)v; }
                else if (k == "bind") c.bind = kv.second;
                else if (k == "bytes") { if (!parseNum(kv.second, v)) err(s, "bytes must be a number"); else c.bytes = (int)v; }
                else if (k == "const") { if (!parseNum(kv.second, v)) err(s, "const must be a number"); else c.constVal = v; }
                else if (k == "description") c.description = kv.second;
                else if (k == "simulated") c.simulated = lower(kv.second) != "false";
                else if (k.size() == 4 && k.compare(0, 3, "bit") == 0 && k[3] >= '0' && k[3] <= '7') c.bit[k[3] - '0'] = kv.second;
                else if (k.size() == 5 && k.compare(0, 4, "role") == 0 && k[4] >= '0' && k[4] <= '7') c.role[k[4] - '0'] = lower(kv.second);
                else err(s, "unknown key '" + k + "'");
            }
            out.csrs.push_back(c);
        } else if (s.kind == "device") {
            HwDevice d;
            d.name = s.name;
            for (auto& kv : s.kv) {
                long long v;
                const std::string& k = kv.first;
                if (k == "addr") { if (!parseNum(kv.second, v)) err(s, "addr must be a number"); else d.addr = (int)v; }
                else if (k == "index") { if (!parseNum(kv.second, v)) err(s, "index must be a number"); else d.index = (int)v; }
                else if (k == "symbol") d.symbol = kv.second;
                else if (k == "kind") d.kind = lower(kv.second);
                else if (k == "simulated") d.simulated = lower(kv.second) != "false";
                else if (k == "description") d.description = kv.second;
                else d.keys.push_back(kv);
            }
            out.devices.push_back(d);
        } else if (s.kind == "service") {
            HwService sv;
            sv.name = s.name;
            for (auto& kv : s.kv) {
                long long v;
                const std::string& k = kv.first;
                if (k == "id") { if (!parseNum(kv.second, v)) err(s, "id must be a number"); else sv.id = (int)v; }
                else if (k == "len") { if (!parseNum(kv.second, v)) err(s, "len must be a number"); else sv.len = (int)v; }
                else if (k == "wire") {
                    std::string w = lower(kv.second);
                    if (w == "big") sv.wire = 'B';
                    else if (w == "little") sv.wire = 'L';
                    else err(s, "wire must be big or little");
                } else if (k == "description") sv.description = kv.second;
                else err(s, "unknown key '" + k + "'");
            }
            out.services.push_back(sv);
        } else if (s.kind == "raw_inc") {
            for (auto& kv : s.kv) {
                if (kv.first == "line") out.rawInc.push_back(kv.second);
                else err(s, "unknown key '" + kv.first + "'");
            }
        } else if (envKind(s.kind)) {
            HwSection h;
            h.kind = s.kind;
            h.name = s.name;
            h.kv = s.kv;
            out.env.push_back(h);
        } else {
            err(s, "unknown section kind");
        }
    }
    return ok;
}

// ---- serialize -----------------------------------------------------------------------------------------------------------------------
namespace {

void writeSection(std::ostringstream& o, const std::string& head, const std::vector<std::pair<std::string, std::string>>& kv) {
    o << "\n[" << head << "]\n";
    for (auto& p : kv) o << p.first << " = " << p.second << "\n";
}

std::vector<std::pair<std::string, std::string>> csrKeys(const HwCsr& c, bool forEnv) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (!c.description.empty() && !forEnv) kv.push_back({"description", c.description});
    if (c.addr >= 0) kv.push_back({"addr", hex(c.addr, 3)});
    if (!c.simulated && !forEnv) kv.push_back({"simulated", "false"});
    kv.push_back({"kind", c.kind});
    if (c.kind == "ram" && c.reset != 0) kv.push_back({"reset", std::to_string(c.reset)});
    if (c.kind == "value") {
        kv.push_back({"bind", c.bind});
        if (c.bytes != 1) kv.push_back({"bytes", std::to_string(c.bytes)});
    }
    if (c.kind == "const") kv.push_back({"const", hex(c.constVal, 2)});
    for (int b = 0; b < 8; b++) if (!c.bit[b].empty()) kv.push_back({"bit" + std::to_string(b), c.bit[b]});
    if (!forEnv) for (int b = 0; b < 8; b++) if (!c.role[b].empty()) kv.push_back({"role" + std::to_string(b), c.role[b]});
    return kv;
}

}  // namespace

std::string serializeHardware(const Hardware& hw) {
    std::ostringstream o;
    o << "; AssemblyCore hardware description. Edited with the Hardware Designer (or by hand); `asmhw` generates the .inc and the environment file from it.\n";
    std::vector<std::pair<std::string, std::string>> head;
    if (!hw.name.empty()) head.push_back({"name", hw.name});
    if (!hw.description.empty()) head.push_back({"description", hw.description});
    if (!hw.includeFile.empty()) head.push_back({"include_file", hw.includeFile});
    if (!hw.envFile.empty()) head.push_back({"env_file", hw.envFile});
    writeSection(o, "hardware", head);
    for (const HwEqu& e : hw.equs) {
        std::vector<std::pair<std::string, std::string>> kv{{"value", e.value}};
        if (!e.description.empty()) kv.push_back({"description", e.description});
        writeSection(o, "equ " + e.name, kv);
    }
    for (const HwConst& c : hw.consts) {
        std::string vals;
        for (size_t i = 0; i < c.values.size(); i++) vals += (i ? ", " : "") + c.values[i];
        std::vector<std::pair<std::string, std::string>> kv{{"width", std::to_string(c.width)}, {"values", vals}};
        if (!c.description.empty()) kv.push_back({"description", c.description});
        writeSection(o, "const " + c.name, kv);
    }
    for (const HwCsr& c : hw.csrs) writeSection(o, "csr " + c.name, csrKeys(c, false));
    for (const HwDevice& d : hw.devices) {
        std::vector<std::pair<std::string, std::string>> kv;
        if (!d.description.empty()) kv.push_back({"description", d.description});
        kv.push_back({"addr", hex(d.addr, 2)});
        if (d.index >= 0) kv.push_back({"index", std::to_string(d.index)});
        if (!d.symbol.empty()) kv.push_back({"symbol", d.symbol});
        if (d.kind != "scripted") kv.push_back({"kind", d.kind});
        if (!d.simulated) kv.push_back({"simulated", "false"});
        for (auto& p : d.keys) kv.push_back(p);
        writeSection(o, "device " + d.name, kv);
    }
    for (const HwService& s : hw.services) {
        std::vector<std::pair<std::string, std::string>> kv{{"id", std::to_string(s.id)}};
        if (s.len >= 0) kv.push_back({"len", std::to_string(s.len)});
        if (s.wire == 'B') kv.push_back({"wire", "big"});
        if (s.wire == 'L') kv.push_back({"wire", "little"});
        if (!s.description.empty()) kv.push_back({"description", s.description});
        writeSection(o, "service " + s.name, kv);
    }
    for (const HwSection& s : hw.env) writeSection(o, s.kind + (s.name.empty() ? "" : " " + s.name), s.kv);
    if (!hw.rawInc.empty()) {
        std::vector<std::pair<std::string, std::string>> kv;
        for (auto& l : hw.rawInc) kv.push_back({"line", l});
        writeSection(o, "raw_inc", kv);
    }
    return o.str();
}

namespace {
std::string pathStem(const std::string& path) {              // directory part (with the separator) and file name without extension
    size_t slash = path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = file.rfind('.');
    return (slash == std::string::npos ? "" : path.substr(0, slash + 1)) + (dot == std::string::npos ? file : file.substr(0, dot));
}
std::string joinRel(const std::string& achwPath, const std::string& rel) {
    if (rel.empty()) return rel;
    const bool absolute = rel[0] == '/' || rel[0] == '\\' || (rel.size() > 1 && rel[1] == ':');
    if (absolute) return rel;
    size_t slash = achwPath.find_last_of("/\\");
    return (slash == std::string::npos ? "" : achwPath.substr(0, slash + 1)) + rel;
}
}  // namespace

std::string hardwareIncludePath(const std::string& achwPath, const Hardware& hw) { return hw.includeFile.empty() ? pathStem(achwPath) + ".gen.inc" : joinRel(achwPath, hw.includeFile); }
std::string hardwareEnvPath(const std::string& achwPath, const Hardware& hw) { return hw.envFile.empty() ? pathStem(achwPath) + ".gen.env.cfg" : joinRel(achwPath, hw.envFile); }

// ---- generators ----------------------------------------------------------------------------------------------------------------------
std::string generateInc(const Hardware& hw, const std::string& sourceName) {
    std::ostringstream o;
    o << "; Generated by the AssemblyCore Hardware Designer" << (sourceName.empty() ? "" : " from " + sourceName) << " -- do not edit; change the description and generate again.\n";
    if (!hw.name.empty()) o << "; Hardware: " << hw.name << "\n";
    if (!hw.equs.empty()) o << "\n";
    for (const HwEqu& e : hw.equs) o << withNote(".equ " + e.name + " = " + e.value, e.description) << "\n";
    if (!hw.consts.empty()) o << "\n";
    for (const HwConst& c : hw.consts) {
        std::string vals;
        for (size_t i = 0; i < c.values.size(); i++) vals += (i ? ", " : "") + c.values[i];
        o << withNote(".const " + c.name + (c.width == 2 ? " dw " : " db ") + vals, c.description) << "\n";
    }
    if (!hw.csrs.empty()) o << "\n";
    for (const HwCsr& c : hw.csrs) o << withNote(".csr " + c.name + " = " + hex(c.addr < 0 ? 0 : c.addr, 3), c.description) << "\n";
    bool anyRole = false;
    for (const HwCsr& c : hw.csrs) for (int b = 0; b < 8; b++) if (!c.role[b].empty()) {
        if (!anyRole) o << "\n; which CSR bits start / clear the timer and arm / clear the poll evaluator (lets the assembler check the flows)\n";
        anyRole = true;
        o << ".csrbit " << c.name << "[" << b << "] = " << c.role[b] << "\n";
    }
    if (!hw.services.empty()) o << "\n";
    for (const HwService& s : hw.services) {
        std::string line = ".service " + s.name + " = " + std::to_string(s.id);
        if (s.len >= 0) line += ", len=" + std::to_string(s.len);
        if (s.wire == 'B') line += ", big";
        if (s.wire == 'L') line += ", little";
        o << withNote(line, s.description) << "\n";
    }
    std::vector<const HwDevice*> mapped;
    for (const HwDevice& d : hw.devices) if (d.index >= 0) mapped.push_back(&d);
    std::sort(mapped.begin(), mapped.end(), [](const HwDevice* a, const HwDevice* b) { return a->index < b->index; });
    if (!mapped.empty()) o << "\n; DeviceMap: the physical endpoints EXEC_XFER dev=... selects\n";
    for (const HwDevice* d : mapped) {
        const std::string sym = d->symbol.empty() ? "DEV_" + upper(d->name) : d->symbol;
        o << withNote(".devmap " + std::to_string(d->index) + ", I2C, " + hex(d->addr, 2) + ", " + sym, d->description) << "\n";
    }
    if (!hw.rawInc.empty()) o << "\n";
    for (const std::string& l : hw.rawInc) o << l << "\n";
    return o.str();
}

std::string generateEnv(const Hardware& hw, const std::string& sourceName) {
    std::ostringstream o;
    o << "; Generated by the AssemblyCore Hardware Designer" << (sourceName.empty() ? "" : " from " + sourceName) << " -- do not edit; change the description and generate again.\n";
    auto emitKind = [&](const char* kind) {
        for (const HwSection& s : hw.env) if (s.kind == kind) writeSection(o, s.kind + (s.name.empty() ? "" : " " + s.name), s.kv);
    };
    for (const char* k : {"sim", "timing", "latency", "bus", "signal"}) emitKind(k);
    for (const HwCsr& c : hw.csrs) {
        if (c.simulated) writeSection(o, "csr " + c.name, csrKeys(c, true));
    }
    for (const HwDevice& d : hw.devices) {
        if (!d.simulated) continue;
        std::vector<std::pair<std::string, std::string>> kv;
        kv.push_back({"addr", hex(d.addr, 2)});
        if (d.kind != "scripted") kv.push_back({"kind", d.kind});
        for (auto& p : d.keys) kv.push_back(p);
        writeSection(o, "device " + d.name, kv);
    }
    emitKind("condition");
    emitKind("inject");
    return o.str();
}

// ---- import --------------------------------------------------------------------------------------------------------------------------
bool importHardware(const std::string& incText, const std::string& envText, Hardware& out, std::vector<std::string>& errors) {
    out = Hardware();
    bool ok = true;
    struct PendingBit { std::string csr; int bit; std::string role; int line; };
    std::vector<PendingBit> pendingBits;
    {
        std::istringstream in(incText);
        std::string raw;
        int n = 0;
        while (std::getline(in, raw)) {
            n++;
            std::string note;
            std::string line = trim(splitComment(raw, note));
            if (line.empty()) continue;
            auto bad = [&](const std::string& m) { errors.push_back("defs line " + std::to_string(n) + ": " + m); ok = false; };
            std::string low = lower(line);
            auto after = [&](size_t len) { return trim(line.substr(len)); };
            if (low.compare(0, 5, ".equ ") == 0) {
                std::string r = after(5);
                size_t eq = r.find('=');
                std::vector<std::string> a = eq != std::string::npos && r.find(',') == std::string::npos ? std::vector<std::string>{trim(r.substr(0, eq)), trim(r.substr(eq + 1))} : splitTop(r);
                if (a.size() != 2) { out.rawInc.push_back(line); continue; }
                out.equs.push_back({trim(a[0]), trim(a[1]), note});
            } else if (low.compare(0, 5, ".csr ") == 0) {
                std::string r = after(5);
                size_t eq = r.find('=');
                long long v;
                if (eq == std::string::npos || !parseNum(r.substr(eq + 1), v)) { bad(".csr needs NAME = number"); out.rawInc.push_back(line); continue; }
                HwCsr c;
                c.name = trim(r.substr(0, eq));
                c.addr = (int)v;
                c.description = note;
                out.csrs.push_back(c);
            } else if (low.compare(0, 8, ".csrbit ") == 0) {
                std::string r = after(8);
                size_t lb = r.find('['), rb = r.find(']'), eq = r.find('=');
                long long bit;
                if (lb == std::string::npos || rb == std::string::npos || eq == std::string::npos || !parseNum(r.substr(lb + 1, rb - lb - 1), bit) || bit < 0 || bit > 7) { bad(".csrbit CSR[bit] = role"); out.rawInc.push_back(line); continue; }
                pendingBits.push_back({trim(r.substr(0, lb)), (int)bit, lower(trim(r.substr(eq + 1))), n});
            } else if (low.compare(0, 9, ".service ") == 0) {
                std::string r = after(9);
                size_t eq = r.find('=');
                if (eq == std::string::npos) { bad(".service NAME = id"); out.rawInc.push_back(line); continue; }
                std::vector<std::string> a = splitTop(r.substr(eq + 1));
                long long id;
                if (a.empty() || !parseNum(a[0], id)) { bad("the service id must be a number"); out.rawInc.push_back(line); continue; }
                HwService s;
                s.name = trim(r.substr(0, eq));
                s.id = (int)id;
                s.description = note;
                for (size_t k = 1; k < a.size(); k++) {
                    std::string o2 = lower(trim(a[k]));
                    long long len;
                    if (o2 == "big") s.wire = 'B';
                    else if (o2 == "little") s.wire = 'L';
                    else if (o2.compare(0, 4, "len=") == 0 && parseNum(o2.substr(4), len)) s.len = (int)len;
                    else bad("unknown .service option '" + o2 + "'");
                }
                out.services.push_back(s);
            } else if (low.compare(0, 8, ".devmap ") == 0) {
                std::vector<std::string> a = splitTop(after(8));
                long long idx, addr;
                if (a.size() < 3 || !parseNum(a[0], idx) || !parseNum(a[2], addr)) { bad(".devmap index, I2C, addr [, NAME]"); out.rawInc.push_back(line); continue; }
                HwDevice d;
                d.index = (int)idx;
                d.addr = (int)addr;
                d.symbol = a.size() > 3 ? trim(a[3]) : "";
                d.name = d.symbol.empty() ? "dev" + std::to_string(idx) : lower(d.symbol.compare(0, 4, "DEV_") == 0 ? d.symbol.substr(4) : d.symbol);
                d.description = note;
                d.simulated = false;                                   // until an environment [device] with this address says otherwise
                out.devices.push_back(d);
            } else if (low.compare(0, 7, ".const ") == 0) {
                std::string r = after(7);
                size_t sp = r.find_first_of(" \t");
                if (sp == std::string::npos) { out.rawInc.push_back(line); continue; }
                std::string rest = trim(r.substr(sp));
                size_t sp2 = rest.find_first_of(" \t");
                if (sp2 == std::string::npos) { out.rawInc.push_back(line); continue; }
                std::string w = lower(rest.substr(0, sp2));
                if (w != "db" && w != "dw") { out.rawInc.push_back(line); continue; }
                HwConst c;
                c.name = r.substr(0, sp);
                c.width = w == "dw" ? 2 : 1;
                for (const std::string& v : splitTop(rest.substr(sp2))) c.values.push_back(trim(v));
                c.description = note;
                out.consts.push_back(c);
            } else {
                out.rawInc.push_back(line);                            // .include, comments are gone, anything else stays as it is
            }
        }
    }
    for (const PendingBit& p : pendingBits) {
        HwCsr* c = out.findCsr(p.csr);
        if (!c) { errors.push_back("defs line " + std::to_string(p.line) + ": .csrbit names the CSR '" + p.csr + "', which is not defined"); ok = false; continue; }
        c->role[p.bit] = p.role;
    }
    if (!envText.empty()) {
        std::set<std::string> inEnv;
        std::vector<std::string> perr;
        std::vector<sim::IniSection> secs = sim::parseIni(envText, perr);
        for (auto& e : perr) { errors.push_back("environment " + e); ok = false; }
        for (const sim::IniSection& s : secs) {
            if (s.kind == "csr") {
                HwCsr* c = out.findCsr(s.name);
                HwCsr fresh;
                if (!c) { fresh.name = s.name; out.csrs.push_back(fresh); c = &out.csrs.back(); }
                inEnv.insert(s.name);
                for (auto& kv : s.kv) {
                    long long v;
                    const std::string& k = kv.first;
                    if (k == "addr" && parseNum(kv.second, v)) c->addr = (int)v;
                    else if (k == "kind") c->kind = lower(kv.second);
                    else if (k == "reset" && parseNum(kv.second, v)) c->reset = (int)v;
                    else if (k == "bind") c->bind = kv.second;
                    else if (k == "bytes" && parseNum(kv.second, v)) c->bytes = (int)v;
                    else if (k == "const" && parseNum(kv.second, v)) c->constVal = v;
                    else if (k.size() == 4 && k.compare(0, 3, "bit") == 0 && k[3] >= '0' && k[3] <= '7') c->bit[k[3] - '0'] = kv.second;
                    else { errors.push_back("environment line " + std::to_string(s.line) + " [csr " + s.name + "]: cannot keep '" + k + " = " + kv.second + "'"); ok = false; }
                }
            } else if (s.kind == "device") {
                long long v = 0;
                const std::string* a = s.get("addr");
                if (!a || !parseNum(*a, v)) { errors.push_back("environment line " + std::to_string(s.line) + " [device " + s.name + "]: addr is required"); ok = false; continue; }
                HwDevice* d = nullptr;
                for (HwDevice& x : out.devices) if (x.addr == (int)v && !x.simulated) d = &x;           // the DeviceMap entry of the same endpoint
                HwDevice fresh;
                if (!d) { out.devices.push_back(fresh); d = &out.devices.back(); }
                d->name = s.name;
                d->addr = (int)v;
                d->simulated = true;
                for (auto& kv : s.kv) {
                    if (kv.first == "addr") continue;
                    if (kv.first == "kind") { d->kind = lower(kv.second); continue; }
                    d->keys.push_back(kv);
                }
            } else if (envKind(s.kind)) {
                HwSection h;
                h.kind = s.kind;
                h.name = s.name;
                h.kv = s.kv;
                out.env.push_back(h);
            } else {
                errors.push_back("environment line " + std::to_string(s.line) + ": unknown section kind '" + s.kind + "'");
                ok = false;
            }
        }
        // a .csr name for the second byte of a multi-byte value CSR must not become a CSR of its own in the environment
        for (HwCsr& c : out.csrs) {
            if (inEnv.count(c.name)) continue;
            for (const HwCsr& o2 : out.csrs)
                if (&o2 != &c && inEnv.count(o2.name) && o2.kind == "value" && c.addr > o2.addr && c.addr < o2.addr + o2.bytes) c.simulated = false;
        }
    }
    return ok;
}

// ---- validation ----------------------------------------------------------------------------------------------------------------------
std::vector<HwProblem> validateHardware(const Hardware& hw) {
    std::vector<HwProblem> p;
    auto err = [&](const std::string& where, const std::string& m) { p.push_back({true, where, m, 0}); };
    auto warn = [&](const std::string& where, const std::string& m) { p.push_back({false, where, m, 0}); };
    const sim::Extensions ext = sim::Extensions::defaults();

    // names that live in the assembler's one symbol table
    std::map<std::string, std::string> symbols;
    auto symbol = [&](const std::string& name, const std::string& where) {
        if (!identOk(name)) { err(where, "'" + name + "' is not a valid name (letters, digits and _, not starting with a digit)"); return; }
        auto it = symbols.find(name);
        if (it != symbols.end()) err(where, "the name '" + name + "' is already used by " + it->second + ": every CSR, service, constant, DeviceMap name and .equ shares one namespace");
        else symbols[name] = where;
    };
    for (const HwEqu& e : hw.equs) {
        symbol(e.name, "equ " + e.name);
        if (trim(e.value).empty()) err("equ " + e.name, "the value is empty");
    }
    for (const HwConst& c : hw.consts) {
        const std::string w = "const " + c.name;
        symbol(c.name, w);
        if (c.values.empty()) err(w, "give at least one value");
        for (const std::string& v : c.values) {
            long long x;
            if (parseNum(v, x)) {
                const long long lo = c.width == 1 ? -128 : -32768, hi = c.width == 1 ? 255 : 65535;
                if (x < lo || x > hi) err(w, "the value " + v + " does not fit " + (c.width == 1 ? "a byte (-128..255)" : "16 bits (-32768..65535)"));
            } else if (trim(v).empty()) err(w, "an empty value");
        }
    }
    int pool = 0;
    for (const HwConst& c : hw.consts) pool += c.width * (int)c.values.size();
    if (pool > 256) err("", "the constants take " + std::to_string(pool) + " bytes; MAIN_CONST_POOL holds 256");

    // CSRs
    std::map<int, std::string> csrAddr;
    for (const HwCsr& c : hw.csrs) {
        const std::string w = "csr " + c.name;
        symbol(c.name, w);
        if (c.addr < 0 || c.addr > 0xFFF) err(w, "the address must be 0x000..0xFFF (12 bits)");
        static const char* const kinds[] = {"ram", "value", "action", "status", "const"};
        bool kindOk = false;
        for (const char* k : kinds) if (c.kind == k) kindOk = true;
        if (!kindOk) err(w, "kind '" + c.kind + "' is not ram, value, action, status or const");
        const int span = c.kind == "value" ? std::max(1, c.bytes) : 1;
        if (c.addr >= 0 && c.addr <= 0xFFF && c.simulated)
            for (int k = 0; k < span; k++) {
                auto it = csrAddr.find(c.addr + k);
                if (it != csrAddr.end()) err(w, "address " + hex(c.addr + k, 3) + " is already taken by " + it->second);
                else csrAddr[c.addr + k] = w;
            }
        if (c.kind == "ram" && (c.reset < 0 || c.reset > 255)) err(w, "the reset value must fit a byte (0..255)");
        if (c.kind == "value") {
            if (c.bind.empty()) err(w, "a value CSR needs 'bind': the signal it reads and writes (e.g. timer_period)");
            if (c.bytes < 1 || c.bytes > 4) err(w, "bytes must be 1..4");
        }
        if (c.kind == "const" && (c.constVal < 0 || c.constVal > 255)) err(w, "the constant must fit a byte (0..255)");
        for (int b = 0; b < 8; b++) {
            const std::string bw = w + " bit " + std::to_string(b);
            if (!c.bit[b].empty() && c.kind == "action") {
                std::string nm = c.bit[b].substr(0, c.bit[b].find(':'));
                if (nm != "set" && nm != "clr" && !ext.actions().count(nm)) err(bw, "unknown action '" + nm + "'");
            }
            if (!c.bit[b].empty() && c.kind != "action" && c.kind != "status") warn(bw, "a bit meaning is only used by action and status CSRs");
            if (!c.role[b].empty()) {
                if (!roleOk(c.role[b])) err(bw, "role '" + c.role[b] + "' is not timer_start, timer_clear, poll_arm or poll_clear");
                else if (c.kind == "action" && c.bit[b].empty()) warn(bw, "has the role " + c.role[b] + " but no action: the simulator will do nothing when it is written");
                else if (c.kind == "action") {
                    const std::string act = c.bit[b].substr(0, c.bit[b].find(':'));
                    if (act != c.role[b]) warn(bw, "role " + c.role[b] + " but the simulator action is " + act + ": the assembler and the simulator disagree on what this bit does");
                }
            }
        }
    }

    // devices
    std::set<int> i2c;
    std::map<int, std::string> index;
    std::set<std::string> dnames;
    for (const HwDevice& d : hw.devices) {
        const std::string w = "device " + d.name;
        if (!identOk(d.name)) err(w, "'" + d.name + "' is not a valid name");
        if (!dnames.insert(d.name).second) err(w, "two devices are called " + d.name);
        if (d.addr < 0 || d.addr > 0x7F) err(w, "the I2C address must be a 7-bit value (0x00..0x7F)");
        else if (d.simulated && !i2c.insert(d.addr).second) err(w, "I2C address " + hex(d.addr, 2) + " is used by another device");
        if (d.index >= 0) {
            if (d.index > 7) err(w, "the DeviceMap has 8 entries: index 0..7");
            else if (index.count(d.index)) err(w, "DeviceMap entry " + std::to_string(d.index) + " is already used by " + index[d.index]);
            else index[d.index] = w;
            symbol(d.symbol.empty() ? "DEV_" + upper(d.name) : d.symbol, w + " (DeviceMap name)");
        } else {
            warn(w, "is not in the DeviceMap (no index): EXEC_XFER cannot address it");
        }
        if (!ext.devices().count(d.kind)) err(w, "unknown device kind '" + d.kind + "'");
        if (d.kind == "scripted")
            for (auto& kv : d.keys) {
                const std::string& k = kv.first;
                const std::string head = k.substr(0, k.find(' '));
                long long v;
                if (head == "reg" || head == "regseq" || head == "block") {
                    if (k.find(' ') == std::string::npos || !parseNum(k.substr(k.find(' ') + 1), v) || v < 0 || v > 0xFFFF) err(w, "'" + k + "': the register address must be a number");
                    std::string vals = kv.second;
                    std::replace(vals.begin(), vals.end(), ' ', ',');                                   // block 0x43 = 0x0A 0x00 ...  uses spaces
                    for (const std::string& x : splitTop(vals))
                        if (!trim(x).empty() && (!parseNum(x, v) || v < 0 || v > 255)) err(w, "'" + k + "': " + trim(x) + " is not a byte value (0..255)");
                } else if (k == "default" || k == "extra_cycles") {
                    if (!parseNum(kv.second, v) || v < 0) err(w, "'" + k + "' must be a number");
                } else if (k == "nack" || k == "write_nack") {
                    const std::string t = lower(kv.second);
                    if (t != "true" && t != "false") err(w, "'" + k + "' must be true or false");
                } else err(w, "unknown device key '" + k + "'");
            }
    }

    // services
    std::map<int, std::string> ids;
    for (const HwService& s : hw.services) {
        const std::string w = "service " + s.name;
        symbol(s.name, w);
        if (s.id < 1 || s.id > 15) err(w, "the service id must be 1..15 (0 is reserved)");
        else if (ids.count(s.id)) err(w, "service id " + std::to_string(s.id) + " is already used by " + ids[s.id]);
        else ids[s.id] = w;
        if (s.len != -1 && (s.len < 1 || s.len > 32)) err(w, "the payload length must be 1..32 bytes (the result slot holds 32)");
        if (s.len == -1) warn(w, "no payload length declared: the assembler cannot check that Peri publishes and Main consumes the same number of bytes");
        if (s.wire == '?' && s.len >= 2) warn(w, "no byte order declared for a multi-byte payload: the assembler cannot warn about a missing byte swap (W514)");
    }
    return p;
}

// ---- tool-based check ----------------------------------------------------------------------------------------------------------------
std::vector<HwProblem> checkHardwareWithTools(const Hardware& hw) {
    std::vector<HwProblem> p;
    std::vector<Source> src{Source{"hardware.inc", generateInc(hw)}, Source{"probe.asm", ".main\nprobe_loop: JMP probe_loop\n"}};
    Options opt;
    opt.dataflow = DataflowMode::Off;
    Result r = assemble(src, nullptr, opt);
    for (const Diagnostic& d : r.diags) {
        if (d.sev != Diagnostic::Error && d.sev != Diagnostic::Warning) continue;
        HwProblem x;
        x.error = d.sev == Diagnostic::Error;
        x.where = "generated definitions";
        x.message = "the assembler says (" + d.code + ", line " + std::to_string(d.loc.line) + "): " + d.msg;
        p.push_back(x);
    }
    if (!r.ok) return p;
    sim::SimConfig cfg;
    cfg.env = generateEnv(hw);
    sim::Simulator sim(r, cfg);
    for (const std::string& e : sim.configErrors()) {
        HwProblem x;
        x.where = "generated environment";
        x.message = "the simulator says: " + e;
        p.push_back(x);
    }
    return p;
}

}  // namespace asmcore
