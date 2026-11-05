#pragma once
// Which simulator parameters belong to an instruction: what its own time is made of (latency.*, the decoder's own CSR access), and what else changes how
// long it takes or what it sees (the fetch, the I2C path, a branch's flag sampling ...). The block editor's "Set parameters..." on a block shows these for the
// instructions the block makes. No Qt.
#include <string>
#include <vector>

namespace acide {

struct InstrParams {
    std::vector<std::string> own;          // the instruction's cycle time
    std::vector<std::string> related;      // what else changes it (hardware around it, the decoder)
    std::vector<std::string> mnemonics;    // what was recognised ("MOV", "EXEC_CSR" ...), in order, once each
    bool empty() const { return own.empty() && related.empty(); }
};

// One line of assembler of a sequencer (0 Main, 1 Peri): a label, a comment or a directive gives nothing.
InstrParams instructionParams(const std::string& line, int domain);
// Several lines (the lines one block generates), merged in order without repeats.
InstrParams instructionParams(const std::vector<std::string>& lines, int domain);

// The parameters that belong to no box of the ISA Top diagram and to no instruction: the clock, the pace, the scheduling rules.
const std::vector<std::string>& globalParamKeys();

}  // namespace acide
