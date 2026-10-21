#include "asmcore/diagnostics.h"

namespace asmcore {

namespace {
const std::vector<DiagInfo> kTable = {
    {"E101", "", "Unknown instruction or directive.", "Check the spelling against docs/ASM_SYNTAX.md; directives start with a dot (.main, .peri, .csr...)."},
    {"E102", "", "The operands are malformed.", "The message shows the expected form; compare it with the syntax in docs/ASM_SYNTAX.md."},
    {"E103", "", "Bad expression, or a symbol is not defined.", "Define the symbol with .equ / .csr / .service / .const before use, or fix the expression."},
    {"E104", "", "A symbol, label or event id is defined twice.", "Rename one of them (labels are per domain; procedure-local labels are scoped to their .proc)."},
    {"E105", "", "A directive is in the wrong place (e.g. an instruction before .main/.peri, .org, nested .proc).", "Start each domain with .main or .peri; the PC always starts at 0."},
    {"E106", "", ".include failed.", "Check the file name and the -I search directories."},
    {"E201", "E_DESCRIPTOR_INDEX", "A number is out of range for its field.", "Use a value inside the range the message gives (CSR addresses are 12 bits, service ids 1..15, lengths 1..32...)."},
    {"E202", "E_MOV_REGION_OR_RANGE", "Illegal MOV: wrong region direction, out-of-range bytes, overlapping same-region copy, or a constant not going to the scratchpad (E_CONST_DEST).", "v31: constants go to MAIN_SCRATCHPAD first, then on to the ALU; MAIN_CSR_DATA_BUF is read-only for MOV; keep ranges inside 32 bytes and non-overlapping."},
    {"E203", "E_ALU_SUBOP_UNDEFINED", "Illegal ALU operation or mode (signed/unsigned, shift amount 1..8).", "Use a mode and shift amount the ISA allows; the numeric sub-operation codes come from the profile (alu_* keys)."},
    {"E204", "E_CSR_ACCESS_PERMISSION", "Illegal EXEC_CSR form. WRITE_BUF was renamed WRITE_SP in v31 (its source is the scratchpad).", "Use WRITE_SP with a scratchpad range; READ_BUF/READ8 fill MAIN_CSR_DATA_BUF."},
    {"E205", "", "The branch condition is not defined.", "Use one of the ConditionMap names; PROFILE_COND0 needs a profile that defines it."},
    {"E206", "E_XFER_PREFIX", "Illegal EXEC_XFER / EXEC_PUB (address length, multi-byte WRITE, service id 0, range over 32 bytes, unsupported bus).", "Follow the form in docs/ASM_SYNTAX.md section 5; service ids are 1..15."},
    {"E207", "E_DESCRIPTOR_INDEX", "A descriptor table is full (more distinct descriptors than the profile allows).", "Reuse identical forms (identical descriptors are shared) or raise the table depth in the profile if the hardware has it."},
    {"E208", "E_EVENT_VECTOR_OR_ERET", "event_safe ('safe') used where it is not allowed: cascade BEGIN/END, BR.ERET, inside a cascade region.", "Put 'safe' on an ordinary BR.COND outside any cascade."},
    {"E301", "", "A branch target is undefined or not an instruction.", "Define the label in the same domain; a branch target must be an opcode byte."},
    {"E302", "E_PC_RANGE", "The code is larger than the PC / code limit of the profile.", "Shorten the program or give the profile the approved main_pc_width / peri_pc_width / max_code_bytes."},
    {"E303", "E_PC_RANGE", "Execution can run off the end of the code or fall into a procedure.", "End the flow with an unconditional branch (JMP)."},
    {"E304", "E_CASCADE_CFG", "Cascade structure error (nesting, step order, selector too large, branch inside the region).", "A cascade is BEGIN, then blocks that each end in CASCADE_END i with i = 0,1,2...; nothing else may branch inside."},
    {"E305", "", "Procedure / call-graph error (recursion, undefined, falls off the end, branch into a body).", "Procedures must not recurse and must end in RET or an unconditional branch."},
    {"E306", "", "softret scratch bytes are not enough or collide with your program.", "Keep your data out of ret_slot_base / ret_stage_base ranges (profile), or use the inline call model."},
    {"E307", "", "This instruction does not belong to this domain.", "Main and Peri have different instruction sets; move it under the right .main/.peri."},
    {"E308", "", "The constant pool is full or an offset is out of range.", "Reduce the constants (identical ones are shared) or use the profile's const_pool_max if the hardware has more."},
    {"E309", "E_PROFILE_MISSING", "A DeviceMap entry is used but not defined.", "Add '.devmap i, I2C, addr'."},
    {"E310", "BR_TARGET_UNREPRESENTABLE", "A branch target does not fit the 8-bit target_pc_byte (>255). Nothing is truncated.", "Move the label into the first 256 bytes, or restructure; no long-branch opcode is invented."},
    {"E311", "E_SRAM_PLACEMENT", "The Main/Peri SRAM windows overlap or do not fit the SRAM.", "Fix sram_depth_words and the *_image_base_word / *_capacity_bytes keys of the profile."},
    {"E312", "E_SECTION_LAYOUT", "An image is larger than its SRAM window.", "Shrink the program or enlarge the window in the profile."},
    {"E313", "E_PROFILE_MISSING", "The DeviceMap entry fields overlap or do not fit the entry.", "Fix device_map_entry_bytes / device_map_bus_shift / device_map_addr_shift (the packing must come from the approved profile)."},
    {"E314", "E_LAYOUT_OFFSET_WIDTH", "An ImageLayout offset does not fit the sideband field width.", "Raise layout_offset_bits to the width the hardware has, or shrink the image."},
    {"E315", "E_EVENT_VECTOR_OR_ERET", "Event vector or BR.ERET problem (undefined or misplaced vector, ERET reachable in normal flow, handler that cannot reach ERET).", "Put handlers after the main loop's JMP, enter them only through '.vector', and end every path in BR.ERET."},
    {"W401", "", "A procedure is never called (no code is generated).", "Call it or remove it."},
    {"W402", "", "Unreachable instruction.", "Remove it, or add the branch/vector that reaches it."},
    {"W403", "", "Peri cannot use softret; inline calls are used.", "Informational."},
    {"W404", "", "The DeviceMap has a hole.", "Define the missing entry or renumber."},
    {"W405", "E_CASCADE_CFG", "Cascade exits differ.", "Use one common exit label for all CASCADE_END of a cascade."},
    {"W410", "", "signed/unsigned given to a LOGIC operation.", "Drop it; LOGIC has no signed mode."},
    {"W501", "E_ALU_OPERAND_UNINIT", "ALU_OPA / ALU_OPB are not loaded on every path.", "MOV a 2-byte value into both operands before the ALU operation."},
    {"W502", "E_CSR_READBACK_SOURCE", "Bytes are read (scratchpad, CSR readback, WRITE_SP source) that are not written on every path.", "Write them first, or declare them with .assume / a profile reset value."},
    {"W503", "", "ALU_OUT / ALU_STATUS read before any producing ALU operation.", "Run the ALU operation first."},
    {"W504", "", "An ALU result is overwritten before it was moved out.", "MOV ALU_OUT away before the next ALU operation."},
    {"W505", "E_RESULT_CONSUME", "The result slot is consumed without testing PERI_RESULT_VALID, or twice.", "BR.COND PERI_RESULT_VALID first, and consume once."},
    {"W506", "", "A flag branch without a preceding ALU operation.", "Run CMP / the operation before BR.COND ALU_CMP_TRUE / ALU_OUT_OV16."},
    {"W507", "E_PUBLISH_RANGE", "The handler consumes a different number of bytes than Peri publishes for the service.", "Make publish and consume lengths equal (declare the contract: .service NAME = id, len=N)."},
    {"W508", "E_PUBLISH_RANGE", "EXEC_PUB publishes PERI_RX_BUF bytes that no EXEC_XFER has read in on every path.", "READ the bytes into PERI_RX_BUF before publishing."},
    {"W510", "E_SERVICE_DISPATCH", "A published service has no handler (and no explicit unknown-service path) in Main.", "Add a handler comparing the service id, or an explicit unknown path."},
    {"W511", "E_PUBLISH_RANGE", "The same service id is published with different lengths.", "One service id = one payload length; allocate another id."},
    {"W512", "E_EVENT_UNSAFE_LIVENESS", "A handler may overwrite state that the code after a 'safe' branch still reads.", "Remove 'safe' there, let the handler use other bytes, or make it save and restore (hardware keeps no context)."},
    {"W513", "E_UNBOUNDED_WCET", "The event response time has no upper bound (no safe branch, or a foreground loop without one).", "Put 'safe' on an ordinary BR.COND on every foreground loop if events must be answered in bounded time."},
    {"W514", "E_SENSOR_BYTE_ORDER", "A big-endian (high byte first) 2-byte sensor value goes into the little-endian ALU without a byte swap.", "Swap through the scratchpad with two 1-byte MOVs, then MOV the swapped pair into ALU_OPA/ALU_OPB."},
    {"W515", "E_TIMER_START_FLOW", "A timer flag is tested on a path where the timer was never started.", "Start the timer once before the wait loop (declare the start bit: .csrbit CSR[n] = timer_start)."},
    {"W516", "E_POLL_ARM_PRODUCER", "The poll result is tested without arming the evaluator and reading PERI_RX_BUF[0] first.", "Arm the poll (.csrbit ... = poll_arm), then EXEC_XFER READ into PERI_RX_BUF[0], then test the result."},
    {"W517", "E_PUBLISH_RANGE", "A publish or consume length differs from the service contract (.service ..., len=N).", "Make the program match the contract, or fix the contract."},
};
}  // namespace

const std::vector<DiagInfo>& allDiagnostics() { return kTable; }

const DiagInfo* diagInfo(const std::string& code) {
    if (code.size() < 2) return nullptr;
    for (const DiagInfo& d : kTable) {
        if (code == d.code) return &d;
        if (code[0] == 'E' && d.code[0] == 'W' && code.compare(1, std::string::npos, d.code + 1) == 0) return &d;     // data-flow warning reported as an error
    }
    return nullptr;
}

std::string explainDiagnostic(const std::string& code) {
    const DiagInfo* d = diagInfo(code);
    if (!d) return "";
    std::string s = std::string(d->code) + ": " + d->title + "\n";
    if (d->specName[0]) s += std::string("  spec v31 name: ") + d->specName + "\n";
    s += std::string("  how to fix: ") + d->fix + "\n";
    return s;
}

}  // namespace asmcore
