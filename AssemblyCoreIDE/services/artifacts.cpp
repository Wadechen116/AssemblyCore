#include "artifacts.h"

namespace acide {

std::vector<std::string> writeArtifacts(IFileSystem& fs, const std::string& prefix, const asmcore::Result& r, const ArtifactOptions& opt, std::string& error) {
    std::vector<std::string> written;
    error.clear();
    auto put = [&](const std::string& path, const std::string& data) {
        if (!error.empty()) return;
        if (fs.write(path, data)) written.push_back(path);
        else error = path;
    };
    if (!r.ok) { error = "the build has errors: nothing written"; return written; }
    for (asmcore::Domain d : {asmcore::Domain::Main, asmcore::Domain::Peri}) {
        const asmcore::DomainImage& im = r.image(d);
        if (!im.present) continue;
        std::string base = prefix + (d == asmcore::Domain::Main ? ".main" : ".peri");
        put(base + ".bin", std::string(im.bytes.begin(), im.bytes.end()));
        put(base + ".layout.json", r.layoutJson(d));
        if (opt.hex) put(base + ".hex", r.intelHex(d));
    }
    if (opt.listing) put(prefix + ".lst", r.listing());
    if (opt.map) put(prefix + ".map", r.map());
    if (opt.debug) put(prefix + ".dbg.json", r.debugJson());
    if (opt.v31) {
        put(prefix + ".sram.hex", r.sramWordsHex());
        put(prefix + ".boot.json", r.bootPlanJson());
        put(prefix + ".memory.json", r.memoryProofJson());
        put(prefix + ".sections.json", r.sectionMapJson());
        put(prefix + ".descriptors.json", r.descriptorMapJson());
        put(prefix + ".services.json", r.servicesJson());
        put(prefix + ".events.json", r.eventVectorsJson());
        put(prefix + ".validation.json", r.validationReportJson());
        if (!opt.sources.empty()) put(prefix + ".manifest.json", r.buildManifestJson(opt.sources));
    }
    return written;
}

}  // namespace acide
