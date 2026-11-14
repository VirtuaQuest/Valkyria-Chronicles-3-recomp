// GE display list processor: list queue, command interpreter, signals/finish interrupts, sync.
//
// Lists execute synchronously on the calling (game) thread whenever the GE could make progress:
// on enqueue, on stall-address updates, on sceGeContinue and after interrupt handlers return.
// Signal and finish handlers run like PSP sub-interrupts (on the interrupt stack, with the
// game's handler/arg) right at the point the GE raised them; if the game has interrupts
// disabled at that moment they are queued and delivered as soon as it can take them.
#include <deque>

#include "../kernel/kernel.h"
#include "gpu.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

GeState g_ge;
u32 g_dirty = DIRTY_ALL;
u64 g_frame = 0;
u32 g_hashGen = 1;
u32 g_vaddr = 0, g_iaddr = 0;
bool g_tracing = false;  // tracing the current list (first list at/after VC2_GE_TRACE frame)
GpuStats g_stats;

namespace {

constexpr u32 kListIdMagic = 0x35000000;
constexpr int kMaxLists = 64;

enum DlState { DL_NONE = 0, DL_QUEUED = 1, DL_RUNNING = 2, DL_COMPLETED = 3, DL_PAUSED = 4 };
enum Signal : u32 {
    SIG_NONE = 0x00, SIG_SUSPEND = 0x01, SIG_CONTINUE = 0x02, SIG_PAUSE = 0x03, SIG_SYNC = 0x08,
    SIG_JUMP = 0x10, SIG_CALL = 0x11, SIG_RET = 0x12, SIG_RJUMP = 0x13, SIG_RCALL = 0x14,
    SIG_OJUMP = 0x15, SIG_OCALL = 0x16,
};
enum ListStatus { LS_COMPLETED = 0, LS_QUEUED = 1, LS_DRAWING = 2, LS_STALLING = 3, LS_PAUSED = 4 };

struct StackEntry {
    u32 pc, offsetAddr, baseAddr;  // baseAddr == 0 marks a regular CALL
};

struct DList {
    int id = 0;
    DlState state = DL_NONE;
    u32 startPc = 0, pc = 0, stall = 0;
    int cbid = -1;
    StackEntry stack[32];
    int sp = 0;
    u32 offsetAddr = 0;
    u32 context = 0;
    u32 signal = SIG_NONE;
    bool started = false, interrupted = false, bboxResult = true;
    int hardwareCallDepth() const {
        int d = 0;
        for (int i = sp - 1; i >= 0 && stack[i].baseAddr == 0; --i) d++;
        return d;
    }
};

DList g_dls[kMaxLists];
std::deque<int> g_queue;
DList* g_cur = nullptr;
int g_nextId = 0;
bool g_processing = false;
bool g_isBreak = false;

struct Callbacks {
    bool used = false;
    u32 signalFunc = 0, signalArg = 0, finishFunc = 0, finishArg = 0;
};
Callbacks g_cbs[16];

struct PendingIntr {
    int cbid;
    bool finish;
    u32 token, pc;
};
std::deque<PendingIntr> g_pending;
int g_intrDepth = 0;
u32 g_callerGp = 0;
u64 g_traceFrame = ~0ull;
bool g_geLog = false;

// Command classes for dirty tracking. 0 = consumed on the CPU side only (vertex transform etc.).
u8 g_cmdDirty[256];

void initCmdTable() {
    for (int i = 0; i < 256; i++) g_cmdDirty[i] = DIRTY_RASTER;
    const u8 cpuOnly[] = {CMD_NOP, CMD_VADDR, CMD_IADDR, CMD_PRIM, CMD_BEZIER, CMD_SPLINE, CMD_BBOX,
                          CMD_JUMP, CMD_BJUMP, CMD_CALL, CMD_RET, CMD_END, CMD_SIGNAL, CMD_FINISH,
                          CMD_BASE, CMD_OFFSETADDR, CMD_ORIGIN, CMD_LIGHTING, 0x18, 0x19, 0x1A, 0x1B,
                          CMD_BONENUM, CMD_BONEDATA, CMD_PATCHDIV, CMD_PATCHPRIM, CMD_PATCHFACING,
                          CMD_PATCHCULL, CMD_REVNORMAL, CMD_FOG1, CMD_FOG2, CMD_TEXSYNC,
                          CMD_TRXSRC, CMD_TRXSRCW, CMD_TRXDST, CMD_TRXDSTW, CMD_TRXKICK, CMD_TRXSRCPOS,
                          CMD_TRXDSTPOS, CMD_TRXSIZE};
    for (u8 c : cpuOnly) g_cmdDirty[c] = 0;
    for (int c = CMD_MORPH0; c < CMD_MORPH0 + 8; c++) g_cmdDirty[c] = 0;
    for (int c = CMD_WORLDNUM; c <= CMD_TEXOFFV; c++) g_cmdDirty[c] = 0;  // matrices, viewport, uv scale
    for (int c = CMD_MATUPDATE; c <= CMD_LCOLOR0 + 11; c++) g_cmdDirty[c] = 0;  // material + lights
    g_cmdDirty[CMD_OFFSETX] = g_cmdDirty[CMD_OFFSETY] = 0;
    for (int c = 0xF0; c <= 0xF9; c++) g_cmdDirty[c] = 0;
    for (int c = CMD_TEXADDR0; c < CMD_TEXADDR0 + 16; c++) g_cmdDirty[c] = DIRTY_TEXTURE | DIRTY_RASTER;
    for (int c = CMD_TEXSIZE0; c < CMD_TEXSIZE0 + 8; c++) g_cmdDirty[c] = DIRTY_TEXTURE | DIRTY_RASTER;
    const u8 texCmds[] = {CMD_CLUTADDR, CMD_CLUTADDRUPPER, CMD_TEXMODE, CMD_TEXFORMAT, CMD_LOADCLUT,
                          CMD_CLUTFORMAT, CMD_TEXFILTER, CMD_TEXWRAP, CMD_TEXLEVEL, CMD_TEXLODSLOPE,
                          CMD_TEXFLUSH, CMD_TEXMAPMODE};
    for (u8 c : texCmds) g_cmdDirty[c] = DIRTY_TEXTURE | DIRTY_RASTER;
    const u8 fbCmds[] = {CMD_FBPTR, CMD_FBWIDTH, CMD_FBPIXFMT, CMD_ZBPTR, CMD_ZBWIDTH};
    for (u8 c : fbCmds) g_cmdDirty[c] = DIRTY_FRAMEBUF | DIRTY_RASTER;
}

void resetState() {
    memset(&g_ge, 0, sizeof g_ge);
    for (int i = 0; i < 256; i++) g_ge.cmd[i] = (u32)i << 24;
    // identity-ish defaults so a stray draw before setup is not NaN soup
    const float id43[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    memcpy(g_ge.world, id43, sizeof id43);
    memcpy(g_ge.view, id43, sizeof id43);
    memcpy(g_ge.tgen, id43, sizeof id43);
    for (int b = 0; b < 8; b++) memcpy(g_ge.bone + b * 12, id43, sizeof id43);
    const float id44[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(g_ge.proj, id44, sizeof id44);
    g_ge.morph[0] = 1.0f;
    g_ge.cmd[CMD_CLUTFORMAT] = (CMD_CLUTFORMAT << 24) | 0x00FF00;
    g_ge.cmd[CMD_MASKALPHA] = (CMD_MASKALPHA << 24);
    g_dirty = DIRTY_ALL;
}

u32 intrStack() { return kernel::interruptStackTop() - 0x4000 - (u32)g_intrDepth * 0x4000; }

void runHandler(const PendingIntr& p) {
    if (g_geLog) LOG_INFO("GELOG", "f%llu %s handler token %x", (unsigned long long)g_frame, p.finish ? "finish" : "signal", p.token);
    if (p.cbid < 0 || p.cbid >= 16 || !g_cbs[p.cbid].used) return;
    const Callbacks& cb = g_cbs[p.cbid];
    u32 fn = p.finish ? cb.finishFunc : cb.signalFunc;
    u32 arg = p.finish ? cb.finishArg : cb.signalArg;
    if (!fn) return;
    // The handler may draw (enqueue lists) and read rendered results; make sure GL has them.
    flushDraws();
    g_intrDepth++;
    const u64 t0 = hostMicros();
    callGuestSimple(fn, intrStack(), g_callerGp, {p.token & 0xFFFF, arg, p.pc + 4});
    g_stats.cbMicros += hostMicros() - t0;
    g_intrDepth--;
    g_hashGen++;
}

// Raises a GE interrupt: runs the handler now if the game can take interrupts, else queues it.
void raise(int cbid, bool finish, u32 token, u32 pc) {
    if (cbid < 0) return;
    PendingIntr p{cbid, finish, token, pc};
    if (kernel::interruptsEnabled() && g_intrDepth < 4 && g_pending.empty()) {
        runHandler(p);
    } else {
        g_pending.push_back(p);
        g_stats.deferred++;
    }
}

void wakeSyncWaiters() { kernel::wakeWaiters(kernel::WaitType::GeSync, 0, 0, true); }

void completeList(DList& dl) {
    if (dl.started && dl.context && memValid(dl.context)) {
        restoreContext((const u32*)memPtr(dl.context));
        dl.started = false;
    }
    dl.state = DL_COMPLETED;
    auto it = std::find(g_queue.begin(), g_queue.end(), dl.id);
    if (it != g_queue.end()) g_queue.erase(it);
    if (g_cur == &dl) g_cur = nullptr;
}

enum class Stop { Stall, Finish, Pause, Error, Interrupt };

// Executes commands until the list stalls/ends. Signals that call a handler return Interrupt
// with the list positioned after the END so the caller can run the handler and resume.
Stop runList(DList& dl, PendingIntr& intr) {
    GeState& ge = g_ge;
    u32 guard = 0;
    for (;;) {
        if (dl.pc == dl.stall && dl.stall != 0) return Stop::Stall;
        if (!memValid(dl.pc)) {
            LOG_ERROR("GE", "list %d ran into invalid memory at %08x", dl.id, dl.pc);
            return Stop::Error;
        }
        if (++guard > 50000000) {
            LOG_ERROR("GE", "list %d: runaway execution at %08x, aborting", dl.id, dl.pc);
            return Stop::Error;
        }
        const u32 op = rd32(dl.pc);
        const u32 cmd = op >> 24, data = op & 0xFFFFFF;
        if (g_tracing) LOG_INFO("GETRACE", "%08x: %08x", dl.pc, op);
        const u32 diff = op ^ ge.cmd[cmd];
        ge.cmd[cmd] = op;
        if (diff) g_dirty |= g_cmdDirty[cmd];
        switch (cmd) {
            case CMD_PRIM: drawPrim(data); break;
            case CMD_BEZIER: drawBezier(data); break;
            case CMD_SPLINE: drawSpline(data); break;
            case CMD_BBOX:
                if ((data & 0xFFFF) == 0) dl.bboxResult = true;
                else dl.bboxResult = bboxVisible(data & 0xFFFF);
                break;
            case CMD_JUMP: {
                u32 t = ge.relAddr(data & 0xFFFFFC);
                if (!memValid(t)) {
                    LOG_ERROR("GE", "JUMP to bad address %08x", t);
                    return Stop::Error;
                }
                dl.pc = t;
                continue;
            }
            case CMD_BJUMP:
                if (!dl.bboxResult) {
                    u32 t = ge.relAddr(data & 0xFFFFFC);
                    if (memValid(t)) {
                        dl.pc = t;
                        continue;
                    }
                }
                break;
            case CMD_CALL: {
                u32 t = ge.relAddr(data & 0xFFFFFC);
                if (!memValid(t)) {
                    LOG_ERROR("GE", "CALL to bad address %08x", t);
                    return Stop::Error;
                }
                if (dl.hardwareCallDepth() >= 2) break;  // the GE only nests two levels of CALL
                if (dl.sp >= 32) {
                    LOG_ERROR("GE", "CALL stack overflow");
                    break;
                }
                dl.stack[dl.sp++] = {dl.pc + 4, ge.offsetAddr, 0};
                dl.pc = t;
                continue;
            }
            case CMD_RET:
                if (dl.sp > 0) {
                    const StackEntry& e = dl.stack[--dl.sp];
                    ge.offsetAddr = e.offsetAddr;
                    dl.pc = e.pc & 0x0FFFFFFF;
                    continue;
                }
                break;
            case CMD_END: {
                const u32 prev = rd32(dl.pc - 4);
                const u32 prevCmd = prev >> 24;
                const u32 here = dl.pc;
                dl.pc += 4;
                if (prevCmd == CMD_SIGNAL) {
                    const u32 behaviour = (prev >> 16) & 0xFF;
                    const u32 sig = prev & 0xFFFF, enddata = data & 0xFFFF;
                    switch (behaviour) {
                        case SIG_SUSPEND:
                        case SIG_CONTINUE:
                            dl.signal = behaviour;
                            intr = {dl.cbid, false, sig, here};
                            return Stop::Interrupt;
                        case SIG_PAUSE:
                            dl.signal = behaviour;
                            dl.state = DL_PAUSED;
                            break;
                        case SIG_SYNC: dl.signal = behaviour; break;
                        case SIG_JUMP:
                        case SIG_RJUMP:
                        case SIG_OJUMP: {
                            dl.signal = behaviour;
                            u32 t = ((sig << 16) | enddata) & 0xFFFFFFFC;
                            if (behaviour == SIG_RJUMP) t += here - 4;
                            else if (behaviour == SIG_OJUMP) t = ge.relAddr(t);
                            t &= 0x0FFFFFFF;
                            if (!memValid(t)) return Stop::Error;
                            dl.pc = t;
                            break;
                        }
                        case SIG_CALL:
                        case SIG_RCALL:
                        case SIG_OCALL: {
                            dl.signal = behaviour;
                            u32 t = ((sig << 16) | enddata) & 0xFFFFFFFC;
                            if (behaviour == SIG_RCALL) t += here - 4;
                            else if (behaviour == SIG_OCALL) t = ge.relAddr(t);
                            t &= 0x0FFFFFFF;
                            if (!memValid(t)) return Stop::Error;
                            if (dl.sp < 32) {
                                dl.stack[dl.sp++] = {dl.pc, ge.offsetAddr, ge.cmd[CMD_BASE] | (CMD_BASE << 24)};
                                dl.pc = t;
                            }
                            break;
                        }
                        case SIG_RET:
                            dl.signal = behaviour;
                            if (dl.sp > 0) {
                                const StackEntry& e = dl.stack[--dl.sp];
                                ge.offsetAddr = e.offsetAddr;
                                ge.cmd[CMD_BASE] = e.baseAddr;
                                dl.pc = e.pc;
                            }
                            break;
                        default:
                            LOG_WARN("GE", "unknown signal behaviour %02x (signal %04x)", behaviour, sig);
                            break;
                    }
                } else if (prevCmd == CMD_FINISH) {
                    if (dl.signal == SIG_PAUSE) {
                        intr = {dl.cbid, false, prev & 0xFFFF, here};
                        return Stop::Pause;
                    }
                    if (dl.signal == SIG_SYNC) {
                        dl.signal = SIG_NONE;
                        break;
                    }
                    intr = {dl.cbid, true, prev & 0xFFFF, here};
                    return Stop::Finish;
                }
                continue;
            }
            case CMD_BASE: break;
            case CMD_OFFSETADDR: ge.offsetAddr = data << 8; break;
            case CMD_ORIGIN: ge.offsetAddr = dl.pc; break;
            case CMD_VADDR: g_vaddr = ge.relAddr(data); break;
            case CMD_IADDR: g_iaddr = ge.relAddr(data); break;
            case CMD_BONENUM: break;
            case CMD_BONEDATA: {
                u32 n = ge.r(CMD_BONENUM) & 0x7F;
                if (n < 96) ge.bone[n] = f24(data);
                ge.cmd[CMD_BONENUM] = (CMD_BONENUM << 24) | ((n + 1) & 0x7F);
                break;
            }
            case CMD_WORLDDATA: {
                u32 n = ge.r(CMD_WORLDNUM) & 0xF;
                if (n < 12) ge.world[n] = f24(data);
                ge.cmd[CMD_WORLDNUM] = (CMD_WORLDNUM << 24) | ((n + 1) & 0xF);
                break;
            }
            case CMD_VIEWDATA: {
                u32 n = ge.r(CMD_VIEWNUM) & 0xF;
                if (n < 12) ge.view[n] = f24(data);
                ge.cmd[CMD_VIEWNUM] = (CMD_VIEWNUM << 24) | ((n + 1) & 0xF);
                break;
            }
            case CMD_PROJDATA: {
                u32 n = ge.r(CMD_PROJNUM) & 0xF;
                ge.proj[n] = f24(data);
                ge.cmd[CMD_PROJNUM] = (CMD_PROJNUM << 24) | ((n + 1) & 0xF);
                break;
            }
            case CMD_TGENDATA: {
                u32 n = ge.r(CMD_TGENNUM) & 0xF;
                if (n < 12) ge.tgen[n] = f24(data);
                ge.cmd[CMD_TGENNUM] = (CMD_TGENNUM << 24) | ((n + 1) & 0xF);
                break;
            }
            case CMD_LOADCLUT: loadClut(); break;
            case CMD_TRXKICK: blockTransfer(); break;
            case CMD_TEXFLUSH: g_dirty |= DIRTY_TEXTURE; break;
            default:
                if (cmd >= CMD_MORPH0 && cmd < CMD_MORPH0 + 8) ge.morph[cmd - CMD_MORPH0] = f24(data);
                else if (cmd >= CMD_VSCX && cmd <= CMD_VSCV) {
                    static bool warned = false;
                    if (!warned) LOG_WARN("GE", "immediate-mode vertex commands are not supported");
                    warned = true;
                }
                break;
        }
        dl.pc += 4;
    }
}

void processQueue() {
    if (g_processing) {  // re-entered from a handler: the outer loop picks new work up
        g_stats.reentered++;
        return;
    }
    g_processing = true;
    const u64 t0 = hostMicros();
    while (!g_queue.empty()) {
        DList& dl = g_dls[g_queue.front()];
        if (dl.state == DL_PAUSED) break;
        if (dl.state == DL_COMPLETED || dl.state == DL_NONE) {
            g_queue.pop_front();
            continue;
        }
        g_cur = &dl;
        if (!dl.started && dl.context && memValid(dl.context)) saveContext((u32*)memPtr(dl.context));
        dl.started = true;
        dl.state = DL_RUNNING;
        dl.interrupted = false;
        g_ge.offsetAddr = dl.offsetAddr;
        if (g_frame >= g_traceFrame && !g_tracing) {
            g_tracing = true;
            LOG_INFO("GETRACE", "---- list %d at frame %llu", dl.id, (unsigned long long)g_frame);
        }
        PendingIntr intr{};
        Stop s = runList(dl, intr);
        dl.offsetAddr = g_ge.offsetAddr;
        if (s == Stop::Stall) break;
        if (s == Stop::Interrupt) {
            // SIGNAL with a handler: the list resumes after the handler returns
            raise(intr.cbid, false, intr.token, intr.pc);
            if (dl.state == DL_RUNNING && dl.signal == SIG_SUSPEND) dl.signal = SIG_NONE;
            continue;
        }
        if (s == Stop::Pause) {
            dl.state = DL_PAUSED;
            dl.signal = SIG_SUSPEND;  // sceGeContinue resumes it
            raise(intr.cbid, false, intr.token, intr.pc);
            break;
        }
        if (s == Stop::Finish) {
            if (g_tracing) {
                g_tracing = false;
                // VC2_GE_TRACE_LISTS=N traces N consecutive lists instead of one
                static int remaining = getenv("VC2_GE_TRACE_LISTS") ? atoi(getenv("VC2_GE_TRACE_LISTS")) - 1 : 0;
                if (remaining > 0) {
                    remaining--;
                    g_traceFrame = g_frame;
                } else {
                    g_traceFrame = ~0ull;
                }
            }
            flushDraws();
            dl.state = DL_COMPLETED;
            g_stats.finished++;
            raise(intr.cbid, true, intr.token, intr.pc);
            completeList(dl);
        } else {
            completeList(dl);  // error
        }
        wakeSyncWaiters();
    }
    if (g_queue.empty()) g_cur = nullptr;
    g_processing = false;
    g_stats.geMicros += hostMicros() - t0;
    wakeSyncWaiters();
}

bool queueIdle() {
    for (int i : g_queue)
        if (g_dls[i].state != DL_COMPLETED && g_dls[i].state != DL_NONE) return false;
    return true;
}

}  // namespace

void debugRequestTrace(u64 frame) { g_traceFrame = frame; }

// ------------------------------------------------------------------ context save/restore
void saveContext(u32* dst) {
    memcpy(dst, g_ge.cmd, 256 * 4);
    memcpy(dst + 256, g_ge.world, 12 * 4);
    memcpy(dst + 268, g_ge.view, 12 * 4);
    memcpy(dst + 280, g_ge.proj, 16 * 4);
    memcpy(dst + 296, g_ge.tgen, 12 * 4);
    memcpy(dst + 308, g_ge.bone, 96 * 4);
    dst[404] = 0x56433247;  // 'VC2G' marker
}
void restoreContext(const u32* src) {
    if (src[404] != 0x56433247) return;  // never saved by us
    memcpy(g_ge.cmd, src, 256 * 4);
    memcpy(g_ge.world, src + 256, 12 * 4);
    memcpy(g_ge.view, src + 268, 12 * 4);
    memcpy(g_ge.proj, src + 280, 16 * 4);
    memcpy(g_ge.tgen, src + 296, 12 * 4);
    memcpy(g_ge.bone, src + 308, 96 * 4);
    for (int i = 0; i < 8; i++) g_ge.morph[i] = f24(g_ge.r(CMD_MORPH0 + i));
    g_dirty = DIRTY_ALL;
}

void deliverPendingInterrupts() {
    if (g_intrDepth > 0) return;
    bool any = false;
    while (!g_pending.empty() && kernel::interruptsEnabled()) {
        PendingIntr p = g_pending.front();
        g_pending.pop_front();
        runHandler(p);
        any = true;
    }
    if (any) processQueue();
}

void setCallerGp(u32 gp) { g_callerGp = gp; }

// ------------------------------------------------------------------ sceGe backend
void geInitLists() {
    if (const char* t = getenv("VC2_GE_TRACE")) g_traceFrame = strtoull(t, nullptr, 10);
    g_geLog = getenv("VC2_GE_LOG") != nullptr;
    initCmdTable();
    resetState();
}

u32 listEnqueue(u32 list, u32 stall, s32 cbid, u32 optAddr, bool head) {
    if (g_geLog) LOG_INFO("GELOG", "f%llu enqueue %08x stall %08x cb %d head %d", (unsigned long long)g_frame, list, stall, cbid, head);
    g_hashGen++;
    deliverPendingInterrupts();
    if (((list | stall) & 3) || !memValid(list)) return 0x80000103;  // SCE_KERNEL_ERROR_INVALID_POINTER
    u32 ctx = 0;
    if (optAddr && memValid(optAddr)) {
        u32 size = rd32(optAddr);
        if (size >= 8) {
            u32 c = rd32(optAddr + 4);
            if (c && memValid(c) && memValid(c + 512 * 4 - 1)) ctx = c & 0x0FFFFFFF;
        }
    }
    const u32 lp = list & 0x0FFFFFFF;
    for (int i : g_queue) {
        const DList& o = g_dls[i];
        if (o.state != DL_COMPLETED && o.state != DL_NONE && o.startPc == lp) {
            LOG_WARN("GE", "enqueue: list %08x already queued", lp);
            return 0x80000021;
        }
    }
    int id = -1;
    for (int i = 0; i < kMaxLists; i++) {
        int c = (i + g_nextId) % kMaxLists;
        if (g_dls[c].state == DL_NONE) {
            id = c;
            break;
        }
        if (g_dls[c].state == DL_COMPLETED && id < 0) id = c;
    }
    if (id < 0) {
        LOG_ERROR("GE", "no free display list slot");
        return 0x80000022;
    }
    g_nextId = id + 1;
    DList& dl = g_dls[id];
    dl = DList{};
    dl.id = id;
    dl.startPc = dl.pc = lp;
    dl.stall = stall & 0x0FFFFFFF;
    dl.cbid = (cbid >= 0 && cbid < 16) ? cbid : -1;
    dl.context = ctx;
    if (head) {
        if (g_cur) {
            if (g_cur->state != DL_PAUSED) return 0x800001FE;  // SCE_KERNEL_ERROR_INVALID_VALUE
            g_cur->state = DL_QUEUED;
            g_cur->signal = SIG_NONE;
        }
        dl.state = DL_PAUSED;
        g_cur = &dl;
        g_queue.push_front(id);
    } else {
        dl.state = DL_QUEUED;
        g_queue.push_back(id);
        g_stats.lists++;
        processQueue();
    }
    return (u32)id ^ kListIdMagic;
}

int listUpdateStall(u32 lid, u32 stall) {
    g_hashGen++;
    deliverPendingInterrupts();
    u32 id = lid ^ kListIdMagic;
    if (id >= (u32)kMaxLists || g_dls[id].state == DL_NONE) return (int)0x80000100;
    DList& dl = g_dls[id];
    if (dl.state == DL_COMPLETED) return (int)0x80000020;
    dl.stall = stall & 0x0FFFFFFC;
    processQueue();
    return 0;
}

int listSync(u32 lid, u32 mode) {
    deliverPendingInterrupts();
    u32 id = lid ^ kListIdMagic;
    if (id >= (u32)kMaxLists) return (int)0x80000100;
    if (mode > 1) return (int)0x80000107;
    DList& dl = g_dls[id];
    if (mode == 1) {
        switch (dl.state) {
            case DL_QUEUED: return dl.interrupted ? LS_PAUSED : LS_QUEUED;
            case DL_RUNNING: return (dl.pc == dl.stall) ? LS_STALLING : LS_DRAWING;
            case DL_COMPLETED: return LS_COMPLETED;
            case DL_PAUSED: return LS_PAUSED;
            default: return (int)0x80000100;
        }
    }
    if (dl.state != DL_COMPLETED && dl.state != DL_NONE && kernel::currentThread()) {
        if (g_intrDepth > 0 || g_processing) return (int)0x80020064;  // ILLEGAL_CONTEXT: cannot wait here
        flushDraws();
        kernel::currentThread()->waitCheck = [id]() {
            return g_dls[id].state == DL_COMPLETED || g_dls[id].state == DL_NONE;
        };
        kernel::waitCurrent(kernel::WaitType::GeSync, 0, 0, false);
    }
    return LS_COMPLETED;
}

int drawSync(u32 mode) {
    deliverPendingInterrupts();
    if (mode > 1) return (int)0x80000107;
    if (mode == 0) {
        flushDraws();
        if (!queueIdle() && kernel::currentThread()) {
            if (g_intrDepth > 0 || g_processing) return (int)0x80020064;
            kernel::currentThread()->waitCheck = []() { return queueIdle(); };
            if (g_geLog) LOG_INFO("GELOG", "f%llu drawSync waits", (unsigned long long)g_frame);
            kernel::waitCurrent(kernel::WaitType::GeSync, 0, 0, false);
            if (g_geLog) LOG_INFO("GELOG", "f%llu drawSync done", (unsigned long long)g_frame);
        }
        for (auto& dl : g_dls)
            if (dl.state == DL_COMPLETED) dl.state = DL_NONE;
        return 0;
    }
    for (int i : g_queue) {
        const DList& dl = g_dls[i];
        if (dl.state == DL_COMPLETED) continue;
        if (dl.state != DL_QUEUED && dl.pc == dl.stall) return LS_STALLING;
        return LS_DRAWING;
    }
    return LS_COMPLETED;
}

int setCallback(u32 cbData) {
    for (int i = 0; i < 16; i++) {
        if (g_cbs[i].used) continue;
        g_cbs[i].used = true;
        g_cbs[i].signalFunc = rd32(cbData);
        g_cbs[i].signalArg = rd32(cbData + 4);
        g_cbs[i].finishFunc = rd32(cbData + 8);
        g_cbs[i].finishArg = rd32(cbData + 12);
        LOG_INFO("GE", "callback %d: signal %08x(%08x) finish %08x(%08x)", i, g_cbs[i].signalFunc,
                 g_cbs[i].signalArg, g_cbs[i].finishFunc, g_cbs[i].finishArg);
        return i;
    }
    return (int)0x80000022;
}

void unsetCallback(int id) {
    if (id >= 0 && id < 16) g_cbs[id] = Callbacks{};
}

int geBreak(u32 mode, u32) {
    if (mode > 1) return (int)0x80000107;
    if (!g_cur) return (int)0x80000020;  // ALREADY
    flushDraws();
    if (mode == 1) {
        g_queue.clear();
        for (auto& dl : g_dls) {
            dl.state = DL_NONE;
            dl.signal = SIG_NONE;
        }
        g_pending.clear();
        g_nextId = 0;
        g_cur = nullptr;
        wakeSyncWaiters();
        return 0;
    }
    if (g_cur->state == DL_NONE || g_cur->state == DL_COMPLETED) return (int)0x80000004;
    if (g_cur->state == DL_PAUSED) return (int)0x80000020;
    if (g_cur->state == DL_QUEUED) {
        g_cur->state = DL_PAUSED;
        return g_cur->id ^ (int)kListIdMagic;
    }
    g_cur->startPc = g_cur->pc;
    g_cur->interrupted = true;
    g_cur->state = DL_PAUSED;
    g_cur->signal = SIG_SUSPEND;
    g_isBreak = g_intrDepth > 0 || !kernel::interruptsEnabled();
    return g_cur->id ^ (int)kListIdMagic;
}

int geContinue() {
    deliverPendingInterrupts();
    if (!g_cur) return 0;
    if (g_cur->state == DL_PAUSED) {
        if (!g_isBreak) {
            if (g_cur->signal == SIG_PAUSE) return (int)0x80000021;  // BUSY
            g_cur->state = DL_RUNNING;
            g_cur->signal = SIG_NONE;
        } else {
            g_cur->state = DL_QUEUED;
            g_cur->signal = SIG_NONE;
            g_isBreak = false;
        }
    } else if (g_cur->state == DL_RUNNING) {
        return (int)0x80000020;
    } else {
        return (int)0x80000004;
    }
    processQueue();
    return 0;
}

u32 getCmd(u32 cmd) {
    if (cmd > 255) return 0x80000102;
    u32 v = g_ge.cmd[cmd];
    switch (cmd) {
        case CMD_BONEDATA:
        case CMD_WORLDDATA:
        case CMD_VIEWDATA:
        case CMD_PROJDATA:
        case CMD_TGENDATA: v &= 0xFF000000; break;
        case CMD_BONENUM: v &= 0xFF00007F; break;
        case CMD_WORLDNUM:
        case CMD_VIEWNUM:
        case CMD_PROJNUM:
        case CMD_TGENNUM: v &= 0xFF00000F; break;
    }
    return v;
}

int getMtx(u32 type, u32 dst) {
    const float* m = nullptr;
    int n = 12;
    if (type < 8) m = g_ge.bone + type * 12;
    else if (type == 8) m = g_ge.world;
    else if (type == 9) m = g_ge.view;
    else if (type == 10) m = g_ge.proj, n = 16;
    else if (type == 11) m = g_ge.tgen;
    else return (int)0x80000102;
    if (!memValid(dst)) return (int)0x80000103;
    for (int i = 0; i < n; i++) wr32(dst + i * 4, toF24(m[i]));
    return 0;
}

int saveContextTo(u32 addr) {
    if (!memValid(addr)) return -1;
    saveContext((u32*)memPtr(addr));
    return 0;
}
int restoreContextFrom(u32 addr) {
    if (!memValid(addr)) return -1;
    restoreContext((const u32*)memPtr(addr));
    return 0;
}

}  // namespace vc2rt::gpu
