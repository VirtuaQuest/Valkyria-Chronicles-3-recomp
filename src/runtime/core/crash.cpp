// Crash reporting: translates native faults inside recompiled code back to guest terms.
#include <windows.h>

#include <algorithm>
#include <vector>

#include "../kernel/kernel.h"
#include "runtime.h"

namespace vc2rt {

namespace {
struct NativeFn {
    uintptr_t native;
    u32 guest;
};
std::vector<NativeFn> g_byNative;

const NativeFn* guestForNative(uintptr_t rip) {
    if (g_byNative.empty()) return nullptr;
    auto it = std::upper_bound(g_byNative.begin(), g_byNative.end(), rip,
                               [](uintptr_t a, const NativeFn& f) { return a < f.native; });
    if (it == g_byNative.begin()) return nullptr;
    return &*(it - 1);
}

// Debug: VC2_WATCH=<psp address> arms an x86 hardware write watchpoint (DR0, 4 bytes) on that
// guest address and logs each distinct guest function that writes it.
u32 g_watchAddr = 0;

LONG WINAPI onException(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 4)) {
        // VC2_PRINTF: the game's (release-disabled) debug printf; format it into our log.
        const PspContext* gc = (const PspContext*)ep->ContextRecord->Rcx;
        std::string fmt = readCString(gc->r[4], 256), out;
        int arg = 5;
        for (size_t i = 0; i < fmt.size(); i++) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) {
                out += fmt[i];
                continue;
            }
            size_t j = i + 1;
            while (j < fmt.size() && strchr("0123456789.-+ l", fmt[j])) j++;
            char conv = j < fmt.size() ? fmt[j] : 'd';
            u32 v = arg <= 11 ? gc->r[arg++] : 0;
            char buf[300];
            if (conv == 's') snprintf(buf, sizeof buf, "%s", readCString(v, 256).c_str());
            else if (conv == 'x' || conv == 'X' || conv == 'p') snprintf(buf, sizeof buf, "%x", v);
            else if (conv == 'f') snprintf(buf, sizeof buf, "<f>");
            else if (conv == '%') { snprintf(buf, sizeof buf, "%%"); arg--; }
            else snprintf(buf, sizeof buf, "%d", (s32)v);
            out += buf;
            i = j;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        LOG_INFO("GAMELOG", "%s", out.c_str());
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && (ep->ContextRecord->Dr6 & 0xA)) {
        // VC2_BREAK / VC2_BREAK2 execute breakpoints: count calls of a guest function
        static u64 hits[2] = {0, 0};
        int which = (ep->ContextRecord->Dr6 & 2) ? 0 : 1;
        u64 h = ++hits[which];
        if ((h & (h - 1)) == 0) {  // log at powers of two, with the guest arguments
            const PspContext* gc = (const PspContext*)ep->ContextRecord->Rcx;
            LOG_INFO("WATCH", "break%s function called %llu times (a0=%08x a1=%08x a2=%08x ra=%08x)", which ? "2" : "",
                     (unsigned long long)h, gc->r[4], gc->r[5], gc->r[6], gc->r[31]);
        }
        ep->ContextRecord->Dr6 = 0;
        ep->ContextRecord->EFlags |= 0x10000;  // resume flag: don't re-trigger on this instruction
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_watchAddr && (ep->ContextRecord->Dr6 & 1)) {
        static std::vector<uintptr_t> seen;
        uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
        const NativeFn* f = guestForNative(rip);
        if (std::find(seen.begin(), seen.end(), f ? f->native : rip) == seen.end() && seen.size() < 64) {
            seen.push_back(f ? f->native : rip);
            u32 val = rd32(g_watchAddr);
            LOG_INFO("WATCH", "write to %08x by %s%08x (value now %08x)", g_watchAddr, f ? "f_" : "host ",
                     f ? f->guest : (u32)rip, val);
        }
        ep->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    const NativeFn* f = guestForNative(rip);
    HMODULE exe = GetModuleHandleW(nullptr);
    char where[160] = "outside recompiled code";
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)rip, &mbi, sizeof mbi) && mbi.AllocationBase == exe && f)
        snprintf(where, sizeof where, "guest function f_%08x (+0x%llx native)", f->guest,
                 (unsigned long long)(rip - f->native));
    char fault[96] = "";
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        if (a >= (uintptr_t)psp_mem && a < (uintptr_t)psp_mem + 0x20000000)
            snprintf(fault, sizeof fault, "%s PSP address 0x%08llx", write ? "writing" : "reading",
                     (unsigned long long)(a - (uintptr_t)psp_mem));
        else
            snprintf(fault, sizeof fault, "%s host address %p", write ? "writing" : "reading", (void*)a);
    }
    kernel::Thread* t = kernel::currentThread();
    fatal("Native exception 0x%08lx %s\nat %p, %s\nthread: %s\n\nSee vc3recomp.log for details.", code, fault, (void*)rip,
          where, t ? t->name.c_str() : "(scheduler)");
}
}  // namespace

void crashInit(const Vc2GameImage* img) {
    g_byNative.reserve(img->funcCount);
    for (u32 i = 0; i < img->funcCount; i++) g_byNative.push_back({(uintptr_t)img->funcs[i].fn, img->funcs[i].addr});
    std::sort(g_byNative.begin(), g_byNative.end(), [](const NativeFn& a, const NativeFn& b) { return a.native < b.native; });
    AddVectoredExceptionHandler(0, onException);
}

// Arms the watchpoint on the calling OS thread (fibers share it). Call after memInit().
void watchInit() {
    const char* w = getenv("VC2_WATCH");
    const char* b = getenv("VC2_BREAK");
    const char* p = getenv("VC2_PRINTF");
    if (!w && !b && !p && !getenv("VC2_BREAK2")) return;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    HANDLE th = GetCurrentThread();
    GetThreadContext(th, &ctx);
    ctx.Dr7 = 0;
    if (w) {
        g_watchAddr = (u32)strtoul(w, nullptr, 16);
        ctx.Dr0 = (DWORD64)(uintptr_t)memPtr(g_watchAddr);
        ctx.Dr7 |= 1 | (1ull << 16) | (3ull << 18);  // L0, RW0=write, LEN0=4 bytes
    }
    if (b) {
        PspFunc f = findFunction((u32)strtoul(b, nullptr, 16));
        if (f) {
            ctx.Dr1 = (DWORD64)(uintptr_t)f;
            ctx.Dr7 |= 4;  // L1, RW1=execute, LEN1=1
        }
    }
    if (const char* b2 = getenv("VC2_BREAK2")) {
        PspFunc f = findFunction((u32)strtoul(b2, nullptr, 16));
        if (f) {
            ctx.Dr3 = (DWORD64)(uintptr_t)f;
            ctx.Dr7 |= 0x40;  // L3, RW3=execute
        }
    }
    if (p) {
        PspFunc f = findFunction((u32)strtoul(p, nullptr, 16));
        if (f) {
            ctx.Dr2 = (DWORD64)(uintptr_t)f;
            ctx.Dr7 |= 0x10;  // L2, RW2=execute
        }
    }
    if (SetThreadContext(th, &ctx)) LOG_INFO("WATCH", "debug registers armed (watch=%s break=%s)", w ? w : "-", b ? b : "-");
}

}  // namespace vc2rt
