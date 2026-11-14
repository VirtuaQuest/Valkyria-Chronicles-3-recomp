// VFPU (vector floating point unit) interpreter for the PSP's Allegrex CPU.
//
// Recompiled code calls psp_vfpu(ctx, insn) (with ctx->pc set to the instruction's address) for
// every VFPU instruction except the VFPU branches bvf/bvt/bvfl/bvtl, which the code generator emits
// inline using VCC().  Each call decodes the word and executes it; nothing is allocated.
//
// ---------------------------------------------------------------------------------------------
// Register file layout
//
// The 128 VFPU registers live in ctx->v[] / ctx->vi[] as eight 4x4 matrices:
//
//     v[mtx * 16 + col * 4 + row]          mtx 0..7, col 0..3, row 0..3
//
// so a column vector (C<m><c><r>) is contiguous and lv.q/sv.q of a column are plain 16-byte copies.
// A 7-bit VFPU register number encodes:
//     bits 0-1  col   (for transposed / row forms: the row)
//     bits 2-4  matrix
//     bit  5    single: row bit 0; pair/quad/matrix: transpose flag
//     bit  6    single: row bit 1; pair/quad: start row (0 or 2); triple: start row (0 or 1)
// Single S<m><c><r> = v[m*16 + c*4 + r].  Pair/triple/quad elements i are at row (start+i)&3 of
// column c (or, transposed, column (start+i)&3 of row c).  Matrices are read into column-major
// scratch arrays m[j*4+i] = logical column j, logical row i.
//
// Prefixes: vpfxs/vpfxt/vpfxd set vfpuCtrl[SPREFIX/TPREFIX/DPREFIX]; every arithmetic instruction
// applies them and then resets them (S/T = 0xE4 identity swizzle, D = 0).  Loads/stores, moves to
// and from GPRs/control registers and vnop leave them pending.
// A context that was zero-filled (some runtime helpers do that) has S/T = 0, which would mean the
// legal "xxxx" swizzle.  To tell the two apart, an explicitly written all-zero S/T prefix is stored
// as kPfxZeroMarker (bit 31 set, which is outside the 20 prefix bits) and a stored 0 is treated as
// the identity prefix.  Reads through mfvc/vmfvc return the architectural 20-bit value.
// ---------------------------------------------------------------------------------------------
#include <atomic>
#include <cmath>
#include <cstring>

#include "../core/runtime.h"

using namespace vc2rt;

namespace {

union V4 {
    float f[4];
    u32 u[4];
    s32 i[4];
};

constexpr u32 kPfxIdentity = 0xE4;
constexpr u32 kPfxZeroMarker = 0x80000000u;
constexpr u32 kVfpuRevision = 0x7772CEABu;
constexpr double kHalfPi = 1.57079632679489661923;

// S/T prefix constants, indexed by swizzle + 4 * abs.
const float kPfxConst[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};

// vcst constants (VFPU_HUGE, VFPU_SQRT2, ...); unlisted entries are 0.
const float kCst[32] = {
    0.0f,
    3.40282346638528859812e+38f,   // FLT_MAX ("huge")
    1.41421356237309504880f,       // sqrt(2)
    0.70710678118654752440f,       // sqrt(1/2)
    1.12837916709551257390f,       // 2/sqrt(pi)
    0.63661977236758134308f,       // 2/pi
    0.31830988618379067154f,       // 1/pi
    0.78539816339744830962f,       // pi/4
    1.57079632679489661923f,       // pi/2
    3.14159265358979323846f,       // pi
    2.71828182845904523536f,       // e
    1.44269504088896340736f,       // log2(e)
    0.43429448190325182765f,       // log10(e)
    0.69314718055994530942f,       // ln(2)
    2.30258509299404568402f,       // ln(10)
    6.28318530717958647693f,       // 2*pi
    0.52359877559829887308f,       // pi/6
    0.30102999566398119521f,       // log10(2)
    3.32192809488736234787f,       // log2(10)
    0.86602540378443864676f,       // sqrt(3)/2
};

inline u32 asU(float f) { u32 u; memcpy(&u, &f, 4); return u; }
inline float asF(u32 u) { float f; memcpy(&f, &u, 4); return f; }
inline bool isNan(u32 u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }
inline bool isInf(u32 u) { return (u & 0x7FFFFFFFu) == 0x7F800000u; }

// ------------------------------------------------------------------ instruction fields
inline int fVD(u32 i) { return (int)(i & 0x7F); }
inline int fVS(u32 i) { return (int)((i >> 8) & 0x7F); }
inline int fVT(u32 i) { return (int)((i >> 16) & 0x7F); }
inline int vsize(u32 i) { return 1 + (int)((i >> 7) & 1) + (int)((i >> 14) & 2); }
inline u32 memAddr(const PspContext* c, u32 i) {
    return c->r[(i >> 21) & 31] + (u32)(s32)(s16)(i & 0xFFFC);
}

// ------------------------------------------------------------------ register addressing
inline int sIdx(int reg) { return ((reg >> 2) & 7) * 16 + (reg & 3) * 4 + ((reg >> 5) & 3); }

inline void vecIdx(int reg, int n, int* idx) {
    if (n == 1) {
        idx[0] = sIdx(reg);
        return;
    }
    const int base = ((reg >> 2) & 7) * 16, col = reg & 3;
    const int row = (n == 3) ? ((reg >> 6) & 1) : ((reg >> 5) & 2);
    if ((reg >> 5) & 1) {
        for (int i = 0; i < n; i++) idx[i] = base + ((row + i) & 3) * 4 + col;
    } else {
        for (int i = 0; i < n; i++) idx[i] = base + col * 4 + ((row + i) & 3);
    }
}

// m[j*4+i] <-> logical column j, row i of the n x n matrix named by reg.
inline void mtxIdx(int reg, int n, int* idx) {
    if (n == 1) {
        idx[0] = sIdx(reg);
        return;
    }
    const int base = ((reg >> 2) & 7) * 16, col = reg & 3;
    const int row = (n == 3) ? ((reg >> 6) & 1) : ((reg >> 5) & 2);
    const bool tr = (reg >> 5) & 1;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
            idx[j * 4 + i] = tr ? base + ((row + i) & 3) * 4 + ((col + j) & 3)
                                : base + ((col + j) & 3) * 4 + ((row + i) & 3);
}

inline void readV(const PspContext* c, int reg, int n, V4& out) {
    int idx[4];
    vecIdx(reg, n, idx);
    out.u[0] = out.u[1] = out.u[2] = out.u[3] = 0;
    for (int i = 0; i < n; i++) out.u[i] = c->vi[idx[i]];
}

// Writes n lanes, skipping lanes whose write-mask bit (D prefix bits 8-11) is set.
inline void writeV(PspContext* c, int reg, int n, const V4& d, u32 mask = 0) {
    int idx[4];
    vecIdx(reg, n, idx);
    for (int i = 0; i < n; i++)
        if (!((mask >> i) & 1)) c->vi[idx[i]] = d.u[i];
}

inline void readM(const PspContext* c, int reg, int n, float* m) {
    int idx[16];
    mtxIdx(reg, n, idx);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) m[j * 4 + i] = c->v[idx[j * 4 + i]];
}

inline void writeM(PspContext* c, int reg, int n, const float* m) {
    int idx[16];
    mtxIdx(reg, n, idx);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) c->v[idx[j * 4 + i]] = m[j * 4 + i];
}

// ------------------------------------------------------------------ prefixes
inline u32 pfxS(const PspContext* c) {
    u32 p = c->vfpuCtrl[VFPU_CTRL_SPREFIX];
    return p ? (p & 0xFFFFF) : kPfxIdentity;
}
inline u32 pfxT(const PspContext* c) {
    u32 p = c->vfpuCtrl[VFPU_CTRL_TPREFIX];
    return p ? (p & 0xFFFFF) : kPfxIdentity;
}
inline u32 pfxD(const PspContext* c) { return c->vfpuCtrl[VFPU_CTRL_DPREFIX] & 0xFFF; }

inline void eatPrefixes(PspContext* c) {
    c->vfpuCtrl[VFPU_CTRL_SPREFIX] = kPfxIdentity;
    c->vfpuCtrl[VFPU_CTRL_TPREFIX] = kPfxIdentity;
    c->vfpuCtrl[VFPU_CTRL_DPREFIX] = 0;
}

inline bool prefixesPending(const PspContext* c) {
    return pfxS(c) != kPfxIdentity || pfxT(c) != kPfxIdentity || pfxD(c) != 0;
}

// Applies an S/T prefix to lanes 0..n-1 of v (lanes past the source vector's size read as 0).
inline void applyST(V4& v, int n, u32 pfx) {
    if (pfx == kPfxIdentity) return;
    const V4 src = v;
    for (int i = 0; i < n; i++) {
        const u32 swz = (pfx >> (2 * i)) & 3;
        const u32 abs = (pfx >> (8 + i)) & 1;
        u32 x;
        if ((pfx >> (12 + i)) & 1) {
            x = asU(kPfxConst[swz + (abs << 2)]);
        } else {
            x = src.u[swz];
            if (abs) x &= 0x7FFFFFFFu;
        }
        if ((pfx >> (16 + i)) & 1) x ^= 0x80000000u;
        v.u[i] = x;
    }
}

// D prefix saturation: per lane 2 bits, 1 = clamp to [0,1], 3 = clamp to [-1,1]. NaN passes.
inline void applySat(V4& d, int n, u32 dp) {
    if (!(dp & 0xFF)) return;
    for (int i = 0; i < n; i++) {
        const u32 sat = (dp >> (2 * i)) & 3;
        float f = d.f[i];
        if (sat == 1) d.f[i] = f >= 1.0f ? 1.0f : (f <= 0.0f ? 0.0f : f);
        else if (sat == 3) d.f[i] = f >= 1.0f ? 1.0f : (f <= -1.0f ? -1.0f : f);
    }
}

// Common epilogue: D prefix (saturation optional), masked write, prefix reset.
inline void finish(PspContext* c, int vd, int n, V4& d, bool sat = true) {
    const u32 dp = pfxD(c);
    if (sat) applySat(d, n, dp);
    writeV(c, vd, n, d, (dp >> 8) & 0xF);
    eatPrefixes(c);
}

// Reads source operand vs (size n) with the S prefix applied.
inline void srcS(const PspContext* c, int reg, int n, V4& s) {
    readV(c, reg, n, s);
    applyST(s, n, pfxS(c));
}
inline void srcT(const PspContext* c, int reg, int n, V4& t) {
    readV(c, reg, n, t);
    applyST(t, n, pfxT(c));
}

// ------------------------------------------------------------------ diagnostics
void warnUnknown(const PspContext* c, u32 insn, const char* what) {
    static std::atomic<u32> seen[128];
    static std::atomic<int> used{0};
    int n = used.load(std::memory_order_relaxed);
    if (n > 128) n = 128;
    for (int i = 0; i < n; i++)
        if (seen[i].load(std::memory_order_relaxed) == insn) return;
    int k = used.fetch_add(1);
    if (k < 128) seen[k].store(insn, std::memory_order_relaxed);
    else if (k > 160) return;  // stop logging after a while
    LOG_WARN("VFPU", "%s VFPU instruction %08x at %08x not implemented", what, insn, c->pc);
}

void noteMatrixPrefix(const PspContext* c, u32 insn) {
    static std::atomic<int> count{0};
    if (count.fetch_add(1) < 8)
        LOG_WARN("VFPU", "prefix (s=%05x t=%05x d=%03x) on matrix instruction %08x at %08x ignored", pfxS(c),
                 pfxT(c), pfxD(c), insn, c->pc);
}

// ------------------------------------------------------------------ math helpers
inline float dotN(const float* a, const float* b, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += (double)a[i] * (double)b[i];
    return (float)s;
}

// sin(x * pi/2) (quadrant offset q: 0 = sin, 1 = cos), exact at whole quarter turns.
float quarterSin(float x, int q) {
    if (!std::isfinite(x)) return NAN;
    double r = std::fmod((double)x, 4.0);
    if (r < 0.0) r += 4.0;
    int k = (int)r;
    const double f = (r - k) * kHalfPi;
    k = (k + q) & 3;
    double v;
    switch (k) {
        case 0: v = std::sin(f); break;
        case 1: v = std::cos(f); break;
        case 2: v = -std::sin(f); break;
        default: v = -std::cos(f); break;
    }
    if (v == 0.0) v = 0.0;  // no negative zeros from the reduction
    if (q == 0 && x == 0.0f) return x;  // sin(+-0) = +-0
    return (float)v;
}

inline float vsin(float x) { return quarterSin(x, 0); }
inline float vcos(float x) { return quarterSin(x, 1); }
inline float vasin(float x) { return (float)(std::asin((double)x) / kHalfPi); }

// Ordering used by vmin/vmax/vsrt: denormals compare as zero, NaN/inf by sign and magnitude.
inline s32 orderKey(u32 u) {
    u32 mag = u & 0x7FFFFFFFu;
    if (mag < 0x00800000u) mag = 0;
    return (u & 0x80000000u) ? -(s32)mag : (s32)mag;
}
inline u32 vmin(u32 a, u32 b) { return orderKey(a) < orderKey(b) ? a : b; }
inline u32 vmax(u32 a, u32 b) { return orderKey(a) > orderKey(b) ? a : b; }

// Half floats as the VFPU converts them: truncating, no subnormals, inf/NaN mantissa kept.
inline u16 f2h(u32 f) {
    const u16 sign = (u16)((f >> 16) & 0x8000);
    const u32 e = (f >> 23) & 0xFF, m = f & 0x7FFFFF;
    if (e == 255) return sign | 0x7C00 | (u16)(m & 0x3FF);
    if (e < 113) return sign;
    if (e >= 143) return sign | 0x7C00;
    return sign | (u16)((e - 112) << 10) | (u16)(m >> 13);
}
inline u32 h2f(u32 h) {
    const u32 sign = (h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) return sign;
    if (e == 31) return sign | 0x7F800000u | m;
    return sign | ((e + 112) << 23) | (m << 13);
}

// Saturating float -> s32 after scaling by 2^scale, rounding mode 0 nearest-even, 1 truncate,
// 2 ceil, 3 floor.
inline u32 f2iScaled(u32 bits, int scale, int mode) {
    if (isNan(bits)) return 0x7FFFFFFFu;
    double v = std::ldexp((double)asF(bits), scale);
    switch (mode) {
        case 0: v = std::nearbyint(v); break;
        case 1: v = std::trunc(v); break;
        case 2: v = std::ceil(v); break;
        default: v = std::floor(v); break;
    }
    if (v >= 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return (u32)(s32)v;
}

// Random number generator state lives in the RCX control registers (8..15), 20 usable bits each.
u32 rngNext(PspContext* c) {
    u32* r = &c->vfpuCtrl[VFPU_CTRL_RCX0];
    u64 s = (u64)(r[0] & 0xFFFFF) | ((u64)(r[1] & 0xFFFFF) << 20) | ((u64)(r[2] & 0xFFFFF) << 40) |
            ((u64)(r[3] & 0xF) << 60);
    if (!s) s = 0x9E3779B97F4A7C15ull;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    r[0] = 0x3F800000u | (u32)(s & 0xFFFFF);
    r[1] = 0x3F800000u | (u32)((s >> 20) & 0xFFFFF);
    r[2] = 0x3F800000u | (u32)((s >> 40) & 0xFFFFF);
    r[3] = 0x3F800000u | (u32)((s >> 60) & 0xF);
    return (u32)((s * 0x2545F4914F6CDD1Dull) >> 32);
}
void rngSeed(PspContext* c, u32 seed) {
    u32* r = &c->vfpuCtrl[VFPU_CTRL_RCX0];
    for (int i = 0; i < 8; i++) {
        const u32 mix = (seed ^ (0x9E3779B9u * (u32)(i + 1))) * 0x85EBCA6Bu;
        r[i] = 0x3F800000u | ((mix ^ (mix >> 13)) & 0xFFFFF);
    }
}

// ------------------------------------------------------------------ control registers
u32 readCtrl(const PspContext* c, int n) {
    switch (n) {
        case VFPU_CTRL_SPREFIX: return pfxS(c);
        case VFPU_CTRL_TPREFIX: return pfxT(c);
        case VFPU_CTRL_DPREFIX: return pfxD(c);
        case VFPU_CTRL_CC: return c->vfpuCtrl[VFPU_CTRL_CC] & 0x3F;
        case VFPU_CTRL_REV: return kVfpuRevision;
        default: return (n >= 0 && n < 16) ? c->vfpuCtrl[n] : 0;
    }
}

void writeCtrl(PspContext* c, int n, u32 v) {
    switch (n) {
        case VFPU_CTRL_SPREFIX:
        case VFPU_CTRL_TPREFIX:
            v &= 0xFFFFF;
            c->vfpuCtrl[n] = v ? v : kPfxZeroMarker;
            break;
        case VFPU_CTRL_DPREFIX: c->vfpuCtrl[n] = v & 0xFFF; break;
        case VFPU_CTRL_CC: c->vfpuCtrl[n] = v & 0x3F; break;
        case VFPU_CTRL_INF4: c->vfpuCtrl[n] = v; break;
        case 5: case 6: case VFPU_CTRL_REV: break;  // reserved / read only
        default:
            if (n >= VFPU_CTRL_RCX0 && n < 16) c->vfpuCtrl[n] = (v & 0xFFFFF) | 0x3F800000u;
            break;
    }
}

// ------------------------------------------------------------------ loads / stores
void opLoadStore(PspContext* c, u32 insn) {
    const u32 op = insn >> 26;
    const u32 addr = memAddr(c, insn);
    if (op == 0x32 || op == 0x3A) {  // lv.s / sv.s
        const int vt = (int)(((insn >> 16) & 0x1F) | ((insn & 3) << 5));
        if (op == 0x32) c->vi[sIdx(vt)] = LW(addr);
        else SW(addr, c->vi[sIdx(vt)]);
        return;
    }
    const int vt = (int)(((insn >> 16) & 0x1F) | ((insn & 1) << 5));
    int idx[4];
    vecIdx(vt, 4, idx);
    switch (op) {
        case 0x36:  // lv.q
            for (int i = 0; i < 4; i++) c->vi[idx[i]] = LW(addr + 4 * i);
            break;
        case 0x3E:  // sv.q (bit 1 = write-back hint, same data effect)
            for (int i = 0; i < 4; i++) SW(addr + 4 * i, c->vi[idx[i]]);
            break;
        case 0x35: {  // lvl.q / lvr.q
            const u32 a = addr & ~3u;
            const int off = (int)((a >> 2) & 3);
            if (!(insn & 2)) {
                for (int i = 0; i <= off; i++) c->vi[idx[3 - i]] = LW(a - 4 * i);
            } else {
                for (int i = 0; i <= 3 - off; i++) c->vi[idx[i]] = LW(a + 4 * i);
            }
            break;
        }
        case 0x3D: {  // svl.q / svr.q
            const u32 a = addr & ~3u;
            const int off = (int)((a >> 2) & 3);
            if (!(insn & 2)) {
                for (int i = 0; i <= off; i++) SW(a - 4 * i, c->vi[idx[3 - i]]);
            } else {
                for (int i = 0; i <= 3 - off; i++) SW(a + 4 * i, c->vi[idx[i]]);
            }
            break;
        }
    }
}

// ------------------------------------------------------------------ COP2: mfv/mfvc/mtv/mtvc
void opCop2(PspContext* c, u32 insn) {
    const u32 rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, imm = insn & 0xFF;
    if (rs == 3) {  // mfv / mfvc
        if (rt == 0) return;  // "mfv zero, S<x>" is used as a pipeline interlock
        if (imm < 128) c->r[rt] = c->vi[sIdx((int)imm)];
        else if (imm < 128 + 16) c->r[rt] = readCtrl(c, (int)imm - 128);
    } else if (rs == 7) {  // mtv / mtvc
        if (imm < 128) c->vi[sIdx((int)imm)] = c->r[rt];
        else if (imm < 128 + 16) writeCtrl(c, (int)imm - 128, c->r[rt]);
    } else {
        warnUnknown(c, insn, "cop2");
    }
}

// ------------------------------------------------------------------ VFPU0 / VFPU1 / VFPU3
void opVfpu0(PspContext* c, u32 insn) {  // vadd vsub vsbn vdiv
    const int n = vsize(insn);
    V4 s, t, d;
    srcS(c, fVS(insn), n, s);
    srcT(c, fVT(insn), n, t);
    switch ((insn >> 23) & 7) {
        case 0: for (int i = 0; i < n; i++) d.f[i] = s.f[i] + t.f[i]; break;
        case 1: for (int i = 0; i < n; i++) d.f[i] = s.f[i] - t.f[i]; break;
        case 2: {  // vsbn: lane 0 gets exponent 127 + t (as int); other lanes copy s
            d = s;
            const u32 e = s.u[0] & 0x7F800000u;
            if (e != 0 && e != 0x7F800000u) d.u[0] = (s.u[0] & ~0x7F800000u) | ((u32)(u8)(127 + t.i[0]) << 23);
            break;
        }
        case 7: for (int i = 0; i < n; i++) d.f[i] = s.f[i] / t.f[i]; break;
        default: warnUnknown(c, insn, "vfpu0"); eatPrefixes(c); return;
    }
    finish(c, fVD(insn), n, d);
}

void opVfpu1(PspContext* c, u32 insn) {  // vmul vdot vscl vhdp vcrs vdet
    const int n = vsize(insn), vd = fVD(insn);
    V4 s, t, d;
    switch ((insn >> 23) & 7) {
        case 0:  // vmul
            srcS(c, fVS(insn), n, s);
            srcT(c, fVT(insn), n, t);
            for (int i = 0; i < n; i++) d.f[i] = s.f[i] * t.f[i];
            finish(c, vd, n, d);
            return;
        case 1:  // vdot
            srcS(c, fVS(insn), n, s);
            srcT(c, fVT(insn), n, t);
            d.f[0] = dotN(s.f, t.f, n);
            finish(c, vd, 1, d);
            return;
        case 2: {  // vscl: s * scalar t
            srcS(c, fVS(insn), n, s);
            const u32 x = c->vi[sIdx(fVT(insn))];
            t.u[0] = t.u[1] = t.u[2] = t.u[3] = x;
            applyST(t, n, pfxT(c));
            for (int i = 0; i < n; i++) d.f[i] = s.f[i] * t.f[i];
            finish(c, vd, n, d);
            return;
        }
        case 4:  // vhdp: homogeneous dot, last lane of s is 1
            srcS(c, fVS(insn), n, s);
            srcT(c, fVT(insn), n, t);
            s.f[n - 1] = 1.0f;
            d.f[0] = dotN(s.f, t.f, n);
            finish(c, vd, 1, d);
            return;
        case 5:  // vcrs.t: half cross product (s.yzx * t.zxy)
            srcS(c, fVS(insn), n, s);
            srcT(c, fVT(insn), n, t);
            d.f[0] = s.f[1] * t.f[2];
            d.f[1] = s.f[2] * t.f[0];
            d.f[2] = s.f[0] * t.f[1];
            d.f[3] = 0.0f;
            finish(c, vd, n, d);
            return;
        case 6:  // vdet.p
            srcS(c, fVS(insn), n, s);
            srcT(c, fVT(insn), n, t);
            d.f[0] = (float)((double)s.f[0] * t.f[1] - (double)s.f[1] * t.f[0]);
            finish(c, vd, 1, d);
            return;
        default:
            warnUnknown(c, insn, "vfpu1");
            eatPrefixes(c);
            return;
    }
}

bool vcmpLane(u32 cond, float s, float t) {
    const u32 su = asU(s);
    switch (cond) {
        case 0: return false;                    // FL
        case 1: return s == t;                   // EQ
        case 2: return s < t;                    // LT
        case 3: return s <= t;                   // LE
        case 4: return true;                     // TR
        case 5: return s != t;                   // NE
        case 6: return s >= t;                   // GE
        case 7: return s > t;                    // GT
        case 8: return s == 0.0f;                // EZ
        case 9: return isNan(su);                // EN
        case 10: return isInf(su);               // EI
        case 11: return isNan(su) || isInf(su);  // ES
        case 12: return s != 0.0f;               // NZ
        case 13: return !isNan(su);              // NN
        case 14: return !isInf(su);              // NI
        default: return !(isNan(su) || isInf(su));  // NS
    }
}

void opVfpu3(PspContext* c, u32 insn) {  // vcmp vmin vmax vscmp vsge vslt
    const int n = vsize(insn);
    V4 s, t, d;
    srcS(c, fVS(insn), n, s);
    srcT(c, fVT(insn), n, t);
    switch ((insn >> 23) & 7) {
        case 0: {  // vcmp: CC bit i per lane, bit 4 = any, bit 5 = all
            const u32 cond = insn & 0xF;
            u32 cc = 0, any = 0, all = 1;
            for (int i = 0; i < n; i++) {
                const u32 r = vcmpLane(cond, s.f[i], t.f[i]) ? 1u : 0u;
                cc |= r << i;
                any |= r;
                all &= r;
            }
            const u32 affected = ((1u << n) - 1) | 0x30;
            u32& CC = c->vfpuCtrl[VFPU_CTRL_CC];
            CC = (CC & ~affected) | ((cc | (any << 4) | (all << 5)) & affected);
            eatPrefixes(c);
            return;
        }
        case 2: for (int i = 0; i < n; i++) d.u[i] = vmin(s.u[i], t.u[i]); break;
        case 3: for (int i = 0; i < n; i++) d.u[i] = vmax(s.u[i], t.u[i]); break;
        case 5:  // vscmp: sign(s - t)
            for (int i = 0; i < n; i++) {
                if (isNan(s.u[i]) || isNan(t.u[i])) {
                    const s32 a = orderKey(s.u[i]), b = orderKey(t.u[i]);
                    d.f[i] = (float)((a > b) - (a < b));
                } else {
                    const float x = s.f[i] - t.f[i];
                    d.f[i] = (float)((x > 0.0f) - (x < 0.0f));
                }
            }
            break;
        case 6:
            for (int i = 0; i < n; i++)
                d.f[i] = (!isNan(s.u[i]) && !isNan(t.u[i]) && s.f[i] >= t.f[i]) ? 1.0f : 0.0f;
            break;
        case 7:
            for (int i = 0; i < n; i++)
                d.f[i] = (!isNan(s.u[i]) && !isNan(t.u[i]) && s.f[i] < t.f[i]) ? 1.0f : 0.0f;
            break;
        default: warnUnknown(c, insn, "vfpu3"); eatPrefixes(c); return;
    }
    finish(c, fVD(insn), n, d);
}

// ------------------------------------------------------------------ 0x34 sub 0: unary ops
void opVfpu4(PspContext* c, u32 insn) {
    const int n = vsize(insn), vd = fVD(insn);
    const u32 sub = (insn >> 16) & 31;
    V4 s, d;
    switch (sub) {
        case 3: {  // vidt: lane (vd & 3) (pairs: vd & 1) is 1, others 0
            const int one = (n == 2) ? (vd & 1) : (vd & 3);
            for (int i = 0; i < 4; i++) d.f[i] = (i == one) ? 1.0f : 0.0f;
            finish(c, vd, n, d);
            return;
        }
        case 6:  // vzero
        case 7:  // vone
            for (int i = 0; i < 4; i++) d.f[i] = sub == 6 ? 0.0f : 1.0f;
            finish(c, vd, n, d);
            return;
        default: break;
    }
    readV(c, fVS(insn), n, s);
    u32 sp = pfxS(c);
    if (sub == 1) sp |= 0xF00;    // vabs = forced abs
    if (sub == 2) sp |= 0xF0000;  // vneg = forced negate
    applyST(s, n, sp);
    for (int i = 0; i < n; i++) {
        const float x = s.f[i];
        float r;
        switch (sub) {
            case 0: case 1: case 2: d.u[i] = s.u[i]; continue;  // vmov / vabs / vneg
            case 4: r = x <= 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); break;   // vsat0
            case 5: r = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x); break;  // vsat1
            case 16: r = 1.0f / x; break;                                   // vrcp
            case 17: r = (float)(1.0 / std::sqrt((double)x)); break;        // vrsq
            case 18: r = vsin(x); break;                                    // vsin
            case 19: r = vcos(x); break;                                    // vcos
            case 20: r = std::exp2(x); break;                               // vexp2
            case 21: r = std::log2(x); break;                               // vlog2
            case 22: r = std::sqrt(x); break;                               // vsqrt
            case 23: r = vasin(x); break;                                   // vasin
            case 24: r = -1.0f / x; break;                                  // vnrcp
            case 26: r = -vsin(x); break;                                   // vnsin
            case 28: r = std::exp2(-x); break;                              // vrexp2
            default:
                warnUnknown(c, insn, "vfpu4");
                eatPrefixes(c);
                return;
        }
        d.f[i] = r;
    }
    finish(c, vd, n, d);
}

// ------------------------------------------------------------------ 0x34 sub 1: conversions, rng
void opVfpu7(PspContext* c, u32 insn) {
    const int n = vsize(insn), vd = fVD(insn);
    const u32 sub = (insn >> 16) & 31;
    V4 s, d{};
    switch (sub) {
        case 0:  // vrnds
            srcS(c, fVS(insn), 1, s);
            rngSeed(c, s.u[0]);
            eatPrefixes(c);
            return;
        case 1: case 2: case 3:  // vrndi / vrndf1 / vrndf2
            for (int i = n - 1; i >= 0; i--) {
                const u32 r = rngNext(c);
                d.u[i] = sub == 1 ? r : (sub == 2 ? 0x3F800000u : 0x40000000u) | (r & 0x007FFFFFu);
            }
            finish(c, vd, n, d, sub != 1);
            return;
        case 18: {  // vf2h: pairs of floats -> packed halves
            srcS(c, fVS(insn), n, s);
            const int on = n <= 2 ? 1 : 2;
            for (int i = 0; i < on; i++) d.u[i] = (u32)f2h(s.u[2 * i]) | ((u32)f2h(s.u[2 * i + 1]) << 16);
            finish(c, vd, on, d, false);
            return;
        }
        case 19: {  // vh2f: packed halves -> floats
            srcS(c, fVS(insn), n, s);
            const int in = n == 1 ? 1 : 2;
            for (int i = 0; i < in; i++) {
                d.u[2 * i] = h2f(s.u[i] & 0xFFFF);
                d.u[2 * i + 1] = h2f(s.u[i] >> 16);
            }
            finish(c, vd, in * 2, d);
            return;
        }
        case 22:  // vsbz: lane 0 = mantissa scaled to [1,2); rest copied
            srcS(c, fVS(insn), n, s);
            d = s;
            if (!isNan(s.u[0]) && (s.u[0] & 0x7F800000u) != 0) d.u[0] = 0x3F800000u | (s.u[0] & 0x007FFFFFu);
            finish(c, vd, n, d);
            return;
        case 23: {  // vlgb: lane 0 = unbiased exponent; rest copied
            srcS(c, fVS(insn), n, s);
            d = s;
            const u32 e = (s.u[0] >> 23) & 0xFF;
            if (e == 0) d.f[0] = -INFINITY;
            else if (e != 0xFF) d.f[0] = (float)((int)e - 127);
            finish(c, vd, n, d);
            return;
        }
        case 24: {  // vuc2i: 4 unsigned bytes -> 4 ints (byte replicated, >> 1)
            srcS(c, fVS(insn), 1, s);
            for (int i = 0; i < 4; i++) d.u[i] = (((s.u[0] >> (8 * i)) & 0xFF) * 0x01010101u) >> 1;
            finish(c, vd, 4, d, false);
            return;
        }
        case 25:  // vc2i: 4 signed bytes -> top byte of 4 ints
            srcS(c, fVS(insn), 1, s);
            for (int i = 0; i < 4; i++) d.u[i] = ((s.u[0] >> (8 * i)) & 0xFF) << 24;
            finish(c, vd, 4, d, false);
            return;
        case 26:    // vus2i: unsigned halves -> ints << 15
        case 27: {  // vs2i: signed halves -> ints << 16
            const int in = n == 1 ? 1 : 2;
            srcS(c, fVS(insn), in, s);
            const int sh = sub == 26 ? 15 : 16;
            for (int i = 0; i < in; i++) {
                d.u[2 * i] = (s.u[i] & 0xFFFF) << sh;
                d.u[2 * i + 1] = (s.u[i] >> 16) << sh;
            }
            finish(c, vd, in * 2, d, false);
            return;
        }
        case 28:  // vi2uc: 4 ints -> 4 unsigned bytes (clamp negatives, >> 23)
        case 29:  // vi2c: 4 ints -> 4 signed bytes (>> 24)
            srcS(c, fVS(insn), 4, s);
            d.u[0] = 0;
            for (int i = 0; i < 4; i++) {
                const u32 b = sub == 28 ? (s.i[i] < 0 ? 0u : ((u32)s.i[i] >> 23)) : ((u32)s.i[i] >> 24);
                d.u[0] |= (b & 0xFF) << (8 * i);
            }
            finish(c, vd, 1, d, false);
            return;
        case 30:    // vi2us: ints -> unsigned halves (clamp negatives, >> 15)
        case 31: {  // vi2s: ints -> signed halves (>> 16)
            srcS(c, fVS(insn), n, s);
            const int on = (n + 1) / 2;
            for (int i = 0; i < on; i++) {
                u32 lo, hi;
                if (sub == 30) {
                    lo = s.i[2 * i] < 0 ? 0u : ((u32)s.i[2 * i] >> 15);
                    hi = s.i[2 * i + 1] < 0 ? 0u : ((u32)s.i[2 * i + 1] >> 15);
                } else {
                    lo = s.u[2 * i] >> 16;
                    hi = s.u[2 * i + 1] >> 16;
                }
                d.u[i] = (lo & 0xFFFF) | (hi << 16);
            }
            finish(c, vd, on, d, false);
            return;
        }
        default:
            warnUnknown(c, insn, "vfpu7");
            eatPrefixes(c);
            return;
    }
}

// ------------------------------------------------------------------ 0x34 sub 2
void opVfpu9(PspContext* c, u32 insn) {
    const int n = vsize(insn), vd = fVD(insn);
    const u32 sub = (insn >> 16) & 31;
    V4 s, d{};
    switch (sub) {
        case 16: {  // vmfvc
            const int r = (int)((insn >> 8) & 0x7F);
            c->vi[sIdx(vd)] = r < 16 ? readCtrl(c, r) : 0;
            return;
        }
        case 17:  // vmtvc
            writeCtrl(c, (int)(insn & 0x7F), c->vi[sIdx(fVS(insn))]);
            return;
        case 25: case 26: case 27: {  // vt4444 / vt5551 / vt5650: ABGR8888 -> 16-bit colours
            srcS(c, fVS(insn), 4, s);
            u16 col[4];
            for (int i = 0; i < 4; i++) {
                const u32 x = s.u[i];
                const u32 r = x & 0xFF, g = (x >> 8) & 0xFF, b = (x >> 16) & 0xFF, a = x >> 24;
                if (sub == 25) col[i] = (u16)(((a >> 4) << 12) | ((b >> 4) << 8) | ((g >> 4) << 4) | (r >> 4));
                else if (sub == 26) col[i] = (u16)(((a >> 7) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
                else col[i] = (u16)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
            }
            d.u[0] = col[0] | ((u32)col[1] << 16);
            d.u[1] = col[2] | ((u32)col[3] << 16);
            finish(c, vd, n == 1 ? 1 : 2, d, false);
            return;
        }
        default: break;
    }
    srcS(c, fVS(insn), n, s);
    const float* x = s.f;
    switch (sub) {
        case 0:  // vsrt1
            d.u[0] = vmin(s.u[0], s.u[1]); d.u[1] = vmax(s.u[0], s.u[1]);
            d.u[2] = vmin(s.u[2], s.u[3]); d.u[3] = vmax(s.u[2], s.u[3]);
            break;
        case 1:  // vsrt2
            d.u[0] = vmin(s.u[0], s.u[3]); d.u[1] = vmin(s.u[1], s.u[2]);
            d.u[2] = vmax(s.u[1], s.u[2]); d.u[3] = vmax(s.u[0], s.u[3]);
            break;
        case 8:  // vsrt3
            d.u[0] = vmax(s.u[0], s.u[1]); d.u[1] = vmin(s.u[0], s.u[1]);
            d.u[2] = vmax(s.u[2], s.u[3]); d.u[3] = vmin(s.u[2], s.u[3]);
            break;
        case 9:  // vsrt4
            d.u[0] = vmax(s.u[0], s.u[3]); d.u[1] = vmax(s.u[1], s.u[2]);
            d.u[2] = vmin(s.u[1], s.u[2]); d.u[3] = vmin(s.u[0], s.u[3]);
            break;
        case 2:  // vbfy1
            d.f[0] = x[0] + x[1]; d.f[1] = x[0] - x[1];
            d.f[2] = x[2] + x[3]; d.f[3] = x[2] - x[3];
            break;
        case 3:  // vbfy2
            d.f[0] = x[0] + x[2]; d.f[1] = x[1] + x[3];
            d.f[2] = x[0] - x[2]; d.f[3] = x[1] - x[3];
            break;
        case 4:  // vocp: 1 - s
            for (int i = 0; i < n; i++) d.f[i] = 1.0f - x[i];
            break;
        case 5: {  // vsocp: (1-x, x, 1-y, y) saturated; output twice the size
            const int on = n == 1 ? 2 : 4;
            for (int i = 0; i < on / 2; i++) {
                float a = 1.0f - x[i], b = x[i];
                d.f[2 * i] = a >= 1.0f ? 1.0f : (a <= 0.0f ? 0.0f : a);
                d.f[2 * i + 1] = b >= 1.0f ? 1.0f : (b <= 0.0f ? 0.0f : b);
            }
            finish(c, vd, on, d);
            return;
        }
        case 6:    // vfad: horizontal add
        case 7: {  // vavg: horizontal average
            double sum = 0.0;
            for (int i = 0; i < n; i++) sum += x[i];
            d.f[0] = (float)(sub == 6 ? sum : sum / n);
            finish(c, vd, 1, d);
            return;
        }
        case 10:  // vsgn
            for (int i = 0; i < n; i++) {
                const u32 u = s.u[i];
                d.f[i] = (u & 0x7F800000u) == 0 ? 0.0f : ((u >> 31) ? -1.0f : 1.0f);
            }
            break;
        default:
            warnUnknown(c, insn, "vfpu9");
            eatPrefixes(c);
            return;
    }
    finish(c, vd, n, d);
}

// ------------------------------------------------------------------ 0x34 dispatcher
void opVfpu4Jump(PspContext* c, u32 insn) {
    const u32 sub = (insn >> 21) & 31;
    const int n = vsize(insn), vd = fVD(insn);
    V4 s, d;
    switch (sub) {
        case 0: opVfpu4(c, insn); return;
        case 1: opVfpu7(c, insn); return;
        case 2: opVfpu9(c, insn); return;
        case 3: {  // vcst
            const float k = kCst[(insn >> 16) & 31];
            d.f[0] = d.f[1] = d.f[2] = d.f[3] = k;
            finish(c, vd, n, d);
            return;
        }
        case 16: case 17: case 18: case 19: {  // vf2in / vf2iz / vf2iu / vf2id
            srcS(c, fVS(insn), n, s);
            const int scale = (int)((insn >> 16) & 31);
            for (int i = 0; i < n; i++) d.u[i] = f2iScaled(s.u[i], scale, (int)(sub - 16));
            finish(c, vd, n, d, false);
            return;
        }
        case 20: {  // vi2f
            srcS(c, fVS(insn), n, s);
            const int scale = (int)((insn >> 16) & 31);
            for (int i = 0; i < n; i++) d.f[i] = (float)std::ldexp((double)s.i[i], -scale);
            finish(c, vd, n, d);
            return;
        }
        case 21: {  // vcmovt / vcmovf
            const u32 tf = (insn >> 19) & 1, cc = (insn >> 16) & 7;
            srcS(c, fVS(insn), n, s);
            srcT(c, vd, n, d);  // the destination is read as the T operand
            const u32 CC = c->vfpuCtrl[VFPU_CTRL_CC];
            if (cc == 6) {
                for (int i = 0; i < n; i++)
                    if (((CC >> i) & 1) != tf) d.u[i] = s.u[i];
            } else if (cc < 6) {  // cc 7 tests a bit that is always clear
                if (((CC >> cc) & 1) != tf)
                    for (int i = 0; i < n; i++) d.u[i] = s.u[i];
            } else if (tf) {
                for (int i = 0; i < n; i++) d.u[i] = s.u[i];
            }
            finish(c, vd, n, d);
            return;
        }
        case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: {  // vwbn
            srcS(c, fVS(insn), n, s);
            d = s;
            const u32 e = (insn >> 16) & 0xFF;
            const u32 sign = s.u[0] & 0x80000000u, pe = (s.u[0] >> 23) & 0xFF;
            u32 m = (s.u[0] & 0x007FFFFFu) | 0x00800000u;
            if (pe != 0xFF && pe != 0) {
                if (e > pe) m >>= (e - pe) & 0xF;
                else m <<= (pe - e) & 0xF;
                d.u[0] = sign | (m & 0x007FFFFFu) | (e << 23);
            } else {
                d.u[0] = s.u[0] | (e << 23);
            }
            finish(c, vd, n, d);
            return;
        }
        default:
            warnUnknown(c, insn, "vfpu4jump");
            eatPrefixes(c);
            return;
    }
}

// ------------------------------------------------------------------ 0x37: prefixes, viim/vfim
void opVfpu5(PspContext* c, u32 insn) {
    switch ((insn >> 23) & 7) {
        case 0: case 1: writeCtrl(c, VFPU_CTRL_SPREFIX, insn & 0xFFFFF); return;
        case 2: case 3: writeCtrl(c, VFPU_CTRL_TPREFIX, insn & 0xFFFFF); return;
        case 4: case 5: writeCtrl(c, VFPU_CTRL_DPREFIX, insn & 0xFFF); return;
        case 6:    // viim.s: signed 16-bit integer immediate
        case 7: {  // vfim.s: half-float immediate
            V4 d;
            if ((insn >> 23) & 1) d.u[0] = h2f(insn & 0xFFFF);
            else d.f[0] = (float)(s16)(insn & 0xFFFF);
            finish(c, fVT(insn), 1, d);
            return;
        }
    }
}

// ------------------------------------------------------------------ 0x3C: matrix ops
void opVfpu6(PspContext* c, u32 insn) {
    const u32 sub = (insn >> 21) & 31;
    const int n = vsize(insn), vd = fVD(insn), vs = fVS(insn), vt = fVT(insn);
    float s[16], t[16], m[16];
    V4 d;

    if (sub == 29) {  // vrot: one row of a rotation matrix
        const u32 imm = (insn >> 16) & 31;
        V4 a;
        srcS(c, vs, 1, a);
        float sine = vsin(a.f[0]);
        const float cosine = vcos(a.f[0]);
        if (imm & 0x10) sine = -sine;
        const u32 sl = (imm >> 2) & 3, cl = imm & 3;
        for (int i = 0; i < 4; i++) d.f[i] = (sl == cl) ? sine : 0.0f;
        d.f[sl] = sine;
        d.f[cl] = cosine;
        finish(c, vd, n, d);
        return;
    }
    if (sub == 20 || sub == 21 || sub == 22 || sub == 23) {  // vcrsp.t / vqmul.q
        V4 a, b;
        srcS(c, vs, n, a);
        srcT(c, vt, n, b);
        const float* x = a.f;
        const float* y = b.f;
        if (n == 3) {
            d.f[0] = (float)((double)x[1] * y[2] - (double)x[2] * y[1]);
            d.f[1] = (float)((double)x[2] * y[0] - (double)x[0] * y[2]);
            d.f[2] = (float)((double)x[0] * y[1] - (double)x[1] * y[0]);
        } else if (n == 4) {
            d.f[0] = (float)((double)x[0] * y[3] + (double)x[1] * y[2] - (double)x[2] * y[1] + (double)x[3] * y[0]);
            d.f[1] = (float)(-(double)x[0] * y[2] + (double)x[1] * y[3] + (double)x[2] * y[0] + (double)x[3] * y[1]);
            d.f[2] = (float)((double)x[0] * y[1] - (double)x[1] * y[0] + (double)x[2] * y[3] + (double)x[3] * y[2]);
            d.f[3] = (float)(-(double)x[0] * y[0] - (double)x[1] * y[1] - (double)x[2] * y[2] + (double)x[3] * y[3]);
        } else {
            warnUnknown(c, insn, "vcrsp/vqmul size");
            eatPrefixes(c);
            return;
        }
        finish(c, vd, n, d);
        return;
    }

    if (prefixesPending(c)) noteMatrixPrefix(c, insn);
    if (sub <= 3) {  // vmmul: vd = transpose(vs) * vt
        readM(c, vs, n, s);
        readM(c, vt, n, t);
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) m[a * 4 + b] = dotN(&s[b * 4], &t[a * 4], n);
        writeM(c, vd, n, m);
    } else if (sub <= 15) {  // vtfm2/3/4, vhtfm2/3/4: vd = transpose(vs) * (vt [,1])
        const int side = (int)((insn >> 23) & 3) + 1;
        readM(c, vs, side, s);
        V4 x;
        readV(c, vt, n < side ? n : side, x);
        for (int k = n; k < side; k++) x.f[k] = (k == side - 1) ? 1.0f : 0.0f;
        for (int i = 0; i < side; i++) d.f[i] = dotN(&s[i * 4], x.f, side);
        writeV(c, vd, side, d);
    } else if (sub <= 19) {  // vmscl
        readM(c, vs, n, s);
        const float k = c->v[sIdx(vt)];
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++) m[j * 4 + i] = s[j * 4 + i] * k;
        writeM(c, vd, n, m);
    } else if (sub == 28) {
        switch ((insn >> 16) & 15) {
            case 0:  // vmmov
                readM(c, vs, n, m);
                writeM(c, vd, n, m);
                break;
            case 3:  // vmidt
                for (int j = 0; j < n; j++)
                    for (int i = 0; i < n; i++) m[j * 4 + i] = (i == j) ? 1.0f : 0.0f;
                writeM(c, vd, n, m);
                break;
            case 6:  // vmzero
            case 7:  // vmone
                for (int j = 0; j < 16; j++) m[j] = ((insn >> 16) & 1) ? 1.0f : 0.0f;
                writeM(c, vd, n, m);
                break;
            default: warnUnknown(c, insn, "matrix"); break;
        }
    } else {
        warnUnknown(c, insn, "vfpu6");
    }
    eatPrefixes(c);
}

}  // namespace

extern "C" PSP_API void psp_vfpu(PspContext* c, u32 insn) {
    switch (insn >> 26) {
        case 0x12: opCop2(c, insn); return;
        case 0x18: opVfpu0(c, insn); return;
        case 0x19: opVfpu1(c, insn); return;
        case 0x1B: opVfpu3(c, insn); return;
        case 0x32: case 0x35: case 0x36: case 0x3A: case 0x3D: case 0x3E: opLoadStore(c, insn); return;
        case 0x34: opVfpu4Jump(c, insn); return;
        case 0x37: opVfpu5(c, insn); return;
        case 0x3C: opVfpu6(c, insn); return;
        case 0x3F:  // vnop / vsync / vflush (0xFFFFxxxx) keep pending prefixes; other forms reset them
            if ((insn & 0xFFFF0000u) != 0xFFFF0000u) eatPrefixes(c);
            return;
        default: warnUnknown(c, insn, "unknown"); return;
    }
}
