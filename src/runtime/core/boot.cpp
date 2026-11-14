// vc2_run: boots the recompiled game.
#include <windows.h>

#include "../../common/build_info.h"
#include "../../common/game_layout.h"
#include "../../common/iso9660.h"
#include "../../common/sfo.h"
#include "../../crypto/prx_decrypt.h"
#include "../../loader/elf.h"
#include "../gpu/gpu.h"
#include "../hle/audio.h"
#include "../hle/display.h"
#include "../hle/hle.h"
#include "../host/config.h"
#include "../host/host.h"
#include "../kernel/io.h"
#include "../kernel/kernel.h"
#include "../kernel/modules.h"
#include "../media/atrac_decoder.h"
#include "runtime.h"

namespace vc2rt {

void cpuInit(const Vc2GameImage* img);
void crashInit(const Vc2GameImage* img);
void restoreInstallOverlays();
void watchInit();

Paths& paths() {
    static Paths p;
    return p;
}

static std::string sha1Of(const ::Bytes& d) {
    u8 h[20];
    vc2::sha1(d.data(), d.size(), h);
    char s[41];
    for (int i = 0; i < 20; i++) snprintf(s + i * 2, 3, "%02x", h[i]);
    return s;
}

static std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

static std::filesystem::path readIsoPathSetting(const std::filesystem::path& ini) {
    FILE* f = _wfopen(ini.c_str(), L"r");
    if (!f) return {};
    char line[2048];
    std::filesystem::path result;
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("IsoPath=", 0) == 0) result = std::filesystem::u8path(s.substr(8));
    }
    fclose(f);
    return result;
}

static int boot(const Vc2GameImage* img, int argc, char** argv) {
    Paths& P = paths();
    P.exeDir = exeDir();
    P.configFile = P.exeDir / "vc3recomp.ini";
    P.logFile = P.exeDir / "vc3recomp.log";
    P.saveDir = P.exeDir / "memstick";
    std::filesystem::create_directories(P.saveDir / "PSP" / "SAVEDATA");
    {
        // keep the logs of the previous two runs (vc3recomp.1.log, vc3recomp.2.log) for bug reports
        std::error_code ec;
        const auto old = [&](int n) { return P.exeDir / ("vc3recomp." + std::to_string(n) + ".log"); };
        std::filesystem::rename(old(1), old(2), ec);
        std::filesystem::rename(P.logFile, old(1), ec);
    }
    logInit(P.logFile);
    config().load(P.configFile);
    if (config().logHle) {
        setLogLevel(LogLevel::Debug);
        hleSetTrace(true);
    }
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trace") {
            setLogLevel(LogLevel::Debug);
            hleSetTrace(true);
        } else if (a == "--debug")
            setLogLevel(LogLevel::Debug);
        else if (a.size() > 4 && a.rfind("--", 0) != 0)
            P.isoPath = std::filesystem::u8path(a);
    }
    if (P.isoPath.empty()) P.isoPath = readIsoPathSetting(P.exeDir / "game.ini");
    LOG_INFO("BOOT", "%s runtime %s, ABI %u", vc2::buildInfo().product, vc2::buildInfoLine().c_str(), img->abiVersion);
    LOG_INFO("BOOT", "built with %s (%s), branch %s, commit %s", vc2::buildInfo().compiler, vc2::buildInfo().buildType,
             vc2::buildInfo().branch, vc2::buildInfo().commitFull);
    if (img->abiVersion != VC2_RUNTIME_ABI) fatal("Game executable ABI %u does not match runtime ABI %u. Re-run the launcher.", img->abiVersion, VC2_RUNTIME_ABI);

    cpuInit(img);
    crashInit(img);
    if (!memInit()) fatal("Could not reserve PSP memory");
    watchInit();

    // ---- disc
    std::string err;
    if (P.isoPath.empty() || !disc().open(P.isoPath, &err))
        fatal("Could not open the game disc image:\n%s\n\nRun the launcher and select your game ISO.",
              pathToUtf8(P.isoPath).c_str());
    ::Bytes sfoRaw;
    vc2::SfoData sfo;
    const vc2::GameProfile* profile = nullptr;
    if (disc().readFile("PSP_GAME/PARAM.SFO", sfoRaw) && vc2::parseSfo(sfoRaw, sfo))
        profile = vc2::findGameProfile(sfo.get("DISC_ID").c_str());
    if (!profile) fatal("The selected disc image is not a supported Valkyria Chronicles disc.");
    // The module hashes checked below guarantee the disc matches the code this EXE was built from.
    LOG_INFO("BOOT", "disc %s (%s)", profile->discId, profile->name);
    restoreInstallOverlays();

    // ---- modules
    u32 mainStart = 0, mainGp = 0, mainPrio = 0x20, mainStack = 0x40000, mainAttr = 0;
    for (u32 i = 0; i < img->moduleCount; i++) {
        const PspModuleEntry& me = img->modules[i];
        ::Bytes raw, elf;
        if (!disc().readFile(me.discPath, raw)) fatal("Missing %s on disc", me.discPath);
        auto dr = vc2::decryptPspExecutable(raw, elf);
        if (dr != vc2::DecryptResult::Ok && dr != vc2::DecryptResult::NotEncrypted)
            fatal("Could not load %s: %s", me.discPath, vc2::decryptResultString(dr));
        if (sha1Of(elf) != me.sha1)
            fatal("%s on this disc does not match the one the game was recompiled from.\nRe-run the launcher.", me.discPath);
        vc2::LoadedModule m;
        if (!vc2::loadPspElf(elf, me.base, m, &err)) fatal("Could not load %s: %s", me.discPath, err.c_str());
        for (auto& s : m.segments) memcpy(memPtr(s.vaddr), s.data.data(), s.data.size());
        u32 lo = i == 0 ? kUserBase : (me.base & ~0xFFu);
        kernel::reserveFixedRange(lo, ((m.imageEnd + 0xFF) & ~0xFFu) - lo, m.name);
        LOG_INFO("BOOT", "loaded %s (%s) at %08x-%08x", m.name.c_str(), me.discPath, m.baseAddr, m.imageEnd);
        for (auto& ex : m.exports)
            if (!ex.name.empty())
                for (auto& [nid, addr] : ex.funcs) hleRegisterGuestExport(ex.name.c_str(), nid, addr);
        if (i == 0) {
            mainStart = m.moduleStart;
            mainGp = m.gp;
            for (auto& ex : m.exports)
                for (auto& [nid, addr] : ex.vars)
                    if (nid == 0x0F7C276C) {  // module_start_thread_parameter
                        mainPrio = rd32(addr + 4);
                        mainStack = rd32(addr + 8);
                        mainAttr = rd32(addr + 12);
                    }
        } else {
            u32 stop = 0;
            for (auto& ex : m.exports)
                if (ex.name.empty())
                    for (auto& [nid, addr] : ex.funcs)
                        if (nid == 0xCEE8593C) stop = addr;
            std::string fn = me.discPath;
            fn = fn.substr(fn.find_last_of('/') + 1);
            kernel::registerResidentModule(vc2::toUpper(fn), {m.moduleStart, stop, m.gp});
        }
    }
    hleBindImports();

    // ---- host
    kernel::kernelInit();
    std::string title = std::string(profile->name) + " - Recompiled";
    if (!host::init(title)) fatal("Could not create the game window (OpenGL 3.3 is required).");
    ::Bytes icon;
    if (disc().readFile("PSP_GAME/ICON0.PNG", icon)) host::setIconFromPng(icon);
    gpu::init();
    if (media::installNativeAtracDecoder()) LOG_INFO("BOOT", "ATRAC3plus decoder: native (src/runtime/media/atrac3plus)");
    audioInit();
    displayInit();

    // ---- main thread
    if (mainPrio < 0x08 || mainPrio > 0x77) mainPrio = 0x20;
    if (mainStack < 0x1000) mainStack = 0x40000;
    u32 th = kernel::createThread("user_main", mainStart, (int)mainPrio, mainStack, mainAttr, mainGp);
    const char* bootPath = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    u32 argBuf = 0;
    kernel::allocUserMemory(0x100, 1, 0, "bootargs", &argBuf);
    writeCString(argBuf, bootPath, 0x100);
    kernel::ioSetCwd("disc0:/PSP_GAME/SYSDIR");
    kernel::startThread(th, (u32)strlen(bootPath) + 1, argBuf);
    LOG_INFO("BOOT", "starting user_main at %08x (prio %x, stack %x)", mainStart, mainPrio, mainStack);

    int rc = kernel::runScheduler();
    config().save(P.configFile);
    gpu::shutdown();
    host::shutdown();
    return rc;
}

}  // namespace vc2rt

extern "C" PSP_API int vc2_run(const Vc2GameImage* image, int argc, char** argv) {
    return vc2rt::boot(image, argc, argv);
}
