#pragma once
// HLE (high level emulation) registry: PSP system library functions implemented natively.
#include <cstddef>

#include "../core/runtime.h"

namespace vc2rt {

using HleHandler = void (*)(PspContext* c);

struct HleFunction {
    u32 nid;
    const char* name;
    HleHandler fn;
};

struct HleModuleReg {
    const char* lib;
    const HleFunction* funcs;
    size_t count;
    HleModuleReg* next;
};

void registerHleModule(HleModuleReg* m);
const HleFunction* findHle(const char* lib, u32 nid);

// Binds every import of the game image (HLE handler or a guest module export).
void hleBindImports();
// Register exports of a recompiled guest module so imports resolving to it call guest code.
void hleRegisterGuestExport(const char* lib, u32 nid, u32 addr);

void hleSetTrace(bool on);

#define HLE_CONCAT2(a, b) a##b
#define HLE_CONCAT(a, b) HLE_CONCAT2(a, b)
#define HLE_MODULE(libname, table)                                                             \
    static ::vc2rt::HleModuleReg HLE_CONCAT(s_hleReg_, __LINE__){libname, table, std::size(table), nullptr}; \
    static const int HLE_CONCAT(s_hleRegI_, __LINE__) = (::vc2rt::registerHleModule(&HLE_CONCAT(s_hleReg_, __LINE__)), 0)

}  // namespace vc2rt
