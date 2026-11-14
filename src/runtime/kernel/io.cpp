// IoFileMgrForUser: disc0:/umd0: (ISO image), ms0: (host save folder), raw LBN access.
#include <windows.h>

#include <algorithm>
#include <map>

#include "../../common/iso9660.h"
#include "../hle/hle.h"
#include "io.h"
#include "kernel.h"

namespace vc2rt {

static vc2::IsoImage g_disc;
vc2::IsoImage& disc() { return g_disc; }

namespace kernel {
namespace {

enum : u32 {
    ERRNO_FILE_NOT_FOUND = 0x80010002,
    ERRNO_INVALID_ARGUMENT = 0x80010016,
    ERRNO_FILE_ALREADY_EXISTS = 0x80010011,
    ERR_BADF = 0x80020323,
    ERR_NODEV = 0x80020321,
    ERR_NOASYNC = 0x8002032a,
    ERR_ASYNC_BUSY = 0x80020329,
};
enum : u32 { O_RDONLY = 1, O_WRONLY = 2, O_RDWR = 3, O_APPEND = 0x100, O_CREAT = 0x200, O_TRUNC = 0x400, O_EXCL = 0x800 };

struct OpenFile {
    enum Kind { Disc, DiscRaw, Host } kind = Disc;
    std::string path;
    u64 start = 0, size = 0, pos = 0;  // Disc: byte range in ISO; DiscRaw: sector units
    HANDLE h = INVALID_HANDLE_VALUE;
    bool asyncPending = false;
    s64 asyncResult = 0;
};
struct OpenDir {
    std::vector<std::pair<std::string, bool>> entries;  // name, isDir
    std::vector<u64> sizes;
    size_t idx = 0;
};

std::map<u32, OpenFile> g_files;
std::map<u32, OpenDir> g_dirs;
u32 g_nextFd = 3;
std::string g_cwd = "disc0:/PSP_GAME/USRDIR";

struct Resolved {
    std::string device;  // "disc0", "ms0", ...
    std::string path;    // without leading slash, '/' separated
};

Resolved resolve(const std::string& in) {
    std::string p = in;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.find(':') == std::string::npos) p = g_cwd + "/" + p;
    Resolved r;
    size_t colon = p.find(':');
    r.device = vc2::toUpper(p.substr(0, colon));
    std::string rest = p.substr(colon + 1);
    // normalise . and ..
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= rest.size()) {
        size_t j = rest.find('/', i);
        if (j == std::string::npos) j = rest.size();
        std::string seg = rest.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".")
            parts.push_back(seg);
        i = j + 1;
    }
    for (size_t k = 0; k < parts.size(); k++) r.path += (k ? "/" : "") + parts[k];
    if (r.device == "UMD0" || r.device == "UMD1" || r.device == "UMD" || r.device == "ISOFS") r.device = "DISC0";
    if (r.device == "FATMS0") r.device = "MS0";
    return r;
}

std::filesystem::path hostPath(const Resolved& r) {
    std::filesystem::path base = paths().saveDir;
    std::filesystem::path p = base;
    if (!r.path.empty()) p /= std::filesystem::u8path(r.path);
    return p;
}

// Read-only overlays: "MS0:/<prefix>" folders whose missing files are served from a disc folder
// (used for game data "installed" from PSP_GAME/INSDIR without copying it).
std::vector<std::pair<std::string, std::string>> g_overlays;  // upper-case MS0 prefix, disc dir

const vc2::IsoEntry* overlayEntry(const Resolved& r) {
    std::string full = vc2::toUpper("MS0:/" + r.path);
    for (auto& [prefix, discDir] : g_overlays) {
        if (full.size() <= prefix.size() + 1 || full.compare(0, prefix.size(), prefix) != 0 || full[prefix.size()] != '/')
            continue;
        const vc2::IsoEntry* e = g_disc.find(discDir + full.substr(prefix.size()));
        if (e) return e;
    }
    return nullptr;
}

bool parseLbn(const std::string& path, u64& lba, u64& size) {
    // "sce_lbn0x5fa0_size0x1000"
    std::string up = vc2::toUpper(path);
    auto lp = up.find("SCE_LBN");
    if (lp == std::string::npos) return false;
    auto sp = up.find("_SIZE", lp);
    if (sp == std::string::npos) return false;
    lba = strtoull(up.substr(lp + 7, sp - lp - 7).c_str(), nullptr, 0);
    size = strtoull(up.substr(sp + 5).c_str(), nullptr, 0);
    return true;
}

void fillStat(u32 p, bool dir, u64 size) {
    memset(memPtr(p), 0, 0x58);
    wr32(p + 0, dir ? (0x1000 | 0x16D) : (0x2000 | 0x1FF));
    wr32(p + 4, dir ? 0x10 : 0x20);
    wr64(p + 8, size);
    // timestamps: 2010-09-01 (ScePspDateTime: u16 year, month, day, hour, min, sec; u32 usec)
    for (u32 t = 0; t < 3; t++) {
        u32 q = p + 0x10 + t * 16;
        wr16(q, 2010);
        wr16(q + 2, 9);
        wr16(q + 4, 1);
        wr16(q + 6, 12);
    }
}

s64 doOpen(const std::string& pathIn, u32 flags) {
    Resolved r = resolve(pathIn);
    OpenFile f;
    f.path = pathIn;
    if (r.device == "DISC0") {
        u64 lba, size;
        if (r.path.empty()) {
            f.kind = OpenFile::DiscRaw;
            f.start = 0;
            f.size = g_disc.totalSize() / 2048;
        } else if (parseLbn(r.path, lba, size)) {
            f.kind = OpenFile::Disc;
            f.start = lba * 2048;
            f.size = size;
        } else {
            const vc2::IsoEntry* e = g_disc.find(r.path);
            if (!e || e->isDir) {
                LOG_WARN("IO", "open failed (not on disc): %s", pathIn.c_str());
                return (s32)ERRNO_FILE_NOT_FOUND;
            }
            f.kind = OpenFile::Disc;
            f.start = (u64)e->lba * 2048;
            f.size = e->size;
        }
        if (flags & O_WRONLY) LOG_WARN("IO", "write-open of disc file %s", pathIn.c_str());
    } else if (r.device == "MS0" || r.device == "HOST0") {
        f.kind = OpenFile::Host;
        auto hp = hostPath(r);
        std::error_code ec;
        if ((flags & 3) == O_RDONLY && !std::filesystem::exists(hp, ec)) {
            if (const vc2::IsoEntry* e = overlayEntry(r); e && !e->isDir) {
                f.kind = OpenFile::Disc;
                f.start = (u64)e->lba * 2048;
                f.size = e->size;
                u32 fd = g_nextFd++;
                g_files[fd] = std::move(f);
                LOG_DEBUG("IO", "open %s -> fd %u (installed data, served from disc)", pathIn.c_str(), fd);
                return fd;
            }
        }
        DWORD access = 0;
        if ((flags & 3) == O_RDONLY) access = GENERIC_READ;
        else if ((flags & 3) == O_WRONLY) access = GENERIC_WRITE;
        else access = GENERIC_READ | GENERIC_WRITE;
        DWORD disp = OPEN_EXISTING;
        if (flags & O_CREAT) disp = (flags & O_EXCL) ? CREATE_NEW : ((flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS);
        else if (flags & O_TRUNC) disp = TRUNCATE_EXISTING;
        if (flags & O_CREAT) std::filesystem::create_directories(hp.parent_path());
        f.h = CreateFileW(hp.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disp, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
        if (f.h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            LOG_DEBUG("IO", "host open failed: %s (%lu)", pathIn.c_str(), e);
            return (s32)(e == ERROR_FILE_EXISTS ? ERRNO_FILE_ALREADY_EXISTS : ERRNO_FILE_NOT_FOUND);
        }
        if (flags & O_APPEND) SetFilePointer(f.h, 0, nullptr, FILE_END);
    } else {
        LOG_WARN("IO", "open on unsupported device: %s", pathIn.c_str());
        return (s32)ERR_NODEV;
    }
    u32 fd = g_nextFd++;
    g_files[fd] = std::move(f);
    LOG_DEBUG("IO", "open %s -> fd %u", pathIn.c_str(), fd);
    return fd;
}

s64 doRead(u32 fd, u32 dst, u32 len) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return (s32)ERR_BADF;
    OpenFile& f = it->second;
    if (f.kind == OpenFile::Host) {
        DWORD got = 0;
        ReadFile(f.h, memPtr(dst), len, &got, nullptr);
        return got;
    }
    if (f.kind == OpenFile::DiscRaw) {  // sector units
        u64 sectors = std::min<u64>(len, f.size > f.pos ? f.size - f.pos : 0);
        if (sectors) g_disc.readBytes((f.start + f.pos) * 2048, memPtr(dst), (size_t)sectors * 2048);
        f.pos += sectors;
        return (s64)sectors;
    }
    u64 n = std::min<u64>(len, f.size > f.pos ? f.size - f.pos : 0);
    if (n) g_disc.readBytes(f.start + f.pos, memPtr(dst), (size_t)n);
    LOG_DEBUG("IO", "read fd %u (%s) pos %llx len %x -> %llx", fd, f.path.c_str(), (unsigned long long)f.pos, len,
              (unsigned long long)n);
    f.pos += n;
    return (s64)n;
}

s64 doWrite(u32 fd, u32 src, u32 len) {
    if (fd == 1 || fd == 2) {
        std::string s((const char*)memPtr(src), len);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        LOG_INFO("STDOUT", "%s", s.c_str());
        return len;
    }
    auto it = g_files.find(fd);
    if (it == g_files.end()) return (s32)ERR_BADF;
    OpenFile& f = it->second;
    if (f.kind != OpenFile::Host) return (s32)ERRNO_INVALID_ARGUMENT;
    DWORD put = 0;
    WriteFile(f.h, memPtr(src), len, &put, nullptr);
    return put;
}

s64 doSeek(u32 fd, s64 off, u32 whence) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return (s32)ERR_BADF;
    OpenFile& f = it->second;
    if (f.kind == OpenFile::Host) {
        LARGE_INTEGER li, out;
        li.QuadPart = off;
        SetFilePointerEx(f.h, li, &out, whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END);
        return out.QuadPart;
    }
    s64 np = whence == 0 ? off : whence == 1 ? (s64)f.pos + off : (s64)f.size + off;
    if (np < 0) return (s32)ERRNO_INVALID_ARGUMENT;
    f.pos = (u64)np;
    return np;
}

s64 doClose(u32 fd) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return (s32)ERR_BADF;
    if (it->second.h != INVALID_HANDLE_VALUE) CloseHandle(it->second.h);
    g_files.erase(it);
    return 0;
}

void setAsync(u32 fd, s64 result) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return;
    it->second.asyncPending = true;
    it->second.asyncResult = result;
}

// ---------------------------------------------------------------- HLE wrappers
void sceIoOpen(PspContext* c) { RET((u32)doOpen(readCString(A0), A1)); }
void sceIoClose(PspContext* c) { RET((u32)doClose(A0)); }
void sceIoRead(PspContext* c) { RET((u32)doRead(A0, A1, A2)); }
void sceIoWrite(PspContext* c) { RET((u32)doWrite(A0, A1, A2)); }
void sceIoLseek(PspContext* c) {
    // (fd, s64 offset in a2:a3, whence in t0)
    s64 off = (s64)(((u64)A3 << 32) | A2);
    RET64(doSeek(A0, off, A4));
}
void sceIoLseek32(PspContext* c) { RET((u32)doSeek(A0, (s32)A1, A2)); }

void sceIoOpenAsync(PspContext* c) {
    s64 fd = doOpen(readCString(A0), A1);
    if (fd < 0) {
        // PSP still returns a descriptor and reports the error through the async result
        u32 nfd = g_nextFd++;
        OpenFile bad;
        bad.kind = OpenFile::Disc;
        bad.size = 0;
        g_files[nfd] = bad;
        setAsync(nfd, fd);
        RET(nfd);
        return;
    }
    setAsync((u32)fd, fd);
    RET((u32)fd);
}
void sceIoReadAsync(PspContext* c) {
    if (!g_files.count(A0)) {
        RET(ERR_BADF);
        return;
    }
    setAsync(A0, doRead(A0, A1, A2));
    RET(0);
}
void sceIoWriteAsync(PspContext* c) {
    if (!g_files.count(A0)) {
        RET(ERR_BADF);
        return;
    }
    setAsync(A0, doWrite(A0, A1, A2));
    RET(0);
}
void sceIoLseekAsync(PspContext* c) {
    if (!g_files.count(A0)) {
        RET(ERR_BADF);
        return;
    }
    s64 off = (s64)(((u64)A3 << 32) | A2);
    setAsync(A0, doSeek(A0, off, A4));
    RET(0);
}
void sceIoCloseAsync(PspContext* c) {
    auto it = g_files.find(A0);
    if (it == g_files.end()) {
        RET(ERR_BADF);
        return;
    }
    if (it->second.h != INVALID_HANDLE_VALUE) {
        CloseHandle(it->second.h);
        it->second.h = INVALID_HANDLE_VALUE;
    }
    it->second.kind = OpenFile::Disc;
    it->second.size = 0;
    it->second.path = "<closing>";
    setAsync(A0, 0);
    RET(0);
}
void asyncWait(PspContext* c, bool poll, bool cb) {
    auto it = g_files.find(A0);
    if (it == g_files.end()) {
        RET(ERR_BADF);
        return;
    }
    if (!it->second.asyncPending) {
        RET(ERR_NOASYNC);
        return;
    }
    if (cb) checkCallbacks();
    it->second.asyncPending = false;
    if (A1) wr64(A1, (u64)it->second.asyncResult);
    bool closing = it->second.path == "<closing>";
    if (closing) g_files.erase(it);
    RET(0);
    (void)poll;
}
void sceIoWaitAsync(PspContext* c) { asyncWait(c, false, false); }
void sceIoWaitAsyncCB(PspContext* c) { asyncWait(c, false, true); }
void sceIoPollAsync(PspContext* c) { asyncWait(c, true, false); }
void sceIoChangeAsyncPriority(PspContext* c) { RET(0); }

void sceIoGetstat(PspContext* c) {
    std::string in = readCString(A0);
    Resolved r = resolve(in);
    if (r.device == "DISC0") {
        if (r.path.empty()) {
            fillStat(A1, true, 0);
            RET(0);
            return;
        }
        const vc2::IsoEntry* e = g_disc.find(r.path);
        if (!e) {
            RET(ERRNO_FILE_NOT_FOUND);
            return;
        }
        fillStat(A1, e->isDir, e->size);
        RET(0);
        return;
    }
    if (r.device == "MS0") {
        std::error_code ec;
        auto hp = hostPath(r);
        if (!std::filesystem::exists(hp, ec)) {
            if (const vc2::IsoEntry* e = overlayEntry(r)) {
                fillStat(A1, e->isDir, e->size);
                RET(0);
                return;
            }
            RET(ERRNO_FILE_NOT_FOUND);
            return;
        }
        bool dir = std::filesystem::is_directory(hp, ec);
        fillStat(A1, dir, dir ? 0 : std::filesystem::file_size(hp, ec));
        RET(0);
        return;
    }
    RET(ERR_NODEV);
}

void sceIoDopen(PspContext* c) {
    std::string in = readCString(A0);
    Resolved r = resolve(in);
    OpenDir d;
    d.entries.push_back({".", true});
    d.sizes.push_back(0);
    d.entries.push_back({"..", true});
    d.sizes.push_back(0);
    if (r.device == "DISC0") {
        std::string prefix = r.path.empty() ? "" : vc2::toUpper(r.path) + "/";
        if (!r.path.empty() && !g_disc.find(r.path)) {
            RET(ERRNO_FILE_NOT_FOUND);
            return;
        }
        for (auto& [p, e] : g_disc.entries()) {
            if (p.compare(0, prefix.size(), prefix) != 0) continue;
            std::string rest = p.substr(prefix.size());
            if (rest.empty() || rest.find('/') != std::string::npos) continue;
            d.entries.push_back({rest, e.isDir});
            d.sizes.push_back(e.size);
        }
    } else if (r.device == "MS0") {
        std::error_code ec;
        auto hp = hostPath(r);
        if (!std::filesystem::is_directory(hp, ec)) {
            RET(ERRNO_FILE_NOT_FOUND);
            return;
        }
        for (auto& de : std::filesystem::directory_iterator(hp, ec)) {
            d.entries.push_back({pathToUtf8(de.path().filename()), de.is_directory()});
            d.sizes.push_back(de.is_directory() ? 0 : de.file_size());
        }
    } else {
        RET(ERR_NODEV);
        return;
    }
    u32 fd = g_nextFd++;
    g_dirs[fd] = std::move(d);
    RET(fd);
}
void sceIoDread(PspContext* c) {
    auto it = g_dirs.find(A0);
    if (it == g_dirs.end()) {
        RET(ERR_BADF);
        return;
    }
    OpenDir& d = it->second;
    if (d.idx >= d.entries.size()) {
        RET(0);
        return;
    }
    u32 p = A1;
    fillStat(p, d.entries[d.idx].second, d.sizes[d.idx]);
    writeCString(p + 0x58, d.entries[d.idx].first, 256);
    d.idx++;
    RET(1);
}
void sceIoDclose(PspContext* c) {
    g_dirs.erase(A0);
    RET(0);
}

void sceIoRemove(PspContext* c) {
    Resolved r = resolve(readCString(A0));
    std::error_code ec;
    if (r.device != "MS0" || !std::filesystem::remove(hostPath(r), ec)) {
        RET(ERRNO_FILE_NOT_FOUND);
        return;
    }
    RET(0);
}
void sceIoMkdir(PspContext* c) {
    Resolved r = resolve(readCString(A0));
    std::error_code ec;
    if (r.device != "MS0") {
        RET(ERR_NODEV);
        return;
    }
    std::filesystem::create_directories(hostPath(r), ec);
    RET(0);
}
void sceIoRmdir(PspContext* c) {
    Resolved r = resolve(readCString(A0));
    std::error_code ec;
    if (r.device != "MS0" || !std::filesystem::remove(hostPath(r), ec)) {
        RET(ERRNO_FILE_NOT_FOUND);
        return;
    }
    RET(0);
}
void sceIoRename(PspContext* c) {
    Resolved a = resolve(readCString(A0)), b = resolve(readCString(A1));
    std::error_code ec;
    if (a.device != "MS0") {
        RET(ERR_NODEV);
        return;
    }
    if (b.device != "MS0") b = Resolved{"MS0", b.path};
    std::filesystem::rename(hostPath(a), hostPath(b), ec);
    RET(ec ? ERRNO_FILE_NOT_FOUND : 0);
}
void sceIoChdir(PspContext* c) {
    g_cwd = readCString(A0);
    RET(0);
}

void sceIoDevctl(PspContext* c) {
    std::string dev = readCString(A0);
    u32 cmd = A1, in = A2, inLen = A3, out = A4, outLen = A5;
    (void)inLen;
    std::string D = vc2::toUpper(dev);
    if (D.rfind("UMD", 0) == 0 || D.rfind("DISC", 0) == 0) {
        switch (cmd) {
            case 0x01F20001:  // get disc type
                if (out && outLen >= 8) {
                    wr32(out, 0xFFFFFFFF);
                    wr32(out + 4, 0x10);  // game disc
                }
                RET(0);
                return;
            case 0x01F20002:  // current LBA
            case 0x01F20003:
                if (out) wr32(out, 0);
                RET(0);
                return;
            case 0x01F100A3:  // seek
            case 0x01F100A4:  // prepare into cache
            case 0x01F300A5:
            case 0x01F300A7:
            case 0x01F300A8:
            case 0x01F300A9:
                RET(0);
                return;
            case 0x01E18030:  // check UMD media
                RET(1);
                return;
        }
    }
    if (D.rfind("MS", 0) == 0 || D.rfind("FATMS", 0) == 0 || D.rfind("MSCMHC", 0) == 0) {
        switch (cmd) {
            case 0x02025801:  // MS driver state
                if (out) wr32(out, 4);
                RET(0);
                return;
            case 0x02025806:  // MS inserted
                if (out) wr32(out, 1);
                RET(0);
                return;
            case 0x02415821:  // register insert/eject callback
            case 0x02415822:
                RET(0);
                return;
            case 0x02425818: {  // free space info (pointer in indata)
                u32 p = in ? rd32(in) : 0;
                if (p) {
                    wr32(p + 0, 0x10000);   // max clusters
                    wr32(p + 4, 0x8000);    // free clusters
                    wr32(p + 8, 0x8000);    // max sectors
                    wr32(p + 12, 0x200);    // sector size
                    wr32(p + 16, 0x40);     // sectors per cluster
                }
                RET(0);
                return;
            }
            case 0x02425823:  // fat check
                if (out) wr32(out, 1);
                RET(0);
                return;
        }
    }
    LOG_WARN("IO", "unhandled devctl %s cmd=%08x", dev.c_str(), cmd);
    RET(0);
}

void sceIoIoctl(PspContext* c) {
    LOG_DEBUG("IO", "ioctl fd=%u cmd=%08x", A0, A1);
    RET(0);
}

const HleFunction kIo[] = {
    {0x109F50BC, "sceIoOpen", sceIoOpen},
    {0x810C4BC3, "sceIoClose", sceIoClose},
    {0x6A638D83, "sceIoRead", sceIoRead},
    {0x42EC03AC, "sceIoWrite", sceIoWrite},
    {0x27EB27B8, "sceIoLseek", sceIoLseek},
    {0x68963324, "sceIoLseek32", sceIoLseek32},
    {0x89AA9906, "sceIoOpenAsync", sceIoOpenAsync},
    {0xA0B5A7C2, "sceIoReadAsync", sceIoReadAsync},
    {0x0FACAB19, "sceIoWriteAsync", sceIoWriteAsync},
    {0x71B19E77, "sceIoLseekAsync", sceIoLseekAsync},
    {0xFF5940B6, "sceIoCloseAsync", sceIoCloseAsync},
    {0xE23EEC33, "sceIoWaitAsync", sceIoWaitAsync},
    {0x35DBD746, "sceIoWaitAsyncCB", sceIoWaitAsyncCB},
    {0x3251EA56, "sceIoPollAsync", sceIoPollAsync},
    {0xB293727F, "sceIoChangeAsyncPriority", sceIoChangeAsyncPriority},
    {0xACE946E8, "sceIoGetstat", sceIoGetstat},
    {0xB29DDF9C, "sceIoDopen", sceIoDopen},
    {0xE3EB004C, "sceIoDread", sceIoDread},
    {0xEB092469, "sceIoDclose", sceIoDclose},
    {0xF27A9C51, "sceIoRemove", sceIoRemove},
    {0x06A70004, "sceIoMkdir", sceIoMkdir},
    {0x1117C65F, "sceIoRmdir", sceIoRmdir},
    {0x779103A0, "sceIoRename", sceIoRename},
    {0x55F4717D, "sceIoChdir", sceIoChdir},
    {0x54F5FB11, "sceIoDevctl", sceIoDevctl},
    {0x63632449, "sceIoIoctl", sceIoIoctl},
    {0xB8A740F4, "sceIoChstat", [](PspContext* c) { RET(0); }},
};

}  // namespace

void ioSetCwd(const std::string& cwd) { g_cwd = cwd; }

void ioAddDiscOverlay(const std::string& ms0Prefix, const std::string& discDir) {
    std::string p = vc2::toUpper(ms0Prefix);
    while (!p.empty() && p.back() == '/') p.pop_back();
    for (auto& o : g_overlays)
        if (o.first == p) return;
    g_overlays.push_back({p, vc2::toUpper(discDir)});
}

}  // namespace kernel

HLE_MODULE("IoFileMgrForUser", kernel::kIo);

}  // namespace vc2rt
