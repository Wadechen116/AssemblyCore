#pragma once
// The open project and its text buffers. Owns the truth about what the user is editing (including text not yet saved) and hands out
// immutable snapshots for the build service, so a build never sees a half-edited state.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "asmcore/asmcore.h"
#include "fs.h"
#include "project.h"
#include "signal.h"

namespace acide {

struct Document {
    std::string path;                 // absolute, '/'-separated
    std::string text;
    int version = 0;                  // bumped on every change
    bool dirty = false;               // differs from the file on disk
    long long diskStamp = -1;         // IFileSystem::modified when read / saved
};

// Everything a build needs, by value: safe to hand to another thread.
struct BuildInput {
    std::vector<asmcore::Source> sources;                   // the project sources, buffers first
    std::map<std::string, std::string> overlay;             // path -> text of every open buffer (for .include)
    std::vector<std::string> includeDirs;                   // absolute
    std::shared_ptr<IFileSystem> fs;
    asmcore::Options options;
    std::string profilePath;
    std::string prefix;                                     // output path prefix (absolute)
    uint64_t revision = 0;                                  // the workspace revision this was taken at
};

class Workspace {
public:
    explicit Workspace(std::shared_ptr<IFileSystem> fs);

    // ---- project
    bool openProject(const std::string& file, std::vector<std::string>& errors);
    void newProject(const std::string& file, const std::string& name);
    bool saveProject(std::string& error);
    Project& project() { return project_; }
    const Project& project() const { return project_; }
    void projectEdited();                                    // call after changing project() so listeners learn of it
    bool hasProject() const { return !project_.file.empty(); }

    // ---- documents
    Document* open(const std::string& path);                 // reads the file if not open yet; nullptr if it cannot be read
    Document* find(const std::string& path);
    void setText(const std::string& path, const std::string& text);
    bool save(const std::string& path, std::string& error);
    bool saveAll(std::string& error);
    void close(const std::string& path);
    std::vector<std::string> openPaths() const;
    std::vector<std::string> externallyChanged();           // open, clean documents whose file changed on disk
    bool reload(const std::string& path);                    // discard the buffer, read the file again
    void keepBuffer(const std::string& path);                // the file changed on disk but the buffer wins: stop reporting the change
    bool anyDirty() const;

    // ---- builds
    BuildInput snapshot() const;
    uint64_t revision() const { return revision_; }

    std::shared_ptr<IFileSystem> fileSystem() const { return fs_; }

    Signal<const std::string&> documentChanged;              // path (text or dirty flag changed)
    Signal<const std::string&> documentOpened;
    Signal<const std::string&> documentClosed;
    Signal<> projectChanged;

private:
    std::shared_ptr<IFileSystem> fs_;
    Project project_;
    std::map<std::string, Document> docs_;
    uint64_t revision_ = 0;
};

}  // namespace acide
