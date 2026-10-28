#pragma once
// The simulator's hardware parameters as the IDE holds them: a working set of values (what the parameter panel edits), the file they belong to (a
// .simparams file, listed in the project as `simparams = ...`) and whether they differ from it. The simulator and the comparison runs take their values
// from here. No Qt.
#include <string>

#include "asmcore/sim_params.h"
#include "signal.h"
#include "workspace.h"

namespace acide {

class SimParamsStore {
public:
    explicit SimParamsStore(Workspace& ws) : ws_(ws) {}

    const asmcore::SimParams& params() const { return p_; }
    bool set(const std::string& key, const std::string& text, std::string& error);
    void clear(const std::string& key);                       // back to the initial value
    void clearAll();
    bool setStatus(const std::string& key, const std::string& status, std::string& error);
    void setNote(const std::string& key, const std::string& note);
    bool dirty() const { return dirty_; }                     // the values differ from the file (or there is no file and something is set)

    // ---- the file
    const std::string& path() const { return path_; }         // absolute; "" = the values belong to no file
    // Reads a .simparams file (an open buffer first, else the disk). The values are replaced; `errors` holds the lines that could not be used.
    bool load(const std::string& absPath, std::vector<std::string>& errors);
    bool save(std::string& error);                            // to path()
    // Writes to `absPath`, makes it the file of these values and lists it in the project (`simparams = ...`, saved) when it lies in the project's folder.
    bool saveAs(const std::string& absPath, std::string& error);
    void closeFile();                                         // forget the file (the values stay)
    // After the project opened or changed: the file the project names is loaded (once); a project without one lets go of the file.
    void syncWithProject();

    Signal<> changed;                                // a value, the status, a note, the file or the dirty flag changed

private:
    void touch();
    Workspace& ws_;
    asmcore::SimParams p_;
    std::string path_;
    bool dirty_ = false;
};

}  // namespace acide
