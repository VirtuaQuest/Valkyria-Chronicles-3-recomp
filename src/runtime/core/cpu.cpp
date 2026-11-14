// Glue between recompiled code and the runtime: indirect dispatch, traps, guest calls.
#include <algorithm>
#include <vector>

#include "../kernel/kernel.h"
#include "runtime.h"

namespace vc2rt {

static const Vc2GameImage* g_image = nullptr;
static std::vector<PspFunc> g_dispatch;  // indexed by (addr - g_dispatchBase) / 4
static u32 g_dispatchBase = 0, g_dispatchEnd = 0;

const Vc2GameImage& gameImage() { return *g_image; }

void cpuInit(const Vc2GameImage* img) {
    g_image = img;
    g_dispatchBase = 0xFFFFFFFF;
    g_dispatchEnd = 0;
    for (u32 i = 0; i < img->funcCount; i++) {
        g_dispatchBase = std::min(g_dispatchBase, img->funcs[i].addr);
        g_dispatchEnd = std::max(g_dispatchEnd, img->funcs[i].addr + img->funcs[i].size);
    }
    g_dispatch.assign((g_dispatchEnd - g_dispatchBase) / 4 + 1, nullptr);
    for (u32 i = 0; i < img->funcCount; i++) g_dispatch[(img->funcs[i].addr - g_dispatchBase) / 4] = img->funcs[i].fn;
}

PspFunc findFunction(u32 addr) {
    addr &= 0x3FFFFFFF;  // kernel/uncached mirrors of code addresses
    if (addr < g_dispatchBase || addr >= g_dispatchEnd || (addr & 3)) return nullptr;
    return g_dispatch[(addr - g_dispatchBase) / 4];
}

void callGuest(PspContext* ctx, u32 addr) {
    PspFunc f = findFunction(addr);
    if (!f) fatal("Call to unknown guest address 0x%08x (ra=0x%08x)", addr, ctx->r[31]);
    f(ctx);
}

u32 callGuestSimple(u32 addr, u32 sp, u32 gp, std::initializer_list<u32> args) {
    PspContext ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.r[29] = sp;
    ctx.r[28] = gp;
    ctx.r[31] = 0;
    int i = 4;
    for (u32 a : args) ctx.r[i++] = a;
    ctx.threadId = kernel::currentThreadId();
    callGuest(&ctx, addr);
    return ctx.r[2];
}

}  // namespace vc2rt

using namespace vc2rt;

extern "C" {

PSP_API void psp_call_indirect(PspContext* c, u32 addr) {
    PspFunc f = findFunction(addr);
    if (!f) fatal("Indirect call to 0x%08x has no recompiled function (from pc~0x%08x)", addr, c->r[31] - 8);
    f(c);
}

PSP_API void psp_jump_indirect(PspContext* c, u32 addr) {
    // Indirect tail jump: behaves like a call whose return is our return.
    PspFunc f = findFunction(addr);
    if (!f) fatal("Indirect jump to 0x%08x has no recompiled function", addr);
    f(c);
}

PSP_API void psp_syscall(PspContext* c, u32 pc, u32 code) {
    LOG_WARN("CPU", "raw syscall 0x%x at %08x ignored", code, pc);
    (void)c;
}

PSP_API void psp_break(PspContext* c, u32 pc, u32 code) {
    LOG_ERROR("CPU", "break 0x%x at %08x (ra=%08x)", code, pc, c->r[31]);
}

PSP_API void psp_unhandled(PspContext* c, u32 pc, u32 insn) {
    static int count = 0;
    if (count++ < 64) LOG_ERROR("CPU", "unhandled instruction %08x at %08x", insn, pc);
    (void)c;
}

PSP_API u32 psp_mfic(PspContext* c) {
    (void)c;
    return kernel::interruptsEnabled() ? 1 : 0;
}
PSP_API void psp_mtic(PspContext* c, u32 v) {
    (void)c;
    kernel::setInterruptsEnabled(v != 0);
}

}  // extern "C"
