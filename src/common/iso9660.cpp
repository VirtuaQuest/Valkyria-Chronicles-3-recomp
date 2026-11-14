#include "iso9660.h"
#include <algorithm>
#include <cctype>

namespace vc2 {

static constexpr u32 kSector = 2048;

std::string toUpper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

IsoImage::~IsoImage() { close(); }

void IsoImage::close() {
    if (fp_) fclose(fp_);
    fp_ = nullptr;
    entries_.clear();
}

bool IsoImage::readBytes(u64 offset, void* dst, size_t len) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!fp_) return false;
    if (_fseeki64(fp_, (s64)offset, SEEK_SET) != 0) return false;
    return fread(dst, 1, len, fp_) == len;
}

bool IsoImage::open(const std::filesystem::path& file, std::string* err) {
    close();
    fp_ = _wfopen(file.c_str(), L"rb");
    if (!fp_) {
        if (err) *err = "Could not open file";
        return false;
    }
    _fseeki64(fp_, 0, SEEK_END);
    totalSize_ = (u64)_ftelli64(fp_);
    u8 pvd[kSector];
    if (!readBytes(16ull * kSector, pvd, kSector) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        if (err) *err = "Not an ISO9660 image (missing primary volume descriptor)";
        close();
        return false;
    }
    volumeId_.assign((const char*)pvd + 40, 32);
    while (!volumeId_.empty() && volumeId_.back() == ' ') volumeId_.pop_back();
    const u8* root = pvd + 156;
    walk(rd32(root + 2), rd32(root + 10), "", 0);
    return true;
}

void IsoImage::walk(u32 lba, u32 size, const std::string& prefix, int depth) {
    if (depth > 16) return;
    Bytes data(((size + kSector - 1) / kSector) * kSector);
    if (!readBytes((u64)lba * kSector, data.data(), data.size())) return;
    size_t i = 0;
    while (i < data.size()) {
        u8 len = data[i];
        if (len == 0) {
            i = (i / kSector + 1) * kSector;
            continue;
        }
        const u8* r = &data[i];
        i += len;
        u8 nameLen = r[32];
        if (nameLen == 1 && (r[33] == 0 || r[33] == 1)) continue;
        std::string name((const char*)r + 33, nameLen);
        auto semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        IsoEntry e;
        e.path = toUpper(prefix.empty() ? name : prefix + "/" + name);
        e.lba = rd32(r + 2);
        e.size = rd32(r + 10);
        e.isDir = (r[25] & 2) != 0;
        entries_[e.path] = e;
        if (e.isDir) walk(e.lba, e.size, e.path, depth + 1);
    }
}

const IsoEntry* IsoImage::find(const std::string& path) const {
    std::string p = toUpper(path);
    std::replace(p.begin(), p.end(), '\\', '/');
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    auto it = entries_.find(p);
    return it == entries_.end() ? nullptr : &it->second;
}

bool IsoImage::readFile(const std::string& path, Bytes& out) const {
    const IsoEntry* e = find(path);
    if (!e || e->isDir) return false;
    out.resize(e->size);
    return e->size == 0 || readBytes((u64)e->lba * kSector, out.data(), e->size);
}

}  // namespace vc2
