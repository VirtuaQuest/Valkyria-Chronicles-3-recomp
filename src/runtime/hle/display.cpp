// sceDisplay (vblank timing, framebuffer selection) and sceCtrl (pad sampling).
#include <map>
#include <string>
#include <vector>

#include "../gpu/gpu.h"
#include "../host/config.h"
#include "../host/host.h"
#include "../kernel/kernel.h"
#include "display.h"
#include "hle.h"

namespace vc2rt {

namespace {
using namespace kernel;

constexpr u64 kVblankPeriod = 16683;  // 59.94 Hz
u32 g_vcount = 0;
u64 g_lastVblank = 0;
struct Fb {
    u32 addr = 0, stride = 512, fmt = 3;
} g_fb, g_pendingFb;
bool g_fbPending = false;
int g_mode = 0, g_width = 480, g_height = 272;

// ctrl
u32 g_ctrlCycle = 0, g_ctrlMode = 0;
struct Sample {
    u32 ts, buttons;
    u8 lx, ly;
};
Sample g_samples[64];
u32 g_sampleIdx = 0, g_sampleCount = 0;

// Debug/experiment: VC2_POKE=addr:from:to[,...] rewrites a guest word from `from` to `to` at every
// vblank (between game frames). Used to test engine frame-pacing changes.
void applyPokes() {
    struct Poke {
        u32 addr, from, to;
    };
    static std::vector<Poke> pokes = [] {
        std::vector<Poke> v;
        if (const char* e = getenv("VC2_POKE")) {
            std::string s = e;
            size_t i = 0;
            while (i < s.size()) {
                size_t j = s.find(',', i);
                if (j == std::string::npos) j = s.size();
                std::string item = s.substr(i, j - i);
                unsigned a = 0, f = 0, t = 0;
                if (sscanf(item.c_str(), "%x:%x:%x", &a, &f, &t) == 3) v.push_back({a, f, t});
                i = j + 1;
            }
        }
        return v;
    }();
    for (auto& p : pokes)
        if (rd32(p.addr) == p.from) wr32(p.addr, p.to);
}

void vblank(u64 now) {
    g_vcount++;
    applyPokes();
    g_lastVblank = now;
    if (g_fbPending) {
        g_fb = g_pendingFb;
        g_fbPending = false;
        gpu::setDisplayFramebuffer(g_fb.addr, g_fb.stride, g_fb.fmt);
    }
    if (!host::pumpEvents()) {
        LOG_INFO("DISPLAY", "window closed");
        requestExit(0);
        return;
    }
    // sample the pad once per vblank (the PSP default sampling cycle)
    host::PadState p = host::pad();
    Sample& s = g_samples[g_sampleIdx];
    s.ts = (u32)pspTimeMicros();
    s.buttons = p.buttons;
    s.lx = p.lx;
    s.ly = p.ly;
    g_sampleIdx = (g_sampleIdx + 1) % 64;
    g_sampleCount = std::min<u32>(g_sampleCount + 1, 64);

    gpu::present();
    int woke = wakeWaiters(WaitType::Vblank, 0, 0, false);
    if (g_vcount % 300 == 0) LOG_DEBUG("DISPLAY", "vblank %u (woke %d)", g_vcount, woke);
    runSubIntrHandlers(30 /* PSP_VBLANK_INT */, g_vcount);
}

s32 waitVblank(bool cb) { return waitCurrent(WaitType::Vblank, 0, 0, cb); }

void sceDisplaySetMode(PspContext* c) {
    g_mode = (int)A0;
    g_width = (int)A1;
    g_height = (int)A2;
    RET(0);
}
void sceDisplayGetMode(PspContext* c) {
    if (A0) wr32(A0, g_mode);
    if (A1) wr32(A1, g_width);
    if (A2) wr32(A2, g_height);
    RET(0);
}
// Game framerate = how often the game flips to a different framebuffer (the window itself always
// presents at the vblank rate). Shown in the FPS overlay; VC2_FPS_STATS=1 also logs it.
void countFlip(u32 addr) {
    static u32 lastAddr = 0, flips = 0;
    static u64 windowStart = 0;
    static const bool logFps = getenv("VC2_FPS_STATS") != nullptr;
    if (addr != lastAddr) {
        flips++;
        lastAddr = addr;
    }
    const u64 now = hostMicros();
    if (!windowStart) windowStart = now;
    if (now - windowStart >= 1000000) {
        const float fps = flips * 1e6f / (float)(now - windowStart);
        host::setGameFps(fps);
        if (logFps) LOG_INFO("DISPLAY", "game fps %.1f (vcount %u)", fps, g_vcount);
        flips = 0;
        windowStart = now;
    }
}

void sceDisplaySetFrameBuf(PspContext* c) {
    Fb f{A0, A1, A2};
    countFlip(f.addr);
    static u32 lastLogged = 0xFFFFFFFF;
    if (f.addr != lastLogged) {
        LOG_DEBUG("DISPLAY", "framebuffer -> %08x stride %u fmt %u (sync %u)", f.addr, f.stride, f.fmt, A3);
        lastLogged = f.addr;
    }
    if (A3 == 0) {  // PSP_DISPLAY_SETBUF_IMMEDIATE
        g_fb = f;
        g_fbPending = false;
        gpu::setDisplayFramebuffer(f.addr, f.stride, f.fmt);
    } else {
        g_pendingFb = f;
        g_fbPending = true;
    }
    RET(0);
}
void sceDisplayGetFrameBuf(PspContext* c) {
    const Fb& f = (A3 == 0 && g_fbPending) ? g_pendingFb : g_fb;
    if (A0) wr32(A0, f.addr);
    if (A1) wr32(A1, f.stride);
    if (A2) wr32(A2, f.fmt);
    RET(0);
}
// VC2_FPS_STATS: which game code waits for vblanks (return addresses), to find frame pacing.
void noteVblankCaller(PspContext* c) {
    static const bool on = getenv("VC2_FPS_STATS") != nullptr;
    if (!on) return;
    static std::map<std::pair<u32, u32>, u32> counts;  // (thread, ra) -> calls
    static u64 last = 0;
    counts[{c->threadId, c->r[31]}]++;
    const u64 now = hostMicros();
    if (now - last >= 5000000) {
        last = now;
        std::string s;
        for (auto& [k, n] : counts) {
            char b[48];
            snprintf(b, sizeof b, " t%x@%08x:%u", k.first, k.second, n);
            s += b;
        }
        LOG_INFO("DISPLAY", "vblank waits (5s):%s", s.c_str());
        counts.clear();
    }
}
void sceDisplayWaitVblankStart(PspContext* c) {
    noteVblankCaller(c);
    RET(waitVblank(false));
}
void sceDisplayWaitVblankStartCB(PspContext* c) {
    noteVblankCaller(c);
    RET(waitVblank(true));
}
void sceDisplayGetVcount(PspContext* c) { RET(g_vcount); }
void sceDisplayIsVblank(PspContext* c) {
    // vblank lasts ~0.73ms of each 16.68ms frame
    RET((hostMicros() - g_lastVblank) < 730 ? 1 : 0);
}
void sceDisplayGetFramePerSec(PspContext* c) {
    float f = 59.94f;
    u32 bits;
    memcpy(&bits, &f, 4);
    c->fi[0] = bits;
    RET(bits);
}

const HleFunction kDisplay[] = {
    {0x0E20F177, "sceDisplaySetMode", sceDisplaySetMode},
    {0xDEA197D4, "sceDisplayGetMode", sceDisplayGetMode},
    {0x289D82FE, "sceDisplaySetFrameBuf", sceDisplaySetFrameBuf},
    {0xEEDA2E54, "sceDisplayGetFrameBuf", sceDisplayGetFrameBuf},
    {0x984C27E7, "sceDisplayWaitVblankStart", sceDisplayWaitVblankStart},
    {0x46F186C3, "sceDisplayWaitVblankStartCB", sceDisplayWaitVblankStartCB},
    {0x36CDFADE, "sceDisplayWaitVblank", sceDisplayWaitVblankStart},
    {0x8EB9EC49, "sceDisplayWaitVblankCB", sceDisplayWaitVblankStartCB},
    {0x9C6EAAD7, "sceDisplayGetVcount", sceDisplayGetVcount},
    {0x4D4E10EC, "sceDisplayIsVblank", sceDisplayIsVblank},
    {0xDBA6C4C4, "sceDisplayGetFramePerSec", sceDisplayGetFramePerSec},
};

// ---------------------------------------------------------------- sceCtrl
void writeSamples(u32 dst, u32 count, bool negative) {
    count = std::clamp<u32>(count, 1, 64);
    for (u32 i = 0; i < count; i++) {
        // newest sample last
        u32 idx = (g_sampleIdx + 64 - count + i) % 64;
        const Sample& s = g_samples[idx];
        u32 p = dst + i * 16;
        wr32(p, s.ts);
        wr32(p + 4, negative ? ~s.buttons : s.buttons);
        // analog is reported only in analog sampling mode
        wr8(p + 8, g_ctrlMode ? s.lx : 128);
        wr8(p + 9, g_ctrlMode ? s.ly : 128);
        for (u32 k = 10; k < 16; k++) wr8(p + k, 0);
    }
}
void sceCtrlSetSamplingCycle(PspContext* c) {
    RET(g_ctrlCycle);
    g_ctrlCycle = A0;
}
void sceCtrlSetSamplingMode(PspContext* c) {
    RET(g_ctrlMode);
    g_ctrlMode = A0;
}
void sceCtrlGetSamplingMode(PspContext* c) {
    if (A0) wr32(A0, g_ctrlMode);
    RET(0);
}
void sceCtrlPeekBufferPositive(PspContext* c) {
    u32 n = std::clamp<u32>(A1, 1, 64);
    writeSamples(A0, n, false);
    RET(n);
}
void sceCtrlPeekBufferNegative(PspContext* c) {
    u32 n = std::clamp<u32>(A1, 1, 64);
    writeSamples(A0, n, true);
    RET(n);
}
void sceCtrlReadBufferPositive(PspContext* c) {
    waitVblank(false);
    u32 n = std::clamp<u32>(A1, 1, 64);
    writeSamples(A0, n, false);
    RET(n);
}
void sceCtrlReadBufferNegative(PspContext* c) {
    waitVblank(false);
    u32 n = std::clamp<u32>(A1, 1, 64);
    writeSamples(A0, n, true);
    RET(n);
}
void sceCtrlPeekLatch(PspContext* c) {
    static u32 prev = 0;
    u32 cur = g_samples[(g_sampleIdx + 63) % 64].buttons;
    if (A0) {
        wr32(A0, cur & ~prev);       // make
        wr32(A0 + 4, prev & ~cur);   // break
        wr32(A0 + 8, cur);           // press
        wr32(A0 + 12, ~cur);         // release
    }
    prev = cur;
    RET(1);
}

const HleFunction kCtrl[] = {
    {0x6A2774F3, "sceCtrlSetSamplingCycle", sceCtrlSetSamplingCycle},
    {0x1F4011E6, "sceCtrlSetSamplingMode", sceCtrlSetSamplingMode},
    {0xDA6B76A1, "sceCtrlGetSamplingMode", sceCtrlGetSamplingMode},
    {0x3A622550, "sceCtrlPeekBufferPositive", sceCtrlPeekBufferPositive},
    {0xC152080A, "sceCtrlPeekBufferNegative", sceCtrlPeekBufferNegative},
    {0x1F803938, "sceCtrlReadBufferPositive", sceCtrlReadBufferPositive},
    {0x60B81F86, "sceCtrlReadBufferNegative", sceCtrlReadBufferNegative},
    {0xB1D0E5CD, "sceCtrlPeekLatch", sceCtrlPeekLatch},
};
}  // namespace

void displayInit() {
    for (auto& s : g_samples) s = {0, 0, 128, 128};
    // The vblank period shrinks while fast-forward is held / frame limit is changed.
    addPeriodicTimer(kVblankPeriod, [](u64 now) { vblank(now); });
}

HLE_MODULE("sceDisplay", kDisplay);
HLE_MODULE("sceCtrl", kCtrl);

}  // namespace vc2rt
