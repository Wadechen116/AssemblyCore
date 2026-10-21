#pragma once
// Hardware description (the "Hardware Designer" model): the things the target board has that the ISA spec only NAMES --
// CSRs (address, bit meanings), I2C devices, services (payload contract), constants, simulation options.
//
// One file (PREFIX.achw) is the single source of truth. From it two plain files are generated, which every other tool already understands:
//   generateInc()  -> assembler definitions (.csr .csrbit .service .devmap .const .equ)   -- include it / list it as the first source
//   generateEnv()  -> simulator environment file (docs/SIM.md)
// importHardware() goes the other way (existing defs.inc + environment file -> model), so a project can adopt the designer without retyping.
// Standard C++17, no Qt, no file I/O: the CLI (asmhw) and the IDE both use it.
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace asmcore {

struct HwCsr {
    std::string name;
    int addr = -1;                                   // 0..0xFFF
    std::string kind = "ram";                        // ram | value | action | status | const   (docs/SIM.md section 3)
    int reset = 0;                                   // ram
    std::string bind;                                // value: the signal it reads / writes
    int bytes = 1;                                   // value: how many bytes of the signal the CSR spans
    long long constVal = 0;                          // const
    std::string bit[8];                              // action: "name[:arg]";  status: an expression;  "" = unused
    std::string role[8];                             // assembler meaning of a bit (.csrbit): timer_start | timer_clear | poll_arm | poll_clear
    bool simulated = true;                           // false: only a .csr NAME for the assembler (e.g. the second byte of a 2-byte value CSR), no [csr] in the environment
    std::string description;
};

struct HwDevice {
    std::string name;                                // the environment section name ([device name])
    std::string symbol;                              // DeviceMap name for EXEC_XFER dev=...  (default DEV_<NAME>)
    int addr = 0x68;                                 // 7-bit I2C address
    int index = -1;                                  // DeviceMap entry 0..7, -1 = not in the DeviceMap
    bool simulated = true;                           // false: only a DeviceMap entry for the assembler, no [device] in the environment (the simulator NACKs it)
    std::string kind = "scripted";
    std::vector<std::pair<std::string, std::string>> keys;    // reg / regseq / block / default / nack / extra_cycles ... (as in docs/SIM.md section 4)
    std::string description;
};

struct HwService {
    std::string name;
    int id = 1;                                      // 1..15
    int len = -1;                                    // payload bytes 1..32, -1 = not declared
    char wire = '?';                                 // 'B' high byte first, 'L' low byte first, '?' not declared
    std::string description;
};

struct HwConst {
    std::string name;
    int width = 1;                                   // 1 (db) or 2 (dw)
    std::vector<std::string> values;                 // expressions
    std::string description;
};

struct HwEqu {
    std::string name, value, description;
};

// Environment sections the designer does not model field by field ([sim] [timing] [latency] [bus] [signal] [condition] [inject]):
// kept as written and written out again.
struct HwSection {
    std::string kind, name;
    std::vector<std::pair<std::string, std::string>> kv;
};

struct Hardware {
    std::string name;
    std::string description;
    std::vector<HwEqu> equs;
    std::vector<HwConst> consts;
    std::vector<HwCsr> csrs;
    std::vector<HwDevice> devices;
    std::vector<HwService> services;
    std::vector<HwSection> env;
    std::vector<std::string> rawInc;                 // assembler lines the designer does not understand (kept verbatim)

    HwCsr* findCsr(const std::string& n);
    const HwCsr* findCsr(const std::string& n) const;
    HwDevice* findDevice(const std::string& n);
    HwService* findService(const std::string& n);
};

// ---- the .achw file ------------------------------------------------------------------------------------------------------------------
// INI-like, same style as the environment file: [hardware] [csr NAME] [device NAME] [service NAME] [const NAME] [equ NAME] [raw_inc]
// plus the environment sections above. Errors carry the line number.
bool parseHardware(const std::string& text, Hardware& out, std::vector<std::string>& errors);
std::string serializeHardware(const Hardware& hw);

// ---- generated files (deterministic; the first line says where they come from and that they are generated) --------------------------
std::string generateInc(const Hardware& hw, const std::string& sourceName = "");
std::string generateEnv(const Hardware& hw, const std::string& sourceName = "");

// ---- existing files -> model ---------------------------------------------------------------------------------------------------------
// `incText` and `envText` may be empty. Lines that cannot be modelled are kept (rawInc / env sections), so nothing is lost.
bool importHardware(const std::string& incText, const std::string& envText, Hardware& out, std::vector<std::string>& errors);

// ---- checking ------------------------------------------------------------------------------------------------------------------------
struct HwProblem {
    bool error = true;                               // false: a warning
    std::string where;                               // "csr AE_CTRL", "device imu", "service GYRO", "" for the whole file
    std::string message;
    int line = 0;                                    // line in the .achw if known (0 = unknown)
};
// Model-level rules with messages written for the person filling in the form.
std::vector<HwProblem> validateHardware(const Hardware& hw);
// The safety net: the generated files are really assembled and loaded by the real assembler and simulator; whatever they reject is
// reported too (so a rule that validateHardware does not know can never produce files the tools refuse).
std::vector<HwProblem> checkHardwareWithTools(const Hardware& hw);

}  // namespace asmcore
