#pragma once
// Assembling in the background. A request carries the whole input by value; when it finishes, an immutable BuildSnapshot is posted back
// to the owner thread. Requests are coalesced (only the newest is built) and results of superseded requests are dropped, so typing
// quickly never shows a stale answer.
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "asmcore/index.h"
#include "dispatcher.h"
#include "signal.h"
#include "workspace.h"

namespace acide {

struct BuildSnapshot {
    uint64_t id = 0;                              // the request number
    uint64_t revision = 0;                        // the workspace revision it was built from
    asmcore::Result result;
    asmcore::Index index;
    std::unique_ptr<asmcore::SourceMap> map;      // refers to `result`: the snapshot must not be copied or moved
    std::map<std::string, std::vector<asmcore::Diagnostic>> diagsByFile;
    std::vector<std::string> notes;               // problems outside the source (a profile that does not load...)
    std::vector<std::string> sourcePaths;
    std::string prefix;
    double milliseconds = 0;

    BuildSnapshot() = default;
    BuildSnapshot(const BuildSnapshot&) = delete;
    BuildSnapshot& operator=(const BuildSnapshot&) = delete;
    const std::vector<asmcore::Diagnostic>& diagnosticsFor(const std::string& file) const;
};
using SnapshotPtr = std::shared_ptr<const BuildSnapshot>;

// Pure function: the build itself, usable without threads.
SnapshotPtr buildNow(const BuildInput& in, const asmcore::lang::Language& language);

class BuildService {
public:
    BuildService(std::shared_ptr<IDispatcher> dispatcher, std::shared_ptr<const asmcore::lang::Language> language);
    ~BuildService();
    BuildService(const BuildService&) = delete;
    BuildService& operator=(const BuildService&) = delete;

    uint64_t request(BuildInput in);              // returns the request number
    bool busy() const { return pending_.load() > 0; }
    SnapshotPtr latest() const;                   // the newest finished build (owner thread)

    Signal<SnapshotPtr> finished;                 // emitted on the owner thread
    Signal<> started;

private:
    void run();
    std::shared_ptr<IDispatcher> disp_;
    std::shared_ptr<const asmcore::lang::Language> lang_;
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool have_ = false;
    BuildInput next_;
    uint64_t nextId_ = 0, issued_ = 0;
    std::atomic<int> pending_{0};
    SnapshotPtr latest_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}  // namespace acide
