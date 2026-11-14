#pragma once
// Minimal read-only ISO9660 reader for PSP UMD images: plain .iso files and CSO (CISO v1,
// deflate-compressed blocks) images, which are decompressed block by block as they are read.
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
    u64 totalSize() const { return totalSize_; }  // uncompressed image size
    std::string volumeId() const { return volumeId_; }
    bool compressed() const { return cso_; }

private:
    void walk(u32 lba, u32 size, const std::string& prefix, int depth);
    bool openCso(std::string* err);
    bool readRaw(u64 offset, void* dst, size_t len) const;  // file bytes (lock held)
    bool loadCsoBlock(u64 block) const;                    // into csoCache_ (lock held)
    FILE* fp_ = nullptr;
    // CSO
    bool cso_ = false;
    u32 csoBlockSize_ = 0, csoAlign_ = 0;
    std::vector<u32> csoIndex_;
    mutable u64 csoCachedBlock_ = ~0ull;
    mutable Bytes csoCache_, csoComp_;
    mutable std::mutex mtx_;
    std::map<std::string, IsoEntry> entries_;
    u64 totalSize_ = 0;
    std::string volumeId_;
};

std::string toUpper(std::string s);

}  // namespace vc2
