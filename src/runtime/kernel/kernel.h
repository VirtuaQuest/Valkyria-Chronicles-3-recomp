#pragma once
// HLE PSP kernel: object table, threads (one Windows fiber per PSP thread), waits, scheduling.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/runtime.h"

namespace vc2rt::kernel {

// ------------------------------------------------------------------ error codes
enum : u32 {
    ERR_ERROR = 0x80020001,
    ERR_ILLEGAL_ARGUMENT = 0x800200d2,
    ERR_ILLEGAL_ADDR = 0x800200d3,
    ERR_ILLEGAL_PARTITION = 0x800200d6,
    ERR_ILLEGAL_MEMBLOCKTYPE = 0x800200d8,
    ERR_UNKNOWN_UID = 0x800200cb,
    ERR_NO_MEMORY = 0x80020190,
    ERR_ILLEGAL_ATTR = 0x80020191,
    ERR_ILLEGAL_ENTRY = 0x80020192,
    ERR_ILLEGAL_PRIORITY = 0x80020193,
    ERR_ILLEGAL_STACK_SIZE = 0x80020194,
    ERR_ILLEGAL_MODE = 0x80020195,
    ERR_ILLEGAL_MASK = 0x80020196,
    ERR_ILLEGAL_THID = 0x80020197,
    ERR_UNKNOWN_THID = 0x80020198,
    ERR_UNKNOWN_SEMID = 0x80020199,
    ERR_UNKNOWN_EVFID = 0x8002019a,
    ERR_UNKNOWN_MBXID = 0x8002019b,
    ERR_UNKNOWN_CBID = 0x800201a1,
    ERR_DORMANT = 0x800201a2,
    ERR_NOT_DORMANT = 0x800201a4,
    ERR_CAN_NOT_WAIT = 0x800201a7,
    ERR_WAIT_TIMEOUT = 0x800201a8,
    ERR_WAIT_CANCEL = 0x800201a9,
    ERR_THREAD_TERMINATED = 0x800201ac,
    ERR_SEMA_ZERO = 0x800201ad,
    ERR_SEMA_OVF = 0x800201ae,
    ERR_EVF_COND = 0x800201af,
    ERR_EVF_MULTI = 0x800201b0,
    ERR_EVF_ILPAT = 0x800201b1,
    ERR_MBOX_NOMSG = 0x800201b2,
    ERR_WAIT_DELETE = 0x800201b5,
    ERR_ILLEGAL_MEMBLOCK = 0x800201b6,
    ERR_ILLEGAL_MEMSIZE = 0x800201b7,
    ERR_ILLEGAL_COUNT = 0x800201bd,
    ERR_MUTEX_NOT_FOUND = 0x800201c3,
    ERR_MUTEX_LOCKED = 0x800201c4,
    ERR_MUTEX_UNLOCKED = 0x800201c5,
    ERR_MUTEX_UNLOCK_UNDERFLOW = 0x800201c7,
    ERR_MUTEX_RECURSIVE_NOT_ALLOWED = 0x800201c8,
    ERR_LWMUTEX_NOT_FOUND = 0x800201ca,
    ERR_LWMUTEX_LOCKED = 0x800201cb,
    ERR_LWMUTEX_UNLOCKED = 0x800201cc,
    ERR_LWMUTEX_UNLOCK_UNDERFLOW = 0x800201ce,
    ERR_LWMUTEX_RECURSIVE_NOT_ALLOWED = 0x800201cf,
};

// ------------------------------------------------------------------ objects
enum class ObjType { Thread, Sema, EventFlag, Mutex, LwMutex, Mbx, Callback, MemBlock, Module, Other };

struct KObject {
    u32 uid = 0;
    ObjType type = ObjType::Other;
    std::string name;
    virtual ~KObject() = default;
};

u32 addObject(std::unique_ptr<KObject> obj);
KObject* findObject(u32 uid);
template <class T>
T* getObject(u32 uid, ObjType t) {
    KObject* o = findObject(uid);
    return (o && o->type == t) ? static_cast<T*>(o) : nullptr;
}
void removeObject(u32 uid);
std::vector<KObject*> objectsOfType(ObjType t);

// ------------------------------------------------------------------ threads
enum class ThreadState { Dormant, Ready, Running, Waiting, Suspended, WaitSuspended, Dead };
enum class WaitType : u32 {
    None = 0, Sleep = 1, Delay = 2, Sema = 3, EventFlag = 4, Mbx = 5, Vpl = 6, Fpl = 7, MsgPipe = 8,
    ThreadEnd = 9, EventHandler = 10, Callback = 11, Mutex = 12, LwMutex = 13,
    // runtime specific (reported as Delay/Sleep to the game)
    Vblank = 100, GeSync, Audio, Io, Umd, Generic,
};

struct Thread : KObject {
    u32 entry = 0, attr = 0, gp = 0;
    int initPriority = 0, priority = 0;
    u32 stackBlock = 0, stackAddr = 0, stackSize = 0;
    PspContext ctx{};
    void* fiber = nullptr;
    ThreadState state = ThreadState::Dormant;
    s64 readySeq = 0;
    // wait
    WaitType waitType = WaitType::None;
    u32 waitId = 0;
    u64 wakeTime = 0;          // 0 = no timeout
    u32 timeoutPtr = 0;        // guest address receiving remaining timeout
    u64 waitStart = 0;
    bool waitCB = false;       // callbacks may run during this wait
    s32 waitResult = 0;
    u32 waitValue0 = 0, waitValue1 = 0, waitValue2 = 0;  // per-wait-type data
    std::function<bool()> waitCheck;  // optional: returns true when condition satisfied
    int wakeupCount = 0;
    u32 exitStatus = 0;
    u32 argSize = 0, argAddr = 0;
    std::vector<u32> callbacks;
    bool runCallbacksPending = false;
    // diagnostics (thread dumps): last kernel call and time spent running since the last dump
    const char* lastHle = nullptr;
    u32 lastHleRa = 0;
    u64 hleCalls = 0, runMicros = 0;
};

Thread* currentThread();
u32 currentThreadId();
Thread* getThread(u32 uid);  // 0 => current

void kernelInit();
// Creates and starts the module's main thread.
u32 createThread(const std::string& name, u32 entry, int prio, u32 stackSize, u32 attr, u32 gp);
int startThread(u32 uid, u32 argSize, u32 argp);
// Runs the scheduler loop on the host main thread until the game exits.
int runScheduler();
void requestExit(int code);
bool exitRequested();

// Blocks the current thread. Returns the wait result (0 or an error code). timeoutPtr is a guest
// pointer to a u32 microsecond timeout (0 = infinite).
s32 waitCurrent(WaitType type, u32 id, u32 timeoutPtr, bool callbacks);
s32 waitCurrentFor(WaitType type, u32 id, u64 micros, bool callbacks);  // fixed host timeout
void wakeThread(Thread* t, s32 result);
// Wakes every thread waiting on (type,id) whose waitCheck passes (or all, if none).
int wakeWaiters(WaitType type, u32 id, s32 result, bool onlyIfCheckPasses);
std::vector<Thread*> waitingThreads(WaitType type, u32 id);
void yieldCurrent();           // rotate within priority
void rescheduleSoon();         // request a check after the current HLE call
void afterHleCall(PspContext* c);
void exitCurrentThread(u32 status, bool deleteSelf);
void terminateThread(Thread* t);

// callbacks
u32 createCallback(const std::string& name, u32 func, u32 arg);
void notifyCallback(u32 cbid, u32 arg2);
int checkCallbacks();  // runs pending callbacks of the current thread; returns count

// interrupts
bool interruptsEnabled();
void setInterruptsEnabled(bool on);
void registerSubIntr(u32 intr, u32 sub, u32 handler, u32 arg);
void releaseSubIntr(u32 intr, u32 sub);
void enableSubIntr(u32 intr, u32 sub, bool on);
void runSubIntrHandlers(u32 intr, u32 arg = 0);
u32 interruptStackTop();

// periodic events (driven by the scheduler loop on the host thread)
using HostTimerFn = std::function<void(u64 now)>;
void addPeriodicTimer(u64 periodMicros, HostTimerFn fn);   // e.g. vblank
void addOneShotTimer(u64 atMicros, HostTimerFn fn);

// memory partitions
u32 allocUserMemory(u32 size, int type, u32 addr, const std::string& name, u32* outAddr);
void freeUserMemory(u32 blockUid);
u32 blockAddress(u32 blockUid);
u32 reserveFixedRange(u32 addr, u32 size, const std::string& name);
u32 maxFreeMemory();
u32 totalFreeMemory();

}  // namespace vc2rt::kernel
