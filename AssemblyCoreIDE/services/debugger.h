#pragma once
// Debugging as the user thinks of it: source lines, a focused sequencer, "run / step / continue", "where am I". Translates between that and
// the simulator session, which only knows addresses.
//
//   * Line breakpoints are kept by (file, line) and turned into addresses with the SourceMap of the program being debugged. A line that an
//     inline CALL expanded at three places becomes three address breakpoints.
//   * The location of each sequencer is its pc mapped back to a source line.
//   * Breakpoints survive a rebuild: after a restart they are resolved again against the new program.
#include "sim_params_store.h"
#include <functional>
#include <set>

#include "settings.h"
#include "signal.h"
#include "sim_session.h"
#include "workspace.h"

namespace acide {

struct DebugLocation {
    bool valid = false;
    asmcore::Domain domain = asmcore::Domain::Main;
    uint32_t pc = 0;
    std::string file;
    int line = 0;
    std::vector<asmcore::Frame> frames;                   // the CALL chain an expanded instruction came from, innermost last
};

struct LineBreakpoint {
    std::string file;
    int line = 0;
    std::string condition;
    bool enabled = true;
    // filled in by the debugger
    bool resolved = false;                                // the line produces code in the program being debugged
    uint64_t hits = 0;
};

class Debugger {
public:
    // `latestBuild` gives the newest build (used to check that a line has code before any session exists).
    Debugger(SimSession& session, Workspace& workspace, Settings* settings, std::function<SnapshotPtr()> latestBuild);

    // ---- session
    // Starts debugging the newest build. Reads the environment file and the profile of the project. False (with `why`) if there is nothing
    // good to debug. The simulator starts asynchronously: wait for SimSession::loaded.
    bool start(std::string& why);
    // What a simulator for the newest build is made of: the program, the environment file text and the profile of the project (the parameters are
    // simParams().params()). False with `why` if there is nothing good to simulate. Used by start() and by the comparison runs.
    bool inputs(SnapshotPtr& program, std::string& env, asmcore::Profile& profile, std::string& why);
    SimParamsStore& simParams() { return params_; }
    void stop();
    bool active() const { return session_.state() != SimState::Empty && session_.program() != nullptr; }
    bool running() const { return session_.state() == SimState::Running; }
    // True when the sources changed since the program being debugged was built.
    bool stale() const;

    // ---- run control; stepping applies to the focused sequencer, running always runs both
    void setFocus(asmcore::Domain d) { focus_ = d; focusChanged.fire(d); }
    asmcore::Domain focus() const { return focus_; }
    void run(const RunSpec& spec = RunSpec());
    void pause() { session_.pause(); }
    void stepInto() { session_.step(focus_); }
    void stepOver() { session_.stepOver(focus_); }
    void stepOut() { session_.stepOut(focus_); }
    // Runs to the first address of the line (in the focused sequencer if the line has code in both). False if the line has no code.
    bool runToLine(const std::string& file, int line, std::string& why);
    void restart();                                        // rebuild is the caller's business: this resets the program being debugged

    // ---- line breakpoints
    // Toggles a breakpoint on the line, or on the next line that has code. Returns the line used, 0 if no line from there on has code.
    int toggleLineBreakpoint(const std::string& file, int line);
    bool hasLineBreakpoint(const std::string& file, int line) const;
    std::vector<LineBreakpoint> lineBreakpoints() const;   // with hit counts
    void setCondition(const std::string& file, int line, const std::string& condition);
    void setEnabled(const std::string& file, int line, bool enabled);
    void removeLineBreakpoint(const std::string& file, int line);
    void clearLineBreakpoints();
    // An edit inserted (delta > 0) or removed (delta < 0) lines before `fromLine` of `file`: breakpoints below move with the text.
    void linesShifted(const std::string& file, int fromLine, int delta);
    void saveBreakpoints();                                // to the settings, keyed by the project file
    void loadBreakpoints();

    // ---- data watches and expressions (kept across sessions)
    void setDataWatches(const std::vector<std::string>& targets) { dataWatches_ = targets; session_.setDataWatches(targets); }
    const std::vector<std::string>& dataWatches() const { return dataWatches_; }
    void setWatchExprs(const std::vector<std::string>& exprs) { watchExprs_ = exprs; session_.setWatchExprs(exprs); }
    const std::vector<std::string>& watchExprs() const { return watchExprs_; }

    // ---- where things are
    DebugLocation location(asmcore::Domain d) const;       // from the latest view
    DebugLocation locationOf(asmcore::Domain d, uint32_t pc) const;
    // Where the user should be taken after `stop`: the sequencer that caused it.
    DebugLocation stopLocation(const SimUpdate& u) const;
    SimSession& session() { return session_; }

    Signal<> breakpointsChanged;
    Signal<asmcore::Domain> focusChanged;
    Signal<const SimUpdate&> updated;                      // the session's updates, after the debugger has processed them
    Signal<const SimUpdate&, const DebugLocation&> stopped;   // a command finished: why, and where the user should look

private:
    SnapshotPtr programForMapping() const;
    std::vector<BreakpointSpec> specs(std::vector<size_t>* owner = nullptr) const;
    void pushBreakpoints();
    void onUpdate(const SimUpdate& u);

    SimParamsStore params_;
    SimSession& session_;
    Workspace& ws_;
    Settings* settings_;
    std::function<SnapshotPtr()> latest_;
    asmcore::Domain focus_ = asmcore::Domain::Main;
    std::vector<LineBreakpoint> bps_;
    std::vector<std::string> dataWatches_, watchExprs_;
    struct RunTo { bool active = false; std::string file; int line = 0; asmcore::Domain domain = asmcore::Domain::Main; uint32_t pc = 0; } runTo_;
    std::vector<BreakpointSpec> specList_;                 // what was last sent to the session
    std::vector<size_t> specOwner_;                        // spec index -> index into bps_ (to attribute hits)
    ConnectionBag conns_;
};

}  // namespace acide
