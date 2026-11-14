// User memory partition allocator (partition 2: 0x08800000 - 0x0A000000) and SysMemUserForUser.
#include <map>

#include "../hle/hle.h"
#include "kernel.h"

namespace vc2rt::kernel {

struct MemBlock : KObject {
    u32 addr = 0, size = 0;
};

static std::map<u32, u32> g_used;  // addr -> size

static bool rangeFree(u32 a, u32 size) {
    if (a < kUserBase || a + size > kUserEnd || a + size < a) return false;
    auto it = g_used.upper_bound(a);
    if (it != g_used.end() && it->first < a + size) return false;
    if (it != g_used.begin()) {
        --it;
        if (it->first + it->second > a) return false;
    }
    return true;
}

static u32 findLow(u32 size, u32 align) {
    u32 a = kUserBase;
    for (auto& [ua, us] : g_used) {
        u32 cand = (a + align - 1) & ~(align - 1);
        if (cand + size <= ua) return cand;
        a = std::max(a, ua + us);
    }
    u32 cand = (a + align - 1) & ~(align - 1);
    return cand + size <= kUserEnd ? cand : 0;
}

static u32 findHigh(u32 size, u32 align) {
    u32 end = kUserEnd;
    for (auto it = g_used.rbegin(); it != g_used.rend(); ++it) {
        u32 blockEnd = it->first + it->second;
        if (blockEnd <= end) {
            if (end - blockEnd >= size) {
                u32 cand = (end - size) & ~(align - 1);
                if (cand >= blockEnd) return cand;
            }
            end = it->first;
        } else {
            end = std::min(end, it->first);
        }
    }
    if (end >= kUserBase + size) {
        u32 cand = (end - size) & ~(align - 1);
        if (cand >= kUserBase) return cand;
    }
    return 0;
}

u32 allocUserMemory(u32 size, int type, u32 addr, const std::string& name, u32* outAddr) {
    if (size == 0) return 0;
    size = (size + 0xFF) & ~0xFFu;
    u32 a = 0;
    switch (type) {
        case 0: a = findLow(size, 0x100); break;
        case 1: a = findHigh(size, 0x100); break;
        case 2: a = rangeFree(addr & ~0xFFu, size) ? (addr & ~0xFFu) : 0; break;
        case 3: a = findLow(size, std::max<u32>(addr, 0x100)); break;
        case 4: a = findHigh(size, std::max<u32>(addr, 0x100)); break;
        default: return 0;
    }
    if (!a) {
        LOG_WARN("SYSMEM", "allocation of %x bytes (%s, type %d) failed", size, name.c_str(), type);
        return 0;
    }
    g_used[a] = size;
    auto b = std::make_unique<MemBlock>();
    b->type = ObjType::MemBlock;
    b->name = name;
    b->addr = a;
    b->size = size;
    if (outAddr) *outAddr = a;
    u32 uid = addObject(std::move(b));
    LOG_DEBUG("SYSMEM", "alloc '%s' %x bytes at %08x (uid %x)", name.c_str(), size, a, uid);
    return uid;
}

u32 reserveFixedRange(u32 addr, u32 size, const std::string& name) {
    g_used[addr] = size;
    auto b = std::make_unique<MemBlock>();
    b->type = ObjType::MemBlock;
    b->name = name;
    b->addr = addr;
    b->size = size;
    return addObject(std::move(b));
}

void freeUserMemory(u32 uid) {
    MemBlock* b = getObject<MemBlock>(uid, ObjType::MemBlock);
    if (!b) return;
    g_used.erase(b->addr);
    removeObject(uid);
}

u32 blockAddress(u32 uid) {
    MemBlock* b = getObject<MemBlock>(uid, ObjType::MemBlock);
    return b ? b->addr : 0;
}

u32 maxFreeMemory() {
    u32 best = 0, a = kUserBase;
    for (auto& [ua, us] : g_used) {
        if (ua > a) best = std::max(best, ua - a);
        a = std::max(a, ua + us);
    }
    if (kUserEnd > a) best = std::max(best, kUserEnd - a);
    return best & ~0xFFu;
}
u32 totalFreeMemory() {
    u32 used = 0;
    for (auto& [ua, us] : g_used)
        if (ua >= kUserBase && ua < kUserEnd) used += us;
    return (kUserEnd - kUserBase) - used;
}

// ------------------------------------------------------------------ HLE
namespace {
void sceKernelAllocPartitionMemory(PspContext* c) {
    u32 part = A0, type = A2, size = A3, addr = A4;
    std::string name = readCString(A1, 32);
    if (part != 2 && part != 5 && part != 6) {
        RET(ERR_ILLEGAL_PARTITION);
        return;
    }
    if (type > 4) {
        RET(ERR_ILLEGAL_MEMBLOCKTYPE);
        return;
    }
    u32 uid = allocUserMemory(size, (int)type, addr, name, nullptr);
    RET(uid ? uid : ERR_NO_MEMORY);
}
void sceKernelFreePartitionMemory(PspContext* c) {
    if (!getObject<MemBlock>(A0, ObjType::MemBlock)) {
        RET(ERR_ILLEGAL_MEMBLOCK);
        return;
    }
    freeUserMemory(A0);
    RET(0);
}
void sceKernelGetBlockHeadAddr(PspContext* c) { RET(blockAddress(A0)); }
void sceKernelMaxFreeMemSize(PspContext* c) { RET(maxFreeMemory()); }
void sceKernelTotalFreeMemSize(PspContext* c) { RET(totalFreeMemory()); }
void sceKernelAllocMemoryBlock(PspContext* c) {
    // (name, type 0=low 1=high, size, opt)
    std::string name = readCString(A0, 32);
    u32 uid = allocUserMemory(A2, A1 == 1 ? 1 : 0, 0, name, nullptr);
    RET(uid ? uid : ERR_NO_MEMORY);
}
void sceKernelGetMemoryBlockAddr(PspContext* c) {
    u32 a = blockAddress(A0);
    if (!a) {
        RET(ERR_ILLEGAL_MEMBLOCK);
        return;
    }
    if (A1) wr32(A1, a);
    RET(0);
}
void sceKernelFreeMemoryBlock(PspContext* c) {
    freeUserMemory(A0);
    RET(0);
}
void sceKernelSetCompiledSdkVersion(PspContext* c) { RET(0); }
void sceKernelPrintf(PspContext* c) {
    LOG_INFO("PRINTF", "%s", readCString(A0, 512).c_str());
    RET(0);
}

const HleFunction kSysMem[] = {
    {0x237DBD4F, "sceKernelAllocPartitionMemory", sceKernelAllocPartitionMemory},
    {0xB6D61D02, "sceKernelFreePartitionMemory", sceKernelFreePartitionMemory},
    {0x9D9A5BA1, "sceKernelGetBlockHeadAddr", sceKernelGetBlockHeadAddr},
    {0xA291F107, "sceKernelMaxFreeMemSize", sceKernelMaxFreeMemSize},
    {0xF919F628, "sceKernelTotalFreeMemSize", sceKernelTotalFreeMemSize},
    {0xFE707FDF, "sceKernelAllocMemoryBlock", sceKernelAllocMemoryBlock},
    {0xDB83A952, "sceKernelGetMemoryBlockAddr", sceKernelGetMemoryBlockAddr},
    {0x50F61D8A, "sceKernelFreeMemoryBlock", sceKernelFreeMemoryBlock},
    {0x91DE343C, "sceKernelSetCompiledSdkVersion500_505", sceKernelSetCompiledSdkVersion},
    {0xF77D77CB, "sceKernelSetCompilerVersion", sceKernelSetCompiledSdkVersion},
    {0x13A5ABEF, "sceKernelPrintf", sceKernelPrintf},
};
}  // namespace
HLE_MODULE("SysMemUserForUser", kSysMem);

}  // namespace vc2rt::kernel
