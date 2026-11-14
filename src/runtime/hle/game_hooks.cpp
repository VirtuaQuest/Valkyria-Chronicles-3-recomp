// Entry hooks for specific recompiled game functions (see GameProfile::hooks in
// src/common/game_layout.h). The generated code calls psp_hook(c, addr) before the function body,
// so a hook can inspect or adjust the guest arguments.
#include "../host/config.h"
#include "hle.h"

using namespace vc2rt;

namespace {

// Valkyria Chronicles III, engine "set frame rate" (a0 = vblanks per rendered frame). The engine
// derives its float time step from the same value, and game logic scales by that step, so
// requesting 1 instead of 2 renders 30 FPS scenes at 60 FPS without changing game speed.
void vc3SetFrameRate(PspContext* c) {
    static u32 lastLogged = 0;
    const u32 requested = c->r[4];
    if (config().highFrameRate && requested == 2) c->r[4] = 1;
    if (requested != lastLogged) {
        LOG_INFO("HOOK", "frame rate request: every %u vblank(s)%s", requested,
                 c->r[4] != requested ? " -> 1 (60 FPS gameplay)" : "");
        lastLogged = requested;
    }
}

}  // namespace

extern "C" PSP_API void psp_hook(PspContext* c, u32 addr) {
    switch (addr) {
        case 0x08bc3ebc: vc3SetFrameRate(c); break;
        default: break;
    }
}
