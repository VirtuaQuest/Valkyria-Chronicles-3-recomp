/*
 * Interface between statically recompiled PSP code and the runtime.
 * Generated C files include only this header (plus the generated function prototype list).
 */
#ifndef PSP_RECOMP_H
#define PSP_RECOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#if defined(VC2_RUNTIME_BUILD)
#define PSP_API __declspec(dllexport)
#else
#define PSP_API __declspec(dllimport)
#endif

#if defined(_MSC_VER)
#define PSP_INLINE static __forceinline
#else
#define PSP_INLINE static inline __attribute__((always_inline))
#endif

/* VFPU control register indices (vfpuCtrl[]) */
enum {
    VFPU_CTRL_SPREFIX = 0,
    VFPU_CTRL_TPREFIX = 1,
    VFPU_CTRL_DPREFIX = 2,
    VFPU_CTRL_CC = 3,
    VFPU_CTRL_INF4 = 4,
    VFPU_CTRL_REV = 7,
    VFPU_CTRL_RCX0 = 8,
};

typedef struct PspContext {
    u32 r[32];
    u32 hi, lo;
    union {
        float f[32];
        u32 fi[32];
    };
    u32 fcr31;       /* bit 23 = FP condition */
    union {
        float v[128];
        u32 vi[128];
    };
    u32 vfpuCtrl[16];
    u32 pc;          /* last known guest pc (diagnostics) */
    u32 threadId;    /* owning kernel thread (runtime use) */
    u32 callDepth;   /* runtime use */
    u32 reserved[5];
} PspContext;

typedef void (*PspFunc)(PspContext* c);

typedef struct PspFuncEntry {
    u32 addr;
    u32 size;
    PspFunc fn;
} PspFuncEntry;

typedef struct PspImportEntry {
    const char* lib;
    u32 nid;
    u32 stubAddr;
} PspImportEntry;

typedef struct PspModuleEntry {
    const char* name;
    const char* discPath;
    u32 base;
    const char* sha1;
} PspModuleEntry;

/* ---- provided by the runtime ---- */
/* psp_mem is defined separately in the game executable and in the runtime; both point at the
   same 512MB reservation that mirrors the PSP physical address space. */
extern u8* psp_mem;
PSP_API void psp_hle_call(PspContext* c, u32 importIndex);
PSP_API void psp_call_indirect(PspContext* c, u32 addr);
PSP_API void psp_jump_indirect(PspContext* c, u32 addr);
PSP_API void psp_vfpu(PspContext* c, u32 insn);
PSP_API void psp_syscall(PspContext* c, u32 pc, u32 code);
PSP_API void psp_break(PspContext* c, u32 pc, u32 code);
PSP_API void psp_unhandled(PspContext* c, u32 pc, u32 insn);
PSP_API u32 psp_mfic(PspContext* c);
PSP_API void psp_mtic(PspContext* c, u32 v);
/* Called on entry of game functions listed in the game profile's hook table. */
PSP_API void psp_hook(PspContext* c, u32 addr);

/* ---- provided by the generated code (tables.c) ---- */
#ifndef VC2_RUNTIME_BUILD
extern const PspFuncEntry psp_func_table[];
extern const u32 psp_func_table_count;
extern const PspImportEntry psp_import_table[];
extern const u32 psp_import_table_count;
extern const PspModuleEntry psp_module_table[];
extern const u32 psp_module_table_count;
#endif

/* ---- memory access ---- */
#define PSP_ADDR(a) (psp_mem + ((u32)(a)&0x1FFFFFFFu))

PSP_INLINE u32 LW(u32 a) { u32 v; memcpy(&v, PSP_ADDR(a), 4); return v; }
PSP_INLINE u32 LHU(u32 a) { u16 v; memcpy(&v, PSP_ADDR(a), 2); return v; }
PSP_INLINE u32 LH(u32 a) { s16 v; memcpy(&v, PSP_ADDR(a), 2); return (u32)(s32)v; }
PSP_INLINE u32 LBU(u32 a) { return *PSP_ADDR(a); }
PSP_INLINE u32 LB(u32 a) { return (u32)(s32)(s8)*PSP_ADDR(a); }
PSP_INLINE void SW(u32 a, u32 v) { memcpy(PSP_ADDR(a), &v, 4); }
PSP_INLINE void SH(u32 a, u32 v) { u16 x = (u16)v; memcpy(PSP_ADDR(a), &x, 2); }
PSP_INLINE void SB(u32 a, u32 v) { *PSP_ADDR(a) = (u8)v; }

/* unaligned word helpers (little endian MIPS semantics) */
PSP_INLINE u32 LWL(u32 a, u32 old) {
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    return (old & (0x00FFFFFFu >> sh)) | (w << (24 - sh));
}
PSP_INLINE u32 LWR(u32 a, u32 old) {
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    return (old & ~(0xFFFFFFFFu >> sh)) | (w >> sh);
}
PSP_INLINE void SWL(u32 a, u32 v) {
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    SW(a & ~3u, (w & ~(0xFFFFFFFFu >> (24 - sh))) | (v >> (24 - sh)));
}
PSP_INLINE void SWR(u32 a, u32 v) {
    u32 sh = (a & 3) * 8, w = LW(a & ~3u);
    SW(a & ~3u, (w & ~(0xFFFFFFFFu << sh)) | (v << sh));
}

/* ---- integer helpers ---- */
PSP_INLINE u32 ROTR(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
PSP_INLINE u32 CLZ(u32 v) {
    u32 n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}
PSP_INLINE void DIV(PspContext* c, u32 a, u32 b) {
    s32 x = (s32)a, y = (s32)b;
    if (y == 0) { c->lo = x < 0 ? 1u : 0xFFFFFFFFu; c->hi = a; }
    else if (x == (s32)0x80000000 && y == -1) { c->lo = 0x80000000u; c->hi = 0; }
    else { c->lo = (u32)(x / y); c->hi = (u32)(x % y); }
}
PSP_INLINE void DIVU(PspContext* c, u32 a, u32 b) {
    if (b == 0) { c->lo = a <= 0xFFFFu ? 0xFFFFu : 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}
PSP_INLINE u64 HILO(PspContext* c) { return ((u64)c->hi << 32) | c->lo; }
PSP_INLINE void SETHILO(PspContext* c, u64 v) { c->lo = (u32)v; c->hi = (u32)(v >> 32); }
PSP_INLINE u32 WSBH(u32 v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
PSP_INLINE u32 WSBW(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
PSP_INLINE u32 BITREV(u32 v) {
    u32 r = 0;
    for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}

/* ---- FPU helpers ---- */
PSP_INLINE u32 F2W_SAT(double d) {
    if (d != d) return 0x7FFFFFFFu;
    if (d >= 2147483648.0) return 0x7FFFFFFFu;
    if (d < -2147483648.0) return 0x80000000u;
    return (u32)(s32)d;
}
PSP_INLINE u32 FROUNDW(float f) { return F2W_SAT(nearbyint((double)f)); }
PSP_INLINE u32 FTRUNCW(float f) { return F2W_SAT(trunc((double)f)); }
PSP_INLINE u32 FCEILW(float f) { return F2W_SAT(ceil((double)f)); }
PSP_INLINE u32 FFLOORW(float f) { return F2W_SAT(floor((double)f)); }
PSP_INLINE u32 FCVTWS(PspContext* c, float f) {
    switch (c->fcr31 & 3) {
        case 0: return FROUNDW(f);
        case 1: return FTRUNCW(f);
        case 2: return FCEILW(f);
        default: return FFLOORW(f);
    }
}
PSP_INLINE void FCMP(PspContext* c, float a, float b, u32 cond) {
    int un = (a != a) || (b != b);
    int r = ((cond & 1) && un) || ((cond & 2) && !un && a == b) || ((cond & 4) && !un && a < b);
    c->fcr31 = r ? (c->fcr31 | (1u << 23)) : (c->fcr31 & ~(1u << 23));
}
#define FCOND(c) (((c)->fcr31 >> 23) & 1)
#define VCC(c, n) (((c)->vfpuCtrl[VFPU_CTRL_CC] >> (n)) & 1)

#ifdef __cplusplus
}
#endif
#endif /* PSP_RECOMP_H */
