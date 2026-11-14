// System functions first needed by Valkyria Chronicles III:
//  - sceUtilityGamedataInstall*: the optional "install to Memory Stick" of PSP_GAME/INSDIR.
//    A PC has no slow UMD to work around, so installing completes instantly: the install folder
//    gets its PARAM.SFO and the file system serves its data files straight from the disc.
//  - sceUtilityScreenshot*: the game only enables/disables the system screenshot feature.
//  - Kernel_Library memset/memcpy, SDK-version and CPU-clock setters.
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "../kernel/io.h"
#include "../kernel/kernel.h"
#include "hle.h"

namespace vc2rt {
namespace {

// ---------------------------------------------------------------- dialog status machine
// 0 none -> 1 initializing -> 2 running -> 3 finished -> (ShutdownStart) 4 shutting down -> 0
struct Dialog {
    int status = 0;
    u32 param = 0;
    u64 changeAt = 0;
};
constexpr u32 ERR_UTILITY_INVALID_STATUS = 0x80110001;
constexpr u64 kStepMicros = 50000;

int dialogStatus(Dialog& d) {
    // Advance automatically like the system dialogs do across a few frames.
    if (d.changeAt && hostMicros() >= d.changeAt) {
        if (d.status == 1) d.status = 2;
        else if (d.status == 4) d.status = 0;
        d.changeAt = 0;
    }
    return d.status;
}

void setCommonResult(u32 param, u32 result) {
    if (param && memValid(param)) wr32(param + 0x1C, result);  // pspUtilityDialogCommon.result
}

// Minimal PARAM.SFO writer (UTF-8 strings + one int) for the install folder.
void writeSfo(const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& strs) {
    struct Entry {
        std::string key;
        std::string val;
        bool isInt;
        u32 intVal;
    };
    std::vector<Entry> e;
    for (auto& [k, v] : strs) e.push_back({k, v, false, 0});
    e.push_back({"PARENTAL_LEVEL", "", true, 1});
    std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
    std::string keys, data;
    std::vector<u8> index;
    auto put16 = [&](u16 v) { index.push_back((u8)v); index.push_back((u8)(v >> 8)); };
    auto put32 = [&](u32 v) { for (int i = 0; i < 4; i++) index.push_back((u8)(v >> (8 * i))); };
    for (auto& x : e) {
        u32 len = x.isInt ? 4 : (u32)x.val.size() + 1;
        u32 maxLen = x.isInt ? 4 : (len + 3) & ~3u;
        put16((u16)keys.size());
        put16(x.isInt ? 0x0404 : 0x0204);
        put32(len);
        put32(maxLen);
        put32((u32)data.size());
        keys += x.key;
        keys.push_back('\0');
        if (x.isInt) data.append((const char*)&x.intVal, 4);
        else {
            data += x.val;
            data.append(maxLen - x.val.size(), '\0');
        }
    }
    while (keys.size() % 4) keys.push_back('\0');
    u32 keyOff = 20 + (u32)index.size(), dataOff = keyOff + (u32)keys.size();
    std::vector<u8> out;
    auto o32 = [&](u32 v) { for (int i = 0; i < 4; i++) out.push_back((u8)(v >> (8 * i))); };
    o32(0x46535000);
    o32(0x00000101);
    o32(keyOff);
    o32(dataOff);
    o32((u32)e.size());
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    std::ofstream(file, std::ios::binary).write((const char*)out.data(), (std::streamsize)out.size());
}

// ---------------------------------------------------------------- game data install
// SceUtilityGamedataInstallParam: common(0x30) mode@0x30 gameName[13]@0x34 dataName[20]@0x44
// sfoParam{title[0x80], savedataTitle[0x80], detail[0x400], parental u8}@0x58 progress@0x55C
Dialog g_install;

void doInstall(u32 p) {
    std::string game = readCString(p + 0x34, 13), data = readCString(p + 0x44, 20);
    std::string dir = "PSP/SAVEDATA/" + game + data;
    std::filesystem::path host = paths().saveDir / std::filesystem::u8path(dir);
    std::error_code ec;
    std::filesystem::create_directories(host, ec);
    writeSfo(host / "PARAM.SFO", {{"CATEGORY", "MG"},
                                  {"SAVEDATA_DIRECTORY", game + data},
                                  {"SAVEDATA_DETAIL", readCString(p + 0x158, 0x400)},
                                  {"SAVEDATA_TITLE", readCString(p + 0xD8, 0x80)},
                                  {"TITLE", readCString(p + 0x58, 0x80)}});
    // Marker so the overlay is re-registered on later boots (see restoreInstallOverlays).
    std::ofstream(host / "VC2_INSTALLED_FROM_DISC.txt")
        << "This folder's data files are read from PSP_GAME/INSDIR on the game disc.\n";
    kernel::ioAddDiscOverlay("MS0:/" + dir, "PSP_GAME/INSDIR");
    wr32(p + 0x55C, 100);
    setCommonResult(p, 0);
    LOG_INFO("UTIL", "game data install for %s%s: served directly from disc INSDIR", game.c_str(), data.c_str());
}

void sceUtilityGamedataInstallInitStart(PspContext* c) {
    if (dialogStatus(g_install) != 0) return (void)RET(ERR_UTILITY_INVALID_STATUS);
    g_install = {1, A0, hostMicros() + kStepMicros};
    RET(0);
}
void sceUtilityGamedataInstallGetStatus(PspContext* c) { RET(dialogStatus(g_install)); }
void sceUtilityGamedataInstallUpdate(PspContext* c) {
    if (dialogStatus(g_install) != 2) return (void)RET(ERR_UTILITY_INVALID_STATUS);
    doInstall(g_install.param);
    g_install.status = 3;
    RET(0);
}
void sceUtilityGamedataInstallShutdownStart(PspContext* c) {
    if (dialogStatus(g_install) != 3) return (void)RET(ERR_UTILITY_INVALID_STATUS);
    g_install.status = 4;
    g_install.changeAt = hostMicros() + kStepMicros;
    RET(0);
}
void sceUtilityGamedataInstallAbort(PspContext* c) {
    if (dialogStatus(g_install) != 2) return (void)RET(ERR_UTILITY_INVALID_STATUS);
    setCommonResult(g_install.param, 1);  // cancelled
    g_install.status = 3;
    RET(0);
}

// ---------------------------------------------------------------- screenshot
Dialog g_shot;
void sceUtilityScreenshotInitStart(PspContext* c) {
    if (dialogStatus(g_shot) != 0) return (void)RET(ERR_UTILITY_INVALID_STATUS);
    g_shot = {1, A0, hostMicros() + kStepMicros};
    RET(0);
}
void sceUtilityScreenshotGetStatus(PspContext* c) { RET(dialogStatus(g_shot)); }
void sceUtilityScreenshotUpdate(PspContext* c) {
    // The screenshot feature stays armed; the host already offers screenshots of its own.
    RET(dialogStatus(g_shot) == 2 ? 0 : ERR_UTILITY_INVALID_STATUS);
}
void sceUtilityScreenshotShutdownStart(PspContext* c) {
    int s = dialogStatus(g_shot);
    if (s != 2 && s != 3) return (void)RET(ERR_UTILITY_INVALID_STATUS);
    setCommonResult(g_shot.param, 0);
    g_shot.status = 4;
    g_shot.changeAt = hostMicros() + kStepMicros;
    RET(0);
}

const HleFunction kUtilityVc3[] = {
    {0x24AC31EB, "sceUtilityGamedataInstallInitStart", sceUtilityGamedataInstallInitStart},
    {0xB57E95D9, "sceUtilityGamedataInstallGetStatus", sceUtilityGamedataInstallGetStatus},
    {0x4AECD179, "sceUtilityGamedataInstallUpdate", sceUtilityGamedataInstallUpdate},
    {0x32E32DCB, "sceUtilityGamedataInstallShutdownStart", sceUtilityGamedataInstallShutdownStart},
    {0x180F7B62, "sceUtilityGamedataInstallAbort", sceUtilityGamedataInstallAbort},
    {0x0251B134, "sceUtilityScreenshotInitStart", sceUtilityScreenshotInitStart},
    {0xD81957B7, "sceUtilityScreenshotGetStatus", sceUtilityScreenshotGetStatus},
    {0xAB083EA9, "sceUtilityScreenshotUpdate", sceUtilityScreenshotUpdate},
    {0xF9E0008C, "sceUtilityScreenshotShutdownStart", sceUtilityScreenshotShutdownStart},
};

// ---------------------------------------------------------------- kernel helpers
void sceKernelMemset(PspContext* c) {
    if (A2) memset(memPtr(A0), (int)(A1 & 0xFF), A2);
    RET(A0);
}
void sceKernelMemcpy(PspContext* c) {
    if (A2) memmove(memPtr(A0), memPtr(A1), A2);
    RET(A0);
}
const HleFunction kKernelLibVc3[] = {
    {0xA089ECA4, "sceKernelMemset", sceKernelMemset},
    {0x1839852A, "sceKernelMemcpy", sceKernelMemcpy},
    // Newer SDKs export this one from Kernel_Library as well as ThreadManForUser.
    {0x293B45B8, "sceKernelGetThreadId", [](PspContext* c) { RET(kernel::currentThreadId()); }},
};
const HleFunction kSysMemVc3[] = {
    {0x1B4217BC, "sceKernelSetCompiledSdkVersion603_605", [](PspContext* c) { RET(0); }},
};
const HleFunction kPowerVc3[] = {
    {0x469989AD, "scePowerSetClockFrequency630", [](PspContext* c) { RET(0); }},
};

}  // namespace

void restoreInstallOverlays() {
    std::error_code ec;
    std::filesystem::path root = paths().saveDir / "PSP" / "SAVEDATA";
    for (auto& d : std::filesystem::directory_iterator(root, ec)) {
        if (!d.is_directory() || !std::filesystem::exists(d.path() / "VC2_INSTALLED_FROM_DISC.txt", ec)) continue;
        kernel::ioAddDiscOverlay("MS0:/PSP/SAVEDATA/" + pathToUtf8(d.path().filename()), "PSP_GAME/INSDIR");
        LOG_INFO("UTIL", "installed game data %s is served from the disc", pathToUtf8(d.path().filename()).c_str());
    }
}

HLE_MODULE("sceUtility", kUtilityVc3);
HLE_MODULE("Kernel_Library", kKernelLibVc3);
HLE_MODULE("SysMemUserForUser", kSysMemVc3);
HLE_MODULE("scePower", kPowerVc3);

}  // namespace vc2rt
