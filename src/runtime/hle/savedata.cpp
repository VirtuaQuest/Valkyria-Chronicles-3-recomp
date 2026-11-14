// sceUtilitySavedata: the PSP savedata utility (SceUtilitySavedataParam), all modes.
//
// Saves live in <memstick>/PSP/SAVEDATA/<gameName><saveName>/ exactly like on a memory stick:
// PARAM.SFO (written here), the data file, ICON0.PNG / ICON1.PMF / PIC1.PNG / SND0.AT3.
//
// "Secure" modes encrypt the data file on a real PSP (sceChnnlsv + per-game key). We store the data
// file unencrypted and leave SAVEDATA_PARAMS zeroed (which is how the PSP marks unencrypted saves),
// so saves are not portable to real hardware, but everything the game reads back round-trips exactly.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "../kernel/kernel.h"
#include "utility.h"

namespace fs = std::filesystem;

namespace vc2rt::utility::savedata {

namespace {

// ------------------------------------------------------------------ request layout
enum : u32 {
    OFF_RESULT = 0x1C,
    OFF_MODE = 0x30,
    OFF_BIND = 0x34,
    OFF_OVERWRITE = 0x38,
    OFF_GAMENAME = 0x3C,      // char[13]
    OFF_SAVENAME = 0x4C,      // char[20]
    OFF_SAVENAMELIST = 0x60,  // char (*)[20], terminated by an empty name
    OFF_FILENAME = 0x64,      // char[13]
    OFF_DATABUF = 0x74,
    OFF_DATABUFSIZE = 0x78,
    OFF_DATASIZE = 0x7C,
    OFF_SFO_TITLE = 0x80,      // char[0x80]
    OFF_SFO_SAVETITLE = 0x100, // char[0x80]
    OFF_SFO_DETAIL = 0x180,    // char[0x400]
    OFF_SFO_PARENTAL = 0x580,  // u8
    OFF_ICON0 = 0x584,         // PspUtilitySavedataFileData {buf, bufSize, size, unknown}
    OFF_ICON1 = 0x594,
    OFF_PIC1 = 0x5A4,
    OFF_SND0 = 0x5B4,
    OFF_NEWDATA = 0x5C4,  // PspUtilitySavedataFileData* (icon shown for a new save)
    OFF_FOCUS = 0x5C8,
    OFF_ABORT = 0x5CC,
    OFF_MSFREE = 0x5D0,
    OFF_MSDATA = 0x5D4,
    OFF_UTILDATA = 0x5D8,
    OFF_KEY = 0x5DC,  // u8[16]
    OFF_SECUREVER = 0x5EC,
    OFF_MULTISTATUS = 0x5F0,
    OFF_IDLIST = 0x5F4,
    OFF_FILELIST = 0x5F8,
    OFF_SIZEINFO = 0x5FC,
    REQ_MAX = 0x600,
};
constexpr u32 kSizeV1 = 1480, kSizeV2 = 1500, kSizeV3 = 1536;

enum Mode : u32 {
    AUTOLOAD = 0, AUTOSAVE = 1, LOAD = 2, SAVE = 3, LISTLOAD = 4, LISTSAVE = 5, LISTDELETE = 6,
    LISTALLDELETE = 7, SIZES = 8, AUTODELETE = 9, DELETE_ = 10, LIST = 11, FILES = 12,
    MAKEDATASECURE = 13, MAKEDATA = 14, READDATASECURE = 15, READDATA = 16, WRITEDATASECURE = 17,
    WRITEDATA = 18, ERASESECURE = 19, ERASE = 20, DELETEDATA = 21, GETSIZE = 22,
};
const char* const kModeNames[] = {
    "AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE", "LISTDELETE", "LISTALLDELETE",
    "SIZES", "AUTODELETE", "DELETE", "LIST", "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE",
    "READDATA", "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA", "GETSIZE"};

enum Focus : u32 {
    FOCUS_NAME = 0, FOCUS_FIRSTLIST, FOCUS_LASTLIST, FOCUS_LATEST, FOCUS_OLDEST, FOCUS_FIRSTDATA,
    FOCUS_LASTDATA, FOCUS_FIRSTEMPTY, FOCUS_LASTEMPTY,
};

constexpr u64 kInitDelay = 200000;     // the PSP takes ~0.2s to bring the dialog up
constexpr u64 kShutdownDelay = 2000;
constexpr u32 kClusterSize = 0x8000;   // memory stick cluster
constexpr u64 kMaxReportedFree = 1ull << 30;  // keep 32-bit size math in games sane
constexpr u32 kBindValue = 1021;       // what the PSP writes into `bind` after a load

const char* const ICON0 = "ICON0.PNG";
const char* const ICON1 = "ICON1.PMF";
const char* const PIC1 = "PIC1.PNG";
const char* const SND0 = "SND0.AT3";
const char* const PARAMSFO = "PARAM.SFO";

// ------------------------------------------------------------------ PARAM.SFO
struct SfoValue {
    u16 fmt = 0x0204;  // 0x0004 binary, 0x0204 UTF-8 string, 0x0404 u32
    std::vector<u8> data;
    u32 maxLen = 0;
};
using Sfo = std::map<std::string, SfoValue>;  // keys are stored sorted, as the PSP does

bool parseSfo(const std::vector<u8>& d, Sfo& out) {
    auto r32 = [&](size_t o) { u32 v; memcpy(&v, &d[o], 4); return v; };
    auto r16 = [&](size_t o) { u16 v; memcpy(&v, &d[o], 2); return v; };
    if (d.size() < 20 || r32(0) != 0x46535000) return false;
    u32 keyTable = r32(8), dataTable = r32(12), count = r32(16);
    for (u32 i = 0; i < count; i++) {
        size_t o = 20 + (size_t)i * 16;
        if (o + 16 > d.size()) return false;
        size_t ko = keyTable + r16(o), dO = dataTable + r32(o + 12);
        u32 len = r32(o + 4);
        if (ko >= d.size() || dO + len > d.size()) return false;
        std::string key;
        while (ko < d.size() && d[ko]) key.push_back((char)d[ko++]);
        SfoValue v;
        v.fmt = r16(o + 2);
        v.maxLen = r32(o + 8);
        v.data.assign(d.begin() + dO, d.begin() + dO + len);
        out[key] = std::move(v);
    }
    return true;
}

std::vector<u8> buildSfo(const Sfo& sfo) {
    u32 n = (u32)sfo.size();
    u32 keyBytes = 0;
    for (auto& [k, v] : sfo) keyBytes += (u32)k.size() + 1;
    u32 keyTable = 20 + n * 16;
    u32 dataTable = (keyTable + keyBytes + 3) & ~3u;
    u32 dataBytes = 0;
    for (auto& [k, v] : sfo) dataBytes += (std::max<u32>(v.maxLen, (u32)v.data.size()) + 3) & ~3u;
    std::vector<u8> out(dataTable + dataBytes, 0);
    auto w32 = [&](size_t o, u32 v) { memcpy(&out[o], &v, 4); };
    auto w16 = [&](size_t o, u16 v) { memcpy(&out[o], &v, 2); };
    w32(0, 0x46535000);  // "\0PSF"
    w32(4, 0x00000101);
    w32(8, keyTable);
    w32(12, dataTable);
    w32(16, n);
    u32 i = 0, ko = 0, dO = 0;
    for (auto& [k, v] : sfo) {
        u32 maxLen = (std::max<u32>(v.maxLen, (u32)v.data.size()) + 3) & ~3u;
        size_t e = 20 + (size_t)i * 16;
        w16(e, (u16)ko);
        w16(e + 2, v.fmt);
        w32(e + 4, (u32)v.data.size());
        w32(e + 8, maxLen);
        w32(e + 12, dO);
        memcpy(&out[keyTable + ko], k.c_str(), k.size() + 1);
        if (!v.data.empty()) memcpy(&out[dataTable + dO], v.data.data(), v.data.size());
        ko += (u32)k.size() + 1;
        dO += maxLen;
        i++;
    }
    return out;
}

void sfoSetStr(Sfo& s, const std::string& key, std::string v, u32 maxLen) {
    if (v.size() >= maxLen) v.resize(maxLen - 1);
    SfoValue& e = s[key];
    e.fmt = 0x0204;
    e.maxLen = maxLen;
    e.data.assign(v.begin(), v.end());
    e.data.push_back(0);
}
void sfoSetInt(Sfo& s, const std::string& key, u32 v) {
    SfoValue& e = s[key];
    e.fmt = 0x0404;
    e.maxLen = 4;
    e.data.resize(4);
    memcpy(e.data.data(), &v, 4);
}
void sfoSetBin(Sfo& s, const std::string& key, const std::vector<u8>& v, u32 maxLen) {
    SfoValue& e = s[key];
    e.fmt = 0x0004;
    e.maxLen = maxLen;
    e.data = v;
    e.data.resize(maxLen, 0);
}
std::string sfoStr(const Sfo& s, const std::string& key) {
    auto it = s.find(key);
    if (it == s.end()) return {};
    std::string v(it->second.data.begin(), it->second.data.end());
    size_t z = v.find('\0');
    if (z != std::string::npos) v.resize(z);
    return v;
}
u32 sfoInt(const Sfo& s, const std::string& key) {
    auto it = s.find(key);
    if (it == s.end() || it->second.data.size() < 4) return 0;
    u32 v;
    memcpy(&v, it->second.data.data(), 4);
    return v;
}

constexpr u32 kFileListEntries = 99, kFileListEntrySize = 32;  // name[13] hash[16] pad[3]
constexpr u32 kFileListBytes = kFileListEntries * kFileListEntrySize;

// ------------------------------------------------------------------ host file helpers
fs::path u8p(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
fs::path saveRoot() { return paths().saveDir / "PSP" / "SAVEDATA"; }
fs::path saveDirPath(const std::string& dirName) { return saveRoot() / u8p(dirName); }

bool readFile(const fs::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize((size_t)n);
    if (n) f.read((char*)out.data(), n);
    return (bool)f;
}

bool writeFile(const fs::path& p, const u8* data, size_t n) {
    fs::path tmp = p;
    tmp += u8".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (n) f.write((const char*)data, (std::streamsize)n);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);  // replaces atomically on NTFS
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

struct FileTimes {
    FILETIME ctime{}, atime{}, mtime{};
    u64 size = 0;
    bool dir = false;
};
bool fileTimes(const fs::path& p, FileTimes& t) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
    t.ctime = d.ftCreationTime;
    t.atime = d.ftLastAccessTime;
    t.mtime = d.ftLastWriteTime;
    t.size = ((u64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    t.dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}
u64 ftToU64(const FILETIME& f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// ScePspDateTime {u16 year, month, day, hour, minute, second; u32 microsecond}
void writeDateTime(u32 addr, const FILETIME& ft) {
    FILETIME local;
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    wr16(addr, st.wYear);
    wr16(addr + 2, st.wMonth);
    wr16(addr + 4, st.wDay);
    wr16(addr + 6, st.wHour);
    wr16(addr + 8, st.wMinute);
    wr16(addr + 10, st.wSecond);
    wr32(addr + 12, (u32)((ftToU64(local) / 10) % 1000000));
}

struct DirEntry {
    std::string name;
    FileTimes t;
};
std::vector<DirEntry> listDir(const fs::path& p, bool* exists = nullptr) {
    std::vector<DirEntry> v;
    std::error_code ec;
    bool ok = fs::is_directory(p, ec);
    if (exists) *exists = ok;
    if (!ok) return v;
    for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        DirEntry e;
        auto u = it->path().filename().u8string();
        e.name.assign(u.begin(), u.end());
        if (e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".tmp") == 0) continue;
        if (!fileTimes(it->path(), e.t)) continue;
        v.push_back(std::move(e));
    }
    std::sort(v.begin(), v.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return v;
}

u64 freeBytes() {
    ULARGE_INTEGER avail{};
    std::error_code ec;
    fs::create_directories(saveRoot(), ec);
    if (!GetDiskFreeSpaceExW(saveRoot().c_str(), &avail, nullptr, nullptr)) return kMaxReportedFree;
    return std::min<u64>(avail.QuadPart, kMaxReportedFree);
}
u64 clusterRound(u64 n) { return (n + kClusterSize - 1) / kClusterSize * kClusterSize; }

// "736 KB" style text as the PSP prints it (rounding up or down per field).
std::string spaceText(u64 size, bool roundUp) {
    static const char* const suffix[] = {"B", "KB", "MB", "GB"};
    char buf[32];
    for (const char* s : suffix) {
        if (size < 1024) {
            snprintf(buf, sizeof buf, "%llu %s", (unsigned long long)size, s);
            return buf;
        }
        size = roundUp ? (size + 1023) / 1024 : size / 1024;
    }
    snprintf(buf, sizeof buf, "%llu TB", (unsigned long long)size);
    return buf;
}
void writeFixed(u32 addr, const std::string& s, size_t cap) {
    // strncpy semantics: zero padded, no terminator if it fills the field
    for (size_t i = 0; i < cap; i++) wr8(addr + (u32)i, i < s.size() ? (u8)s[i] : 0);
}

bool globMatch(const char* s, const char* p) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            for (const char* t = s;; t++) {
                if (globMatch(t, p + 1)) return true;
                if (!*t) return false;
            }
        }
        if (!*s) return false;
        if (*p != '?' && toupper((u8)*p) != toupper((u8)*s)) return false;
    }
    return *s == 0;
}

bool safeName(const std::string& s) {
    if (s == "." || s == "..") return false;
    return s.find_first_of("/\\:") == std::string::npos;
}

// ------------------------------------------------------------------ dialog state
struct Slot {
    std::string saveName;  // directory = gameName + saveName
    bool exists = false, broken = false;
    u64 mtime = 0, size = 0;
};

enum class Phase { None, List, Confirm, Message };
enum class Action { None, Load, Save, Delete };

struct Dialog {
    DialogState st;
    u32 addr = 0, size = 0;
    u8 req[REQ_MAX]{};
    u32 mode = 0;
    bool visible = false, uiStarted = false;
    std::vector<Slot> slots;
    int sel = 0;
    Phase phase = Phase::None;
    Action action = Action::None;
    bool finishAfterMessage = false;
} g;

u32 r32(u32 off) { u32 v = 0; if (off + 4 <= g.size) memcpy(&v, g.req + off, 4); return v; }
void w32(u32 off, u32 v) { if (off + 4 <= g.size) memcpy(g.req + off, &v, 4); }
std::string rstr(u32 off, size_t len) {
    std::string s;
    for (size_t i = 0; i < len && off + i < g.size && g.req[off + i]; i++) s.push_back((char)g.req[off + i]);
    return s;
}
void wstr(u32 off, const std::string& s, size_t len) {
    for (size_t i = 0; i < len && off + i < g.size; i++) g.req[off + i] = i < s.size() ? (u8)s[i] : 0;
}

std::string gameName() { return rstr(OFF_GAMENAME, 13); }
std::string saveNameField() {
    std::string s = rstr(OFF_SAVENAME, 20);
    return s == "<>" ? std::string() : s;
}
std::string fileName() { return rstr(OFF_FILENAME, 13); }
bool isSecureMode(u32 m) { return m != MAKEDATA && m != READDATA && m != WRITEDATA && m != ERASE; }
bool hasSecureVersion() { return g.size >= kSizeV3; }

struct FileData {
    u32 buf, bufSize, size;
};
FileData fileData(u32 off) { return {r32(off), r32(off + 4), r32(off + 8)}; }

bool guestRange(u32 addr, u32 n) { return n == 0 || (memValid(addr) && memValid(addr + n - 1)); }

Sfo loadSfo(const fs::path& dir, bool* ok = nullptr) {
    Sfo s;
    std::vector<u8> d;
    bool good = readFile(dir / PARAMSFO, d) && parseSfo(d, s);
    if (ok) *ok = good;
    return s;
}

std::vector<std::string> secureFileNames(const Sfo& sfo) {
    std::vector<std::string> names;
    auto it = sfo.find("SAVEDATA_FILE_LIST");
    if (it == sfo.end()) return names;
    const auto& d = it->second.data;
    for (size_t o = 0; o + kFileListEntrySize <= d.size(); o += kFileListEntrySize) {
        std::string n((const char*)&d[o], strnlen((const char*)&d[o], 13));
        if (!n.empty()) names.push_back(n);
    }
    return names;
}

// ------------------------------------------------------------------ IO operations
s32 doSave(const std::string& saveName, bool secure) {
    const std::string dirName = gameName() + saveName;
    if (gameName().empty() || !safeName(dirName)) return (s32)SAVE_ERR_SAVE_PARAM;
    u32 dataBuf = r32(OFF_DATABUF), dataBufSize = r32(OFF_DATABUFSIZE), dataSize = r32(OFF_DATASIZE);
    if (dataBuf && dataSize > dataBufSize) return (s32)SAVE_ERR_RW_BAD_PARAMS;
    if (dataBuf && !guestRange(dataBuf, dataSize)) return (s32)SAVE_ERR_RW_BAD_PARAMS;
    const u32 subOffs[4] = {OFF_ICON0, OFF_ICON1, OFF_PIC1, OFF_SND0};
    const char* subNames[4] = {ICON0, ICON1, PIC1, SND0};
    u64 needed = clusterRound(kFileListBytes + 0x1000) + (dataBuf ? clusterRound(dataSize) : 0);
    for (u32 off : subOffs) {
        FileData f = fileData(off);
        if (f.buf && (f.bufSize < f.size || !guestRange(f.buf, f.size))) return (s32)SAVE_ERR_RW_BAD_PARAMS;
        if (f.buf) needed += clusterRound(f.size);
    }
    if (hasSecureVersion() && r32(OFF_SECUREVER) > 3) return (s32)SAVE_ERR_SAVE_PARAM;

    fs::path dir = saveDirPath(dirName);
    std::error_code ec;
    bool existed = fs::is_directory(dir, ec);
    u64 existing = 0;
    if (existed)
        for (auto& e : listDir(dir)) existing += clusterRound(e.t.size);
    if (needed > existing && needed - existing > freeBytes()) return (s32)SAVE_ERR_SAVE_MS_NOSPACE;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) return (s32)SAVE_ERR_SAVE_ACCESS_ERROR;

    // PARAM.SFO: update the visible strings (only when the game supplies a title, so partial
    // WRITEDATA updates keep them), keep/extend the secure file list.
    Sfo sfo = loadSfo(dir);
    std::string title = rstr(OFF_SFO_TITLE, 0x80);
    if (!title.empty() || sfo.empty()) {
        sfoSetStr(sfo, "TITLE", title, 128);
        sfoSetStr(sfo, "SAVEDATA_TITLE", rstr(OFF_SFO_SAVETITLE, 0x80), 128);
        sfoSetStr(sfo, "SAVEDATA_DETAIL", rstr(OFF_SFO_DETAIL, 0x400), 1024);
        sfoSetInt(sfo, "PARENTAL_LEVEL", g.req[OFF_SFO_PARENTAL]);
    }
    sfoSetStr(sfo, "CATEGORY", "MS", 4);
    sfoSetStr(sfo, "SAVEDATA_DIRECTORY", dirName, 64);
    std::vector<u8> list(kFileListBytes, 0);
    if (auto it = sfo.find("SAVEDATA_FILE_LIST"); it != sfo.end())
        memcpy(list.data(), it->second.data.data(), std::min<size_t>(it->second.data.size(), list.size()));
    const std::string fname = fileName();
    if (secure && dataBuf && !fname.empty() && safeName(fname)) {
        for (u32 i = 0; i < kFileListEntries; i++) {
            char* e = (char*)&list[i * kFileListEntrySize];
            if (e[0] && strncmp(e, fname.c_str(), 13) != 0) continue;
            memset(e, 0, kFileListEntrySize);  // name + (zero) hash: we don't encrypt
            memcpy(e, fname.c_str(), std::min<size_t>(fname.size(), 12));
            break;
        }
    }
    sfoSetBin(sfo, "SAVEDATA_FILE_LIST", list, kFileListBytes);
    sfoSetBin(sfo, "SAVEDATA_PARAMS", std::vector<u8>(128, 0), 128);  // all zero = not encrypted
    std::vector<u8> sfoBytes = buildSfo(sfo);
    if (!writeFile(dir / PARAMSFO, sfoBytes.data(), sfoBytes.size())) return (s32)SAVE_ERR_SAVE_ACCESS_ERROR;

    if (dataBuf) {
        wstr(OFF_SAVENAME, saveName, 20);
        if (!fname.empty()) {
            if (!safeName(fname)) return (s32)SAVE_ERR_SAVE_PARAM;
            if (!writeFile(dir / u8p(fname), memPtr(dataBuf), dataSize)) return (s32)SAVE_ERR_SAVE_MS_NOSPACE;
        }
    }
    for (int i = 0; i < 4; i++) {
        FileData f = fileData(subOffs[i]);
        if (f.buf && f.size) writeFile(dir / subNames[i], memPtr(f.buf), f.size);
    }
    LOG_INFO("SAVE", "saved %s/%s (%u bytes%s)", dirName.c_str(), fname.c_str(), dataSize,
             secure ? ", secure stored plain" : "");
    return 0;
}

void loadSubFile(const fs::path& dir, const char* name, u32 off) {
    FileData f = fileData(off);
    if (!f.buf) return;
    std::vector<u8> d;
    if (!readFile(dir / name, d)) return;
    u32 n = (u32)std::min<size_t>(d.size(), f.bufSize);
    if (!guestRange(f.buf, n)) return;
    if (n) memcpy(memPtr(f.buf), d.data(), n);
    w32(off + 8, n);
}

s32 doLoad(const std::string& saveName, bool rwMode) {
    const std::string dirName = gameName() + saveName;
    fs::path dir = saveDirPath(dirName);
    std::error_code ec;
    if (!safeName(dirName) || !fs::is_directory(dir, ec))
        return (s32)(rwMode ? SAVE_ERR_RW_NO_DATA : SAVE_ERR_LOAD_NO_DATA);
    const std::string fname = fileName();
    if (!fname.empty() && (!safeName(fname) || !fs::is_regular_file(dir / u8p(fname), ec)))
        return (s32)(rwMode ? SAVE_ERR_RW_FILE_NOT_FOUND : SAVE_ERR_LOAD_FILE_NOT_FOUND);
    w32(OFF_DATASIZE, 0);
    if (hasSecureVersion() && r32(OFF_SECUREVER) > 3) return (s32)SAVE_ERR_LOAD_PARAM;
    if (!fname.empty()) {
        std::vector<u8> d;
        if (!readFile(dir / u8p(fname), d)) return (s32)(rwMode ? SAVE_ERR_RW_NO_DATA : SAVE_ERR_LOAD_NO_DATA);
        wstr(OFF_SAVENAME, saveName, 20);
        u32 buf = r32(OFF_DATABUF), bufSize = r32(OFF_DATABUFSIZE);
        if (buf) {
            if (d.size() > bufSize || !guestRange(buf, (u32)d.size()))
                return (s32)(rwMode ? SAVE_ERR_RW_DATA_BROKEN : SAVE_ERR_LOAD_DATA_BROKEN);
            if (!d.empty()) memcpy(memPtr(buf), d.data(), d.size());
        }
        w32(OFF_DATASIZE, (u32)d.size());
    }
    bool sfoOk = false;
    Sfo sfo = loadSfo(dir, &sfoOk);
    if (!sfoOk) return (s32)(rwMode ? SAVE_ERR_RW_DATA_BROKEN : SAVE_ERR_LOAD_DATA_BROKEN);
    wstr(OFF_SFO_TITLE, sfoStr(sfo, "TITLE"), 0x80);
    wstr(OFF_SFO_SAVETITLE, sfoStr(sfo, "SAVEDATA_TITLE"), 0x80);
    wstr(OFF_SFO_DETAIL, sfoStr(sfo, "SAVEDATA_DETAIL"), 0x400);
    g.req[OFF_SFO_PARENTAL] = (u8)sfoInt(sfo, "PARENTAL_LEVEL");
    w32(OFF_BIND, kBindValue);
    loadSubFile(dir, ICON0, OFF_ICON0);
    loadSubFile(dir, ICON1, OFF_ICON1);
    loadSubFile(dir, PIC1, OFF_PIC1);
    loadSubFile(dir, SND0, OFF_SND0);
    LOG_INFO("SAVE", "loaded %s/%s (%u bytes)", dirName.c_str(), fname.c_str(), r32(OFF_DATASIZE));
    return 0;
}

bool doDelete(const std::string& dirName) {
    if (dirName.empty() || !safeName(dirName)) return false;
    if (gameName().empty() && g.mode != LISTALLDELETE) return false;
    fs::path dir = saveDirPath(dirName);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    fs::remove_all(dir, ec);
    LOG_INFO("SAVE", "deleted %s%s", dirName.c_str(), ec ? " (with errors)" : "");
    return !ec;
}

// ERASE / ERASESECURE: removes the data file only, and its entry in the secure file list.
s32 doErase() {
    const std::string dirName = gameName() + saveNameField();
    fs::path dir = saveDirPath(dirName);
    std::error_code ec;
    if (!safeName(dirName) || !fs::is_directory(dir, ec)) return (s32)SAVE_ERR_RW_NO_DATA;
    if (!fs::is_regular_file(dir / PARAMSFO, ec)) return (s32)SAVE_ERR_RW_DATA_BROKEN;
    const std::string fname = fileName();
    if (fname.empty()) return 0;
    if (!safeName(fname) || !fs::is_regular_file(dir / u8p(fname), ec)) return (s32)SAVE_ERR_RW_FILE_NOT_FOUND;
    fs::remove(dir / u8p(fname), ec);
    Sfo sfo = loadSfo(dir);
    auto it = sfo.find("SAVEDATA_FILE_LIST");
    if (it != sfo.end()) {
        auto& d = it->second.data;
        for (size_t o = 0; o + kFileListEntrySize <= d.size(); o += kFileListEntrySize)
            if (strncmp((const char*)&d[o], fname.c_str(), 13) == 0) memset(&d[o], 0, kFileListEntrySize);
        std::vector<u8> bytes = buildSfo(sfo);
        writeFile(dir / PARAMSFO, bytes.data(), bytes.size());
    }
    return 0;
}

s32 doSizes() {
    s32 ret = 0;
    const u64 freeB = freeBytes();
    if (u32 p = r32(OFF_MSFREE); p && guestRange(p, 20)) {
        wr32(p, kClusterSize);
        wr32(p + 4, (u32)(freeB / kClusterSize));
        wr32(p + 8, (u32)(freeB / 1024));
        writeFixed(p + 12, spaceText(freeB, false), 8);
    }
    if (u32 p = r32(OFF_MSDATA); p && guestRange(p, 64)) {
        std::string gn = readCString(p, 13), sn = readCString(p + 16, 20);
        if (sn == "<>") sn.clear();
        bool exists = false;
        auto files = safeName(gn + sn) ? listDir(saveDirPath(gn + sn), &exists) : std::vector<DirEntry>{};
        if (exists && !(gn + sn).empty()) {
            u32 clusters = 0;
            for (auto& f : files) clusters += (u32)((f.t.size + kClusterSize - 1) / kClusterSize);
            u64 total = (u64)clusters * kClusterSize;
            wr32(p + 36, clusters);
            wr32(p + 40, (u32)(total / 1024));
            writeFixed(p + 44, spaceText(total, true), 8);
            wr32(p + 52, (u32)(total / 1024));
            writeFixed(p + 56, spaceText(total, true), 8);
        } else {
            wr32(p + 36, 0);
            wr32(p + 40, 0);
            writeFixed(p + 44, "", 8);
            wr32(p + 52, 0);
            writeFixed(p + 56, "", 8);
            ret = (s32)SAVE_ERR_SIZES_NO_DATA;
        }
    }
    if (u32 p = r32(OFF_UTILDATA); p && guestRange(p, 28)) {
        // directory record + PARAM.SFO + data file (+16 for the encryption header a PSP adds) + extras
        u64 total = 2 * (u64)kClusterSize;
        if (!fileName().empty()) total += clusterRound((u64)r32(OFF_DATASIZE) + 16);
        for (u32 off : {OFF_ICON0, OFF_ICON1, OFF_PIC1, OFF_SND0}) total += clusterRound(fileData(off).size);
        wr32(p, (u32)(total / kClusterSize));
        wr32(p + 4, (u32)(total / 1024));
        writeFixed(p + 8, spaceText(total, true), 8);
        wr32(p + 16, (u32)(total / 1024));
        writeFixed(p + 20, spaceText(total, true), 8);
    }
    return ret;
}

void doList() {
    u32 p = r32(OFF_IDLIST);
    if (!p || !guestRange(p, 12)) return;
    u32 maxCount = rd32(p), entries = rd32(p + 8);
    std::string sn = rstr(OFF_SAVENAME, 20);
    std::string pattern = gameName() + (sn == "<>" ? std::string("*") : sn);
    u32 n = 0;
    for (auto& d : listDir(saveRoot())) {
        if (n >= maxCount) break;
        if (!d.t.dir || !globMatch(d.name.c_str(), pattern.c_str())) continue;
        u32 e = entries + n * 72;
        if (!guestRange(e, 72)) break;
        FileTimes t = d.t;
        fileTimes(saveDirPath(d.name) / PARAMSFO, t);  // the SFO's dates, like the PSP
        wr32(e, 0x11FF);
        writeDateTime(e + 4, t.ctime);
        writeDateTime(e + 20, t.atime);
        writeDateTime(e + 36, t.mtime);
        writeFixed(e + 52, d.name.substr(std::min(d.name.size(), gameName().size())), 20);
        n++;
    }
    wr32(p + 4, n);
    LOG_INFO("SAVE", "LIST %s: %u entries", pattern.c_str(), n);
}

s32 doFiles() {
    u32 p = r32(OFF_FILELIST);
    if (!p || !guestRange(p, 36)) return -1;
    u32 maxSecure = rd32(p), maxNormal = rd32(p + 4), maxSystem = rd32(p + 8);
    u32 secureE = rd32(p + 24), normalE = rd32(p + 28), systemE = rd32(p + 32);
    if (secureE && maxSecure > 99) return (s32)SAVE_ERR_RW_BAD_PARAMS;
    if (normalE && maxNormal > 8192) return (s32)SAVE_ERR_RW_BAD_PARAMS;
    if (systemE && maxSystem > 5) return (s32)SAVE_ERR_RW_BAD_PARAMS;
    const std::string dirName = gameName() + saveNameField();
    bool exists = false;
    auto files = safeName(dirName) ? listDir(saveDirPath(dirName), &exists) : std::vector<DirEntry>{};
    if (!exists) return (s32)SAVE_ERR_RW_NO_DATA;
    wr32(p + 12, 0);
    wr32(p + 16, 0);
    wr32(p + 20, 0);
    bool sfoOk = false;
    Sfo sfo = loadSfo(saveDirPath(dirName), &sfoOk);
    if (!sfoOk) return (s32)SAVE_ERR_RW_DATA_BROKEN;
    auto secure = secureFileNames(sfo);
    w32(OFF_BIND, kBindValue);
    wr32(g.addr + OFF_BIND, kBindValue);
    u32 nSec = 0, nNorm = 0, nSys = 0;
    for (auto& f : files) {
        if (f.t.dir) continue;
        if (f.name.find_first_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos) continue;  // PSP skips these
        bool system = f.name == ICON0 || f.name == ICON1 || f.name == PIC1 || f.name == SND0 || f.name == PARAMSFO;
        bool isSecure = std::find(secure.begin(), secure.end(), f.name) != secure.end();
        u32 e = 0;
        if (system) {
            if (systemE && nSys < maxSystem) e = systemE + 80 * nSys++;
        } else if (isSecure) {
            if (secureE && nSec < maxSecure) e = secureE + 80 * nSec++;
        } else if (normalE && nNorm < maxNormal) {
            e = normalE + 80 * nNorm++;
        }
        if (!e || !guestRange(e, 80)) continue;
        wr32(e, 0x21FF);
        wr32(e + 4, 0);
        wr64(e + 8, f.t.size);  // secure files are stored plain, so this is the game's data size
        writeDateTime(e + 16, f.t.ctime);
        writeDateTime(e + 32, f.t.atime);
        writeDateTime(e + 48, f.t.mtime);
        writeFixed(e + 64, f.name.substr(0, 15), 16);
    }
    wr32(p + 12, nSec);
    wr32(p + 16, nNorm);
    wr32(p + 20, nSys);
    LOG_INFO("SAVE", "FILES %s: %u secure, %u normal, %u system", dirName.c_str(), nSec, nNorm, nSys);
    return 0;
}

bool doGetSize() {
    const std::string dirName = gameName() + saveNameField();
    bool exists = false;
    auto files = safeName(dirName) ? listDir(saveDirPath(dirName), &exists) : std::vector<DirEntry>{};
    u32 p = r32(OFF_SIZEINFO);
    if (!p || !guestRange(p, 60)) return exists;
    s32 numSecure = (s32)rd32(p), numNormal = (s32)rd32(p + 4);
    u32 secureE = rd32(p + 8), normalE = rd32(p + 12);
    if (numSecure < 0 || numNormal < 0 || (numSecure && !guestRange(secureE, numSecure * 24)) ||
        (numNormal && !guestRange(normalE, numNormal * 24)))
        return false;
    auto existingSize = [&](u32 nameAddr) -> u64 {
        std::string n = readCString(nameAddr, 16);
        for (auto& f : files)
            if (f.name == n) return f.t.size;
        return 0;
    };
    s64 overwrite = 0, write = 0;
    for (s32 i = 0; i < numNormal; i++) {
        overwrite += existingSize(normalE + i * 24 + 8);
        write += (s64)rd64(normalE + i * 24);
    }
    for (s32 i = 0; i < numSecure; i++) {
        overwrite += existingSize(secureE + i * 24 + 8);
        write += (s64)rd64(secureE + i * 24) + 0x10;
    }
    const u64 freeB = freeBytes();
    wr32(p + 16, kClusterSize);
    wr32(p + 20, (u32)(freeB / kClusterSize));
    wr32(p + 24, (u32)(freeB / 1024));
    writeFixed(p + 28, spaceText(freeB, false), 8);
    if (write - overwrite < (s64)freeB) {
        wr32(p + 36, 0);
        wr32(p + 48, 0);
    } else {
        s64 need = write - (s64)freeB;
        wr32(p + 36, (u32)((need + 1023) / 1024));
        writeFixed(p + 40, spaceText((u64)need, true), 8);
        s64 needOw = std::max<s64>(0, write - (s64)freeB - overwrite);
        wr32(p + 48, (u32)((needOw + 1023) / 1024));
        writeFixed(p + 52, spaceText((u64)needOw, true), 8);
    }
    return exists;
}

// ------------------------------------------------------------------ slots (list modes / focus)
bool multiNameMode(u32 m) {
    switch (m) {
        case LISTLOAD: case LISTSAVE: case LISTDELETE: case LISTALLDELETE: case SIZES: case LIST:
        case FILES: case GETSIZE: return true;
        default: return false;
    }
}

Slot probeSlot(const std::string& saveName, const std::string& dirOverride = {}) {
    Slot s;
    s.saveName = saveName;
    std::string dirName = dirOverride.empty() ? gameName() + saveName : dirOverride;
    if (!safeName(dirName)) return s;
    bool exists = false;
    auto files = listDir(saveDirPath(dirName), &exists);
    s.exists = exists;
    if (!exists) return s;
    bool first = true;
    for (auto& f : files) {
        if (f.t.dir) continue;
        s.size += f.t.size;
        if (first || f.name == PARAMSFO) s.mtime = ftToU64(f.t.mtime);
        first = false;
    }
    std::error_code ec;
    s.broken = !fs::is_regular_file(saveDirPath(dirName) / PARAMSFO, ec);
    return s;
}

void buildSlots() {
    g.slots.clear();
    bool includeMissing = !(g.mode == LISTLOAD || g.mode == LISTDELETE || g.mode == LISTALLDELETE);
    if (g.mode == LISTALLDELETE) {
        for (auto& d : listDir(saveRoot()))
            if (d.t.dir) g.slots.push_back(probeSlot("", d.name)), g.slots.back().saveName = d.name;
        return;
    }
    u32 list = r32(OFF_SAVENAMELIST);
    if (multiNameMode(g.mode) && list && memValid(list)) {
        std::vector<std::string> names;
        for (u32 i = 0; i < 1000 && memValid(list + i * 20 + 19); i++) {
            std::string n = readCString(list + i * 20, 20);
            if (n.empty()) break;
            names.push_back(n);
        }
        auto listed = [&](const std::string& n) {
            for (auto& s : g.slots)
                if (s.saveName == n) return true;
            return false;
        };
        const std::string gn = gameName();
        for (auto& n : names) {
            if (n == "<>") {  // any save of this game not listed yet
                for (auto& d : listDir(saveRoot()))
                    if (d.t.dir && d.name.size() >= gn.size() && d.name.compare(0, gn.size(), gn) == 0) {
                        std::string sn = d.name.substr(gn.size());
                        if (!listed(sn)) g.slots.push_back(probeSlot(sn));
                    }
                continue;
            }
            if (!safeName(n) || listed(n)) continue;
            Slot s = probeSlot(n);
            if (s.exists || includeMissing) g.slots.push_back(s);
        }
        if (!names.empty()) return;
    }
    Slot s = probeSlot(saveNameField());
    if (s.exists || includeMissing) g.slots.push_back(s);
}

int focusIndex() {
    const int n = (int)g.slots.size();
    if (n == 0) return 0;
    auto firstWhere = [&](bool exists, bool last) {
        for (int k = 0; k < n; k++) {
            int i = last ? n - 1 - k : k;
            if (g.slots[i].exists == exists) return i;
        }
        return 0;
    };
    switch (r32(OFF_FOCUS)) {
        case FOCUS_NAME: {
            std::string sn = saveNameField();
            for (int i = 0; i < n; i++)
                if (g.slots[i].saveName == sn) return i;
            return 0;
        }
        case FOCUS_FIRSTLIST: return 0;
        case FOCUS_LASTLIST: return n - 1;
        case FOCUS_LATEST:
        case FOCUS_OLDEST: {
            int best = -1;
            for (int i = 0; i < n; i++) {
                if (!g.slots[i].exists) continue;
                bool latest = r32(OFF_FOCUS) == FOCUS_LATEST;
                if (best < 0 || (latest ? g.slots[i].mtime > g.slots[best].mtime : g.slots[i].mtime < g.slots[best].mtime))
                    best = i;
            }
            return best < 0 ? 0 : best;
        }
        case FOCUS_FIRSTDATA: return firstWhere(true, false);
        case FOCUS_LASTDATA: return firstWhere(true, true);
        case FOCUS_FIRSTEMPTY: return firstWhere(false, false);
        case FOCUS_LASTEMPTY: return firstWhere(false, true);
        default: return 0;
    }
}

std::string slotDir(const Slot& s) { return g.mode == LISTALLDELETE ? s.saveName : gameName() + s.saveName; }

std::shared_ptr<std::vector<u8>> guestBlob(u32 buf, u32 size) {
    if (!buf || !size || size > (4u << 20) || !guestRange(buf, size)) return nullptr;
    return std::make_shared<std::vector<u8>>(memPtr(buf), memPtr(buf) + size);
}

SlotView makeView(const Slot& s) {
    SlotView v;
    v.saveName = s.saveName;
    v.exists = s.exists;
    v.broken = s.broken;
    v.iconId = uiNewIconId();
    if (s.exists) {
        fs::path dir = saveDirPath(slotDir(s));
        Sfo sfo = loadSfo(dir);
        v.title = sfoStr(sfo, "TITLE");
        v.savedataTitle = sfoStr(sfo, "SAVEDATA_TITLE");
        v.detail = sfoStr(sfo, "SAVEDATA_DETAIL");
        if (s.broken) v.savedataTitle = "Corrupted data";
        auto png = std::make_shared<std::vector<u8>>();
        if (readFile(dir / ICON0, *png) && !png->empty()) v.iconPng = png;
        FILETIME ft{(DWORD)s.mtime, (DWORD)(s.mtime >> 32)}, local;
        SYSTEMTIME st{};
        FileTimeToLocalFileTime(&ft, &local);
        FileTimeToSystemTime(&local, &st);
        char buf[64];
        snprintf(buf, sizeof buf, "%04u/%02u/%02u  %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        v.dateText = buf;
        v.sizeText = spaceText(s.size, true);
    } else {
        v.title = rstr(OFF_SFO_TITLE, 0x80);
        v.savedataTitle = "New Saved Data";
        if (u32 nd = r32(OFF_NEWDATA); nd && guestRange(nd, 16)) v.iconPng = guestBlob(rd32(nd), rd32(nd + 8));
        if (!v.iconPng && (g.mode == SAVE || g.mode == LISTSAVE))
            v.iconPng = guestBlob(fileData(OFF_ICON0).buf, fileData(OFF_ICON0).size);
    }
    return v;
}

// ------------------------------------------------------------------ visible dialog flow
void finish() {
    // results go back into the game's request block
    if (g.addr && guestRange(g.addr, g.size)) memcpy(memPtr(g.addr), g.req, g.size);
    ui().screen = UiScreen::Hidden;
    ui().event = UiEvent::None;
    g.phase = Phase::None;
    g.st.change(ST_FINISHED, 0);
    LOG_INFO("SAVE", "%s finished: result %08x", g.mode < 23 ? kModeNames[g.mode] : "?", r32(OFF_RESULT));
}

bool isListMode() { return g.mode == LISTLOAD || g.mode == LISTSAVE || g.mode == LISTDELETE || g.mode == LISTALLDELETE; }

void showList() {
    UiState& u = ui();
    u.screen = UiScreen::List;
    u.slots.clear();
    for (auto& s : g.slots) u.slots.push_back(makeView(s));
    u.selected = std::clamp(g.sel, 0, std::max(0, (int)g.slots.size() - 1));
    u.event = UiEvent::None;
    g.phase = Phase::List;
}

void showConfirm(const std::string& text, Action a, bool defaultYes) {
    UiState& u = ui();
    if (u.slots.size() != g.slots.size()) {
        u.slots.clear();
        for (auto& s : g.slots) u.slots.push_back(makeView(s));
    }
    u.screen = UiScreen::Confirm;
    u.message = text;
    u.focusSlot = g.sel;
    u.yes = defaultYes;
    u.event = UiEvent::None;
    g.phase = Phase::Confirm;
    g.action = a;
}

void showMessage(const std::string& text, bool thenFinish) {
    UiState& u = ui();
    if (u.slots.size() != g.slots.size()) {
        u.slots.clear();
        for (auto& s : g.slots) u.slots.push_back(makeView(s));
    }
    u.screen = UiScreen::Message;
    u.message = text;
    u.focusSlot = g.slots.empty() ? -1 : g.sel;
    u.event = UiEvent::None;
    g.phase = Phase::Message;
    g.finishAfterMessage = thenFinish;
}

void refreshSlots() {
    int sel = g.sel;
    buildSlots();
    g.sel = std::clamp(sel, 0, std::max(0, (int)g.slots.size() - 1));
}

void performAction(Action a) {
    Slot& s = g.slots[g.sel];
    if (a == Action::Load) {
        s32 r = doLoad(s.saveName, false);
        w32(OFF_RESULT, (u32)r);
        if (r == 0) showMessage("Loading completed.", true);
        else showMessage(r == (s32)SAVE_ERR_LOAD_DATA_BROKEN ? "Unable to load. The data is corrupted." : "Unable to load the data.", !isListMode());
    } else if (a == Action::Save) {
        s32 r = doSave(s.saveName, true);
        w32(OFF_RESULT, (u32)r);
        if (r == 0) {
            refreshSlots();
            ui().slots.clear();
            showMessage("Saving completed.", true);
        } else {
            showMessage(r == (s32)SAVE_ERR_SAVE_MS_NOSPACE ? "There is not enough free space to save." : "Unable to save the data.", !isListMode());
        }
    } else if (a == Action::Delete) {
        bool ok = doDelete(slotDir(s));
        w32(OFF_RESULT, ok ? 0 : SAVE_ERR_DELETE_ACCESS_ERROR);
        ui().slots.clear();
        if (ok) refreshSlots();
        showMessage(ok ? "Deletion completed." : "Unable to delete the data.", ok || !isListMode());
    }
}

void startVisible() {
    buildSlots();
    g.sel = focusIndex();
    UiState& u = ui();
    u.slots.clear();
    u.gameTitle = rstr(OFF_SFO_TITLE, 0x80);
    for (auto& s : g.slots)
        if (u.gameTitle.empty() && s.exists) u.gameTitle = sfoStr(loadSfo(saveDirPath(slotDir(s))), "TITLE");
    bool anyData = std::any_of(g.slots.begin(), g.slots.end(), [](const Slot& s) { return s.exists; });
    switch (g.mode) {
        case LOAD:
            u.heading = "Load";
            if (g.slots.empty() || !g.slots[0].exists) {
                w32(OFF_RESULT, SAVE_ERR_LOAD_NO_DATA);
                showMessage("There is no saved data.", true);
            } else if (g.slots[0].broken) {
                w32(OFF_RESULT, SAVE_ERR_LOAD_DATA_BROKEN);
                showMessage("The saved data is corrupted.", true);
            } else {
                showConfirm("Do you want to load this data?", Action::Load, true);
            }
            break;
        case SAVE:
            u.heading = "Save";
            if (g.slots.empty()) g.slots.push_back(probeSlot(saveNameField()));
            if (g.slots[0].exists) showConfirm("Do you want to overwrite the saved data?", Action::Save, false);
            else showConfirm("Do you want to save the data?", Action::Save, true);
            break;
        case DELETE_:
            u.heading = "Delete";
            if (!g.slots.empty() && g.slots[0].exists) {
                showConfirm("Do you want to delete this data?", Action::Delete, false);
            } else {
                w32(OFF_RESULT, SAVE_ERR_DELETE_NO_DATA);
                showMessage("There is no saved data.", true);
            }
            break;
        case LISTLOAD:
            u.heading = "Load";
            if (!anyData) {
                w32(OFF_RESULT, SAVE_ERR_LOAD_NO_DATA);
                showMessage("There is no saved data.", true);
            } else {
                showList();
            }
            break;
        case LISTSAVE:
            u.heading = "Save";
            if (g.slots.empty()) g.slots.push_back(probeSlot(saveNameField()));
            showList();
            break;
        default:  // LISTDELETE / LISTALLDELETE
            u.heading = "Delete";
            if (!anyData) {
                w32(OFF_RESULT, SAVE_ERR_DELETE_NO_DATA);
                showMessage("There is no saved data.", true);
            } else {
                showList();
            }
            break;
    }
}

void handleUiEvent() {
    UiState& u = ui();
    UiEvent ev = u.event;
    if (ev == UiEvent::None) return;
    u.event = UiEvent::None;
    switch (g.phase) {
        case Phase::List:
            g.sel = std::clamp(u.selected, 0, std::max(0, (int)g.slots.size() - 1));
            if (ev == UiEvent::Cancel) {
                w32(OFF_RESULT, DIALOG_RESULT_CANCEL);
                finish();
            } else if (ev == UiEvent::Select && !g.slots.empty()) {
                Slot& s = g.slots[g.sel];
                if (g.mode == LISTLOAD) {
                    if (s.broken) showMessage("The saved data is corrupted.", false);
                    else performAction(Action::Load);
                } else if (g.mode == LISTSAVE) {
                    if (s.exists) showConfirm("Do you want to overwrite the saved data?", Action::Save, false);
                    else performAction(Action::Save);
                } else {
                    showConfirm("Do you want to delete this data?", Action::Delete, false);
                }
            }
            break;
        case Phase::Confirm:
            if (ev == UiEvent::Yes) {
                performAction(g.action);
            } else if (ev == UiEvent::No || ev == UiEvent::Cancel) {
                if (isListMode()) {
                    showList();
                } else {
                    w32(OFF_RESULT, DIALOG_RESULT_CANCEL);
                    finish();
                }
            }
            break;
        case Phase::Message:
            if (ev == UiEvent::Ok || ev == UiEvent::Cancel) {
                if (g.finishAfterMessage) finish();
                else showList();
            }
            break;
        default: break;
    }
}

// ------------------------------------------------------------------ non-visible modes
void runHidden() {
    const u32 m = g.mode;
    const std::string sn = saveNameField();
    s32 r = 0;
    switch (m) {
        case AUTOLOAD: r = doLoad(sn, false); break;
        case AUTOSAVE: r = doSave(sn, true); break;
        case SIZES: r = doSizes(); break;
        case LIST: doList(); r = 0; break;
        case FILES: r = doFiles(); break;
        case GETSIZE: r = doGetSize() ? 0 : (s32)SAVE_ERR_RW_NO_DATA; break;
        case AUTODELETE: r = doDelete(gameName() + sn) ? 0 : (s32)SAVE_ERR_DELETE_NO_DATA; break;
        case DELETEDATA: r = doDelete(gameName() + sn) ? 0 : (s32)SAVE_ERR_RW_NO_DATA; break;
        case MAKEDATA:
        case MAKEDATASECURE:
            r = doSave(sn, m == MAKEDATASECURE);
            if (r == (s32)SAVE_ERR_SAVE_MS_NOSPACE) r = (s32)SAVE_ERR_RW_MEMSTICK_FULL;
            break;
        case WRITEDATA:
        case WRITEDATASECURE: r = doSave(sn, m == WRITEDATASECURE); break;
        case READDATA:
        case READDATASECURE:
            r = doLoad(sn, true);
            break;
        case ERASE:
        case ERASESECURE: r = doErase(); break;
        default: LOG_WARN("SAVE", "savedata mode %u not implemented", m); r = 0; break;
    }
    w32(OFF_RESULT, (u32)r);
}

bool modeIsVisible(u32 m) {
    return m == LOAD || m == SAVE || m == LISTLOAD || m == LISTSAVE || m == LISTDELETE || m == LISTALLDELETE || m == DELETE_;
}

}  // namespace

DialogState& state() { return g.st; }

s32 initStart(u32 paramAddr) {
    if (g.st.busy()) return (s32)ERR_UTILITY_INVALID_STATUS;
    s32 chk = checkRequest(paramAddr, {kSizeV1, kSizeV2, kSizeV3});
    if (chk < 0) {
        LOG_ERROR("SAVE", "sceUtilitySavedataInitStart(%08x): bad request (%08x)", paramAddr, (u32)chk);
        return chk;
    }
    if (paramAddr & 3) return (s32)0x800200d2;  // illegal argument
    g.addr = paramAddr;
    g.size = rd32(paramAddr);
    memset(g.req, 0, sizeof g.req);
    memcpy(g.req, memPtr(paramAddr), g.size);
    g.mode = r32(OFF_MODE);
    g.uiStarted = false;
    g.phase = Phase::None;
    g.slots.clear();
    g.sel = 0;
    if (!safeName(gameName()) || !safeName(rstr(OFF_SAVENAME, 20) == "<>" ? "" : rstr(OFF_SAVENAME, 20)) ||
        !safeName(fileName()))
        return (s32)ERR_UTILITY_INVALID_PARAM_SIZE;
    g.visible = modeIsVisible(g.mode);
    LOG_INFO("SAVE", "sceUtilitySavedataInitStart: %s game=%s save=%s file=%s size=%u", g.mode < 23 ? kModeNames[g.mode] : "?",
             gameName().c_str(), rstr(OFF_SAVENAME, 20).c_str(), fileName().c_str(), g.size);
    g.st.change(ST_INIT, 0);
    g.st.change(ST_RUNNING, kInitDelay);
    if (g.visible) uiEnsureRegistered();
    return 0;
}

s32 update(s32 /*animSpeed*/) {
    if (g.st.poll() != ST_RUNNING) return (s32)ERR_UTILITY_INVALID_STATUS;
    if (g.st.pendingAt) return 0;  // already done, waiting to report it
    if (!g.visible) {
        runHidden();
        finish();
        return 0;
    }
    if (!g.uiStarted) {
        g.uiStarted = true;
        startVisible();
        return 0;
    }
    handleUiEvent();
    return 0;
}

s32 shutdownStart() {
    if (g.st.poll() != ST_FINISHED) return (s32)ERR_UTILITY_INVALID_STATUS;
    g.st.change(ST_SHUTDOWN, 0);
    g.st.change(ST_NONE, kShutdownDelay);
    ui().screen = UiScreen::Hidden;
    return 0;
}

}  // namespace vc2rt::utility::savedata
