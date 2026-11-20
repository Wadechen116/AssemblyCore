#include "param_texts.h"

#include <QCoreApplication>

#include "text_util.h"

namespace acide {

namespace {

// Every parameter of the catalog, by key: the "source text" of its translation (a test compares this list with the catalog).
const char* const kKeys[] = {
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.br_cond"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.cascade"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.alu"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.mul"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.mov_setup"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.mov_byte"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.slot_consume_setup"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.csr_access"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.csr_byte"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.csr_done"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.pub_setup"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.pub_byte"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.event_dispatch"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.eret"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "latency.xfer_issue"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "clock.cpu_hz"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "clock.i2c_hz"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "clock.timer_hz"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timing.frame_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timing.vblank_start"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timing.vblank_len"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timing.line_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timing.hblank_start"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timing.hblank_len"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frame.wcet_pgl"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frame.wcet_ae"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frame.wcet_line"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "sim.speed"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "sim.tie_order"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "sim.unmapped_csr"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "sim.fault_stops"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "fetch.source"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "boot.load"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frontend.sram_latency"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frontend.sram_policy"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frontend.hold_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frontend.dec_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "frontend.fire_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "fetch.read_mode"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "fetch.transaction_overhead"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "main.fetch_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "peri.fetch_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "csr.access_main"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "csr.access_peri"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "reset.boot_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "event.dispatch_extra"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "event.eret_extra"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.main.clock_div"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.main.start_latency"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.main.done_latency"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.peri.clock_div"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.peri.start_latency"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.peri.done_latency"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.prescaler"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.count"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "timer.phase"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "i2ch.overhead"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "i2c.ack_delay"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "i2c.stretch"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "i2c.turnaround"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "csr.arbiter_policy"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "csr.arbiter_penalty"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "datapath.contention"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "datapath.contention_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "slot.copy_extra"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "slot.valid_delay"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "flag.arbiter_policy"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "flag.access_cycles"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "flag.arbiter_penalty"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "flag.sample_delay_main"),
        QT_TRANSLATE_NOOP("SimParamPurpose", "flag.sample_delay_peri"),
};

// The parameters that have a hint on how to measure them (context "SimParamMeasure", again by key), and the units.
const char* const kMeasureKeys[] = {
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.br_cond"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.cascade"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.alu"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.mul"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.mov_setup"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.mov_byte"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.slot_consume_setup"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.csr_access"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.csr_byte"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.csr_done"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.pub_setup"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.pub_byte"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.event_dispatch"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.eret"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "latency.xfer_issue"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "clock.cpu_hz"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "clock.i2c_hz"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "clock.timer_hz"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timing.frame_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timing.vblank_start"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timing.vblank_len"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timing.line_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timing.hblank_start"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timing.hblank_len"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frame.wcet_pgl"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frame.wcet_ae"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frame.wcet_line"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "sim.tie_order"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "boot.load"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frontend.sram_latency"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frontend.sram_policy"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frontend.hold_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frontend.dec_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "frontend.fire_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "fetch.read_mode"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "fetch.transaction_overhead"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "main.fetch_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "peri.fetch_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "csr.access_main"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "csr.access_peri"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "reset.boot_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "event.dispatch_extra"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "event.eret_extra"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.main.clock_div"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.main.start_latency"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.main.done_latency"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.peri.clock_div"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.peri.start_latency"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.peri.done_latency"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.prescaler"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.count"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "timer.phase"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "i2ch.overhead"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "i2c.ack_delay"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "i2c.stretch"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "i2c.turnaround"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "csr.arbiter_policy"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "csr.arbiter_penalty"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "datapath.contention"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "datapath.contention_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "slot.copy_extra"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "slot.valid_delay"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "flag.arbiter_policy"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "flag.access_cycles"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "flag.arbiter_penalty"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "flag.sample_delay_main"),
        QT_TRANSLATE_NOOP("SimParamMeasure", "flag.sample_delay_peri"),
};
const char* const kUnits[] = {
        QT_TRANSLATE_NOOP("SimParamUnit", "Hz"),
        QT_TRANSLATE_NOOP("SimParamUnit", "ISA cycles"),
        QT_TRANSLATE_NOOP("SimParamUnit", "ISA cycles per byte"),
        QT_TRANSLATE_NOOP("SimParamUnit", "ISA cycles per second"),
        QT_TRANSLATE_NOOP("SimParamUnit", "ISA cycles per tick"),
        QT_TRANSLATE_NOOP("SimParamUnit", "SRAM cycles"),
};

QString words(const char* s) { return QCoreApplication::translate("SimParamFile", s); }

}  // namespace

const char* const* simParamKeysForTranslation(int& count) {
    count = (int)(sizeof kKeys / sizeof kKeys[0]);
    return kKeys;
}

QString simParamPurpose(const asmcore::ParamDef& d) {
    const QString t = QCoreApplication::translate("SimParamPurpose", d.key.c_str());
    return t == QString::fromUtf8(d.key.c_str()) ? fromStd(d.help) : t;          // no translation: the catalog's help (English)
}

const char* const* simParamMeasureKeysForTranslation(int& count) { count = (int)(sizeof kMeasureKeys / sizeof kMeasureKeys[0]); return kMeasureKeys; }
const char* const* simParamUnitsForTranslation(int& count) { count = (int)(sizeof kUnits / sizeof kUnits[0]); return kUnits; }

QString simParamMeasure(const asmcore::ParamDef& d) {
    if (d.measure.empty()) return QString();
    const QString t = QCoreApplication::translate("SimParamMeasure", d.key.c_str());
    return t == QString::fromUtf8(d.key.c_str()) ? fromStd(d.measure) : t;
}

QString simParamUnit(const asmcore::ParamDef& d) { return d.unit.empty() ? QString() : QCoreApplication::translate("SimParamUnit", d.unit.c_str()); }

QString simParamTip(const asmcore::ParamDef& d) {
    const QString m = simParamMeasure(d);
    return simParamPurpose(d) + (m.isEmpty() ? QString() : "\n" + words(QT_TRANSLATE_NOOP("SimParamFile", "On the FPGA:")) + " " + m);
}

asmcore::SimParams::FileWords simParamsFileWordsForUi() {
    asmcore::SimParams::FileWords w;
    w.header = words(QT_TRANSLATE_NOOP("SimParamFile", "Hardware parameters of the simulator: only what differs from the initial (planning) values.")).toStdString() + "\n"
             + words(QT_TRANSLATE_NOOP("SimParamFile", "The comment above each parameter says what it is for; the comments are written again on every save.")).toStdString() + "\n"
             + words(QT_TRANSLATE_NOOP("SimParamFile", "After measuring on the FPGA:  KEY.status = measured (or confirmed)   KEY.note = how it was measured")).toStdString();
    w.defaultNote = [](const asmcore::ParamDef& d) { return simParamPurpose(d).toStdString(); };      // KEY.note of a parameter the user wrote no note for
    w.describe = [](const asmcore::ParamDef& d) {
        const std::string unit = words(QT_TRANSLATE_NOOP("SimParamFile", "unit:")).toStdString(), initial = words(QT_TRANSLATE_NOOP("SimParamFile", "initial value:")).toStdString(),
                          range = words(QT_TRANSLATE_NOOP("SimParamFile", "range:")).toStdString(), oneOf = words(QT_TRANSLATE_NOOP("SimParamFile", "one of:")).toStdString();
        return simParamPurpose(d).toStdString() + "\n(" + asmcore::paramFacts(d, unit.c_str(), initial.c_str(), range.c_str(), oneOf.c_str()) + ")";
    };
    return w;
}

}  // namespace acide
