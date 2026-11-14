// ModuleMgrForUser / LoadExecForUser.
//
// The modules the game loads at runtime are either statically recompiled (LIBSUPPREACC.PRX is
// already resident at its pinned address) or replaced by HLE (LIBFONT.PRX -> sceLibFont).
#include <map>

#include "../hle/hle.h"
#include "kernel.h"
#include "modules.h"

namespace vc2rt::kernel {

namespace {
struct Module : KObject {
    std::string path;
    u32 startFunc = 0, stopFunc = 0, gp = 0;
    bool started = false, hle = false;
};
std::map<std::string, ResidentModule> g_resident;  // upper-case file name -> info
u32 g_exitCallback = 0;
u32 g_mainModuleUid = 0;

std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\:");
    std::string n = s == std::string::npos ? p : p.substr(s + 1);
    for (auto& ch : n) ch = (char)toupper((unsigned char)ch);
    return n;
}

void sceKernelLoadModule(PspContext* c) {
    std::string path = readCString(A0);
    std::string name = baseName(path);
    auto m = std::make_unique<Module>();
    m->type = ObjType::Module;
    m->path = path;
    m->name = name;
    auto it = g_resident.find(name);
    if (it != g_resident.end()) {
        m->startFunc = it->second.startFunc;
        m->stopFunc = it->second.stopFunc;
        m->gp = it->second.gp;
        LOG_INFO("MODULE", "LoadModule %s -> recompiled module (start=%08x)", path.c_str(), m->startFunc);
    } else if (name == "LIBFONT.PRX") {
        m->hle = true;
        LOG_INFO("MODULE", "LoadModule %s -> HLE sceLibFont", path.c_str());
    } else {
        LOG_ERROR("MODULE", "LoadModule %s: module is not recompiled and has no HLE replacement", path.c_str());
        RET(0x8002012F);  // SCE_KERNEL_ERROR_NOFILE
        return;
    }
    RET(addObject(std::move(m)));
}

void sceKernelStartModule(PspContext* c) {
    Module* m = getObject<Module>(A0, ObjType::Module);
    if (!m) {
        RET(ERR_UNKNOWN_UID);
        return;
    }
    u32 status = 0;
    if (!m->started && m->startFunc) {
        // module_start runs synchronously in the caller's thread context on the PSP when called
        // through StartModule from user code that waits for it; do the same here.
        u32 sp = (c->r[29] - 0x400) & ~0xFu;
        status = callGuestSimple(m->startFunc, sp, m->gp, {A1, A2});
    }
    m->started = true;
    if (A3) wr32(A3, status);
    RET(A0);
}

void sceKernelStopModule(PspContext* c) {
    Module* m = getObject<Module>(A0, ObjType::Module);
    if (!m) {
        RET(ERR_UNKNOWN_UID);
        return;
    }
    u32 status = 0;
    if (m->started && m->stopFunc) status = callGuestSimple(m->stopFunc, (c->r[29] - 0x400) & ~0xFu, m->gp, {A1, A2});
    m->started = false;
    if (A3) wr32(A3, status);
    RET(0);
}

void sceKernelUnloadModule(PspContext* c) {
    removeObject(A0);
    RET(0);
}
void sceKernelGetModuleId(PspContext* c) { RET(g_mainModuleUid); }
void sceKernelGetModuleIdByAddress(PspContext* c) { RET(g_mainModuleUid); }
void sceKernelStopUnloadSelfModuleWithStatus(PspContext* c) {
    LOG_INFO("MODULE", "StopUnloadSelfModule -> exiting");
    requestExit((int)A1);
    exitCurrentThread(0, false);
}

void sceKernelExitGame(PspContext* c) {
    LOG_INFO("MODULE", "sceKernelExitGame");
    requestExit(0);
    exitCurrentThread(0, false);
    (void)c;
}
void sceKernelRegisterExitCallback(PspContext* c) {
    g_exitCallback = A0;
    RET(0);
}

const HleFunction kModMgr[] = {
    {0x977DE386, "sceKernelLoadModule", sceKernelLoadModule},
    {0x50F0C1EC, "sceKernelStartModule", sceKernelStartModule},
    {0xD1FF982A, "sceKernelStopModule", sceKernelStopModule},
    {0x2E0911AA, "sceKernelUnloadModule", sceKernelUnloadModule},
    {0xF0A26395, "sceKernelGetModuleId", sceKernelGetModuleId},
    {0xD8B73127, "sceKernelGetModuleIdByAddress", sceKernelGetModuleIdByAddress},
    {0x8F2DF740, "sceKernelStopUnloadSelfModuleWithStatus", sceKernelStopUnloadSelfModuleWithStatus},
};
const HleFunction kLoadExec[] = {
    {0x05572A5F, "sceKernelExitGame", sceKernelExitGame},
    {0x4AC57943, "sceKernelRegisterExitCallback", sceKernelRegisterExitCallback},
};
}  // namespace

void registerResidentModule(const std::string& fileName, const ResidentModule& m) { g_resident[fileName] = m; }
void setMainModuleUid(u32 uid) { g_mainModuleUid = uid; }
u32 exitCallbackId() { return g_exitCallback; }

HLE_MODULE("ModuleMgrForUser", kModMgr);
HLE_MODULE("LoadExecForUser", kLoadExec);

}  // namespace vc2rt::kernel
