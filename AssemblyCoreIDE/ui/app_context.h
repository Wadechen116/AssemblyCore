#pragma once
// What every UI component can reach. Components talk to each other only through the signals here, never directly, so any of them can be
// replaced or removed.
#include <memory>
#include <string>
#include <vector>

#include <QString>

#include "build.h"
#include "debugger.h"
#include "settings.h"
#include "workspace.h"

namespace acide {

struct Location {
    std::string file;
    int line = 1;                    // 1-based
    int col = 0, endCol = 0;         // bytes, 0-based, [col, endCol)
};

struct AppContext {
    AppContext(Workspace& w, BuildService& b, Settings& s, Debugger& d, std::shared_ptr<const asmcore::lang::Language> l)
        : workspace(w), build(b), settings(s), debugger(d), language(std::move(l)) {}
    Workspace& workspace;
    BuildService& build;
    Settings& settings;
    Debugger& debugger;                                // the simulator session is reached through it (debugger.session())
    std::shared_ptr<const asmcore::lang::Language> language;

    SnapshotPtr snapshot;                              // the newest finished build (may be null before the first one)

    Signal<SnapshotPtr> snapshotChanged;               // a build finished
    Signal<const Location&> goTo;                      // show this place in an editor
    Signal<const std::string&, int> cursorMoved;       // an editor's caret: file, line (for views that follow the caret)
    Signal<const QString&, const std::vector<Location>&> showReferences;    // title, places
    Signal<const QString&> status;                     // a message for the status bar
    Signal<const QString&> log;                        // a line for the output panel
    Signal<> requestBuild;                             // someone wants a build now
    Signal<const std::string&> languageRequested;      // the user picked a language ("auto", "en", "zh_TW"); the shell rebuilds the window
};

}  // namespace acide
