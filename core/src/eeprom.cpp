// The EEPROM image (include/asmcore/eeprom.h): placing the six regions, the map file, back to the logical images, Intel HEX.
#include "asmcore/eeprom.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "json_lite.h"

namespace asmcore {

namespace {
const char* const kNames[ER_COUNT] = {"main.code", "main.desc", "main.const", "peri.code", "peri.desc", "peri.devmap"};
const char* const kRegs[ER_COUNT] = {"main_code_addr", "main_desc_addr", "main_const_addr", "peri_code_addr", "peri_desc_addr", "peri_devmap_addr"};

// The three slices of one domain's logical image: the code (with any padding before the first table), the descriptor tables, the last section
// (const pool / DeviceMap). The tables are in the canonical order, so the first one starts at layout[0] and the last section at layout[5].
void slices(const DomainImage& im, EepromRegion out[3]) {
    for (int k = 0; k < 3; k++) out[k] = EepromRegion();
    if (!im.present || im.layout.size() != 6) return;
    const uint32_t n = (uint32_t)im.bytes.size();
    const uint32_t a = std::min(im.layout[0], n), b = std::min(std::max(im.layout[5], a), n);
    out[0].logicalStart = 0; out[0].size = a;
    out[1].logicalStart = a; out[1].size = b - a;
    out[2].logicalStart = b; out[2].size = n - b;
}
}  // namespace

const char* eepromRegionName(int id) { return id >= 0 && id < ER_COUNT ? kNames[id] : "?"; }
const char* eepromRegisterName(int id) { return id >= 0 && id < ER_COUNT ? kRegs[id] : "?"; }

uint32_t EepromMap::addressOf(int domain, uint32_t o, const uint32_t base[ER_COUNT]) const {
    const int first = domain == 0 ? ER_MAIN_CODE : ER_PERI_CODE;
    int k = first;
    for (int i = first + 2; i > first; i--) if (o >= region[i].logicalStart && region[i].size) { k = i; break; }
    const uint32_t a = base[k] + (o - region[k].logicalStart);
    return capacity ? a % capacity : a;                                  // a serial EEPROM wraps its address counter
}

bool buildEeprom(const Result& r, const Profile& pf, std::vector<uint8_t>& image, EepromMap& map, std::string& error) {
    map = EepromMap();
    map.capacity = (uint32_t)pf.eepromCapacity;
    map.devAddr = pf.eepromDevAddr;
    map.addrBytes = pf.eepromAddrBytes;
    map.fill = pf.eepromFill;
    map.profileId = pf.profileId;
    const DomainImage* ims[2] = {&r.main, &r.peri};
    for (int d = 0; d < 2; d++) {
        const DomainImage& im = *ims[d];
        map.present[d] = im.present;
        map.layout[d] = im.layout;
        map.codeSize[d] = im.codeSize;
        map.imageBytes[d] = (uint32_t)im.bytes.size();
        EepromRegion s[3];
        slices(im, s);
        for (int k = 0; k < 3; k++) map.region[d * 3 + k] = s[k];
    }
    // the addresses: given by the profile, or right after the region before it
    uint32_t next = 0;
    for (int i = 0; i < ER_COUNT; i++) {
        EepromRegion& g = map.region[i];
        if (pf.eepromAddr[i] >= 0) g.addr = (uint32_t)pf.eepromAddr[i];
        else g.addr = next;
        next = g.addr + g.size;
        if ((uint64_t)g.addr + g.size > map.capacity) {
            error = std::string("EEPROM region ") + kNames[i] + " (" + std::to_string(g.size) + " bytes at " + std::to_string(g.addr) + ") does not fit the " +
                    std::to_string(map.capacity) + "-byte EEPROM (profile eeprom_capacity / eeprom_" + std::string(kRegs[i]) + ")";
            return false;
        }
    }
    for (int i = 0; i < ER_COUNT; i++)
        for (int j = i + 1; j < ER_COUNT; j++) {
            const EepromRegion &a = map.region[i], &b = map.region[j];
            if (!a.size || !b.size) continue;
            if (a.addr < b.addr + b.size && b.addr < a.addr + a.size) {
                error = std::string("EEPROM regions ") + kNames[i] + " and " + kNames[j] + " overlap (profile eeprom_" + kRegs[i] + " / eeprom_" + kRegs[j] + ")";
                return false;
            }
        }
    image.assign(map.capacity, (uint8_t)map.fill);
    for (int d = 0; d < 2; d++)
        for (int k = 0; k < 3; k++) {
            const EepromRegion& g = map.region[d * 3 + k];
            for (uint32_t i = 0; i < g.size; i++) image[g.addr + i] = ims[d]->bytes[g.logicalStart + i];
        }
    // the image ends after the last byte used (an EEPROM programmer writes only that much; the rest stays erased)
    uint32_t used = 0;
    for (int i = 0; i < ER_COUNT; i++) if (map.region[i].size) used = std::max(used, map.region[i].addr + map.region[i].size);
    image.resize(used);
    return true;
}

bool domainImagesFromEeprom(const std::vector<uint8_t>& image, const EepromMap& map, DomainImage& main, DomainImage& peri, std::string& error) {
    DomainImage* out[2] = {&main, &peri};
    for (int d = 0; d < 2; d++) {
        DomainImage& im = *out[d];
        im = DomainImage();
        im.domain = d == 0 ? Domain::Main : Domain::Peri;
        im.present = map.present[d];
        if (!im.present) continue;
        if (map.layout[d].size() != 6) { error = std::string("the EEPROM map has no ImageLayout for ") + (d == 0 ? "Main" : "Peri"); return false; }
        im.layout = map.layout[d];
        im.codeSize = map.codeSize[d];
        im.bytes.assign(map.imageBytes[d], (uint8_t)map.fill);
        for (int k = 0; k < 3; k++) {
            const EepromRegion& g = map.region[d * 3 + k];
            for (uint32_t i = 0; i < g.size; i++) {
                const uint32_t a = g.addr + i;
                if (g.logicalStart + i < im.bytes.size()) im.bytes[g.logicalStart + i] = a < image.size() ? image[a] : (uint8_t)map.fill;
            }
        }
    }
    return true;
}

std::string EepromMap::json() const {
    std::ostringstream o;
    o << "{\n \"format\":\"asmcore-eeprom-1\",\n \"profile\":\"" << json::escape(profileId) << "\",\n \"capacity\":" << capacity << ",\n \"i2c_address\":" << devAddr
      << ",\n \"address_bytes\":" << addrBytes << ",\n \"erased\":" << fill << ",\n \"regions\":[";
    for (int i = 0; i < ER_COUNT; i++)
        o << (i ? "," : "") << "\n  {\"name\":\"" << kNames[i] << "\",\"register\":\"" << kRegs[i] << "\",\"address\":" << region[i].addr << ",\"logical_start\":"
          << region[i].logicalStart << ",\"bytes\":" << region[i].size << "}";
    o << "\n ],\n \"domains\":[";
    for (int d = 0; d < 2; d++) {
        o << (d ? "," : "") << "\n  {\"domain\":\"" << (d == 0 ? "main" : "peri") << "\",\"present\":" << (present[d] ? "true" : "false") << ",\"code_bytes\":" << codeSize[d]
          << ",\"logical_image_bytes\":" << imageBytes[d] << ",\"image_layout\":[";
        for (size_t i = 0; i < layout[d].size(); i++) o << (i ? "," : "") << layout[d][i];
        o << "]}";
    }
    o << "\n ]\n}\n";
    return o.str();
}

bool loadEepromMapJson(const std::string& text, EepromMap& map, std::string& error) {
    json::Value v;
    json::Parser p(text);
    if (!p.parse(v)) { error = "EEPROM map: " + p.error(); return false; }
    auto num = [&](const json::Value* x, double fallback) { return x && x->kind == json::Value::Number ? x->num : fallback; };
    const json::Value* f = v.get("format");
    if (!f || f->str != "asmcore-eeprom-1") { error = "EEPROM map: not an asmcore EEPROM map (\"format\":\"asmcore-eeprom-1\")"; return false; }
    map = EepromMap();
    map.capacity = (uint32_t)num(v.get("capacity"), 65536);
    map.devAddr = (int)num(v.get("i2c_address"), 0x50);
    map.addrBytes = (int)num(v.get("address_bytes"), 2);
    map.fill = (int)num(v.get("erased"), 0xFF);
    if (const json::Value* pr = v.get("profile")) map.profileId = pr->str;
    const json::Value* rs = v.get("regions");
    if (!rs || rs->kind != json::Value::Array) { error = "EEPROM map: no \"regions\""; return false; }
    for (const json::Value& g : rs->arr) {
        const json::Value* n = g.get("name");
        int id = -1;
        for (int i = 0; i < ER_COUNT; i++) if (n && n->str == kNames[i]) id = i;
        if (id < 0) { error = "EEPROM map: unknown region " + (n ? n->str : std::string("(no name)")); return false; }
        map.region[id].addr = (uint32_t)num(g.get("address"), 0);
        map.region[id].logicalStart = (uint32_t)num(g.get("logical_start"), 0);
        map.region[id].size = (uint32_t)num(g.get("bytes"), 0);
    }
    const json::Value* ds = v.get("domains");
    if (ds && ds->kind == json::Value::Array)
        for (const json::Value& d : ds->arr) {
            const json::Value* n = d.get("domain");
            const int k = n && n->str == "peri" ? 1 : 0;
            const json::Value* pr = d.get("present");
            map.present[k] = pr && pr->kind == json::Value::Bool && pr->b;
            map.codeSize[k] = (uint32_t)num(d.get("code_bytes"), 0);
            map.imageBytes[k] = (uint32_t)num(d.get("logical_image_bytes"), 0);
            map.layout[k].clear();
            if (const json::Value* l = d.get("image_layout")) for (const json::Value& x : l->arr) map.layout[k].push_back((uint32_t)x.num);
        }
    return true;
}

std::string intelHex(const std::vector<uint8_t>& image) {
    std::string out;
    char line[80];
    auto record = [&](int type, uint32_t addr, const uint8_t* data, size_t n) {
        unsigned sum = (unsigned)n + ((addr >> 8) & 0xFF) + (addr & 0xFF) + (unsigned)type;
        std::snprintf(line, sizeof line, ":%02X%04X%02X", (unsigned)n, (unsigned)(addr & 0xFFFF), (unsigned)type);
        out += line;
        for (size_t i = 0; i < n; i++) { std::snprintf(line, sizeof line, "%02X", data[i]); out += line; sum += data[i]; }
        std::snprintf(line, sizeof line, "%02X\n", (unsigned)((0x100 - (sum & 0xFF)) & 0xFF));
        out += line;
    };
    uint32_t upper = 0;
    for (size_t a = 0; a < image.size(); a += 16) {
        if ((a >> 16) != upper) {
            upper = (uint32_t)(a >> 16);
            const uint8_t u[2] = {(uint8_t)(upper >> 8), (uint8_t)upper};
            record(4, 0, u, 2);
        }
        record(0, (uint32_t)a, image.data() + a, std::min<size_t>(16, image.size() - a));
    }
    record(1, 0, nullptr, 0);
    return out;
}

}  // namespace asmcore

namespace asmcore {

EepromSymbols eepromSymbols(const Result& r) {
    EepromSymbols s;
    for (const SymbolInfo& x : r.symbols) {
        if (x.kind == "label") {
            const size_t colon = x.name.find(':');
            if (colon == std::string::npos) continue;
            const int d = x.name.compare(0, 5, "peri:") == 0 ? 1 : 0;
            if (!s.labels[d].count((uint32_t)x.value)) s.labels[d][(uint32_t)x.value] = x.name.substr(colon + 1);
        } else if (x.kind == "csr") {
            if (!s.csrs.count((uint32_t)x.value)) s.csrs[(uint32_t)x.value] = x.name;
        } else if (x.kind == "service") {
            if (!s.services.count(x.value)) s.services[x.value] = x.name;
        } else if (x.kind == "devmap") {
            if (!s.devices.count(x.value)) s.devices[x.value] = x.name;
        } else if (x.kind == "const") {
            if (!s.consts.count((uint32_t)x.value)) s.consts[(uint32_t)x.value] = x.name;
        }
    }
    return s;
}

std::string EepromSymbols::json() const {
    std::ostringstream o;
    auto list = [&](const char* key, const char* what, const auto& m, bool last) {
        o << " \"" << key << "\":[";
        bool first = true;
        for (const auto& kv : m) { o << (first ? "" : ",") << "\n  {\"" << what << "\":" << kv.first << ",\"name\":\"" << json::escape(kv.second) << "\"}"; first = false; }
        o << (first ? "" : "\n ") << "]" << (last ? "\n" : ",\n");
    };
    o << "{\n \"format\":\"asmcore-eeprom-sym-1\",\n";
    list("main_labels", "pc", labels[0], false);
    list("peri_labels", "pc", labels[1], false);
    list("csrs", "address", csrs, false);
    list("services", "id", services, false);
    list("devices", "index", devices, false);
    list("consts", "offset", consts, true);
    o << "}\n";
    return o.str();
}

bool loadEepromSymbolsJson(const std::string& text, EepromSymbols& out, std::string& error) {
    json::Value v;
    json::Parser p(text);
    if (!p.parse(v)) { error = "symbol file: " + p.error(); return false; }
    const json::Value* f = v.get("format");
    if (!f || f->str != "asmcore-eeprom-sym-1") { error = "symbol file: not an asmcore EEPROM symbol file (\"format\":\"asmcore-eeprom-sym-1\")"; return false; }
    out = EepromSymbols();
    auto read = [&](const char* key, const char* what, auto& m) {
        const json::Value* a = v.get(key);
        if (!a || a->kind != json::Value::Array) return;
        for (const json::Value& e : a->arr) {
            const json::Value *k = e.get(what), *n = e.get("name");
            if (k && n && k->kind == json::Value::Number && n->kind == json::Value::String) m[(typename std::decay<decltype(m)>::type::key_type)k->num] = n->str;
        }
    };
    read("main_labels", "pc", out.labels[0]);
    read("peri_labels", "pc", out.labels[1]);
    read("csrs", "address", out.csrs);
    read("services", "id", out.services);
    read("devices", "index", out.devices);
    read("consts", "offset", out.consts);
    return true;
}

}  // namespace asmcore
