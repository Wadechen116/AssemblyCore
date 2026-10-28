// asmsim -- assemble and simulate: runs a program (or a scenario script) on the Main / Peri simulator.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "asmcore/sim.h"

namespace fs = std::filesystem;
using namespace asmcore;

namespace {
bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
bool writeText(const std::string& p, const std::string& s) {
    std::ofstream o(p, std::ios::binary);
    if (!o) return false;
    o << s;
    return (bool)o;
}
void listParams() {
    std::string group;
    for (const ParamDef& d : paramCatalog()) {
        if (d.group != group) { group = d.group; std::printf("\n%s\n", group.c_str()); }
        std::string range;
        if (d.kind == ParamDef::Enum) { for (const std::string& o : d.options) range += (range.empty() ? "" : "|") + o; }
        else if (d.kind == ParamDef::Bool) range = "true|false";
        else range = std::to_string(d.lo) + ".." + std::to_string(d.hi);
        SimParams none;
        std::printf("  %-30s = %-10s %-20s [%s]%s\n      %s\n", d.key.c_str(), none.text(d.key).c_str(), d.unit.empty() ? "" : d.unit.c_str(), range.c_str(), d.active ? "" : "  (not used by the simulator yet)", d.help.c_str());
        if (!d.measure.empty()) std::printf("      measure: %s\n", d.measure.c_str());
    }
}
void usage() {
    std::puts(
        "usage: asmsim [options] file.asm [more.asm ...]\n"
        "  --env FILE           environment file (CSR behaviours, I2C devices, video timing; docs/SIM.md)\n"
        "  --scenario FILE      run a scenario script instead of a plain run\n"
        "  --cycles N           plain run: stop after N cycles (default 1000000)\n"
        "  --until EXPR         plain run: stop when EXPR becomes true, e.g. \"csr[0x120] == 10\"\n"
        "  --call-model M       inline (default) | softret\n"
        "  --profile FILE       target profile (latencies, table depths)\n"
        "  --params FILE        hardware parameters (a [params] section: key = value; docs/SIM_VISUAL_DESIGN.md)\n"
        "  --param KEY=VALUE    one hardware parameter, e.g. main.fetch_cycles=1 (repeatable; wins over --params and the environment file)\n"
        "  --list-params        every hardware parameter with its group, unit, initial value, range and how to measure it\n"
        "  --dataflow M         off | warn (default) | error   the assembler's data-flow check\n"
        "  --no-warn CODE       drop a warning code, e.g. W502 (repeatable)\n"
        "  -I DIR               include directory\n"
        "  --trace FILE         instruction trace of both domains (text)\n"
        "  --events FILE        every event as CSV\n"
        "  --vcd FILE           value-change dump (GTKWave)\n"
        "  --dump               print the final state\n"
        "Exit status: 0 ok, 1 assembly error / failed scenario / fault, 2 usage or I/O error.");
}
}  // namespace

int main(int argc, char** argv) {
    Options opt;
    sim::SimConfig cfg;
    std::vector<std::string> inputs, incDirs;
    std::string envPath, scnPath, untilExpr, tracePath, eventsPath, vcdPath, profilePath, paramsPath;
    std::vector<std::string> paramArgs;
    long long cycles = 1000000;
    bool dump = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "asmsim: %s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--env") envPath = need();
        else if (a == "--scenario") scnPath = need();
        else if (a == "--cycles") cycles = std::atoll(need().c_str());
        else if (a == "--until") untilExpr = need();
        else if (a == "--call-model") { std::string m = need(); opt.callModel = m == "softret" ? CallModel::SoftRet : CallModel::Inline; }
        else if (a == "--profile") profilePath = need();
        else if (a == "--params") paramsPath = need();
        else if (a == "--param") paramArgs.push_back(need());
        else if (a == "--list-params") { listParams(); return 0; }
        else if (a == "--dataflow") { std::string m = need(); opt.dataflow = m == "off" ? DataflowMode::Off : m == "error" ? DataflowMode::Error : DataflowMode::Warn; }
        else if (a == "--no-warn") opt.suppress.push_back(need());
        else if (a == "-I") incDirs.push_back(need());
        else if (a == "--trace") tracePath = need();
        else if (a == "--events") eventsPath = need();
        else if (a == "--vcd") vcdPath = need();
        else if (a == "--dump") dump = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "asmsim: unknown option %s\n", a.c_str()); return 2; }
        else inputs.push_back(a);
    }
    if (inputs.empty()) { usage(); return 2; }
    if (!profilePath.empty()) {
        std::string t;
        std::vector<std::string> errs;
        if (!readFile(profilePath, t) || !opt.profile.load(t, errs)) { std::fprintf(stderr, "asmsim: cannot use profile %s\n", profilePath.c_str()); return 2; }
    }
    {                                                                    // hardware parameters: a file, then the command line
        if (!paramsPath.empty()) {
            std::string t;
            std::vector<std::string> errs;
            if (!readFile(paramsPath, t)) { std::fprintf(stderr, "asmsim: cannot read %s\n", paramsPath.c_str()); return 2; }
            if (!cfg.params.load(t, errs)) { for (auto& e : errs) std::fprintf(stderr, "%s: %s\n", paramsPath.c_str(), e.c_str()); return 2; }
        }
        for (const std::string& pa : paramArgs) {
            const size_t eq = pa.find('=');
            std::string err;
            if (eq == std::string::npos || !cfg.params.set(pa.substr(0, eq), pa.substr(eq + 1), err)) { std::fprintf(stderr, "asmsim: --param %s: %s\n", pa.c_str(), err.empty() ? "expected KEY=VALUE" : err.c_str()); return 2; }
        }
        cfg.params.apply(opt.profile.latency);                          // the cycle model the assembler's WCET uses is the one the simulator runs with
    }
    std::vector<Source> sources;
    for (auto& in : inputs) {
        Source s;
        s.name = in;
        if (!readFile(in, s.text)) { std::fprintf(stderr, "asmsim: cannot read %s\n", in.c_str()); return 2; }
        sources.push_back(s);
    }
    auto resolver = [&](const std::string& from, const std::string& name, Source& out) {
        std::vector<fs::path> tries{fs::path(from).parent_path() / name};
        for (auto& d : incDirs) tries.push_back(fs::path(d) / name);
        for (auto& p : tries) if (readFile(p, out.text)) { out.name = p.string(); return true; }
        return false;
    };
    Result r = assemble(sources, resolver, opt);
    std::fputs(r.diagnosticsText().c_str(), stderr);
    if (!r.ok) return 1;

    cfg.profile = opt.profile;
    if (!envPath.empty() && !readFile(envPath, cfg.env)) { std::fprintf(stderr, "asmsim: cannot read %s\n", envPath.c_str()); return 2; }
    sim::Simulator s(r, cfg);
    for (auto& e : s.configErrors()) std::fprintf(stderr, "%s: %s\n", envPath.empty() ? "environment" : envPath.c_str(), e.c_str());
    if (!s.ok()) return 1;

    std::string trace, events, vcd;
    if (!tracePath.empty()) s.addSink(sim::makeTextTrace(&trace, false));
    if (!eventsPath.empty()) s.addSink(sim::makeCsvTrace(&events));
    std::shared_ptr<sim::VcdSink> vsink;
    if (!vcdPath.empty()) { vsink = std::make_shared<sim::VcdSink>(&vcd); s.addSink(vsink); }

    int status = 0;
    if (!scnPath.empty()) {
        std::string script;
        if (!readFile(scnPath, script)) { std::fprintf(stderr, "asmsim: cannot read %s\n", scnPath.c_str()); return 2; }
        sim::ScenarioResult sr = sim::runScenario(s, script);
        for (auto& l : sr.log) std::printf("%s\n", l.c_str());
        std::printf("scenario: %s (%d checks, %d failures)\n", sr.pass ? "PASS" : "FAIL", sr.checks, sr.failures);
        if (!sr.pass) status = 1;
    } else {
        sim::RunLimits l;
        l.maxCycles = (sim::Cycle)cycles;
        l.untilExpr = untilExpr;
        sim::RunResult rr = s.run(l);
        static const char* names[] = {"none", "cycle limit", "instruction limit", "breakpoint", "watchpoint", "fault", "idle", "condition"};
        std::printf("stopped: %s at cycle %llu%s%s\n", names[(int)rr.reason], (unsigned long long)rr.now, rr.detail.empty() ? "" : " - ", rr.detail.c_str());
        for (Domain d : {Domain::Main, Domain::Peri}) {
            if (!s.fault(d)) continue;
            std::printf("%s FAULT %s at pc %u: %s\n", domainName(d), s.fault(d)->code.c_str(), s.fault(d)->pc, s.fault(d)->msg.c_str());
            status = 1;
        }
    }
    if (dump) std::fputs(s.dumpState().c_str(), stdout);
    if (vsink) vsink->finish(s.now());
    bool io = true;
    if (!tracePath.empty()) io &= writeText(tracePath, trace);
    if (!eventsPath.empty()) io &= writeText(eventsPath, events);
    if (!vcdPath.empty()) io &= writeText(vcdPath, vcd);
    if (!io) { std::fprintf(stderr, "asmsim: could not write an output file\n"); return 2; }
    return status;
}
