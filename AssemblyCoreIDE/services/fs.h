#pragma once
// All file access of the services goes through this interface. Paths use '/' as the separator everywhere inside the IDE.
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acide {

class IFileSystem {
public:
    virtual ~IFileSystem() {}
    virtual bool read(const std::string& path, std::string& out) = 0;
    virtual bool write(const std::string& path, const std::string& text) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual std::vector<std::string> list(const std::string& dir) = 0;      // names (not paths) of the entries of a directory
    virtual long long modified(const std::string& path) = 0;               // an opaque, increasing stamp; -1 when absent
};

class RealFileSystem : public IFileSystem {
public:
    bool read(const std::string& path, std::string& out) override;
    bool write(const std::string& path, const std::string& text) override;
    bool exists(const std::string& path) override;
    std::vector<std::string> list(const std::string& dir) override;
    long long modified(const std::string& path) override;
};

class MemoryFileSystem : public IFileSystem {
public:
    bool read(const std::string& path, std::string& out) override;
    bool write(const std::string& path, const std::string& text) override;
    bool exists(const std::string& path) override;
    std::vector<std::string> list(const std::string& dir) override;
    long long modified(const std::string& path) override;
private:
    std::mutex m_;
    std::map<std::string, std::pair<std::string, long long>> files_;
    long long clock_ = 0;
};

// Path helpers ('/'-separated, no dependency on the host).
std::string dirName(const std::string& path);
std::string baseName(const std::string& path);
std::string joinPath(const std::string& dir, const std::string& rel);       // collapses "." and ".."; an absolute `rel` wins
std::string relativeTo(const std::string& base, const std::string& path);   // path relative to the directory base (or path itself)

}  // namespace acide
