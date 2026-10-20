#include "image_view.h"

#include <algorithm>

namespace asmcore {

bool ImageView::load(const DomainImage& im, const Profile& pf, std::string& error) {
    present = im.present;
    if (!present) return true;
    bytes = im.bytes;
    big = pf.bigEndian;
    // ImageLayout is sideband (spec v31 3.1): it is not part of the image bytes. Without it nothing can be located.
    if (im.layout.size() != 6) {
        error = std::string(domainName(im.domain)) + " image has no ImageLayout: it is delivered next to the image (assembler output PREFIX.main.layout.json / PREFIX.peri.layout.json), not inside it";
        return false;
    }
    for (int i = 0; i < 6; i++) off[i] = im.layout[(size_t)i];
    const uint32_t tail = (uint32_t)bytes.size();
    static const char* mainN[6] = {"MainMoveDesc_32b", "MainBranchDesc_16b", "MainCsrOneByteDesc_16b", "MainCsrMultiByteDesc_16b", "MainAluDesc_8b", "MAIN_CONST_POOL"};
    static const char* periN[6] = {"PeriXferOneByteDesc_16b", "PeriXferMultiByteDesc_16b", "PeriPublishDesc_16b", "PeriBranchDesc_16b", "PeriCsrOneByteDesc_16b", "DeviceMap"};
    for (int i = 0; i < 6; i++) {
        const char* want = im.domain == Domain::Main ? mainN[i] : periN[i];
        end[i] = off[i];
        bool found = false;
        for (const SectionInfo& s : im.sections) if (s.name == want) { end[i] = s.offset + s.size; found = true; if (s.offset != off[i]) { error = std::string("ImageLayout field for ") + want + " disagrees with the section list"; return false; } }
        // Without a section list (a bare binary plus its layout) a table ends where the next one starts (section padding, if any, is
        // counted with the table before it); the last one ends with the image.
        if (!found) end[i] = i < 5 ? off[i + 1] : tail;
    }
    // The layout must describe tables that lie inside the image. A damaged or mis-parsed layout fails here instead of producing tables of absurd size.
    for (int i = 0; i < 6; i++) {
        if (off[i] > end[i] || end[i] > tail) {
            error = std::string(domainName(im.domain)) + " ImageLayout field " + std::to_string(i) + " (offset " + std::to_string(off[i]) + ") does not describe a table inside the image";
            return false;
        }
    }
    uint32_t lim = tail;
    for (int i = 0; i < 6; i++) lim = std::min(lim, off[i]);
    codeLimit = im.codeSize ? im.codeSize : lim;
    return true;
}

bool ImageView::entry(int t, uint32_t idx, int entryBytes, uint32_t& v) const {
    uint64_t pos = (uint64_t)off[t] + (uint64_t)idx * entryBytes;
    if (pos + entryBytes > end[t] || pos + entryBytes > bytes.size()) return false;
    v = 0;
    for (int k = 0; k < entryBytes; k++) {
        int sh = big ? 8 * (entryBytes - 1 - k) : 8 * k;
        v |= (uint32_t)bytes[pos + k] << sh;
    }
    return true;
}

}  // namespace asmcore
