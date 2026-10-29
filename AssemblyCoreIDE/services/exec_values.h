#pragma once
// What an instruction just did, in a few words, read from the simulator's state: "scratch[0..1] = 05 00" after a MOV, "OUT 0A 00 cmp" after an ALU operation,
// "C=0x01" after an EXEC_CSR write, "PERI_RESULT_VALID: true" with a lamp at a branch. The block editor shows it next to the block that produced the instruction. No Qt.
#include <string>

#include "sim_session.h"

namespace acide {

// `text`: the instruction as the assembler listed it ("MOV CONST(5, 2) -> MAIN_SCRATCHPAD[0..1]"); domain 0 Main / 1 Peri. "" when there is nothing worth showing.
std::string instructionResult(const std::string& text, int domain, const SimView& view);

}  // namespace acide
