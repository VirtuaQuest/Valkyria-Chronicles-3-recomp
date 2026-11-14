#pragma once
// Internal runtime services shared by all HLE modules.
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>

#include "../../common/types.h"
#include "../psp_recomp.h"
#include "../vc2_runtime.h"

namespace vc2rt {

using u8 = ::u8;
using u16 = ::u16;
using u32 = ::u32;
using u64 = ::u64;
using s32 = ::s32;
using s64 = ::s64;

// ------------------------------------------------------------------ logging
enum class LogLevel { Trace, Debug, Info, Warn, Error };
void logInit(const std::filesystem::path& file);
void logMsg(LogLevel lvl, const char* tag, const char* fmt, ...);
// Debug: counts log messages containing `needle` (one needle; used to anchor scripted input).
void logWatch(const std::string& needle);
u32 logWatchHits();
void setLogLevel(LogLevel l);
#define LOG_TRACE(tag, ...) ::vc2rt::logMsg(::vc2rt::LogLevel::Trace, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) ::vc2rt::logMsg(::vc2rt::LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...) ::vc2rt::logMsg(::vc2rt::LogLevel::Info, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) ::vc2rt::logMsg(::vc2rt::LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) ::vc2rt::logMsg(::vc2rt::LogLevel::Error, tag, __VA_ARGS__)
[[noreturn]] void fatal(const char* fmt, ...);

// ------------------------------------------------------------------ memory
// PSP physical map (masked with 0x1FFFFFFF):
//   0x00010000 scratchpad (16KB)   0x04000000 VRAM (2MB)   0x08000000 RAM (32MB, user 0x08800000+)
constexpr u32 kScratchpadBase = 0x00010000;
constexpr u32 kVramBase = 0x04000000;
constexpr u32 kVramSize = 0x00200000;
constexpr u32 kRamBase = 0x08000000;
constexpr u32 kRamSize = 0x02000000;
constexpr u32 kUserBase = 0x08800000;
constexpr u32 kUserEnd = 0x0A000000;

bool memInit();
inline u8* memPtr(u32 addr) { return PSP_ADDR(addr); }
inline bool memValid(u32 addr) {
    u32 a = addr & 0x1FFFFFFF;
    return (a >= kRamBase && a < kRamBase + kRamSize) || (a >= kVramBase && a < kVramBase + 0x00800000) ||
           (a >= kScratchpadBase && a < kScratchpadBase + 0x4000);
}
inline u32 rd32(u32 a) { return LW(a); }
inline void wr32(u32 a, u32 v) { SW(a, v); }
inline u16 rd16(u32 a) { return (u16)LHU(a); }
inline void wr16(u32 a, u16 v) { SH(a, v); }
inline u8 rd8(u32 a) { return (u8)LBU(a); }
inline void wr8(u32 a, u8 v) { SB(a, v); }
inline void wr64(u32 a, u64 v) { memcpy(memPtr(a), &v, 8); }
inline u64 rd64(u32 a) { u64 v; memcpy(&v, memPtr(a), 8); return v; }
std::string readCString(u32 addr, size_t max = 1024);
void writeCString(u32 addr, const std::string& s, size_t cap);

// ------------------------------------------------------------------ cpu glue
const Vc2GameImage& gameImage();
PspFunc findFunction(u32 addr);
// Calls a guest function on the current host stack using `ctx` (registers already set up).
void callGuest(PspContext* ctx, u32 addr);
// Calls a guest function from HLE code (interrupt handlers, callbacks) with up to 6 args using a
// scratch context whose stack pointer is `sp`. Returns v0.
u32 callGuestSimple(u32 addr, u32 sp, u32 gp, std::initializer_list<u32> args);

// ------------------------------------------------------------------ time
u64 hostMicros();       // monotonic microseconds since boot
u64 pspTimeMicros();    // PSP system time (sceKernelGetSystemTime)

// ------------------------------------------------------------------ paths
struct Paths {
    std::filesystem::path exeDir;
    std::filesystem::path isoPath;
    std::filesystem::path saveDir;   // maps ms0:/
    std::filesystem::path configFile;
    std::filesystem::path logFile;
    std::filesystem::path iconFile;
};
Paths& paths();

inline std::string pathToUtf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ------------------------------------------------------------------ HLE argument helpers
#define A0 (c->r[4])
#define A1 (c->r[5])
#define A2 (c->r[6])
#define A3 (c->r[7])
#define A4 (c->r[8])
#define A5 (c->r[9])
#define A6 (c->r[10])
#define A7 (c->r[11])
#define RET(v) (c->r[2] = (u32)(v))
#define RET64(v) (c->r[2] = (u32)(u64)(v), c->r[3] = (u32)((u64)(v) >> 32))

}  // namespace vc2rt
