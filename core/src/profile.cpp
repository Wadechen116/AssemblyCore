#include <cstdlib>
#include <sstream>

#include "asmcore/asmcore.h"
#include "util.h"

namespace asmcore {

const char* domainName(Domain d) { return d == Domain::Main ? "Main" : "Peri"; }

namespace {
struct Key {
    const char* name;
    int Profile::*ip;
    bool Profile::*bp;
    int Latency::*lp;
};
const Key kKeys[] = {
    {"main_move_depth", &Profile::mainMoveDepth, nullptr, nullptr},
    {"main_branch_depth", &Profile::mainBranchDepth, nullptr, nullptr},
    {"main_csr1_depth", &Profile::mainCsr1Depth, nullptr, nullptr},
    {"main_csrm_depth", &Profile::mainCsrMDepth, nullptr, nullptr},
    {"main_alu_depth", &Profile::mainAluDepth, nullptr, nullptr},
    {"peri_xfer1_depth", &Profile::periXfer1Depth, nullptr, nullptr},
    {"peri_xferm_depth", &Profile::periXferMDepth, nullptr, nullptr},
    {"peri_pub_depth", &Profile::periPubDepth, nullptr, nullptr},
    {"peri_branch_depth", &Profile::periBranchDepth, nullptr, nullptr},
    {"peri_csr1_depth", &Profile::periCsr1Depth, nullptr, nullptr},
    {"device_map_depth", &Profile::deviceMapDepth, nullptr, nullptr},
    {"max_code_bytes", &Profile::maxCodeBytes, nullptr, nullptr},
    {"const_pool_max", &Profile::constPoolMax, nullptr, nullptr},
    {"profile_cond0", nullptr, &Profile::profileCond0, nullptr},
    {"event_count", &Profile::eventCount, nullptr, nullptr},
    {"event_dispatch", nullptr, nullptr, &Latency::eventDispatch},
    {"event_eret", nullptr, nullptr, &Latency::eret},
    {"csr_buf_valid", nullptr, &Profile::csrBufValid, nullptr},
    {"same_region_mov", nullptr, &Profile::sameRegionMov, nullptr},
    {"ret_slot_base", &Profile::retSlotBase, nullptr, nullptr},
    {"ret_stage_base", &Profile::retStageBase, nullptr, nullptr},
    {"layout_offset_bits", &Profile::layoutOffsetBits, nullptr, nullptr},
    {"sram_depth_words", &Profile::physicalSramDepthWords, nullptr, nullptr},
    {"main_image_base_word", &Profile::mainImageBaseWord, nullptr, nullptr},
    {"peri_image_base_word", &Profile::periImageBaseWord, nullptr, nullptr},
    {"main_capacity_bytes", &Profile::mainCapacityBytes, nullptr, nullptr},
    {"peri_capacity_bytes", &Profile::periCapacityBytes, nullptr, nullptr},
    {"main_pc_width", &Profile::mainPcWidth, nullptr, nullptr},
    {"peri_pc_width", &Profile::periPcWidth, nullptr, nullptr},
    {"boot_pad_byte", &Profile::bootPadByte, nullptr, nullptr},
    {"device_map_entry_bytes", &Profile::deviceMapEntryBytes, nullptr, nullptr},
    {"device_map_bus_shift", &Profile::deviceMapBusShift, nullptr, nullptr},
    {"device_map_addr_shift", &Profile::deviceMapAddrShift, nullptr, nullptr},
    {"big_endian", nullptr, &Profile::bigEndian, nullptr},
    {"scratchpad_reset_defined", nullptr, &Profile::scratchpadResetDefined, nullptr},
    {"csrbuf_reset_defined", nullptr, &Profile::csrBufResetDefined, nullptr},
    {"peri_rx_reset_defined", nullptr, &Profile::periRxResetDefined, nullptr},
    {"br_cond", nullptr, nullptr, &Latency::brCond},
    {"cascade", nullptr, nullptr, &Latency::cascade},
    {"alu", nullptr, nullptr, &Latency::alu},
    {"mul", nullptr, nullptr, &Latency::mul},
    {"mov_setup", nullptr, nullptr, &Latency::movSetup},
    {"mov_byte", nullptr, nullptr, &Latency::movByte},
    {"slot_consume_setup", nullptr, nullptr, &Latency::slotConsumeSetup},
    {"csr_access", nullptr, nullptr, &Latency::csrAccess},
    {"csr_byte", nullptr, nullptr, &Latency::csrByte},
    {"csr_done", nullptr, nullptr, &Latency::csrDone},
    {"pub_setup", nullptr, nullptr, &Latency::pubSetup},
    {"pub_byte", nullptr, nullptr, &Latency::pubByte},
    {"xfer_issue", nullptr, nullptr, &Latency::xferIssue},
    {"cpu_hz", nullptr, nullptr, &Latency::cpuHz},
    {"i2c_hz", nullptr, nullptr, &Latency::i2cHz},
};
// Keys that name one element of an array member.
struct ArrayKey {
    const char* name;
    int* (*ref)(Profile&);
    int lo, hi;                       // legal range
};
#define SECT(NAME, IDX) {"align_" NAME, [](Profile& p) { return &p.sectionAlign[IDX]; }, -1, 64}
#define ALUK(NAME, FIELD, I, HI) {NAME, [](Profile& p) { return &p.alu.FIELD[I]; }, 0, HI}
const ArrayKey kArrayKeys[] = {
    SECT("main_move", PS_MAIN_MOVE), SECT("main_branch", PS_MAIN_BRANCH), SECT("main_csr1", PS_MAIN_CSR1), SECT("main_csrm", PS_MAIN_CSRM),
    SECT("main_alu", PS_MAIN_ALU), SECT("main_const", PS_MAIN_CONST), SECT("peri_xfer1", PS_PERI_XFER1), SECT("peri_xferm", PS_PERI_XFERM),
    SECT("peri_pub", PS_PERI_PUB), SECT("peri_branch", PS_PERI_BRANCH), SECT("peri_csr1", PS_PERI_CSR1), SECT("peri_devmap", PS_PERI_DEVMAP),
    ALUK("alu_arith_add", arith, 0, 31), ALUK("alu_arith_sub", arith, 1, 31), ALUK("alu_arith_mul", arith, 2, 31), ALUK("alu_arith_inc", arith, 3, 31),
    ALUK("alu_arith_dec", arith, 4, 31),
    ALUK("alu_cmp_eq", cmp, 0, 3), ALUK("alu_cmp_ne", cmp, 1, 3), ALUK("alu_cmp_lt", cmp, 2, 3), ALUK("alu_cmp_ge", cmp, 3, 3),
    ALUK("alu_logic_and", logic, 0, 7), ALUK("alu_logic_or", logic, 1, 7), ALUK("alu_logic_xor", logic, 2, 7), ALUK("alu_logic_not", logic, 3, 7),
};
#undef SECT
#undef ALUK

// Value ranges of the scalar keys that have a hardware meaning (everything else is range-checked where it is used).
struct Range { const char* name; long lo, hi; };
const Range kRanges[] = {
    {"event_count", 1, 64},
    {"main_pc_width", 8, 16}, {"peri_pc_width", 8, 16}, {"layout_offset_bits", 8, 32}, {"device_map_entry_bytes", 1, 4},
    {"device_map_bus_shift", 0, 31}, {"device_map_addr_shift", 0, 31}, {"boot_pad_byte", 0, 255}, {"sram_depth_words", 1, 1 << 24},
    {"main_image_base_word", 0, 1 << 24}, {"peri_image_base_word", 0, 1 << 24}, {"main_capacity_bytes", 1, 1 << 26}, {"peri_capacity_bytes", 1, 1 << 26},
};

bool aluCodesDistinct(const Profile& p, std::string& what) {
    auto distinct = [&](const int* v, int n, const char* group) {
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (v[i] == v[j]) { what = std::string(group) + " sub-operations share the numeric code " + std::to_string(v[i]); return false; }
        return true;
    };
    return distinct(p.alu.arith, 5, "arith") && distinct(p.alu.cmp, 4, "cmp") && distinct(p.alu.logic, 4, "logic");
}
}  // namespace

const char* const* layoutFieldNames(Domain d) {
    static const char* const mainN[6] = {"main_move_desc_offset", "main_branch_desc_offset", "main_csr_onebyte_desc_offset", "main_csr_multibyte_desc_offset", "main_alu_desc_offset", "main_const_pool_offset"};
    static const char* const periN[6] = {"peri_xfer_onebyte_desc_offset", "peri_xfer_multibyte_desc_offset", "peri_publish_desc_offset", "peri_branch_desc_offset", "peri_csr_onebyte_desc_offset", "device_map_offset"};
    return d == Domain::Main ? mainN : periN;
}

bool Profile::load(const std::string& text, std::vector<std::string>& errors) {
    std::istringstream in(text);
    std::string line;
    int n = 0;
    bool ok = true;
    while (std::getline(in, line)) {
        n++;
        line = trim(stripComment(line));
        if (line.empty() || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) { errors.push_back("profile line " + std::to_string(n) + ": 'key = value' expected"); ok = false; continue; }
        std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
        if (k == "profile_id") { profileId = v; continue; }
        char* end = nullptr;
        long val = std::strtol(v.c_str(), &end, 0);
        if (v == "true" || v == "yes" || v == "on") val = 1, end = (char*)v.c_str() + v.size();
        if (v == "false" || v == "no" || v == "off") val = 0, end = (char*)v.c_str() + v.size();
        const bool numeric = !v.empty() && !*end;
        const std::string where = "profile line " + std::to_string(n) + ": ";
        if (k == "layout_offset_bytes") {                         // the older spelling of layout_offset_bits
            if (!numeric || val < 1 || val > 4) { errors.push_back(where + "layout_offset_bytes is 1..4 (prefer layout_offset_bits)"); ok = false; continue; }
            layoutOffsetBits = (int)val * 8;
            continue;
        }
        const ArrayKey* ak = nullptr;
        for (const ArrayKey& a : kArrayKeys) if (k == a.name) ak = &a;
        if (ak) {
            if (!numeric || val < ak->lo || val > ak->hi) { errors.push_back(where + "'" + k + "' must be a number from " + std::to_string(ak->lo) + " to " + std::to_string(ak->hi)); ok = false; continue; }
            *ak->ref(*this) = (int)val;
            continue;
        }
        const Key* hit = nullptr;
        for (const Key& kk : kKeys) if (k == kk.name) hit = &kk;
        if (!hit) { errors.push_back(where + "unknown key '" + k + "' (docs/ASM_SYNTAX.md lists the profile keys)"); ok = false; continue; }
        if (!numeric) { errors.push_back(where + "number expected for '" + k + "'"); ok = false; continue; }
        bool bad = false;
        for (const Range& r : kRanges) if (k == r.name && (val < r.lo || val > r.hi)) { errors.push_back(where + "'" + k + "' must be from " + std::to_string(r.lo) + " to " + std::to_string(r.hi)); ok = false; bad = true; }
        if (bad) continue;
        if (hit->ip) this->*hit->ip = (int)val;
        else if (hit->bp) this->*hit->bp = val != 0;
        else latency.*hit->lp = (int)val;
    }
    std::string what;
    if (!aluCodesDistinct(*this, what)) { errors.push_back("profile: " + what + " (every sub-operation needs its own code)"); ok = false; }
    return ok;
}

std::string Profile::dump() const {
    std::ostringstream o;
    o << "profile_id = " << profileId << "\n";
    for (const Key& kk : kKeys) {
        o << kk.name << " = ";
        if (kk.ip) o << this->*kk.ip;
        else if (kk.bp) o << (this->*kk.bp ? "true" : "false");
        else o << latency.*kk.lp;
        o << "\n";
    }
    Profile copy = *this;                                           // the array members (the ref functions take a non-const Profile)
    for (const ArrayKey& a : kArrayKeys) o << a.name << " = " << *a.ref(copy) << "\n";
    return o.str();
}

}  // namespace asmcore
