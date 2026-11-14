#include "iso9660.h"
#include <algorithm>
#include <cctype>
#include <cstring>

#include "inflate.h"

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
    cso_ = false;
    csoIndex_.clear();
    csoCachedBlock_ = ~0ull;
}

bool IsoImage::readRaw(u64 offset, void* dst, size_t len) const {
    if (!fp_) return false;
    if (_fseeki64(fp_, (s64)offset, SEEK_SET) != 0) return false;
    return fread(dst, 1, len, fp_) == len;
}

bool IsoImage::loadCsoBlock(u64 block) const {
    if (block == csoCachedBlock_) return true;
    if (block + 1 >= csoIndex_.size()) return false;
    const u32 e0 = csoIndex_[block], e1 = csoIndex_[block + 1];
    const u64 pos = (u64)(e0 & 0x7FFFFFFF) << csoAlign_, end = (u64)(e1 & 0x7FFFFFFF) << csoAlign_;
    csoCache_.resize(csoBlockSize_);
    csoCachedBlock_ = ~0ull;
    if (e0 & 0x80000000) {  // stored uncompressed
        if (!readRaw(pos, csoCache_.data(), csoBlockSize_)) return false;
    } else {
        if (end <= pos || end - pos > 2ull * csoBlockSize_ + 1024) return false;
        csoComp_.resize((size_t)(end - pos));
        size_t got = 0;
        if (!readRaw(pos, csoComp_.data(), csoComp_.size()) ||
            !inflateRaw(csoComp_.data(), csoComp_.size(), csoCache_.data(), csoBlockSize_, &got))
            return false;
        if (got < csoBlockSize_) memset(csoCache_.data() + got, 0, csoBlockSize_ - got);
    }
    csoCachedBlock_ = block;
    return true;
}

bool IsoImage::readBytes(u64 offset, void* dst, size_t len) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!fp_) return false;
    if (!cso_) return readRaw(offset, dst, len);
    if (offset + len > totalSize_) return false;
    u8* out = (u8*)dst;
    while (len) {
        const u64 block = offset / csoBlockSize_;
        const size_t within = (size_t)(offset % csoBlockSize_);
        const size_t n = std::min(len, (size_t)csoBlockSize_ - within);
        if (!loadCsoBlock(block)) return false;
        memcpy(out, csoCache_.data() + within, n);
        out += n;
        offset += n;
        len -= n;
    }
    return true;
}

// CSO ("CISO" v1): 24-byte header, then one u32 per block (+1) giving the block's file offset
// (shifted by `align`; bit 31 = stored uncompressed). Blocks are raw deflate streams.
bool IsoImage::openCso(std::string* err) {
    u8 h[24];
    if (!readRaw(0, h, sizeof h)) return false;
    const u64 total = (u64)rd32(h + 8) | ((u64)rd32(h + 12) << 32);
    const u32 blockSize = rd32(h + 16);
    const u8 version = h[20], align = h[21];
    if (version > 1) {
        if (err) *err = "CSO version 2 images are not supported (re-compress as CSO v1 or use the .iso)";
        return false;
    }
    if (blockSize < kSector || blockSize > (1u << 20) || (blockSize & (blockSize - 1)) || total == 0 || align > 16) {
        if (err) *err = "Damaged CSO header";
        return false;
    }
    const u64 blocks = (total + blockSize - 1) / blockSize;
    csoIndex_.resize((size_t)blocks + 1);
    if (!readRaw(24, csoIndex_.data(), csoIndex_.size() * 4)) {
        if (err) *err = "The CSO image is truncated (block index incomplete)";
        return false;
    }
    cso_ = true;
    csoBlockSize_ = blockSize;
    csoAlign_ = align;
    totalSize_ = total;
    return true;
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
    char magic[4] = {};
    readRaw(0, magic, 4);
    if (memcmp(magic, "ZISO", 4) == 0 || memcmp(magic, "DAX\0", 4) == 0) {
        if (err) *err = "ZSO/DAX compressed images are not supported (use the .iso or a CSO)";
        close();
        return false;
    }
    if (memcmp(magic, "CISO", 4) == 0 && !openCso(err)) {
        if (err && err->empty()) *err = "Could not read the CSO image";
        close();
        return false;
    }
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
