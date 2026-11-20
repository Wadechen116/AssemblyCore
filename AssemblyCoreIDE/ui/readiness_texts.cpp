#include "readiness_texts.h"

#include <cstring>

#include <QCoreApplication>

#include "asmcore/profile_info.h"
#include "text_util.h"

namespace acide {

namespace {

// The titles and explanations of the profile groups that can be confirmed, exactly as core/src/profile_info.cpp has them (a test compares).
const char* const kGroupTexts[] = {
        QT_TRANSLATE_NOOP("ProfileGroup", "Program counter and code size"),
        QT_TRANSLATE_NOOP("ProfileGroup", "Optional hardware features"),
        QT_TRANSLATE_NOOP("ProfileGroup", "Program SRAM placement"),
        QT_TRANSLATE_NOOP("ProfileGroup", "I2C EEPROM (program store)"),
        QT_TRANSLATE_NOOP("ProfileGroup", "Image layout and alignment"),
        QT_TRANSLATE_NOOP("ProfileGroup", "Descriptor byte order"),
        QT_TRANSLATE_NOOP("ProfileGroup", "DeviceMap entry packing"),
        QT_TRANSLATE_NOOP("ProfileGroup", "ALU sub-operation numbers"),
        QT_TRANSLATE_NOOP("ProfileGroup", "Hardware events"),
        QT_TRANSLATE_NOOP("ProfileGroup", "Latency numbers"),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "How far the code may grow and how wide the program counter is. The spec leaves the PC width and any narrower code limit to the target; ordinary branches still reach only bytes 0..255."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "Whether a non-overlapping MOV inside one region is allowed, and whether CSR read-back bytes carry a valid bit. The v31 spec does not decide either; the RTL tests do."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "Where the Main and Peri images sit in the one shared SRAM and how much room each has. The spec shows a toy layout only; the real depth, base addresses and capacities are RTL parameters."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "The EEPROM the decoders fetch the code, the descriptor tables, the const pool and the DeviceMap from: its size, I2C address, address width and where each region starts. Planning values until the board and the RTL give them."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "How wide the sideband offset latches are and how sections are aligned. Not stated by the spec; wrong values make the hardware read the wrong table."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "In which byte order multi-byte descriptors are stored. v31 says to concatenate 'in the existing byte order' and leaves the serialization to the target; the SRAM lane mapping alone does not settle it."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "Which bits of a DeviceMap entry hold the bus type and the I2C address, and how many bytes an entry takes. The spec gives only the bit widths."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "The spec names ADD, SUB, MUL, CMP.EQ ... but never gives their numeric codes. The numbers here are placeholders: a wrong code makes the hardware do a different operation."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "Number of events and the cycles to enter a handler and return from it. Vector sources, priorities and the cycle bounds are defined by the target."),
        QT_TRANSLATE_NOOP("ProfileGroupWhy", "Cycle counts used for the listing, WCET and the simulator. They are the PLANNING numbers of spec section 14, not signoff values; response times are only as good as these."),
};

// A short title for each hardware parameter the readiness check can name (the catalog's help text is a paragraph).
struct ParamTitle { const char* key; const char* title; };
const ParamTitle kParamTitles[] = {
    {"fetch.source", QT_TRANSLATE_NOOP("ReadinessParam", "Where the decoders fetch the program from")},
    {"fetch.read_mode", QT_TRANSLATE_NOOP("ReadinessParam", "How a decoder reads an instruction from the I2C EEPROM")},
    {"fetch.transaction_overhead", QT_TRANSLATE_NOOP("ReadinessParam", "Extra cycles of every EEPROM read of a decoder")},
    {"i2c.ack_delay", QT_TRANSLATE_NOOP("ReadinessParam", "Extra cycles per byte for the acknowledge")},
    {"i2c.stretch", QT_TRANSLATE_NOOP("ReadinessParam", "Clock stretching per byte")},
    {"frontend.sram_latency", QT_TRANSLATE_NOOP("ReadinessParam", "Read latency of the program SRAM")},
    {"frontend.sram_policy", QT_TRANSLATE_NOOP("ReadinessParam", "How the two Frontends share the SRAM port")},
    {"frontend.hold_cycles", QT_TRANSLATE_NOOP("ReadinessParam", "Cycles of the Frontend in CMD_HOLD")},
    {"frontend.dec_cycles", QT_TRANSLATE_NOOP("ReadinessParam", "Cycles of the Frontend in CMD_DEC")},
    {"frontend.fire_cycles", QT_TRANSLATE_NOOP("ReadinessParam", "Cycles of the Frontend in CMD_FIRE")},
};

QString words(const char* s) { return QCoreApplication::translate("Readiness", s); }

}  // namespace

const char* const* readinessTextsForTranslation(int& count) {
    count = (int)(sizeof kGroupTexts / sizeof kGroupTexts[0]);
    return kGroupTexts;
}

bool readinessHasParamTitle(const std::string& key) {
    for (const ParamTitle& p : kParamTitles) if (key == p.key) return true;
    return false;
}

QString profileGroupTitle(const std::string& t) { return QCoreApplication::translate("ProfileGroup", t.c_str()); }
QString profileGroupWhy(const std::string& t) { return QCoreApplication::translate("ProfileGroupWhy", t.c_str()); }

ReadinessWords readinessWords(const asmcore::ReadinessItem& it) {
    ReadinessWords w;
    if (it.code == "group") {
        w.title = profileGroupTitle(it.title);
        for (const asmcore::ProfileGroup& g : asmcore::profileGroups()) if (g.name == it.id) w.why = profileGroupWhy(g.why);
        if (w.why.isEmpty()) w.why = fromStd(it.why);
        if (it.id == "alu") w.why += " " + words(QT_TRANSLATE_NOOP("Readiness", "This group also stands for the shift direction bit (SHL = 0, SHR = 1), which has no key of its own."));
        w.fix = words(QT_TRANSLATE_NOOP("Readiness", "Check the values against the RTL / the board, then add the group to the profile: confirmed = ..., %1")).arg(fromStd(it.id));
    } else if (it.code == "param_planning") {
        w.title = fromStd(it.title);
        for (const ParamTitle& p : kParamTitles) if (it.id == p.key) w.title = QCoreApplication::translate("ReadinessParam", p.title);
        w.why = words(QT_TRANSLATE_NOOP("Readiness", "The time to fetch an instruction is counted with it, and it is still a planning value (%1).")).arg(fromStd(it.value));
        w.fix = words(QT_TRANSLATE_NOOP("Readiness", "Measure it or check it against the RTL, then mark it in the .simparams file: %1.status = measured (or confirmed)")).arg(fromStd(it.id));
    } else if (it.code == "fetch_off_sram" || it.code == "fetch_off_instant") {
        const bool sram = it.code == "fetch_off_sram";
        w.title = sram ? words(QT_TRANSLATE_NOOP("Readiness", "fetch.source = sram: fetching takes no time")) : words(QT_TRANSLATE_NOOP("Readiness", "fetch.read_mode = instant: fetching takes no time"));
        w.why = words(QT_TRANSLATE_NOOP("Readiness", "A setting for looking at a program, not the hardware: the WCET then counts the execution alone."));
        w.fix = sram ? words(QT_TRANSLATE_NOOP("Readiness", "Set fetch.source to how the hardware fetches (eeprom or frontend)."))
                     : words(QT_TRANSLATE_NOOP("Readiness", "Set fetch.read_mode to how the hardware reads (sequential or per_byte)."));
    } else if (it.code == "fetch_time") {
        w.title = words(QT_TRANSLATE_NOOP("Readiness", "The time to fetch each instruction is not in the WCET"));
        w.why = words(QT_TRANSLATE_NOOP("Readiness", "This build was made without the hardware parameters, so its WCET counts the execution alone."));
        w.fix = words(QT_TRANSLATE_NOOP("Readiness", "Build with the parameters (AssemblyCoreIDE does; asmc with --params, --param or --fetch-time)."));
    } else if (it.code == "shared_waits") {
        w.title = words(QT_TRANSLATE_NOOP("Readiness", "The WCET does not bound the waiting for the other sequencer"));
        w.why = words(QT_TRANSLATE_NOOP("Readiness", "Main and Peri share the I2C bus / the program SRAM port, the CSR arbiter, the Status Flag unit and Data Xfer. How long one waits depends on what the other is doing, and the WCET analysis looks at one sequencer alone."));
        w.fix = words(QT_TRANSLATE_NOOP("Readiness", "Not available yet. Until the tools bound these waits, add the longest wait by hand (the simulator shows them) and treat every WCET as planning-only."));
    } else {
        w.title = fromStd(it.title);
        w.why = fromStd(it.why);
        w.fix = fromStd(it.fix);
    }
    return w;
}

}  // namespace acide
