#pragma once
// Minimal read-only ISO9660 reader for PSP UMD images.
#include "types.h"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace vc2 {

struct IsoEntry {
    std::string path;  // upper-case, '/'-separated, no leading slash
    u32 lba = 0;
    u32 size = 0;
    bool isDir = false;
};

class IsoImage {
public:
    ~IsoImage();
    bool open(const std::filesystem::path& file, std::string* err = nullptr);
    void close();
    bool isOpen() const { return fp_ != nullptr; }

    // Returns nullptr if not found. Lookup is case-insensitive.
    const IsoEntry* find(const std::string& path) const;
    const std::map<std::string, IsoEntry>& entries() const { return entries_; }

    bool readFile(const std::string& path, Bytes& out) const;
    // Raw sector-level access (thread-safe).
    bool readBytes(u64 offset, void* dst, size_t len) const;
    u64 totalSize() const { return totalSize_; }
    std::string volumeId() const { return volumeId_; }

private:
    void walk(u32 lba, u32 size, const std::string& prefix, int depth);
    FILE* fp_ = nullptr;
    mutable std::mutex mtx_;
    std::map<std::string, IsoEntry> entries_;
    u64 totalSize_ = 0;
    std::string volumeId_;
};

std::string toUpper(std::string s);

}  // namespace vc2
