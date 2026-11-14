#include <windows.h>

#include "runtime.h"

u8* psp_mem = nullptr;

namespace vc2rt {

bool memInit() {
    // Reserve and commit the whole 512MB window once. Pages are zero-filled lazily by the OS, so
    // only memory the game actually touches uses physical RAM.
    psp_mem = (u8*)VirtualAlloc(nullptr, 0x20000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!psp_mem) return false;
    *gameImage().mem = psp_mem;
    return true;
}

std::string readCString(u32 addr, size_t max) {
    std::string s;
    if (!addr) return s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)rd8(addr + (u32)i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

void writeCString(u32 addr, const std::string& s, size_t cap) {
    if (!cap) return;
    size_t n = std::min(s.size(), cap - 1);
    memcpy(memPtr(addr), s.data(), n);
    wr8(addr + (u32)n, 0);
}

}  // namespace vc2rt
