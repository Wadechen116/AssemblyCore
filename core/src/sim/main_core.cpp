// Main-domain instruction semantics (spec 4-7). One function per opcode, called from stepMain().
#include <algorithm>
#include <cstring>

#include "../codec.h"
#include "impl.h"

namespace asmcore {
namespace sim {

namespace {

const char* regName(int id) {
    static const char* n[] = {"scratch", "csrbuf", "slot", "opa", "opb", "out", "status", "const", "meta"};
    return id >= 0 && id <= 8 ? n[id] : "?";
}

struct Ret {
    bool ok = true;
    uint32_t next = 0;
    int lat = 0;
};

}  // namespace

// EXEC_CSR (spec 4)
void Simulator::Impl::opMainCsr(OpCtx& oc) {
    MainState& m = st.m;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[0];
    auto it = instrAt[0].find(pc);
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Main, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)m; (void)im; (void)arg; (void)next; (void)latency;
        const bool multi = (arg & 32) != 0;
        const int idx = arg & 31;
        uint32_t d = 0;
        if (!im.entry(multi ? MT_CSRM : MT_CSR1, (uint32_t)idx, 2, d)) { fail("fault_desc_index", "EXEC_CSR descriptor " + std::to_string(idx) + " is outside its table"); return; }
        if (pc + 1 >= im.codeLimit) { fail("fault_pc_out_of_code", "EXEC_CSR is missing its second byte"); return; }
        const uint8_t imm = (uint8_t)im.u8(pc + 1);
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 0; e.pc = pc; e.a = 2; e.what = it != instrAt[0].end() ? it->second->text : "EXEC_CSR"; });
        next = pc + 2;
        if (!multi) {
            const codec::Csr1 c1 = codec::unpackCsr1(d);
            if (c1.rsv) { fail("fault_desc_reserved", "CsrOneByteDesc reserved bits are not zero"); return; }
            const int cop = c1.op;
            const uint32_t addr = c1.addr;
            if (cop == 3) {
                if (imm & 0xE0) { fail("fault_desc_reserved", "READ8 destination pointer has bits 7:5 set"); return; }
                m.buf[imm] = csrRead8(addr, Domain::Main);
                noteWrite("csrbuf", imm, 1);
            } else {
                csrWrite(addr, imm, cop, Domain::Main);
            }
            latency = lat.csrOne(cop);
        } else {
            const codec::CsrM cm = codec::unpackCsrM(d);
            const int cop = cm.op;
            const uint32_t page = (uint32_t)cm.page;
            const int len = cm.len, ptr = cm.ptr;
            if (cop == 1 || cop == 2) { fail("fault_desc_reserved", "CsrMultiByte buffer RMW is reserved in the baseline (spec 4.3)"); return; }
            if (ptr + len > 32) { fail("fault_range", std::string("CSR buffer range ptr + len exceeds ") + (cop == 0 ? "MAIN_SCRATCHPAD" : "MAIN_CSR_DATA_BUF")); return; }
            const uint32_t addr = (page << 8) | imm;
            for (int i = 0; i < len && !m.faulted; i++) {
                if (cop == 0) csrWrite(addr + (uint32_t)i, m.scr[ptr + i], 0, Domain::Main);       // WRITE_SP (v31 4.3): the source is the scratchpad
                else m.buf[ptr + i] = csrRead8(addr + (uint32_t)i, Domain::Main);                   // READ_BUF
            }
            if (cop == 3) noteWrite("csrbuf", ptr, len);
            latency = lat.csrMulti(len);
        }
}

// BR, CASCADE_BEGIN, CASCADE_END (spec 7)
void Simulator::Impl::opMainBr(OpCtx& oc) {
    MainState& m = st.m;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[0];
    auto it = instrAt[0].find(pc);
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Main, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)m; (void)im; (void)arg; (void)next; (void)latency;
        uint32_t d = 0;
        if (!im.entry(MT_BRANCH, (uint32_t)arg, 2, d)) { fail("fault_desc_index", "BR descriptor " + std::to_string(arg) + " is outside its table"); return; }
        const codec::MainBranch mb = codec::unpackMainBranch(d);
        const int type = mb.type, carg = mb.arg;
        const uint32_t target = mb.target;
        if (mb.rsv) { fail("fault_desc_reserved", "MainBranchDesc reserved bits are not zero"); return; }
        if (mb.eventSafe && type != 0) { fail("fault_desc_reserved", "event_safe is only legal on BR.COND (cascade and ERET descriptors must have it 0)"); return; }
        if (type == 3) {                                       // BR.ERET (v31 16.5)
            if (carg != 0 || target != 0) { fail("fault_desc_reserved", "a BR.ERET descriptor must have arg and target_pc_byte 0"); return; }
            doEret(0, oc);
            return;
        }
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 0; e.pc = pc; e.a = 1; e.what = it != instrAt[0].end() ? it->second->text : "BR"; });
        if (type == 0) {
            const Cond& c = cond[0][carg];
            if (!c.defined) { fail("fault_cond_undefined", "branch condition 0x" + std::to_string(carg) + " is not defined (reserved, or profile-defined without a definition)"); return; }
            const bool taken = evalBool(c.e);
            if (taken && target >= im.codeLimit) { fail("fault_branch_target", "branch target " + std::to_string(target) + " is outside the code"); return; }
            if (taken && carg == 0 && target == pc && !mb.eventSafe) {          // BR.COND ALWAYS to itself: the program has finished (a safe one waits for events)
                m.halted = true;
                m.readyAt = kNever;
                m.instrs++;
                oc.skipAdvance = true;
                emitNote("Main halted: unconditional branch to itself at pc " + std::to_string(pc));
                return;
            }
            next = taken ? target : pc + 1;
            latency = lat.brCond;
            oc.checkpoint = mb.eventSafe != 0;
        } else if (type == 1) {   // CASCADE_BEGIN (spec 7.6)
            if (m.cascActive) { fail("fault_nested_cascade", "CASCADE_BEGIN while a cascade is active"); return; }
            if (target >= im.codeLimit) { fail("fault_branch_target", "cascade block0 is outside the code"); return; }
            m.cascSel = (uint8_t)carg;
            m.cascStep = 0;
            m.cascActive = true;
            next = target;
            latency = lat.cascade;
        } else {                  // CASCADE_END
            if (!m.cascActive) { fail("fault_cascade_end_without_begin", "CASCADE_END with no active cascade"); return; }
            if (carg != m.cascStep) { fail("fault_cascade_step_mismatch", "CASCADE_END index " + std::to_string(carg) + ", expected " + std::to_string(m.cascStep)); return; }
            if (m.cascStep == m.cascSel) {
                if (target >= im.codeLimit) { fail("fault_branch_target", "cascade exit is outside the code"); return; }
                m.cascActive = false;
                next = target;
            } else {
                m.cascStep++;
                next = pc + 1;
            }
            latency = lat.cascade;
        }
}

// MOV (spec 5): all prechecks first, then the copy, so a fault never leaves a partial write
void Simulator::Impl::opMainMov(OpCtx& oc) {
    MainState& m = st.m;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[0];
    auto it = instrAt[0].find(pc);
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Main, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)m; (void)im; (void)arg; (void)next; (void)latency;
        uint32_t d = 0;
        if (!im.entry(MT_MOVE, (uint32_t)arg, 4, d)) { fail("fault_desc_index", "MOV descriptor " + std::to_string(arg) + " is outside its table"); return; }
        const codec::Move mv = codec::unpackMove(d);
        const int sr = mv.srcRegion, dr = mv.dstRegion;
        const int so = mv.srcOff, dof = mv.dstOff, len = mv.len;
        if (mv.rsv) { fail("fault_desc_reserved", "MainMoveDesc reserved bits are not zero"); return; }
        const bool srcOk = sr == 0 || sr == 1 || sr == 2 || sr == 5 || sr == 6 || sr == 7 || sr == 8;
        const bool dstOk = dr == 0 || dr == 3 || dr == 4;                 // v31 5.1: MAIN_CSR_DATA_BUF is written only by EXEC_CSR READ8 / READ_BUF
        if (!srcOk) { fail("fault_mov_region", std::string("region ") + std::to_string(sr) + " cannot be a MOV source"); return; }
        if (!dstOk) { fail("fault_mov_region", dr == 1 ? "MAIN_CSR_DATA_BUF cannot be a MOV destination (spec v31 5.1: only EXEC_CSR READ8 / READ_BUF write it)" : std::string("region ") + std::to_string(dr) + " cannot be a MOV destination"); return; }
        if (sr == dr) {
            if (so < dof + len && dof < so + len) { fail("fault_mov_overlap", "MOV between overlapping ranges of one region (no memmove semantics, spec v31 5.2)"); return; }
            if (!pf.sameRegionMov) { fail("fault_mov_overlap", "this profile does not allow MOV inside one region"); return; }
        }
        if (sr == 7 && dr != 0) { fail("fault_mov_region", "MAIN_CONST_POOL can only be moved to MAIN_SCRATCHPAD (spec v31 5.1)"); return; }
        if ((dr == 3 || dr == 4) && (dof != 0 || len != 2)) { fail("fault_mov_range", "ALU operands are loaded with offset 0 and length 2"); return; }
        const int srcSize = sr == 5 || sr == 6 ? 4 : sr == 7 ? (int)constSize : 32;
        const int dstSize = dr == 3 || dr == 4 ? 2 : 32;
        if (so + len > srcSize) { fail("fault_mov_range", std::string(regName(sr)) + " source range exceeds the region"); return; }
        if (dof + len > dstSize) { fail("fault_mov_range", std::string(regName(dr)) + " destination range exceeds the region"); return; }
        if (sr == 2 && (!st.slot.valid || st.slot.busy)) { fail("fault_slot_not_valid", "MOV from PERI_RESULT_SLOT while it holds no result"); return; }   // ASSUMED: spec says only that a failed precheck keeps valid_q
        std::vector<uint8_t> bytes;
        if (!regionBytes(regName(sr), so, len, bytes)) { fail("fault_mov_range", "source read failed"); return; }
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 0; e.pc = pc; e.a = 1; e.what = it != instrAt[0].end() ? it->second->text : "MOV"; });
        latency = lat.mov(len, sr == 2);
        if (sr == 2) {
            st.slot.busy = true;
            schedule(st.now + (Cycle)latency, TimedEvent::SlotConsumeDone);
        }
        if (dr == 0) { std::memcpy(m.scr + dof, bytes.data(), (size_t)len); noteWrite("scratch", dof, len); }
        else if (dr == 1) { std::memcpy(m.buf + dof, bytes.data(), (size_t)len); noteWrite("csrbuf", dof, len); }
        else if (dr == 3) { m.opa = (uint16_t)(bytes[0] | (bytes[1] << 8)); noteWrite("opa", 0, 2); }
        else { m.opb = (uint16_t)(bytes[0] | (bytes[1] << 8)); noteWrite("opb", 0, 2); }
}

// ALU (spec 6): operands are the 16-bit ALU_OPA / ALU_OPB
void Simulator::Impl::opMainAlu(OpCtx& oc) {
    MainState& m = st.m;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[0];
    auto it = instrAt[0].find(pc);
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Main, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)m; (void)im; (void)arg; (void)next; (void)latency;
        uint32_t d = 0;
        if (!im.entry(MT_ALU, (uint32_t)arg, 1, d)) { fail("fault_desc_index", "ALU descriptor " + std::to_string(arg) + " is outside its table"); return; }
        const codec::Alu ad = codec::unpackAlu(d, pf.alu);
        if (ad.cls != 3 && ad.op < 0) { fail("fault_desc_reserved", "the ALU sub-operation code is not assigned by the target profile (reserved encoding)"); return; }
        const int cls = ad.cls;
        const uint16_t A = m.opa, B = m.opb;
        bool isMul = false;
        MainState nm = m;
        int64_t r = 0;
        if (cls == 0) {                       // ARITH
            const int sg = ad.signedMode, aop = ad.op;
            if (aop > 4) { fail("fault_desc_reserved", "arith_op " + std::to_string(aop) + " is reserved"); return; }
            const int64_t a = sg ? (int16_t)A : (int64_t)A, bb = sg ? (int16_t)B : (int64_t)B;
            r = aop == 0 ? a + bb : aop == 1 ? a - bb : aop == 2 ? a * bb : aop == 3 ? a + 1 : a - 1;
            isMul = aop == 2;
            nm.out = (uint32_t)r;
            nm.zero = nm.out == 0;
            const bool sub = aop == 1 || aop == 4;
            nm.neg = (sg || sub) && r < 0;
            nm.carry = !sg && !sub && !isMul && r > 0xFFFF;
            nm.borrow = !sg && sub && r < 0;
            nm.ovs = sg && !isMul && (r < -32768 || r > 32767);
            nm.ov16 = sg ? (r < -32768 || r > 32767) : (r < 0 || r > 0xFFFF);
        } else if (cls == 1) {                // CMP
            const int sg = ad.signedMode, cop = ad.op;
            if (ad.rsv) { fail("fault_desc_reserved", "CMP descriptor reserved bits are not zero"); return; }
            if (cop <= 1 && sg) { fail("fault_desc_reserved", "EQ / NE require signed_mode = 0"); return; }
            const int64_t a = sg ? (int16_t)A : (int64_t)A, bb = sg ? (int16_t)B : (int64_t)B;
            nm.cmp = cop == 0 ? A == B : cop == 1 ? A != B : cop == 2 ? a < bb : a >= bb;
        } else if (cls == 2) {                // LOGIC
            if (ad.rsv) { fail("fault_desc_reserved", "LOGIC descriptor reserved bits are not zero"); return; }
            const int lop = ad.op;
            if (lop > 3) { fail("fault_desc_reserved", "logic_op " + std::to_string(lop) + " is reserved"); return; }
            const uint16_t v = lop == 0 ? (uint16_t)(A & B) : lop == 1 ? (uint16_t)(A | B) : lop == 2 ? (uint16_t)(A ^ B) : (uint16_t)~A;
            nm.out = v;
            nm.zero = v == 0;
            nm.neg = (v & 0x8000) != 0;      // ASSUMED: "low 16-bit sign if meaningful" (spec 6.8)
            nm.carry = nm.borrow = nm.ovs = nm.ov16 = false;
        } else {                              // SHIFT
            const int sg = ad.signedMode, dir = ad.dir, amt = ad.amt;
            if (amt < 1 || amt > 8) { fail("fault_desc_reserved", "shift amount " + std::to_string(amt) + " is outside 1..8"); return; }
            if (dir == 0) {
                const int64_t exact = (sg ? (int64_t)(int16_t)A : (int64_t)A) * ((int64_t)1 << amt);   // a product: shifting a negative value is undefined in C++17
                nm.out = (uint32_t)exact;
                nm.neg = exact < 0;
                nm.ov16 = sg ? (exact < -32768 || exact > 32767) : exact > 0xFFFF;
            } else {
                const int64_t res = sg ? (int64_t)((int16_t)A >> amt) : (int64_t)(A >> amt);
                nm.out = (uint32_t)res;
                nm.neg = res < 0;
                nm.ov16 = false;
            }
            nm.zero = nm.out == 0;
            nm.carry = nm.borrow = nm.ovs = false;
        }
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 0; e.pc = pc; e.a = 1; e.what = it != instrAt[0].end() ? it->second->text : "ALU"; });
        m.out = nm.out;
        m.cmp = nm.cmp; m.zero = nm.zero; m.neg = nm.neg; m.carry = nm.carry; m.borrow = nm.borrow; m.ovs = nm.ovs; m.ov16 = nm.ov16;
        latency = lat.aluOp(isMul);
}

// Fetch, dispatch through the opcode table, then advance. To change what an opcode does, replace its entry in registerOps().
void Simulator::Impl::stepMain() {
    MainState& m = st.m;
    const uint32_t pc = m.pc;
    if (!fetchOk(Domain::Main, pc)) return;
    const uint8_t b = (uint8_t)img[0].u8(pc);
    OpCtx oc;
    oc.pc = pc;
    oc.arg = b & 63;
    oc.next = pc + 1;
    oc.latency = 1;
    const auto& h = mainOps[b >> 6];
    if (!h) { raise(Domain::Main, pc, "fault_opcode", "opcode " + std::to_string(b >> 6) + " has no handler"); return; }
    h(oc);
    if (m.faulted || oc.skipAdvance) return;
    m.pc = oc.next;
    m.readyAt = st.now + (Cycle)oc.latency;
    m.instrs++;
    if (oc.checkpoint) eventCheckpoint(0);
}

}  // namespace sim
}  // namespace asmcore
