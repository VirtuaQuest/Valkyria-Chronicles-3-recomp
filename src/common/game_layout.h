#pragma once
// Facts about the supported discs that both the recompiler and the runtime must agree on.
#include <cstring>

#include "types.h"

namespace vc2 {

struct GameModuleSpec {
    const char* discPath;
    u32 base;  // load address chosen for the relocatable module
};

struct GameProfile {
    const char* discId;        // PARAM.SFO DISC_ID
    const char* name;          // display name (plain text, used for window titles / logs)
    const char* exeName;       // file name of the built executable
    GameModuleSpec modules[4]; // first entry is the main executable; {nullptr} terminates
    // Guest functions whose recompiled bodies call psp_hook(c, addr) on entry, so the runtime can
    // adjust arguments (e.g. the engine's frame-rate request). 0 terminates.
    u32 hooks[4];
};

// The main executable is loaded where the PSP kernel would place it. Extra game-supplied PRX
// modules are pinned directly after it (the runtime reserves that range before the game starts
// so sceKernelLoadModule can return it).
inline constexpr GameProfile kGameProfiles[] = {
    // Valkyria Chronicles III (Japan, v1.01). Discs patched with the English fan translation keep
    // the decrypted original executable as EBOOT.OLD and replace EBOOT.BIN with a custom-firmware
    // loader (PROMETHEUS), which a recompilation does not need.
    {"ULJM05781", "Valkyria Chronicles III", "VC3Recompiled.exe",
     {{"PSP_GAME/SYSDIR/EBOOT.OLD", 0x08804000},
      {nullptr, 0}},
     // 0x08bc3ebc: engine "set frame rate" (a0 = vblanks per frame; it also derives the float
     // time step that game logic scales by). Hooked for the 60 FPS gameplay option.
     {0x08bc3ebc, 0}},
};

inline const GameProfile* findGameProfile(const char* discId) {
    for (const auto& p : kGameProfiles)
        if (discId && strcmp(p.discId, discId) == 0) return &p;
    return nullptr;
}

inline size_t profileModuleCount(const GameProfile& p) {
    size_t n = 0;
    while (n < 4 && p.modules[n].discPath) n++;
    return n;
}

}  // namespace vc2
