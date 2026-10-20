#include "build.h"

#include <chrono>

namespace acide {

namespace {
const std::vector<asmcore::Diagnostic> kNone;

// The assembler's way of finding an included file, over overlay (open buffers) first, then the file system.
asmcore::IncludeResolver makeResolver(const BuildInput& in) {
    return [&in](const std::string& from, const std::string& name, asmcore::Source& out) {
        std::vector<std::string> tries;
        tries.push_back(joinPath(dirName(from), name));
        for (const std::string& d : in.includeDirs) tries.push_back(joinPath(d, name));
        for (const std::string& p : tries) {
            auto it = in.overlay.find(p);
            if (it != in.overlay.end()) { out.name = p; out.text = it->second; return true; }
            if (in.fs && in.fs->read(p, out.text)) { out.name = p; return true; }
        }
        return false;
    };
}
}  // namespace

const std::vector<asmcore::Diagnostic>& BuildSnapshot::diagnosticsFor(const std::string& file) const {
    auto it = diagsByFile.find(file);
    return it == diagsByFile.end() ? kNone : it->second;
}

SnapshotPtr buildNow(const BuildInput& in, const asmcore::lang::Language& language) {
    auto t0 = std::chrono::steady_clock::now();
    auto snap = std::make_shared<BuildSnapshot>();
    snap->revision = in.revision;
    snap->prefix = in.prefix;
    asmcore::Options opt = in.options;
    if (!in.profilePath.empty()) {
        std::string text;
        std::vector<std::string> errs;
        if (!in.fs || !in.fs->read(in.profilePath, text)) snap->notes.push_back("cannot read the profile " + in.profilePath);
        else if (!opt.profile.load(text, errs)) for (auto& e : errs) snap->notes.push_back(in.profilePath + ": " + e);
    }
    asmcore::IncludeResolver res = makeResolver(in);
    for (auto& s : in.sources) snap->sourcePaths.push_back(s.name);
    snap->result = asmcore::assemble(in.sources, res, opt);
    snap->index = asmcore::Index::build(in.sources, language, res);
    snap->map.reset(new asmcore::SourceMap(snap->result));
    for (auto& d : snap->result.diags) snap->diagsByFile[d.loc.file].push_back(d);
    snap->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return snap;
}

BuildService::BuildService(std::shared_ptr<IDispatcher> d, std::shared_ptr<const asmcore::lang::Language> l) : disp_(std::move(d)), lang_(std::move(l)) {
    th_ = std::thread([this] { run(); });
}

BuildService::~BuildService() {
    *alive_ = false;
    {
        std::lock_guard<std::mutex> l(m_);
        stop_ = true;
    }
    cv_.notify_all();
    th_.join();
}

uint64_t BuildService::request(BuildInput in) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> l(m_);
        if (have_) pending_--;                               // the older, unstarted request is replaced
        next_ = std::move(in);
        have_ = true;
        id = ++nextId_;
        issued_ = id;
        pending_++;
    }
    cv_.notify_one();
    started.fire();
    return id;
}

SnapshotPtr BuildService::latest() const { return latest_; }

void BuildService::run() {
    for (;;) {
        BuildInput in;
        uint64_t id;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || have_; });
            if (stop_) return;
            in = std::move(next_);
            have_ = false;
            id = nextId_;
        }
        auto snap = std::const_pointer_cast<BuildSnapshot>(buildNow(in, *lang_));
        snap->id = id;
        std::weak_ptr<bool> alive = alive_;
        pending_--;
        SnapshotPtr done = snap;
        disp_->post([this, alive, done, id] {
            auto a = alive.lock();
            if (!a || !*a) return;
            if (id < issued_) return;                           // superseded while it was being built
            latest_ = done;
            finished.fire(done);
        });
    }
}

}  // namespace acide
