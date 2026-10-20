#pragma once
// The shared program SRAM (spec v31 3.1): ONE 32-bit synchronous SRAM holds the logical images of both domains.
//
// Reference functions for the address arithmetic the Frontend does, so the assembler, the tests and an independent checker agree on it:
//     logical_word_addr  = logical_byte_addr >> 2
//     lane               = logical_byte_addr & 3                      (lane k is word bits [8k+7:8k])
//     physical_word_addr = IMAGE_BASE_WORD[domain] + logical_word_addr
// A fetch of N bytes starting anywhere may cross a word boundary and then needs both words.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"

namespace asmcore {

struct PhysicalByte {
    uint32_t word = 0;       // physical word address
    int lane = 0;            // 0..3
};
PhysicalByte physicalByte(const Profile& pf, Domain d, uint32_t logicalByte);

struct WordWrite {
    uint32_t address = 0;    // physical word address
    uint32_t data = 0;
    Domain domain = Domain::Main;
};
// The boot writes of both images, sorted by address (padding words included, nothing between the windows).
std::vector<WordWrite> sramWrites(const Result& r);

// What the SRAM holds after those writes: word address -> data.
typedef std::map<uint32_t, uint32_t> SramContents;
SramContents sramContents(const std::vector<WordWrite>& writes);

// Bytes [byteAddr, byteAddr + n) of a domain's logical image, read the way the Frontend does it: word by word through the shared SRAM, each
// byte from its lane. False if a needed word was never written.
bool readThroughSram(const SramContents& sram, const Profile& pf, Domain d, uint32_t byteAddr, uint32_t n, std::vector<uint8_t>& out);

// The whole logical image (`length` bytes) reconstructed from the SRAM: what an independent disassembler works from.
bool imageFromSram(const SramContents& sram, const Profile& pf, Domain d, uint32_t length, std::vector<uint8_t>& out);

// Physical placement report (the contents of memory_proof.json): occupied ranges, reserved windows, and every rule that was checked.
struct MemoryProof {
    struct Window {
        std::string domain;
        uint32_t baseWord = 0, occupiedWords = 0, reservedWords = 0;
        uint32_t logicalBytes = 0, capacityBytes = 0;
    };
    Window main, peri;
    uint32_t depthWords = 0;
    bool occupiedDisjoint = false, reservedDisjoint = false, insideDepth = false, withinCapacity = false, wordAligned = true;
    bool ok() const { return occupiedDisjoint && reservedDisjoint && insideDepth && withinCapacity && wordAligned; }
    std::vector<std::string> notes;
};
MemoryProof memoryProof(const Result& r, const Profile& pf);

}  // namespace asmcore
