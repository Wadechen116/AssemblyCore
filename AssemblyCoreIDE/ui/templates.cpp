#include "templates.h"

namespace acide {

std::string newProgramText(const std::string& name) {
    return
        "; " + name + " - AssemblyCore project\n"
        "; Peri reads a sensor and publishes the bytes; Main picks them up from the result slot.\n"
        "\n"
        ".service SVC = 1                    ; service id shared by Main and Peri\n"
        ".devmap 0, I2C, 0x68, sensor        ; DeviceMap entry 0\n"
        "\n"
        ".main\n"
        "wait:\n"
        "    BR.COND PERI_RESULT_VALID, check\n"
        "    JMP wait\n"
        "check:\n"
        "    MOV PERI_RESULT_META[0..1] -> ALU_OPA       ; the service id\n"
        "    MOV CONST(SVC) -> MAIN_SCRATCHPAD[20..21]   ; constants go to the scratchpad first\n"
        "    MOV MAIN_SCRATCHPAD[20..21] -> ALU_OPB\n"
        "    ALU CMP.EQ unsigned\n"
        "    BR.COND ALU_CMP_TRUE, handle\n"
        "    MOV PERI_RESULT_SLOT[0..0] -> MAIN_SCRATCHPAD[31]   ; an unknown service: consume and drop\n"
        "    JMP wait\n"
        "handle:\n"
        "    MOV PERI_RESULT_SLOT[0..5] -> MAIN_SCRATCHPAD[0..5]\n"
        "    JMP wait\n"
        "\n"
        ".peri\n"
        "poll:\n"
        "    EXEC_XFER READ dev=0, len=6, ptr=0, ext=[0x10]\n"
        "    EXEC_PUB service=SVC, src=0, len=6\n"
        "    JMP poll\n";
}

}  // namespace acide
