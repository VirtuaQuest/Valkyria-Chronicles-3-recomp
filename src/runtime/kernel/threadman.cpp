// ThreadManForUser / Kernel_Library / InterruptManager HLE.
#include <deque>

#include "../hle/hle.h"
#include "kernel.h"

namespace vc2rt::kernel {

namespace {

// ------------------------------------------------------------------ semaphores
struct Sema : KObject {
    u32 attr = 0;
    s32 count = 0, maxCount = 0;
};

// ------------------------------------------------------------------ event flags
struct EventFlag : KObject {
    u32 attr = 0, pattern = 0, initPattern = 0;
};
enum { EVF_WAITMULTIPLE = 0x200, EVF_AND = 0x00, EVF_OR = 0x01, EVF_CLEARALL = 0x20, EVF_CLEARPAT = 0x10 };

// ------------------------------------------------------------------ mutexes
struct Mutex : KObject {
    u32 attr = 0;
    s32 count = 0;
    u32 owner = 0;
};

// ------------------------------------------------------------------ message boxes
struct Mbx : KObject {
    u32 attr = 0;
    std::deque<u32> messages;
};

Thread* cur() { return currentThread(); }

// ================================================================== threads
void sceKernelCreateThread(PspContext* c) {
    std::string name = readCString(A0, 32);
    int prio = (int)A2;
    if (prio < 0x08 || prio > 0x77) {
        RET(ERR_ILLEGAL_PRIORITY);
        return;
    }
    if (!A1) {
        RET(ERR_ILLEGAL_ENTRY);
        return;
    }
    RET(createThread(name, A1, prio, A3, A4, c->r[28]));
}
void sceKernelStartThread(PspContext* c) { RET(startThread(A0, A1, A2)); }
void sceKernelExitThread(PspContext* c) { exitCurrentThread(A0, false); }
void sceKernelExitDeleteThread(PspContext* c) { exitCurrentThread(A0, true); }
void sceKernelDeleteThread(PspContext* c) {
    Thread* t = getObject<Thread>(A0, ObjType::Thread);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    if (t->state != ThreadState::Dormant) {
        RET(ERR_NOT_DORMANT);
        return;
    }
    freeUserMemory(t->stackBlock);
    t->state = ThreadState::Dead;
    RET(0);
}
void sceKernelTerminateThread(PspContext* c) {
    Thread* t = getObject<Thread>(A0, ObjType::Thread);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    if (t == cur()) {
        RET(ERR_ILLEGAL_THID);
        return;
    }
    if (t->state == ThreadState::Dormant) {
        RET(ERR_DORMANT);
        return;
    }
    terminateThread(t);
    RET(0);
}
void sceKernelGetThreadId(PspContext* c) { RET(currentThreadId()); }
void sceKernelGetThreadCurrentPriority(PspContext* c) { RET(cur()->priority); }
void sceKernelChangeThreadPriority(PspContext* c) {
    Thread* t = getThread(A0);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    int p = (int)A1 == 0 ? cur()->priority : (int)A1;
    if (p < 0x08 || p > 0x77) {
        RET(ERR_ILLEGAL_PRIORITY);
        return;
    }
    t->priority = p;
    rescheduleSoon();
    RET(0);
}
void sceKernelChangeCurrentThreadAttr(PspContext* c) {
    cur()->attr = (cur()->attr & ~A0) | A1;
    RET(0);
}
void sceKernelWaitThreadEnd(PspContext* c) {
    Thread* t = getObject<Thread>(A0, ObjType::Thread);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    if (t->state == ThreadState::Dormant || t->state == ThreadState::Dead) {
        RET(t->exitStatus);
        return;
    }
    s32 r = waitCurrent(WaitType::ThreadEnd, A0, A1, false);
    RET(r == 0 ? (s32)t->exitStatus : r);
}
void sceKernelReferThreadStatus(PspContext* c) {
    Thread* t = getThread(A0);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    u32 p = A1;
    u32 size = rd32(p);
    u8 info[0x68] = {};
    auto put = [&](u32 off, u32 v) { memcpy(info + off, &v, 4); };
    put(0, 0x68);
    strncpy((char*)info + 4, t->name.c_str(), 31);
    put(0x24, t->attr);
    u32 st = 0;
    switch (t->state) {
        case ThreadState::Running: st = 1; break;
        case ThreadState::Ready: st = 2; break;
        case ThreadState::Waiting: st = 4; break;
        case ThreadState::Suspended: st = 8; break;
        case ThreadState::WaitSuspended: st = 12; break;
        case ThreadState::Dormant: st = 16; break;
        case ThreadState::Dead: st = 32; break;
    }
    put(0x28, st);
    put(0x2C, t->entry);
    put(0x30, t->stackAddr);
    put(0x34, t->stackSize);
    put(0x38, t->gp);
    put(0x3C, t->initPriority);
    put(0x40, t->priority);
    u32 wt = (u32)t->waitType;
    if (wt >= 100) wt = (u32)WaitType::Delay;
    put(0x44, t->state == ThreadState::Waiting ? wt : 0);
    put(0x48, t->waitId);
    put(0x4C, t->wakeupCount);
    put(0x50, t->exitStatus);
    memcpy(memPtr(p), info, std::min<u32>(size ? size : 0x68, 0x68));
    RET(0);
}
void sceKernelSuspendThread(PspContext* c) {
    Thread* t = getObject<Thread>(A0, ObjType::Thread);
    if (!t || t == cur()) {
        RET(ERR_ILLEGAL_THID);
        return;
    }
    if (t->state == ThreadState::Ready) t->state = ThreadState::Suspended;
    else if (t->state == ThreadState::Waiting) t->state = ThreadState::WaitSuspended;
    RET(0);
}
void sceKernelResumeThread(PspContext* c) {
    Thread* t = getObject<Thread>(A0, ObjType::Thread);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    if (t->state == ThreadState::Suspended) {
        t->state = ThreadState::Ready;
        rescheduleSoon();
    } else if (t->state == ThreadState::WaitSuspended)
        t->state = ThreadState::Waiting;
    RET(0);
}
void sceKernelDelayThread(PspContext* c) {
    waitCurrentFor(WaitType::Delay, 0, std::max<u32>(A0, 1), false);
    RET(0);
}
void sceKernelDelayThreadCB(PspContext* c) {
    waitCurrentFor(WaitType::Delay, 0, std::max<u32>(A0, 1), true);
    RET(0);
}
void sceKernelSleepThread(PspContext* c) {
    Thread* t = cur();
    if (t->wakeupCount > 0) {
        t->wakeupCount--;
        RET(0);
        return;
    }
    RET(waitCurrent(WaitType::Sleep, 0, 0, false));
}
void sceKernelSleepThreadCB(PspContext* c) {
    Thread* t = cur();
    checkCallbacks();
    if (t->wakeupCount > 0) {
        t->wakeupCount--;
        RET(0);
        return;
    }
    RET(waitCurrent(WaitType::Sleep, 0, 0, true));
}
void sceKernelWakeupThread(PspContext* c) {
    Thread* t = getObject<Thread>(A0, ObjType::Thread);
    if (!t) {
        RET(ERR_UNKNOWN_THID);
        return;
    }
    if (t->state == ThreadState::Waiting && t->waitType == WaitType::Sleep) wakeThread(t, 0);
    else t->wakeupCount++;
    RET(0);
}
void sceKernelCheckCallback(PspContext* c) { RET(checkCallbacks() > 0 ? 1 : 0); }
void sceKernelGetSystemTimeLow(PspContext* c) { RET((u32)pspTimeMicros()); }
void sceKernelGetSystemTimeWide(PspContext* c) { RET64(pspTimeMicros()); }
void sceKernelGetSystemTime(PspContext* c) {
    wr64(A0, pspTimeMicros());
    RET(0);
}
void sceKernelRotateThreadReadyQueue(PspContext* c) {
    yieldCurrent();
    RET(0);
}

// ================================================================== callbacks
void sceKernelCreateCallback(PspContext* c) { RET(createCallback(readCString(A0, 32), A1, A2)); }
void sceKernelDeleteCallback(PspContext* c) {
    removeObject(A0);
    RET(0);
}
void sceKernelNotifyCallback(PspContext* c) {
    notifyCallback(A0, A1);
    RET(0);
}

// ================================================================== semaphores
void sceKernelCreateSema(PspContext* c) {
    auto s = std::make_unique<Sema>();
    s->type = ObjType::Sema;
    s->name = readCString(A0, 32);
    s->attr = A1;
    s->count = (s32)A2;
    s->maxCount = (s32)A3;
    RET(addObject(std::move(s)));
}
void sceKernelDeleteSema(PspContext* c) {
    if (!getObject<Sema>(A0, ObjType::Sema)) {
        RET(ERR_UNKNOWN_SEMID);
        return;
    }
    wakeWaiters(WaitType::Sema, A0, (s32)ERR_WAIT_DELETE, false);
    removeObject(A0);
    rescheduleSoon();
    RET(0);
}
void semaWait(PspContext* c, bool cb) {
    u32 id = A0;
    s32 need = (s32)A1;
    Sema* s = getObject<Sema>(id, ObjType::Sema);
    if (!s) {
        RET(ERR_UNKNOWN_SEMID);
        return;
    }
    if (need <= 0 || need > s->maxCount) {
        RET(ERR_ILLEGAL_COUNT);
        return;
    }
    if (cb) checkCallbacks();
    if (s->count >= need && waitingThreads(WaitType::Sema, id).empty()) {
        s->count -= need;
        RET(0);
        return;
    }
    cur()->waitCheck = [id, need]() {
        Sema* s2 = getObject<Sema>(id, ObjType::Sema);
        if (s2 && s2->count >= need) {
            s2->count -= need;
            return true;
        }
        return false;
    };
    RET(waitCurrent(WaitType::Sema, id, A2, cb));
}
void sceKernelWaitSema(PspContext* c) { semaWait(c, false); }
void sceKernelWaitSemaCB(PspContext* c) { semaWait(c, true); }
void sceKernelSignalSema(PspContext* c) {
    Sema* s = getObject<Sema>(A0, ObjType::Sema);
    if (!s) {
        RET(ERR_UNKNOWN_SEMID);
        return;
    }
    if (s->count + (s32)A1 > s->maxCount) {
        RET(ERR_SEMA_OVF);
        return;
    }
    s->count += (s32)A1;
    for (Thread* t : waitingThreads(WaitType::Sema, A0)) {
        if (t->waitCheck && t->waitCheck()) wakeThread(t, 0);
        else if (!(s->attr & 0x100)) break;  // FIFO: don't let later waiters overtake
    }
    rescheduleSoon();
    RET(0);
}
void sceKernelPollSema(PspContext* c) {
    Sema* s = getObject<Sema>(A0, ObjType::Sema);
    if (!s) {
        RET(ERR_UNKNOWN_SEMID);
        return;
    }
    if ((s32)A1 <= 0) {
        RET(ERR_ILLEGAL_COUNT);
        return;
    }
    if (s->count >= (s32)A1 && waitingThreads(WaitType::Sema, A0).empty()) {
        s->count -= (s32)A1;
        RET(0);
    } else
        RET(ERR_SEMA_ZERO);
}
void sceKernelReferSemaStatus(PspContext* c) {
    Sema* s = getObject<Sema>(A0, ObjType::Sema);
    if (!s) {
        RET(ERR_UNKNOWN_SEMID);
        return;
    }
    u32 p = A1;
    wr32(p, 0x38);
    writeCString(p + 4, s->name, 32);
    wr32(p + 0x24, s->attr);
    wr32(p + 0x28, 0);
    wr32(p + 0x2C, (u32)s->count);
    wr32(p + 0x30, (u32)s->maxCount);
    wr32(p + 0x34, (u32)waitingThreads(WaitType::Sema, A0).size());
    RET(0);
}

// ================================================================== event flags
bool evfMatch(u32 pattern, u32 bits, u32 mode) {
    return (mode & EVF_OR) ? (pattern & bits) != 0 : (pattern & bits) == bits;
}
void sceKernelCreateEventFlag(PspContext* c) {
    auto e = std::make_unique<EventFlag>();
    e->type = ObjType::EventFlag;
    e->name = readCString(A0, 32);
    e->attr = A1;
    e->pattern = e->initPattern = A2;
    RET(addObject(std::move(e)));
}
void sceKernelDeleteEventFlag(PspContext* c) {
    if (!getObject<EventFlag>(A0, ObjType::EventFlag)) {
        RET(ERR_UNKNOWN_EVFID);
        return;
    }
    wakeWaiters(WaitType::EventFlag, A0, (s32)ERR_WAIT_DELETE, false);
    removeObject(A0);
    rescheduleSoon();
    RET(0);
}
void sceKernelSetEventFlag(PspContext* c) {
    EventFlag* e = getObject<EventFlag>(A0, ObjType::EventFlag);
    if (!e) {
        RET(ERR_UNKNOWN_EVFID);
        return;
    }
    e->pattern |= A1;
    for (Thread* t : waitingThreads(WaitType::EventFlag, A0))
        if (t->waitCheck && t->waitCheck()) wakeThread(t, 0);
    rescheduleSoon();
    RET(0);
}
void sceKernelClearEventFlag(PspContext* c) {
    EventFlag* e = getObject<EventFlag>(A0, ObjType::EventFlag);
    if (!e) {
        RET(ERR_UNKNOWN_EVFID);
        return;
    }
    e->pattern &= A1;
    RET(0);
}
void evfWait(PspContext* c, bool cb, bool poll) {
    u32 id = A0, bits = A1, mode = A2, outBits = A3, timeout = poll ? 0 : A4;
    EventFlag* e = getObject<EventFlag>(id, ObjType::EventFlag);
    if (!e) {
        RET(ERR_UNKNOWN_EVFID);
        return;
    }
    if (mode & ~0x31u) {
        RET(ERR_ILLEGAL_MODE);
        return;
    }
    if (bits == 0) {
        RET(ERR_EVF_ILPAT);
        return;
    }
    auto claim = [id, bits, mode, outBits]() {
        EventFlag* e2 = getObject<EventFlag>(id, ObjType::EventFlag);
        if (!e2 || !evfMatch(e2->pattern, bits, mode)) return false;
        if (outBits) wr32(outBits, e2->pattern);
        if (mode & EVF_CLEARALL) e2->pattern = 0;
        else if (mode & EVF_CLEARPAT) e2->pattern &= ~bits;
        return true;
    };
    if (cb) checkCallbacks();
    if (claim()) {
        RET(0);
        return;
    }
    if (poll) {
        if (outBits) wr32(outBits, e->pattern);
        RET(ERR_EVF_COND);
        return;
    }
    if (!(e->attr & EVF_WAITMULTIPLE) && !waitingThreads(WaitType::EventFlag, id).empty()) {
        RET(ERR_EVF_MULTI);
        return;
    }
    cur()->waitCheck = claim;
    s32 r = waitCurrent(WaitType::EventFlag, id, timeout, cb);
    if (r != 0 && outBits) {
        EventFlag* e2 = getObject<EventFlag>(id, ObjType::EventFlag);
        if (e2) wr32(outBits, e2->pattern);
    }
    RET(r);
}
void sceKernelWaitEventFlag(PspContext* c) { evfWait(c, false, false); }
void sceKernelWaitEventFlagCB(PspContext* c) { evfWait(c, true, false); }
void sceKernelPollEventFlag(PspContext* c) { evfWait(c, false, true); }
void sceKernelReferEventFlagStatus(PspContext* c) {
    EventFlag* e = getObject<EventFlag>(A0, ObjType::EventFlag);
    if (!e) {
        RET(ERR_UNKNOWN_EVFID);
        return;
    }
    u32 p = A1;
    wr32(p, 0x34);
    writeCString(p + 4, e->name, 32);
    wr32(p + 0x24, e->attr);
    wr32(p + 0x28, e->initPattern);
    wr32(p + 0x2C, e->pattern);
    wr32(p + 0x30, (u32)waitingThreads(WaitType::EventFlag, A0).size());
    RET(0);
}

// ================================================================== mutexes
void sceKernelCreateMutex(PspContext* c) {
    auto m = std::make_unique<Mutex>();
    m->type = ObjType::Mutex;
    m->name = readCString(A0, 32);
    m->attr = A1;
    m->count = (s32)A2;
    m->owner = A2 ? currentThreadId() : 0;
    RET(addObject(std::move(m)));
}
void sceKernelDeleteMutex(PspContext* c) {
    if (!getObject<Mutex>(A0, ObjType::Mutex)) {
        RET(ERR_MUTEX_NOT_FOUND);
        return;
    }
    wakeWaiters(WaitType::Mutex, A0, (s32)ERR_WAIT_DELETE, false);
    removeObject(A0);
    RET(0);
}
void mutexLock(PspContext* c, bool cb, bool tryOnly) {
    u32 id = A0;
    s32 count = (s32)A1;
    Mutex* m = getObject<Mutex>(id, ObjType::Mutex);
    if (!m) {
        RET(ERR_MUTEX_NOT_FOUND);
        return;
    }
    u32 me = currentThreadId();
    if (m->count == 0) {
        m->count = count;
        m->owner = me;
        RET(0);
        return;
    }
    if (m->owner == me) {
        if (!(m->attr & 0x200)) {
            RET(ERR_MUTEX_RECURSIVE_NOT_ALLOWED);
            return;
        }
        m->count += count;
        RET(0);
        return;
    }
    if (tryOnly) {
        RET(ERR_MUTEX_LOCKED);
        return;
    }
    cur()->waitCheck = [id, count, me]() {
        Mutex* m2 = getObject<Mutex>(id, ObjType::Mutex);
        if (m2 && m2->count == 0) {
            m2->count = count;
            m2->owner = me;
            return true;
        }
        return false;
    };
    RET(waitCurrent(WaitType::Mutex, id, A2, cb));
}
void sceKernelLockMutex(PspContext* c) { mutexLock(c, false, false); }
void sceKernelLockMutexCB(PspContext* c) { mutexLock(c, true, false); }
void sceKernelTryLockMutex(PspContext* c) { mutexLock(c, false, true); }
void sceKernelUnlockMutex(PspContext* c) {
    Mutex* m = getObject<Mutex>(A0, ObjType::Mutex);
    if (!m) {
        RET(ERR_MUTEX_NOT_FOUND);
        return;
    }
    if (m->count == 0 || m->owner != currentThreadId()) {
        RET(ERR_MUTEX_UNLOCKED);
        return;
    }
    if (m->count < (s32)A1) {
        RET(ERR_MUTEX_UNLOCK_UNDERFLOW);
        return;
    }
    m->count -= (s32)A1;
    if (m->count == 0) {
        m->owner = 0;
        for (Thread* t : waitingThreads(WaitType::Mutex, A0))
            if (t->waitCheck && t->waitCheck()) {
                wakeThread(t, 0);
                break;
            }
        rescheduleSoon();
    }
    RET(0);
}

// ================================================================== lightweight mutexes
// Workarea layout: s32 count; u32 ownerThread; u32 attr; s32 numWait; u32 uid; u32 pad[3]
void sceKernelCreateLwMutex(PspContext* c) {
    u32 wa = A0;
    auto m = std::make_unique<Mutex>();
    m->type = ObjType::LwMutex;
    m->name = readCString(A1, 32);
    m->attr = A2;
    u32 uid = addObject(std::move(m));
    wr32(wa + 0, A3);
    wr32(wa + 4, A3 ? currentThreadId() : 0);
    wr32(wa + 8, A2);
    wr32(wa + 12, 0);
    wr32(wa + 16, uid);
    RET(0);
}
void sceKernelDeleteLwMutex(PspContext* c) {
    u32 wa = A0;
    u32 uid = rd32(wa + 16);
    wakeWaiters(WaitType::LwMutex, uid, (s32)ERR_WAIT_DELETE, false);
    removeObject(uid);
    wr32(wa + 16, 0);
    RET(0);
}
void lwLock(PspContext* c, bool cb, bool tryOnly) {
    u32 wa = A0;
    s32 count = (s32)A1;
    u32 me = currentThreadId();
    s32 cnt = (s32)rd32(wa);
    u32 owner = rd32(wa + 4), attr = rd32(wa + 8), uid = rd32(wa + 16);
    if (!uid) {
        RET(ERR_LWMUTEX_NOT_FOUND);
        return;
    }
    if (cnt == 0) {
        wr32(wa, (u32)count);
        wr32(wa + 4, me);
        RET(0);
        return;
    }
    if (owner == me) {
        if (!(attr & 0x200)) {
            RET(ERR_LWMUTEX_RECURSIVE_NOT_ALLOWED);
            return;
        }
        wr32(wa, (u32)(cnt + count));
        RET(0);
        return;
    }
    if (tryOnly) {
        RET(ERR_LWMUTEX_LOCKED);
        return;
    }
    wr32(wa + 12, rd32(wa + 12) + 1);
    cur()->waitCheck = [wa, count, me]() {
        if (rd32(wa) == 0) {
            wr32(wa, (u32)count);
            wr32(wa + 4, me);
            wr32(wa + 12, rd32(wa + 12) - 1);
            return true;
        }
        return false;
    };
    s32 r = waitCurrent(WaitType::LwMutex, uid, A2, cb);
    RET(r);
}
void sceKernelLockLwMutex(PspContext* c) { lwLock(c, false, false); }
void sceKernelLockLwMutexCB(PspContext* c) { lwLock(c, true, false); }
void sceKernelTryLockLwMutex(PspContext* c) { lwLock(c, false, true); }
void sceKernelUnlockLwMutex(PspContext* c) {
    u32 wa = A0;
    s32 cnt = (s32)rd32(wa);
    if (cnt == 0 || rd32(wa + 4) != currentThreadId()) {
        RET(ERR_LWMUTEX_UNLOCKED);
        return;
    }
    if (cnt < (s32)A1) {
        RET(ERR_LWMUTEX_UNLOCK_UNDERFLOW);
        return;
    }
    cnt -= (s32)A1;
    wr32(wa, (u32)cnt);
    if (cnt == 0) {
        wr32(wa + 4, 0);
        for (Thread* t : waitingThreads(WaitType::LwMutex, rd32(wa + 16)))
            if (t->waitCheck && t->waitCheck()) {
                wakeThread(t, 0);
                break;
            }
        rescheduleSoon();
    }
    RET(0);
}

// ================================================================== message boxes
void sceKernelCreateMbx(PspContext* c) {
    auto m = std::make_unique<Mbx>();
    m->type = ObjType::Mbx;
    m->name = readCString(A0, 32);
    m->attr = A1;
    RET(addObject(std::move(m)));
}
void sceKernelDeleteMbx(PspContext* c) {
    if (!getObject<Mbx>(A0, ObjType::Mbx)) {
        RET(ERR_UNKNOWN_MBXID);
        return;
    }
    wakeWaiters(WaitType::Mbx, A0, (s32)ERR_WAIT_DELETE, false);
    removeObject(A0);
    RET(0);
}
void sceKernelSendMbx(PspContext* c) {
    Mbx* m = getObject<Mbx>(A0, ObjType::Mbx);
    if (!m) {
        RET(ERR_UNKNOWN_MBXID);
        return;
    }
    wr32(A1, 0);  // next pointer
    m->messages.push_back(A1);
    for (Thread* t : waitingThreads(WaitType::Mbx, A0))
        if (t->waitCheck && t->waitCheck()) {
            wakeThread(t, 0);
            break;
        }
    rescheduleSoon();
    RET(0);
}
void mbxRecv(PspContext* c, bool poll, bool cb) {
    u32 id = A0, out = A1;
    Mbx* m = getObject<Mbx>(id, ObjType::Mbx);
    if (!m) {
        RET(ERR_UNKNOWN_MBXID);
        return;
    }
    auto claim = [id, out]() {
        Mbx* m2 = getObject<Mbx>(id, ObjType::Mbx);
        if (!m2 || m2->messages.empty()) return false;
        wr32(out, m2->messages.front());
        m2->messages.pop_front();
        return true;
    };
    if (claim()) {
        RET(0);
        return;
    }
    if (poll) {
        RET(ERR_MBOX_NOMSG);
        return;
    }
    cur()->waitCheck = claim;
    RET(waitCurrent(WaitType::Mbx, id, A2, cb));
}
void sceKernelPollMbx(PspContext* c) { mbxRecv(c, true, false); }
void sceKernelReceiveMbx(PspContext* c) { mbxRecv(c, false, false); }
void sceKernelReceiveMbxCB(PspContext* c) { mbxRecv(c, false, true); }

const HleFunction kThreadMan[] = {
    {0x446D8DE6, "sceKernelCreateThread", sceKernelCreateThread},
    {0xF475845D, "sceKernelStartThread", sceKernelStartThread},
    {0xAA73C935, "sceKernelExitThread", sceKernelExitThread},
    {0x809CE29B, "sceKernelExitDeleteThread", sceKernelExitDeleteThread},
    {0x9FA03CD3, "sceKernelDeleteThread", sceKernelDeleteThread},
    {0x616403BA, "sceKernelTerminateThread", sceKernelTerminateThread},
    {0x293B45B8, "sceKernelGetThreadId", sceKernelGetThreadId},
    {0x94AA61EE, "sceKernelGetThreadCurrentPriority", sceKernelGetThreadCurrentPriority},
    {0x71BC9871, "sceKernelChangeThreadPriority", sceKernelChangeThreadPriority},
    {0xEA748E31, "sceKernelChangeCurrentThreadAttr", sceKernelChangeCurrentThreadAttr},
    {0x278C0DF5, "sceKernelWaitThreadEnd", sceKernelWaitThreadEnd},
    {0x17C1684E, "sceKernelReferThreadStatus", sceKernelReferThreadStatus},
    {0x9944F31F, "sceKernelSuspendThread", sceKernelSuspendThread},
    {0x75156E8F, "sceKernelResumeThread", sceKernelResumeThread},
    {0xCEADEB47, "sceKernelDelayThread", sceKernelDelayThread},
    {0x68DA9E36, "sceKernelDelayThreadCB", sceKernelDelayThreadCB},
    {0x9ACE131E, "sceKernelSleepThread", sceKernelSleepThread},
    {0x82826F70, "sceKernelSleepThreadCB", sceKernelSleepThreadCB},
    {0xD59EAD2F, "sceKernelWakeupThread", sceKernelWakeupThread},
    {0x349D6D6C, "sceKernelCheckCallback", sceKernelCheckCallback},
    {0x369ED59D, "sceKernelGetSystemTimeLow", sceKernelGetSystemTimeLow},
    {0x82BC5777, "sceKernelGetSystemTimeWide", sceKernelGetSystemTimeWide},
    {0xDB738F35, "sceKernelGetSystemTime", sceKernelGetSystemTime},
    {0x912354A7, "sceKernelRotateThreadReadyQueue", sceKernelRotateThreadReadyQueue},
    {0xE81CAF8F, "sceKernelCreateCallback", sceKernelCreateCallback},
    {0xEDBA5844, "sceKernelDeleteCallback", sceKernelDeleteCallback},
    {0xC11BA8C4, "sceKernelNotifyCallback", sceKernelNotifyCallback},
    {0xD6DA4BA1, "sceKernelCreateSema", sceKernelCreateSema},
    {0x28B6489C, "sceKernelDeleteSema", sceKernelDeleteSema},
    {0x4E3A1105, "sceKernelWaitSema", sceKernelWaitSema},
    {0x6D212BAC, "sceKernelWaitSemaCB", sceKernelWaitSemaCB},
    {0x3F53E640, "sceKernelSignalSema", sceKernelSignalSema},
    {0x58B1F937, "sceKernelPollSema", sceKernelPollSema},
    {0xBC6FEBC5, "sceKernelReferSemaStatus", sceKernelReferSemaStatus},
    {0x55C20A00, "sceKernelCreateEventFlag", sceKernelCreateEventFlag},
    {0xEF9E4C70, "sceKernelDeleteEventFlag", sceKernelDeleteEventFlag},
    {0x1FB15A32, "sceKernelSetEventFlag", sceKernelSetEventFlag},
    {0x812346E4, "sceKernelClearEventFlag", sceKernelClearEventFlag},
    {0x402FCF22, "sceKernelWaitEventFlag", sceKernelWaitEventFlag},
    {0x328C546A, "sceKernelWaitEventFlagCB", sceKernelWaitEventFlagCB},
    {0x30FD48F0, "sceKernelPollEventFlag", sceKernelPollEventFlag},
    {0xA66B0120, "sceKernelReferEventFlagStatus", sceKernelReferEventFlagStatus},
    {0xB7D098C6, "sceKernelCreateMutex", sceKernelCreateMutex},
    {0xF8170FBE, "sceKernelDeleteMutex", sceKernelDeleteMutex},
    {0xB011B11F, "sceKernelLockMutex", sceKernelLockMutex},
    {0x5BF4DD27, "sceKernelLockMutexCB", sceKernelLockMutexCB},
    {0x0DDCD2C9, "sceKernelTryLockMutex", sceKernelTryLockMutex},
    {0x6B30100F, "sceKernelUnlockMutex", sceKernelUnlockMutex},
    {0x19CFF145, "sceKernelCreateLwMutex", sceKernelCreateLwMutex},
    {0x60107536, "sceKernelDeleteLwMutex", sceKernelDeleteLwMutex},
    {0x8125221D, "sceKernelCreateMbx", sceKernelCreateMbx},
    {0x86255ADA, "sceKernelDeleteMbx", sceKernelDeleteMbx},
    {0xE9B3061E, "sceKernelSendMbx", sceKernelSendMbx},
    {0x0D81716A, "sceKernelPollMbx", sceKernelPollMbx},
    {0x18260574, "sceKernelReceiveMbx", sceKernelReceiveMbx},
    {0xF3986382, "sceKernelReceiveMbxCB", sceKernelReceiveMbxCB},
};

// Kernel_Library (interrupt state + lightweight mutex fast path)
void sceKernelCpuSuspendIntr(PspContext* c) {
    RET(interruptsEnabled() ? 1 : 0);
    setInterruptsEnabled(false);
}
void sceKernelCpuResumeIntr(PspContext* c) {
    setInterruptsEnabled(A0 != 0);
    RET(0);
}
const HleFunction kKernelLib[] = {
    {0x092968F4, "sceKernelCpuSuspendIntr", sceKernelCpuSuspendIntr},
    {0x5F10D406, "sceKernelCpuResumeIntr", sceKernelCpuResumeIntr},
    {0x3B84732D, "sceKernelCpuResumeIntrWithSync", sceKernelCpuResumeIntr},
    {0xBEA46419, "sceKernelLockLwMutex", sceKernelLockLwMutex},
    {0x1FC64E09, "sceKernelLockLwMutexCB", sceKernelLockLwMutexCB},
    {0xDC692EE3, "sceKernelTryLockLwMutex", sceKernelTryLockLwMutex},
    {0x15B6446B, "sceKernelUnlockLwMutex", sceKernelUnlockLwMutex},
};

// InterruptManager
void sceKernelRegisterSubIntrHandler(PspContext* c) {
    registerSubIntr(A0, A1, A2, A3);
    RET(0);
}
void sceKernelReleaseSubIntrHandler(PspContext* c) {
    releaseSubIntr(A0, A1);
    RET(0);
}
void sceKernelEnableSubIntr(PspContext* c) {
    enableSubIntr(A0, A1, true);
    RET(0);
}
void sceKernelDisableSubIntr(PspContext* c) {
    enableSubIntr(A0, A1, false);
    RET(0);
}
const HleFunction kIntr[] = {
    {0xCA04A2B9, "sceKernelRegisterSubIntrHandler", sceKernelRegisterSubIntrHandler},
    {0xD61E6961, "sceKernelReleaseSubIntrHandler", sceKernelReleaseSubIntrHandler},
    {0xFB8E22EC, "sceKernelEnableSubIntr", sceKernelEnableSubIntr},
    {0x8A389411, "sceKernelDisableSubIntr", sceKernelDisableSubIntr},
};

}  // namespace

HLE_MODULE("ThreadManForUser", kThreadMan);
HLE_MODULE("Kernel_Library", kKernelLib);
HLE_MODULE("InterruptManager", kIntr);

}  // namespace vc2rt::kernel
