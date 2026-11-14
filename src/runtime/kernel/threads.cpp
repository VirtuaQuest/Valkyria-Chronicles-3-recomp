// Threads, scheduling, waits, callbacks and interrupts.
//
// Every PSP thread runs on its own Windows fiber; the host main thread's fiber is the scheduler.
// PSP scheduling is strict priority (lower value = higher priority) and only changes at kernel
// calls, so guest code is never preempted mid-instruction stream: a thread runs until it blocks,
// yields, or wakes a higher priority thread.
#include <windows.h>

#include <algorithm>
#include <map>
#include <timeapi.h>

#include "../hle/hle.h"
#include "kernel.h"

#pragma comment(lib, "winmm.lib")

namespace vc2rt::kernel {

// ------------------------------------------------------------------ object table
static std::map<u32, std::unique_ptr<KObject>> g_objects;
static u32 g_nextUid = 0x100;

u32 addObject(std::unique_ptr<KObject> obj) {
    u32 uid = g_nextUid++;
    obj->uid = uid;
    g_objects[uid] = std::move(obj);
    return uid;
}
KObject* findObject(u32 uid) {
    auto it = g_objects.find(uid);
    return it == g_objects.end() ? nullptr : it->second.get();
}
void removeObject(u32 uid) { g_objects.erase(uid); }
std::vector<KObject*> objectsOfType(ObjType t) {
    std::vector<KObject*> v;
    for (auto& [id, o] : g_objects)
        if (o->type == t) v.push_back(o.get());
    return v;
}

// ------------------------------------------------------------------ scheduler state
static void* g_schedFiber = nullptr;
static Thread* g_current = nullptr;
static s64 g_seq = 0, g_frontSeq = 0;
static bool g_exit = false;
static int g_exitCode = 0;
static bool g_rescheduleRequested = false;
static bool g_intrEnabled = true;
static std::vector<void*> g_deadFibers;

struct PeriodicTimer {
    u64 period, next;
    HostTimerFn fn;
};
static std::vector<PeriodicTimer> g_periodic;
static std::multimap<u64, HostTimerFn> g_oneShot;

Thread* currentThread() { return g_current; }
u32 currentThreadId() { return g_current ? g_current->uid : 0; }
Thread* getThread(u32 uid) {
    if (uid == 0) return g_current;
    return getObject<Thread>(uid, ObjType::Thread);
}
void requestExit(int code) {
    g_exit = true;
    g_exitCode = code;
}
bool exitRequested() { return g_exit; }

void addPeriodicTimer(u64 period, HostTimerFn fn) { g_periodic.push_back({period, hostMicros() + period, std::move(fn)}); }
void addOneShotTimer(u64 at, HostTimerFn fn) { g_oneShot.emplace(at, std::move(fn)); }

static void makeReady(Thread* t, bool front = false) {
    t->state = ThreadState::Ready;
    t->readySeq = front ? --g_frontSeq : ++g_seq;
}

static Thread* pickNext() {
    Thread* best = nullptr;
    for (auto& [id, o] : g_objects) {
        if (o->type != ObjType::Thread) continue;
        Thread* t = static_cast<Thread*>(o.get());
        if (t->state != ThreadState::Ready) continue;
        if (!best || t->priority < best->priority || (t->priority == best->priority && t->readySeq < best->readySeq))
            best = t;
    }
    return best;
}

static bool higherPriorityReady() {
    if (!g_current) return false;
    Thread* n = pickNext();
    return n && n->priority < g_current->priority;
}

// Switch from the current thread's fiber back to the scheduler.
static void switchToScheduler() {
    SwitchToFiber(g_schedFiber);
}

// ------------------------------------------------------------------ thread lifecycle
static void WINAPI threadFiberMain(void* param) {
    Thread* t = (Thread*)param;
    callGuest(&t->ctx, t->entry);
    // Returning from the entry function is sceKernelExitThread(v0).
    exitCurrentThread(t->ctx.r[2], false);
}

u32 createThread(const std::string& name, u32 entry, int prio, u32 stackSize, u32 attr, u32 gp) {
    auto t = std::make_unique<Thread>();
    t->type = ObjType::Thread;
    t->name = name;
    t->entry = entry;
    t->initPriority = t->priority = prio;
    t->attr = attr;
    t->gp = gp;
    stackSize = std::max<u32>((stackSize + 0xFF) & ~0xFFu, 0x200);
    t->stackSize = stackSize;
    t->stackBlock = allocUserMemory(stackSize, 1 /*high*/, 0, "stack:" + name, &t->stackAddr);
    if (!t->stackBlock) return ERR_NO_MEMORY;
    // PSP fills new stacks with 0xFF unless the no-fill attribute is set.
    if (!(attr & 0x00100000)) memset(memPtr(t->stackAddr), 0xFF, stackSize);
    u32 uid = addObject(std::move(t));
    LOG_DEBUG("THREAD", "created '%s' uid=%x entry=%08x prio=%d stack=%x@%08x", name.c_str(), uid, entry, prio, stackSize,
              getThread(uid)->stackAddr);
    return uid;
}

int startThread(u32 uid, u32 argSize, u32 argp) {
    Thread* t = getObject<Thread>(uid, ObjType::Thread);
    if (!t) return (int)ERR_UNKNOWN_THID;
    if (t->state != ThreadState::Dormant) return (int)ERR_NOT_DORMANT;
    memset(&t->ctx, 0, sizeof t->ctx);
    u32 top = t->stackAddr + t->stackSize;
    // thread-local "k0" area at the top of the stack, as the PSP kernel does
    u32 sp = top - 0x100;
    wr32(top - 0xC0 + 0x0, uid);
    t->ctx.r[26] = top - 0xC0;
    if (argp && argSize) {
        sp -= (argSize + 0xF) & ~0xFu;
        memcpy(memPtr(sp), memPtr(argp), argSize);
        t->ctx.r[5] = sp;
    } else {
        t->ctx.r[5] = argp;
    }
    t->ctx.r[4] = argSize;
    t->ctx.r[29] = sp - 0x40;
    t->ctx.r[28] = t->gp;
    t->ctx.r[31] = 0;
    t->ctx.fcr31 = 0x00000E00;  // PSP default
    t->ctx.vfpuCtrl[VFPU_CTRL_SPREFIX] = 0xE4;
    t->ctx.vfpuCtrl[VFPU_CTRL_TPREFIX] = 0xE4;
    t->ctx.vfpuCtrl[VFPU_CTRL_DPREFIX] = 0;
    t->ctx.vfpuCtrl[VFPU_CTRL_CC] = 0x3F;
    t->ctx.threadId = uid;
    t->argSize = argSize;
    t->argAddr = argp;
    t->exitStatus = ERR_DORMANT;
    t->priority = t->initPriority;
    t->wakeupCount = 0;
    if (t->fiber) g_deadFibers.push_back(t->fiber);
    t->fiber = CreateFiberEx(64 * 1024, 16 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, threadFiberMain, t);
    if (!t->fiber) fatal("CreateFiberEx failed for thread '%s'", t->name.c_str());
    makeReady(t);
    rescheduleSoon();
    return 0;
}

void exitCurrentThread(u32 status, bool deleteSelf) {
    Thread* t = g_current;
    LOG_DEBUG("THREAD", "thread '%s' exits (%08x)", t->name.c_str(), status);
    t->exitStatus = status;
    t->state = ThreadState::Dormant;
    t->waitType = WaitType::None;
    wakeWaiters(WaitType::ThreadEnd, t->uid, 0, false);
    if (deleteSelf) t->state = ThreadState::Dead;
    switchToScheduler();
    fatal("exited thread resumed");
}

void terminateThread(Thread* t) {
    if (t == g_current) {
        exitCurrentThread(ERR_THREAD_TERMINATED, false);
        return;
    }
    t->state = ThreadState::Dormant;
    t->exitStatus = ERR_THREAD_TERMINATED;
    t->waitType = WaitType::None;
    if (t->fiber) {
        g_deadFibers.push_back(t->fiber);
        t->fiber = nullptr;
    }
    wakeWaiters(WaitType::ThreadEnd, t->uid, 0, false);
}

// ------------------------------------------------------------------ waits
void wakeThread(Thread* t, s32 result) {
    if (t->state != ThreadState::Waiting && t->state != ThreadState::WaitSuspended) return;
    t->waitResult = result;
    if (t->timeoutPtr && t->wakeTime) {
        u64 now = hostMicros();
        u32 remain = now >= t->wakeTime ? 0 : (u32)(t->wakeTime - now);
        wr32(t->timeoutPtr, remain);
    }
    t->waitType = WaitType::None;
    t->waitCheck = nullptr;
    t->wakeTime = 0;
    if (t->state == ThreadState::WaitSuspended) {
        t->state = ThreadState::Suspended;
        return;
    }
    makeReady(t);
    if (g_current && t->priority < g_current->priority) g_rescheduleRequested = true;
}

std::vector<Thread*> waitingThreads(WaitType type, u32 id) {
    std::vector<Thread*> v;
    for (auto& [uid, o] : g_objects) {
        if (o->type != ObjType::Thread) continue;
        Thread* t = static_cast<Thread*>(o.get());
        if ((t->state == ThreadState::Waiting || t->state == ThreadState::WaitSuspended) && t->waitType == type &&
            t->waitId == id)
            v.push_back(t);
    }
    std::sort(v.begin(), v.end(), [](Thread* a, Thread* b) { return a->waitStart < b->waitStart; });
    return v;
}

int wakeWaiters(WaitType type, u32 id, s32 result, bool onlyIfCheckPasses) {
    int n = 0;
    for (Thread* t : waitingThreads(type, id)) {
        if (onlyIfCheckPasses && t->waitCheck && !t->waitCheck()) continue;
        wakeThread(t, result);
        n++;
    }
    return n;
}

static s32 doWait(WaitType type, u32 id, u64 wakeAt, u32 timeoutPtr, bool cb) {
    Thread* t = g_current;
    if (!t) fatal("wait outside of a thread (type %u)", (u32)type);
    for (;;) {
        if (cb && checkCallbacks() > 0 && type == WaitType::Sleep) {
            // callbacks woke a CB sleep
        }
        t->state = ThreadState::Waiting;
        t->waitType = type;
        t->waitId = id;
        t->wakeTime = wakeAt;
        t->timeoutPtr = timeoutPtr;
        t->waitCB = cb;
        t->waitStart = (u64)++g_seq;
        t->waitResult = 0;
        t->runCallbacksPending = false;
        switchToScheduler();
        // resumed
        if (t->runCallbacksPending && t->waitType != WaitType::None) {
            // woken only to service callbacks: run them and continue waiting
            t->state = ThreadState::Running;
            WaitType wt = t->waitType;
            u32 wid = t->waitId;
            auto check = t->waitCheck;
            checkCallbacks();
            if (check && check()) {  // condition satisfied while we were servicing callbacks
                t->waitType = WaitType::None;
                t->waitCheck = nullptr;
                return 0;
            }
            type = wt;
            id = wid;
            t->waitCheck = check;
            continue;
        }
        if (cb) checkCallbacks();
        return t->waitResult;
    }
}

s32 waitCurrent(WaitType type, u32 id, u32 timeoutPtr, bool cb) {
    u64 wakeAt = 0;
    if (timeoutPtr) {
        u32 us = rd32(timeoutPtr);
        wakeAt = hostMicros() + std::max<u32>(us, 1);
    }
    return doWait(type, id, wakeAt, timeoutPtr, cb);
}

s32 waitCurrentFor(WaitType type, u32 id, u64 micros, bool cb) {
    return doWait(type, id, micros ? hostMicros() + micros : 0, 0, cb);
}

void yieldCurrent() {
    if (!g_current) return;
    makeReady(g_current);
    switchToScheduler();
}

void rescheduleSoon() { g_rescheduleRequested = true; }

void afterHleCall(PspContext* c) {
    (void)c;
    if (!g_current) return;
    // Timer interrupts: on the PSP, vblank/audio/timer interrupts preempt a running thread. A
    // thread that keeps making non-blocking kernel calls (spin-waiting on a lock, polling) would
    // otherwise starve every timer here, so treat an overdue timer as an interrupt and let the
    // scheduler run. Never inside an interrupts-disabled critical section, exactly like hardware.
    static const bool noPreempt = getenv("VC2_NO_PREEMPT") != nullptr;  // diagnostic
    if (!noPreempt && g_intrEnabled && !g_periodic.empty()) {
        u64 due = ~0ull;
        for (auto& p : g_periodic) due = std::min(due, p.next);
        if (hostMicros() >= due + 1000) {
            g_rescheduleRequested = false;
            makeReady(g_current, true);
            switchToScheduler();
            return;
        }
    }
    if (!g_rescheduleRequested) return;
    g_rescheduleRequested = false;
    if (higherPriorityReady()) {
        makeReady(g_current, true);
        switchToScheduler();
    }
}

// ------------------------------------------------------------------ callbacks
struct Callback : KObject {
    u32 func = 0, arg = 0, owner = 0;
    int notifyCount = 0;
    u32 notifyArg = 0;
};

u32 createCallback(const std::string& name, u32 func, u32 arg) {
    auto cb = std::make_unique<Callback>();
    cb->type = ObjType::Callback;
    cb->name = name;
    cb->func = func;
    cb->arg = arg;
    cb->owner = currentThreadId();
    u32 id = addObject(std::move(cb));
    if (g_current) g_current->callbacks.push_back(id);
    return id;
}

void notifyCallback(u32 cbid, u32 arg2) {
    Callback* cb = getObject<Callback>(cbid, ObjType::Callback);
    if (!cb) return;
    cb->notifyCount++;
    cb->notifyArg = arg2;
    Thread* owner = getThread(cb->owner);
    if (owner && owner->state == ThreadState::Waiting && owner->waitCB) {
        owner->runCallbacksPending = true;
        owner->state = ThreadState::Ready;
        owner->readySeq = ++g_seq;
    }
}

int checkCallbacks() {
    Thread* t = g_current;
    if (!t) return 0;
    int ran = 0;
    for (u32 id : std::vector<u32>(t->callbacks)) {
        Callback* cb = getObject<Callback>(id, ObjType::Callback);
        if (!cb || cb->notifyCount == 0) continue;
        int count = cb->notifyCount;
        cb->notifyCount = 0;
        PspContext saved = t->ctx;
        u32 r = callGuestSimple(cb->func, (t->ctx.r[29] - 0x200) & ~0xFu, t->gp, {(u32)count, cb->notifyArg, cb->arg});
        t->ctx = saved;
        ran++;
        if (r != 0) {
            // non-zero return deletes the callback
            t->callbacks.erase(std::remove(t->callbacks.begin(), t->callbacks.end(), id), t->callbacks.end());
            removeObject(id);
        }
    }
    return ran;
}

// ------------------------------------------------------------------ interrupts
struct SubIntr {
    u32 handler = 0, arg = 0;
    bool enabled = false;
};
static std::map<std::pair<u32, u32>, SubIntr> g_subIntr;
static u32 g_intrStackTop = 0;
static bool g_inInterrupt = false;

bool interruptsEnabled() { return g_intrEnabled; }
void setInterruptsEnabled(bool on) { g_intrEnabled = on; }
void registerSubIntr(u32 intr, u32 sub, u32 handler, u32 arg) { g_subIntr[{intr, sub}] = {handler, arg, false}; }
void releaseSubIntr(u32 intr, u32 sub) { g_subIntr.erase({intr, sub}); }
void enableSubIntr(u32 intr, u32 sub, bool on) {
    auto it = g_subIntr.find({intr, sub});
    if (it != g_subIntr.end()) it->second.enabled = on;
}
u32 interruptStackTop() { return g_intrStackTop; }

void runSubIntrHandlers(u32 intr, u32 arg) {
    if (g_inInterrupt) return;
    g_inInterrupt = true;
    for (auto& [key, h] : g_subIntr) {
        if (key.first != intr || !h.enabled || !h.handler) continue;
        callGuestSimple(h.handler, g_intrStackTop - 0x40, 0, {key.second, h.arg, arg});
    }
    g_inInterrupt = false;
}

// ------------------------------------------------------------------ scheduler loop
void kernelInit() {
    g_schedFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_schedFiber) fatal("ConvertThreadToFiber failed");
    timeBeginPeriod(1);
    // Interrupt handlers run on a private stack in kernel RAM (below the user partition and the
    // 0x08400000 volatile region, so it can never collide with game allocations).
    g_intrStackTop = 0x08100000;
}

static void processTimers(u64 now) {
    for (auto& p : g_periodic) {
        if (now >= p.next) {
            p.fn(now);
            p.next += p.period;
            if (now > p.next + p.period * 4) p.next = now + p.period;  // fell far behind: resync
        }
    }
    while (!g_oneShot.empty() && g_oneShot.begin()->first <= now) {
        auto fn = std::move(g_oneShot.begin()->second);
        g_oneShot.erase(g_oneShot.begin());
        fn(now);
    }
    for (auto& [uid, o] : g_objects) {
        if (o->type != ObjType::Thread) continue;
        Thread* t = static_cast<Thread*>(o.get());
        if (t->state == ThreadState::Waiting && t->wakeTime && now >= t->wakeTime) {
            if (t->waitType == WaitType::Delay || t->waitType == WaitType::Generic)
                wakeThread(t, 0);
            else
                wakeThread(t, (s32)ERR_WAIT_TIMEOUT);
        }
    }
}

static u64 nextEventTime() {
    u64 next = ~0ull;
    for (auto& p : g_periodic) next = std::min(next, p.next);
    if (!g_oneShot.empty()) next = std::min(next, g_oneShot.begin()->first);
    for (auto& [uid, o] : g_objects) {
        if (o->type != ObjType::Thread) continue;
        Thread* t = static_cast<Thread*>(o.get());
        if (t->state == ThreadState::Waiting && t->wakeTime) next = std::min(next, t->wakeTime);
    }
    return next;
}

static u64 g_schedTimerMicros = 0, g_schedIdleMicros = 0;  // diagnostics for dumps

void dumpThreads(const char* why) {
    static const char* st[] = {"dormant", "ready", "running", "waiting", "suspended", "waitsusp", "dead"};
    LOG_INFO("THREAD", "---- thread dump (%s): scheduler timers %llums, idle %llums ----", why,
             (unsigned long long)(g_schedTimerMicros / 1000), (unsigned long long)(g_schedIdleMicros / 1000));
    g_schedTimerMicros = g_schedIdleMicros = 0;
    u64 now = hostMicros();
    for (auto& [uid, o] : g_objects) {
        if (o->type != ObjType::Thread) continue;
        Thread* t = static_cast<Thread*>(o.get());
        LOG_INFO("THREAD", "%3x %-24s prio=%02x %-9s wait=%u id=%x cb=%d timeout=%lldus entry=%08x | ran %llums, %llu calls, last %s ra=%08x",
                 uid, t->name.c_str(), t->priority, st[(int)t->state], (u32)t->waitType, t->waitId, t->waitCB ? 1 : 0,
                 t->wakeTime ? (long long)(t->wakeTime - now) : -1ll, t->entry, (unsigned long long)(t->runMicros / 1000),
                 (unsigned long long)t->hleCalls, t->lastHle ? t->lastHle : "-", t->lastHleRa);
        t->runMicros = 0;
        t->hleCalls = 0;
    }
}

int runScheduler() {
    u64 nextDump = getenv("VC2_THREAD_DUMP") ? hostMicros() + 5000000 : ~0ull;
    while (!g_exit) {
        if (hostMicros() >= nextDump) {
            dumpThreads("periodic");
            nextDump += 5000000;
        }
        u64 now = hostMicros();
        processTimers(now);
        g_schedTimerMicros += hostMicros() - now;
        for (void* f : g_deadFibers) DeleteFiber(f);
        g_deadFibers.clear();
        Thread* t = pickNext();
        if (t) {
            g_current = t;
            t->state = ThreadState::Running;
            const u64 runStart = hostMicros();
            SwitchToFiber(t->fiber);
            t->runMicros += hostMicros() - runStart;
            if (t->state == ThreadState::Running) makeReady(t);  // defensive
            g_current = nullptr;
            continue;
        }
        u64 next = nextEventTime();
        now = hostMicros();
        if (next == ~0ull) {
            Sleep(1);
            continue;
        }
        if (next > now) {
            g_schedIdleMicros += next - now;
            u64 d = next - now;
            if (d > 1500) Sleep((DWORD)((d - 500) / 1000));
            else
                while (hostMicros() < next) YieldProcessor();
        }
    }
    return g_exitCode;
}

}  // namespace vc2rt::kernel
