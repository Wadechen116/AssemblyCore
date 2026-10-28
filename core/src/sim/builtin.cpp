// Built-in actions (the effects a CSR bit can have) and the built-in "scripted" I2C device.
#include <algorithm>

#include "impl.h"

namespace asmcore {
namespace sim {

namespace {

int64_t numberOrZero(const std::string& s, bool* ok = nullptr) {
    int64_t v = 0;
    std::string e;
    bool good = evalExpr(s, nullptr, v, e);
    if (ok) *ok = good;
    return good ? v : 0;
}

void startTimer(ActionContext& c, const std::string& arg, const char* done, const char* period, const char* gen) {
    bool ok = false;
    int64_t cycles = arg.empty() ? c.signal(period) : numberOrZero(arg, &ok);
    int64_t g = c.signal(gen) + 1;                          // a new generation: earlier expiries no longer count
    c.setSignal(gen, g);
    c.setSignal(done, 0);
    // hardware parameters of this timer: it ticks every clock_div ISA cycles, starts start_latency after the EXEC_CSR and shows done done_latency after the last tick
    const char* who = std::string(done).compare(0, 5, "main_") == 0 ? "main" : "peri";
    const int64_t div = std::max<int64_t>(1, c.param(std::string("timer.") + who + ".clock_div"));
    const int64_t delay = c.param(std::string("timer.") + who + ".start_latency") + cycles * div + c.param(std::string("timer.") + who + ".done_latency");
    if (cycles > 0) c.setSignalAfterGuarded((Cycle)delay, done, 1, gen, g);
    else c.note(std::string(done) + ": timer started with no period (set the '" + period + "' CSR or give 'timer_start:<cycles>')");
}

// A device whose registers are described in the environment file.
//   addr = 0x68
//   reg 0x3A = 0x00, 0x00, 0x01     reads return the values in turn, then stay on the last one
//   regseq 0x3B = 1, 2, 3           reads cycle through the values
//   block 0x43 = 0x12 0x34 0x56     consecutive registers, constant
//   default = 0x00                  value of every register not listed
//   nack = true                     the device never acknowledges
//   write_nack = true               writes are not acknowledged
//   extra_cycles = 100              device-side delay added to every transaction
class ScriptedDevice : public I2cDevice {
public:
    struct Reg {
        uint32_t addr;
        std::vector<uint8_t> vals;
        bool cycle;
    };
    std::vector<Reg> regs;
    uint8_t dflt = 0;
    bool nack = false, wnack = false;
    int extra = 0;

    void init(std::vector<int64_t>& state) override { state.assign(regs.size(), 0); }

    I2cReply read(uint32_t reg, int, int len, std::vector<int64_t>& state, Cycle) override {
        I2cReply r;
        r.extraCycles = extra;
        if (nack) { r.ack = false; return r; }
        if (state.size() != regs.size()) state.assign(regs.size(), 0);
        for (int i = 0; i < len; i++) {
            uint8_t v = dflt;
            for (size_t k = 0; k < regs.size(); k++) if (regs[k].addr == reg + (uint32_t)i) {
                int64_t n = state[k]++;
                size_t sz = regs[k].vals.size();
                v = regs[k].cycle ? regs[k].vals[(size_t)(n % (int64_t)sz)] : regs[k].vals[(size_t)std::min<int64_t>(n, (int64_t)sz - 1)];
                break;
            }
            r.data.push_back(v);
        }
        return r;
    }
    I2cReply write(uint32_t, int, const std::vector<uint8_t>&, std::vector<int64_t>&, Cycle) override {
        I2cReply r;
        r.extraCycles = extra;
        r.ack = !(nack || wnack);
        return r;
    }
};

std::unique_ptr<I2cDevice> makeScripted(const DeviceSpec& s, std::string& error) {
    auto d = std::unique_ptr<ScriptedDevice>(new ScriptedDevice());
    for (auto& kv : s.keys) {
        const std::string& key = kv.first;
        bool isReg = key.compare(0, 4, "reg ") == 0, isSeq = key.compare(0, 7, "regseq ") == 0, isBlock = key.compare(0, 6, "block ") == 0;
        if (isReg || isSeq || isBlock) {
            bool ok = false;
            int64_t a = numberOrZero(key.substr(key.find(' ') + 1), &ok);
            if (!ok || a < 0) { error = "bad register address in '" + key + "'"; return nullptr; }
            for (const std::string& line : kv.second) {
                std::vector<uint8_t> vals;
                std::string norm = line;
                std::replace(norm.begin(), norm.end(), ' ', ',');
                for (const std::string& t : splitTop(norm)) {
                    if (t.empty()) continue;
                    int64_t v = numberOrZero(t, &ok);
                    if (!ok) { error = "bad value '" + t + "' in '" + key + "'"; return nullptr; }
                    vals.push_back((uint8_t)(v & 0xFF));
                }
                if (vals.empty()) { error = "'" + key + "' has no values"; return nullptr; }
                if (isBlock) for (size_t i = 0; i < vals.size(); i++) d->regs.push_back({(uint32_t)a + (uint32_t)i, {vals[i]}, false});
                else d->regs.push_back({(uint32_t)a, vals, isSeq});
            }
        } else if (key == "default") d->dflt = (uint8_t)numberOrZero(kv.second.back());
        else if (key == "nack") d->nack = lower(kv.second.back()) == "true";
        else if (key == "write_nack") d->wnack = lower(kv.second.back()) == "true";
        else if (key == "extra_cycles") d->extra = (int)numberOrZero(kv.second.back());
        else if (key != "addr" && key != "kind") { error = "unknown key '" + key + "'"; return nullptr; }
    }
    return std::unique_ptr<I2cDevice>(std::move(d));
}

}  // namespace

Extensions Extensions::defaults() {
    Extensions x;
    x.registerAction("poll_clear", [](ActionContext& c, const std::string&) { c.setSignal("poll.ready", 0); c.setSignal("poll.not_ready", 0); c.setSignal("poll.timeout", 0); });
    x.registerAction("poll_arm", [](ActionContext& c, const std::string&) { c.setSignal("poll.arm", 1); });
    x.registerAction("poll_timeout_set", [](ActionContext& c, const std::string&) { c.setSignal("poll.timeout", 1); });
    x.registerAction("done_set", [](ActionContext& c, const std::string&) { c.setSignal("peri.done", 1); });
    x.registerAction("done_clear", [](ActionContext& c, const std::string&) { c.setSignal("peri.done", 0); });
    x.registerAction("error_set", [](ActionContext& c, const std::string&) { c.setSignal("peri.error", 1); });
    x.registerAction("error_clear", [](ActionContext& c, const std::string&) { c.setSignal("peri.error", 0); });
    // "clear poll / error / done latches" at the start of a service (spec 13.2)
    x.registerAction("service_reset", [](ActionContext& c, const std::string&) {
        for (const char* n : {"poll.ready", "poll.not_ready", "poll.timeout", "poll.arm", "peri.done", "peri.error"}) c.setSignal(n, 0);
    });
    x.registerAction("timer_start", [](ActionContext& c, const std::string& a) { startTimer(c, a, "timer.done", "timer_period", "timer.gen"); });
    x.registerAction("timer_clear", [](ActionContext& c, const std::string&) { c.setSignal("timer.gen", c.signal("timer.gen") + 1); c.setSignal("timer.done", 0); });
    x.registerAction("main_timer_start", [](ActionContext& c, const std::string& a) { startTimer(c, a, "main_timer.done", "main_timer_period", "main_timer.gen"); });
    x.registerAction("main_timer_clear", [](ActionContext& c, const std::string&) { c.setSignal("main_timer.gen", c.signal("main_timer.gen") + 1); c.setSignal("main_timer.done", 0); });
    x.registerAction("frame_end_clear", [](ActionContext& c, const std::string&) { c.setSignal("frame_end_evt", 0); });
    x.registerAction("overrun_clear", [](ActionContext& c, const std::string&) { c.setSignal("slot.overrun", 0); });
    // generic: any signal can be driven without writing code
    x.registerAction("set", [](ActionContext& c, const std::string& a) { c.setSignal(a, 1); });
    x.registerAction("clr", [](ActionContext& c, const std::string& a) { c.setSignal(a, 0); });
    x.registerDevice("scripted", makeScripted);
    return x;
}

}  // namespace sim
}  // namespace asmcore
