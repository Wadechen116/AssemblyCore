#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "isa.h"
#include "util.h"

namespace asmcore {

// One parsed source line: zero or more labels, then optionally an instruction / directive.
struct Stmt {
    SrcLoc loc;
    std::vector<std::string> labels;
    std::string op;      // mnemonic as written ("" for a label-only line)
    std::string rest;    // operand text, comment removed
};

struct ProcDef {
    std::string name;
    SrcLoc loc;
    std::vector<Stmt> body;
    std::set<std::string> localLabels;
    std::vector<std::string> callees;   // in order of appearance (with repeats)
};

struct DomainProg {
    bool present = false;
    std::vector<Stmt> flow;
    std::vector<ProcDef> procs;
};

struct Sym {
    std::string kind;
    int64_t value = 0;
    int size = 0;        // const: bytes
    SrcLoc loc;
};

// ---- lowered operation -----------------------------------------------------------------------------------------------------
enum class OpKind { Label, Mov, Alu, Csr1, CsrM, MBr, MCascB, MCascE, PBr, Pub, Xfer1, XferM };

struct RangeUse {            // a byte range of a Main data region touched by an instruction (for the reserved-state check)
    int region = -1, off = 0, len = 0;
};

struct Op {
    OpKind kind = OpKind::Label;
    SrcLoc loc;
    std::string text;
    std::vector<Frame> frames;
    bool synthetic = false;
    std::string note;
    std::string label;           // Label: its name.  Branches: target label (already scoped)
    uint32_t desc = 0;           // descriptor value (branches: without target)
    int brType = 0, brArg = 0;   // MBr / MCascB / MCascE / PBr
    bool uncond = false;         // branch that never falls through
    bool eventSafe = false;      // ordinary branch that is an event checkpoint (v31 16.3)
    std::vector<uint8_t> extra;  // bytes after the instruction byte: Csr1 imm, CsrM lo8, Xfer1 data / ptr, XferM address bytes
    std::vector<RangeUse> uses;
    int cycles = 0;
    int dev = -1;                // Xfer: DeviceMap index used
    std::string pushLabel;       // SoftRet call transfer: label execution resumes at (WCET pushes it)
    std::string dispatchOf;      // SoftRet return dispatch block: the procedure it returns from
    // filled by layout
    uint32_t pc = 0;
    int length() const;
};

struct Pool {
    std::vector<uint32_t> vals;
    std::map<uint32_t, int> idx;
    int add(uint32_t v) {
        auto it = idx.find(v);
        if (it != idx.end()) return it->second;
        int i = (int)vals.size();
        vals.push_back(v);
        idx[v] = i;
        return i;
    }
};

struct DevEntry {
    bool defined = false;
    int bus = 0, addr = 0;
    SrcLoc loc;
};

struct Ctx {
    std::map<std::string, std::vector<std::string>> srcLines;   // the text of every source read (for diagnostic columns)
    SrcLoc withColumns(SrcLoc l, const std::string& msg) const;
    Options opt;
    IncludeResolver resolver;
    Result res;
    DomainProg prog[2];
    std::map<std::string, Sym> syms;
    std::vector<uint8_t> constPool;                       // Main MAIN_CONST_POOL
    std::map<std::pair<int64_t, int>, int> autoConst;     // (value, width) -> offset
    DevEntry dev[8];
    struct WcetDecl { std::string name, from, to; SrcLoc loc; };
    std::vector<WcetDecl> wcetDecls;
    // positions that are only known after layout: lowering leaves marker labels, buildImage resolves them
    struct PendingCall { CallSiteInfo info; std::string beginLabel, endLabel; };
    struct PendingProc { ProcInfo info; std::string beginLabel, endLabel; };
    std::vector<PendingCall> pendingCalls;
    std::vector<PendingProc> pendingProcs;
    std::map<std::string, uint32_t> labelPc[2];           // filled by buildImage
    // `.assume REGION[a..b] defined`: ranges initialised by someone else (a boot sequence, the other domain...)
    struct Assume { int domain; std::string region; int off, len; SrcLoc loc; };
    std::vector<Assume> assumes;
    // `.vector ID, label` declarations per domain (resolved against the flow labels after layout)
    struct VecDecl { int id; std::string label; SrcLoc loc; };
    std::vector<VecDecl> vectors[2];
    std::vector<std::pair<uint32_t, uint32_t>> cascBody[2];    // [start, end) byte ranges of cascade regions, for the vector check

    void error(const SrcLoc& l, const std::string& code, const std::string& m);
    void warn(const SrcLoc& l, const std::string& code, const std::string& m);
    bool eval(const std::string& text, int64_t& v, const SrcLoc& l, const char* code = "E103");
    bool defineSym(const std::string& name, const std::string& kind, int64_t v, int size, const SrcLoc& l);
    int constOffsetFor(int64_t value, int width);         // dedup'd auto constant; -1 if the pool is full
};

// front end (assemble.cpp)
void parseSources(Ctx& c, const std::vector<Source>& sources);
// lowering + call models (lower.cpp)
bool lowerDomain(Ctx& c, Domain d, std::vector<Op>& ops, std::vector<ProcInfo>& procs);
// layout, validation, image (layout.cpp)
void buildImage(Ctx& c, Domain d, std::vector<Op>& ops);
// event vectors, ERET context, handler effects and event timing (events.cpp); runs after the branches are resolved
void analyzeEvents(Ctx& c, Domain d, const std::vector<Op>& ops, const std::map<const Op*, uint32_t>& target);
// data-flow check over the lowered programs of both domains (dataflow.cpp); either may be null
void runDataflow(Ctx& c, const std::vector<Op>* mainOps, const std::vector<Op>* periOps);

inline int domIdx(Domain d) { return d == Domain::Main ? 0 : 1; }

}  // namespace asmcore
