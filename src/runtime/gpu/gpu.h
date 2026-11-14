#pragma once
// GE (PSP Graphics Engine) emulation interface. Implemented in src/runtime/gpu/.
#include "../core/runtime.h"

namespace vc2rt::gpu {

void init();      // host GL context is current
void shutdown();

// sceGe_user backend
u32 listEnqueue(u32 list, u32 stall, s32 cbid, u32 optAddr, bool head);
int listUpdateStall(u32 id, u32 stall);
int listSync(u32 id, u32 mode);
int drawSync(u32 mode);
int setCallback(u32 cbDataAddr);
void unsetCallback(int id);
int geBreak(u32 mode, u32 param);
int geContinue();
u32 getCmd(u32 cmd);   // sceGeGetCmd
int getMtx(u32 type, u32 dst);
int saveContextTo(u32 addr);       // sceGeSaveContext
int restoreContextFrom(u32 addr);  // sceGeRestoreContext
// The game's $gp at the last sceGe call; GE interrupt handlers run with it.
void setCallerGp(u32 gp);

// sceDisplay backend
void setDisplayFramebuffer(u32 addr, u32 stride, u32 pixelFormat);
// Called once per vblank on the host thread: draws the displayed framebuffer to the window,
// the overlay on top, and swaps.
void present();

}  // namespace vc2rt::gpu
