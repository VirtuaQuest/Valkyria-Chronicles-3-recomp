#include "hle.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../kernel/kernel.h"

namespace vc2rt {

static HleModuleReg* g_modules = nullptr;
static bool g_trace = false;

void registerHleModule(HleModuleReg* m) {
    m->next = g_modules;
    g_modules = m;
}

const HleFunction* findHle(const char* lib, u32 nid) {
    for (HleModuleReg* m = g_modules; m; m = m->next)
        if (strcmp(m->lib, lib) == 0)
            for (size_t i = 0; i < m->count; i++)
                if (m->funcs[i].nid == nid) return &m->funcs[i];
    return nullptr;
}

struct Binding {
    const HleFunction* hle = nullptr;
    u32 guestAddr = 0;
    const char* lib = "";
    u32 nid = 0;
    u32 calls = 0;
};
static std::vector<Binding> g_bindings;
static std::map<std::pair<std::string, u32>, u32> g_guestExports;

void hleRegisterGuestExport(const char* lib, u32 nid, u32 addr) { g_guestExports[{lib, nid}] = addr; }
void hleSetTrace(bool on) { g_trace = on; }

void hleBindImports() {
    const Vc2GameImage& img = gameImage();
    g_bindings.assign(img.importCount, {});
    int missing = 0;
    for (u32 i = 0; i < img.importCount; i++) {
        const PspImportEntry& e = img.imports[i];
        Binding& b = g_bindings[i];
        b.lib = e.lib;
        b.nid = e.nid;
        auto g = g_guestExports.find({e.lib, e.nid});
        if (g != g_guestExports.end()) {
            b.guestAddr = g->second;
            continue;
        }
        b.hle = findHle(e.lib, e.nid);
        if (!b.hle) {
            missing++;
            LOG_WARN("HLE", "no implementation for %s:%08x (stub %08x)", e.lib, e.nid, e.stubAddr);
        }
    }
    LOG_INFO("HLE", "bound %u imports (%d unimplemented)", img.importCount, missing);
}

}  // namespace vc2rt

using namespace vc2rt;

extern "C" PSP_API void psp_hle_call(PspContext* c, u32 idx) {
    Binding& b = g_bindings[idx];
    if (b.guestAddr) {
        callGuest(c, b.guestAddr);
        return;
    }
    if (!b.hle) {
        if (b.calls++ < 4)
            LOG_WARN("HLE", "unimplemented %s:%08x called (a0=%08x a1=%08x ra=%08x)", b.lib, b.nid, c->r[4], c->r[5],
                     c->r[31]);
        c->r[2] = 0;
        return;
    }
    if (g_trace)
        LOG_DEBUG("HLE", "%s(%08x, %08x, %08x, %08x) thread=%u ra=%08x", b.hle->name, c->r[4], c->r[5], c->r[6], c->r[7],
                  c->threadId, c->r[31]);
    b.calls++;
    if (kernel::Thread* t = kernel::currentThread()) {
        t->lastHle = b.hle->name;
        t->lastHleRa = c->r[31];
        t->hleCalls++;
    }
    b.hle->fn(c);
    if (g_trace) LOG_DEBUG("HLE", "  -> %08x", c->r[2]);
    kernel::afterHleCall(c);
}
