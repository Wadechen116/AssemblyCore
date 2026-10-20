// Peri-domain instruction semantics (spec 8-12): EXEC_CSR, BR, EXEC_PUB, EXEC_XFER, and the Poll Unit.
#include <algorithm>
#include <cstring>

#include "../codec.h"
#include "impl.h"

namespace asmcore {
namespace sim {

// EXEC_CSR (one-byte only in the baseline)
void Simulator::Impl::opPeriCsr(OpCtx& oc) {
    PeriState& p = st.p;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[1];
    auto it = instrAt[1].find(pc);
    auto text = [&](const char* dflt) { return it != instrAt[1].end() ? it->second->text : std::string(dflt); };
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Peri, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)p; (void)im; (void)arg; (void)next; (void)latency;
        if (arg & 32) { fail("fault_peri_csr_multibyte", "Peri EXEC_CSR MultiByte is illegal in the baseline (spec 4.1)"); return; }
        uint32_t d = 0;
        if (!im.entry(PT_CSR1, (uint32_t)(arg & 31), 2, d)) { fail("fault_desc_index", "EXEC_CSR descriptor " + std::to_string(arg & 31) + " is outside its table"); return; }
        const codec::Csr1 c1 = codec::unpackCsr1(d);
        if (c1.rsv) { fail("fault_desc_reserved", "CsrOneByteDesc reserved bits are not zero"); return; }
        const int cop = c1.op;
        if (cop == 3) { fail("fault_peri_csr_read", "Peri EXEC_CSR READ8 is illegal in the baseline (spec 4.2)"); return; }
        if (pc + 1 >= im.codeLimit) { fail("fault_pc_out_of_code", "EXEC_CSR is missing its second byte"); return; }
        const uint8_t imm = (uint8_t)im.u8(pc + 1);
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 1; e.pc = pc; e.a = 2; e.what = text("EXEC_CSR"); });
        csrWrite(c1.addr, imm, cop, Domain::Peri);
        latency = lat.csrOne(cop);
        next = pc + 2;
}

// BR (spec 11.2)
void Simulator::Impl::opPeriBr(OpCtx& oc) {
    PeriState& p = st.p;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[1];
    auto it = instrAt[1].find(pc);
    auto text = [&](const char* dflt) { return it != instrAt[1].end() ? it->second->text : std::string(dflt); };
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Peri, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)p; (void)im; (void)arg; (void)next; (void)latency;
        uint32_t d = 0;
        if (!im.entry(PT_BRANCH, (uint32_t)arg, 2, d)) { fail("fault_desc_index", "BR descriptor " + std::to_string(arg) + " is outside its table"); return; }
        const codec::PeriBranch pb = codec::unpackPeriBranch(d);
        const int type = pb.type;
        const uint32_t target = pb.target;
        if (pb.rsv) { fail("fault_desc_reserved", "PeriBranchDesc reserved bits are not zero"); return; }
        if (type == 8) {                                       // BR.ERET (v31 16.5)
            if (pb.eventSafe || target != 0) { fail("fault_desc_reserved", "a BR.ERET descriptor must have event_safe and target_pc_byte 0"); return; }
            doEret(1, oc);
            return;
        }
        if (type > 7 || !cond[1][type].defined) { fail("fault_cond_undefined", "Peri branch condition " + std::to_string(type) + " is reserved"); return; }
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 1; e.pc = pc; e.a = 1; e.what = text("BR"); });
        const bool taken = evalBool(cond[1][type].e);
        if (taken && target >= im.codeLimit) { fail("fault_branch_target", "branch target " + std::to_string(target) + " is outside the code"); return; }
        if (taken && type == 0 && target == pc && !pb.eventSafe) {
            p.halted = true;
            p.readyAt = kNever;
            p.instrs++;
            oc.skipAdvance = true;
            emitNote("Peri halted: unconditional branch to itself at pc " + std::to_string(pc));
            return;
        }
        next = taken ? target : pc + 1;
        latency = lat.brCond;
        oc.checkpoint = pb.eventSafe != 0;
}

// EXEC_PUB (spec 10)
void Simulator::Impl::opPeriPub(OpCtx& oc) {
    PeriState& p = st.p;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[1];
    auto it = instrAt[1].find(pc);
    auto text = [&](const char* dflt) { return it != instrAt[1].end() ? it->second->text : std::string(dflt); };
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Peri, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)p; (void)im; (void)arg; (void)next; (void)latency;
        uint32_t d = 0;
        if (!im.entry(PT_PUB, (uint32_t)arg, 2, d)) { fail("fault_desc_index", "PublishDesc " + std::to_string(arg) + " is outside its table"); return; }
        const codec::Pub pu = codec::unpackPub(d);
        const int svc = pu.service, src = pu.src, len = pu.len;
        if (pu.rsv) { fail("fault_desc_reserved", "PeriPublishDesc reserved bits are not zero"); return; }
        if (svc == 0) { fail("fault_publish_service", "service_id 0 is reserved"); return; }
        if (src + len > 32) { fail("fault_range", "src_ptr + len exceeds PERI_RX_BUF"); return; }
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 1; e.pc = pc; e.a = 1; e.what = text("EXEC_PUB"); });
        latency = lat.pub(len);
        if (st.slot.valid || st.slot.busy) {          // spec 12.8 / 13.7: keep the old result, drop this one
            st.slot.overrun = true;
            emit([&](Event& e) { e.type = EventType::Overrun; e.domain = 1; e.pc = pc; e.a = svc; e.b = len; });
        } else {
            std::memcpy(st.slot.payload, p.rx + src, (size_t)len);
            st.slot.svc = (uint8_t)svc;
            st.slot.valid = true;                      // last
            emit([&](Event& e) { e.type = EventType::Publish; e.domain = 1; e.pc = pc; e.a = svc; e.b = len; });
        }
}

// EXEC_XFER (spec 8): the transaction itself runs when the bus time has elapsed (xferComplete)
void Simulator::Impl::opPeriXfer(OpCtx& oc) {
    PeriState& p = st.p;
    const uint32_t pc = oc.pc;
    const int arg = oc.arg;
    const ImageView& im = img[1];
    auto it = instrAt[1].find(pc);
    auto text = [&](const char* dflt) { return it != instrAt[1].end() ? it->second->text : std::string(dflt); };
    auto fail = [&](const char* code, const std::string& msg) { raise(Domain::Peri, pc, code, msg); };
    uint32_t& next = oc.next;
    int& latency = oc.latency;
    (void)p; (void)im; (void)arg; (void)next; (void)latency;
        const bool multi = (arg & 32) != 0;
        uint32_t d = 0;
        if (!im.entry(multi ? PT_XFERM : PT_XFER1, (uint32_t)(arg & 31), 2, d)) { fail("fault_desc_index", "XFER descriptor " + std::to_string(arg & 31) + " is outside its table"); return; }
        XferInFlight x;
        x.active = true;
        const codec::Xfer1 d1 = codec::unpackXfer1(d);       // rw / dev / addr_len sit at the same bits in both forms
        x.read = d1.rw != 0;
        x.dev = d1.dev;
        x.addrLen = d1.addrLen;
        if (x.addrLen == 3) { fail("fault_desc_reserved", "addr_len 3 is reserved"); return; }
        uint32_t length = 1;
        if (!multi) {
            if (pc + 1 >= im.codeLimit) { fail("fault_pc_out_of_code", "EXEC_XFER is missing its second byte"); return; }
            const uint8_t b1 = (uint8_t)im.u8(pc + 1);
            x.addr = d1.addr;
            x.len = 1;
            if (x.read) {
                if (b1 & 0xE0) { fail("fault_desc_reserved", "READ destination pointer has bits 7:5 set"); return; }
                x.ptr = b1;
            } else x.data = b1;
            length = 2;
        } else {
            if (!x.read) { fail("fault_xfer_multibyte_write", "MultiByte WRITE is illegal in the baseline (spec 8.5)"); return; }
            const codec::XferM dm_ = codec::unpackXferM(d);
            x.len = dm_.len;
            x.ptr = dm_.ptr;
            if (x.ptr + x.len > 32) { fail("fault_range", "ptr + len exceeds PERI_RX_BUF"); return; }
            length = 1 + (uint32_t)x.addrLen;
            if (pc + length > im.codeLimit) { fail("fault_pc_out_of_code", "EXEC_XFER is missing its address bytes"); return; }
            x.addr = 0;
            for (int i = 0; i < x.addrLen; i++) x.addr = (x.addr << 8) | im.u8(pc + 1 + (uint32_t)i);      // program order = wire order (spec 8.5)
        }
        uint32_t dm = 0;
        if (!im.entry(PT_DEVMAP, (uint32_t)x.dev, pf.deviceMapEntryBytes, dm)) { fail("fault_dev_undefined", "DeviceMap[" + std::to_string(x.dev) + "] is not in the image"); return; }
        const codec::DevEntry devEntry = codec::unpackDev(pf, dm);
        if (devEntry.busType != 0) { fail("xfer_fault_reserved_bus_type", "DeviceMap bus_type is reserved (baseline: I2C only)"); return; }
        emit([&](Event& e) { e.type = EventType::Issue; e.domain = 1; e.pc = pc; e.a = (int64_t)length; e.what = text("EXEC_XFER"); });
        p.xfer = x;
        p.xfer.dev = devEntry.addr7;                   // from here on `dev` is the 7-bit I2C address
        const Cycle done = st.now + (Cycle)lat.xfer(x.read, x.addrLen, x.len);
        p.readyAt = kNever;                            // waits for the bus (spec 14.3: not a 1-cycle instruction)
        schedule(done, TimedEvent::XferDone);
        next = pc + length;
        latency = -1;
}

void Simulator::Impl::stepPeri() {
    PeriState& p = st.p;
    const uint32_t pc = p.pc;
    if (!fetchOk(Domain::Peri, pc)) return;
    const ImageView& im = img[1];
    const uint8_t b = (uint8_t)im.u8(pc);
    const int op = b >> 6, arg = b & 63;
    // EXEC_PUB into a slot that Main cleared in this very cycle waits one cycle (spec 12.6); decided before the instruction is announced
    if (op == 2) {
        uint32_t d = 0;
        if (im.entry(PT_PUB, (uint32_t)arg, 2, d) && !st.slot.valid && !st.slot.busy && st.slot.clearedAt != kNever && st.now <= st.slot.clearedAt) {
            p.readyAt = st.slot.clearedAt + 1;
            return;
        }
    }
    OpCtx oc;
    oc.pc = pc;
    oc.arg = arg;
    oc.next = pc + 1;
    oc.latency = 1;
    const auto& h = periOps[op];
    if (!h) { raise(Domain::Peri, pc, "fault_opcode", "opcode " + std::to_string(op) + " has no handler"); return; }
    h(oc);
    if (p.faulted || oc.skipAdvance) return;
    p.pc = oc.next;
    if (oc.latency >= 0) p.readyAt = st.now + (Cycle)oc.latency;
    p.instrs++;
    if (oc.checkpoint) eventCheckpoint(1);
}

// The external transaction finishes: talk to the device, fill PERI_RX_BUF, run the Poll Unit (spec 11.3, 11.4).
void Simulator::Impl::xferComplete() {
    PeriState& p = st.p;
    XferInFlight x = p.xfer;
    if (!x.active) return;
    p.xfer.active = false;
    bool ack = true;
    I2cReply r;
    auto n = st.nack.find(x.dev);
    auto di_ = devIndexByAddr.find(x.dev);
    if (n != st.nack.end() && n->second > 0) { n->second--; ack = false; }
    else if (di_ == devIndexByAddr.end()) ack = false;               // nobody at that address
    else if (x.read) { r = devs[(size_t)di_->second].dev->read(x.addr, x.addrLen, x.len, st.dev[(size_t)di_->second], st.now); ack = r.ack; }
    else { r = devs[(size_t)di_->second].dev->write(x.addr, x.addrLen, std::vector<uint8_t>{x.data}, st.dev[(size_t)di_->second], st.now); ack = r.ack; }
    emit([&](Event& e) { e.type = EventType::Bus; e.domain = 1; e.pc = p.pc; e.what = x.read ? "i2c.read" : "i2c.write"; e.a = x.dev; e.b = ack ? 1 : 0; e.c = x.len; });
    if (!ack) {
        sigSet(sigId("peri.error"), 1);                              // sticky error latch (spec 11.5.3)
        emitNote("I2C NACK from address " + std::to_string(x.dev));
    } else if (x.read) {
        for (int i = 0; i < x.len; i++) p.rx[x.ptr + i] = i < (int)r.data.size() ? r.data[(size_t)i] : 0;
        noteWrite("rx", x.ptr, x.len);
        if (x.ptr == 0 && signal("poll.arm")) {                       // the armed poll sees PERI_RX_BUF[0] of this READ
            const bool ready = (p.rx[0] & (uint8_t)signal("poll_mask")) != 0;
            setSignal("poll.ready", ready ? 1 : 0);
            setSignal("poll.not_ready", ready ? 0 : 1);
            setSignal("poll.arm", 0);
        }
    }
    p.readyAt = st.now + (Cycle)(ack ? r.extraCycles : 0);
}

}  // namespace sim
}  // namespace asmcore
