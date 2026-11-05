#pragma once
// The EEPROM image: what is burned into the I2C EEPROM that the two decoders fetch their instructions from.
//
// The hardware keeps SIX regions in the EEPROM, each at its own start address (held in registers: main_code_addr ... at run time):
//   main.code    Main executable bytecode (one byte per instruction, plus inline bytes)
//   main.desc    the five Main descriptor tables (MOV, BR, CSR one-byte, CSR multi-byte, ALU), in the order of the logical image
//   main.const   MAIN_CONST_POOL
//   peri.code    Peri executable bytecode
//   peri.desc    the five Peri descriptor tables (XFER one-byte, XFER multi-byte, PUB, BR, CSR one-byte)
//   peri.devmap  the DeviceMap
// A region is a contiguous slice of the domain's logical image (spec v31 3.1), so the ImageLayout offsets of the tables keep their meaning:
// logical offset o of a region lives at  region.addr + (o - region.logicalStart)  in the EEPROM.
//
// The addresses come from the profile (eeprom_*_addr; -1 = placed after the region before it). Standard C++17, no file access.
#include <cstdint>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace asmcore {

enum EepromRegionId { ER_MAIN_CODE = 0, ER_MAIN_DESC = 1, ER_MAIN_CONST = 2, ER_PERI_CODE = 3, ER_PERI_DESC = 4, ER_PERI_DEVMAP = 5, ER_COUNT = 6 };
const char* eepromRegionName(int id);                 // "main.code" ...
const char* eepromRegisterName(int id);               // the simulator signal that holds its start address: "main_code_addr" ...

struct EepromRegion {
    uint32_t addr = 0;                                // EEPROM byte address of the first byte
    uint32_t logicalStart = 0;                        // where the region starts in its domain's logical image
    uint32_t size = 0;                                // bytes
};

struct EepromMap {
    uint32_t capacity = 65536;                        // bytes of the EEPROM
    int devAddr = 0x50;                               // 7-bit I2C address
    int addrBytes = 2;                                // bytes of the memory address in a transaction
    int fill = 0xFF;                                  // what an erased byte reads as
    EepromRegion region[ER_COUNT];
    // what the decoders need besides the bytes: the ImageLayout of each domain (logical offsets, spec order), the code size and the logical size
    std::vector<uint32_t> layout[2];
    uint32_t codeSize[2] = {0, 0};
    uint32_t imageBytes[2] = {0, 0};
    bool present[2] = {false, false};
    std::string profileId;

    // The EEPROM address of logical offset `o` of a domain (0 Main, 1 Peri) when the regions start at `base` (the registers at run time).
    uint32_t addressOf(int domain, uint32_t o, const uint32_t base[ER_COUNT]) const;
    std::string json() const;                         // the map file next to the image (PREFIX.eeprom.json)
};
bool loadEepromMapJson(const std::string& text, EepromMap& map, std::string& error);

// Places both images of an assembler result into one EEPROM image (filled with the erased value, then the six regions). False with `error`
// if a region does not fit the EEPROM or two regions overlap.
bool buildEeprom(const Result& r, const Profile& pf, std::vector<uint8_t>& image, EepromMap& map, std::string& error);
// Back from an EEPROM image to the two logical images (the disassembler's and the simulator's input): what a decoder sees through the regions.
bool domainImagesFromEeprom(const std::vector<uint8_t>& image, const EepromMap& map, DomainImage& main, DomainImage& peri, std::string& error);
// Intel HEX of the image (what EEPROM programmers take), 16 data bytes a record, with an extended linear address record above 64 KiB.
std::string intelHex(const std::vector<uint8_t>& image);

}  // namespace asmcore
