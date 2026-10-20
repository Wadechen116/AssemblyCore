#include "asmcore/sram.h"

#include <algorithm>

namespace asmcore {

PhysicalByte physicalByte(const Profile& pf, Domain d, uint32_t logicalByte) {
    PhysicalByte p;
    p.word = (uint32_t)(d == Domain::Main ? pf.mainImageBaseWord : pf.periImageBaseWord) + (logicalByte >> 2);
    p.lane = (int)(logicalByte & 3);
    return p;
}

std::vector<WordWrite> sramWrites(const Result& r) {
    std::vector<WordWrite> v;
    for (const DomainImage* im : {&r.main, &r.peri}) {
        if (!im->present) continue;
        for (size_t i = 0; i < im->words.size(); i++) v.push_back(WordWrite{im->baseWord + (uint32_t)i, im->words[i], im->domain});
    }
    std::sort(v.begin(), v.end(), [](const WordWrite& a, const WordWrite& b) { return a.address < b.address; });
    return v;
}

SramContents sramContents(const std::vector<WordWrite>& writes) {
    SramContents m;
    for (const WordWrite& w : writes) m[w.address] = w.data;
    return m;
}

bool readThroughSram(const SramContents& sram, const Profile& pf, Domain d, uint32_t byteAddr, uint32_t n, std::vector<uint8_t>& out) {
    out.clear();
    for (uint32_t i = 0; i < n; i++) {
        PhysicalByte p = physicalByte(pf, d, byteAddr + i);
        auto it = sram.find(p.word);
        if (it == sram.end()) return false;
        out.push_back((uint8_t)(it->second >> (8 * p.lane)));
    }
    return true;
}

bool imageFromSram(const SramContents& sram, const Profile& pf, Domain d, uint32_t length, std::vector<uint8_t>& out) {
    return readThroughSram(sram, pf, d, 0, length, out);
}

MemoryProof memoryProof(const Result& r, const Profile& pf) {
    MemoryProof p;
    p.depthWords = (uint32_t)pf.physicalSramDepthWords;
    auto fill = [&](MemoryProof::Window& w, const DomainImage& im, const char* name, int base, int cap) {
        w.domain = name;
        w.baseWord = (uint32_t)base;
        w.occupiedWords = im.present ? (uint32_t)im.words.size() : 0;
        w.reservedWords = (uint32_t)((cap + 3) / 4);
        w.logicalBytes = im.present ? (uint32_t)im.bytes.size() : 0;
        w.capacityBytes = (uint32_t)cap;
    };
    fill(p.main, r.main, "Main", pf.mainImageBaseWord, pf.mainCapacityBytes);
    fill(p.peri, r.peri, "Peri", pf.periImageBaseWord, pf.periCapacityBytes);
    auto overlap = [](uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return an && bn && a < b + bn && b < a + an; };
    p.occupiedDisjoint = !overlap(p.main.baseWord, p.main.occupiedWords, p.peri.baseWord, p.peri.occupiedWords);
    p.reservedDisjoint = !overlap(p.main.baseWord, p.main.reservedWords, p.peri.baseWord, p.peri.reservedWords);
    p.insideDepth = (uint64_t)p.main.baseWord + p.main.reservedWords <= p.depthWords && (uint64_t)p.peri.baseWord + p.peri.reservedWords <= p.depthWords;
    p.withinCapacity = p.main.logicalBytes <= p.main.capacityBytes && p.peri.logicalBytes <= p.peri.capacityBytes;
    p.notes.push_back("occupied range = [base, base + ceil(logical bytes / 4)) words; reserved window = [base, base + ceil(capacity / 4)) words; both are word addresses");
    p.notes.push_back("the images are padded to whole words and never share a word (their windows are disjoint)");
    return p;
}

}  // namespace asmcore
