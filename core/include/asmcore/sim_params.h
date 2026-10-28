#pragma once
// The simulator's hardware parameters: ONE catalog of every number that decides how long things take or how the two sequencers meet, each with a
// unit, a default, a range, a help text and a hint on how to measure it on the FPGA. The values are overrides on top of the defaults; a value that
// was never set means "the initial (planning) value". Where a parameter was set from a measurement it says so (status "measured" / "confirmed"),
// so a simulator tuned against the FPGA and one running on planning numbers can be told apart (docs/SIM_VISUAL_DESIGN.md).
//
// Where they come from (later ones win): the defaults, the environment file's [latency] [timing] [bus i2c] [sim] sections, its [params] section,
// SimConfig::params (the IDE, `asmsim --param key=value`). Standard C++17 only.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "latency.h"

namespace asmcore {

struct ParamDef {
    enum Kind { Int, Bool, Enum };
    std::string key;                 // "latency.alu", "timer.peri.clock_div" ...
    std::string group;               // "Instruction latency", "Clocks", "Frame timing", "Timers", "I2C", "CSR arbiter", ...
    std::string unit;                // "ISA cycles", "Hz", "cycles per tick", "" ...
    std::string help;                // what it is
    std::string measure;             // how to measure it on the FPGA ("" = not applicable)
    Kind kind = Int;
    int64_t def = 0;                 // the initial value (Bool 0/1, Enum: the index in `options`)
    int64_t lo = 0, hi = 0;          // Int: inclusive range
    std::vector<std::string> options;      // Enum: the names
    bool active = true;              // false: defined and stored, but the simulator does not use it yet (listed so the catalog is complete)
    int Latency::* latency = nullptr;      // set: the parameter IS this field of Latency
};

// Every parameter, in display order.
const std::vector<ParamDef>& paramCatalog();
const ParamDef* findParam(const std::string& key);

class SimParams {
public:
    // `text`: a number (decimal, 0x..., negative), a bool (true / false / on / off / yes / no / 1 / 0) or an enum name. False with `error` if the key is unknown,
    // the text is not of the kind, or the number is out of range; nothing changes then.
    bool set(const std::string& key, const std::string& text, std::string& error);
    void clear(const std::string& key);                     // back to the initial value (and forget its status and note)
    void clearAll();

    int64_t get(const std::string& key) const;              // the override, else the initial value (0 for an unknown key)
    bool isSet(const std::string& key) const { return v_.count(key) != 0; }
    std::string text(const std::string& key) const;         // the value as written: 12, true, round_robin
    bool atDefault(const std::string& key) const { return !isSet(key) || get(key) == defaultOf(key); }
    static int64_t defaultOf(const std::string& key);

    // Where the value comes from: "planning" (the initial value, or set by hand without a measurement), "measured" (from an FPGA run), "confirmed" (checked against the RTL).
    bool setStatus(const std::string& key, const std::string& status, std::string& error);
    std::string status(const std::string& key) const;
    void setNote(const std::string& key, const std::string& note) { if (note.empty()) note_.erase(key); else note_[key] = note; }
    std::string note(const std::string& key) const { auto it = note_.find(key); return it == note_.end() ? std::string() : it->second; }

    void merge(const SimParams& other);                     // the values of `other` win
    size_t size() const { return v_.size(); }
    std::vector<std::string> keys() const;                  // the keys that are set, in catalog order

    // Text form (an INI section): "[params]" then  key = value,  key.status = measured,  key.note = text.  `load` also takes the bare lines (no header) and
    // the key = value lines of an environment file's [params] section; ; and # start a comment. A bad line is reported and skipped.
    bool load(const std::string& text, std::vector<std::string>& errors);
    std::string serialize() const;                          // only what is set, in catalog order

    // The parameters that are fields of Latency are written into it (the ones not set are left as they are).
    void apply(Latency& l) const;

private:
    std::map<std::string, int64_t> v_;
    std::map<std::string, std::string> status_, note_;
};

}  // namespace asmcore
