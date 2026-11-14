// Self-test for the VFPU interpreter (src/runtime/cpu/vfpu.cpp).
//
// Builds vfpu.cpp into a standalone executable with a tiny stub environment (guest memory and the
// logger), runs hand-assembled VFPU instructions and compares against hand-computed results.
//
// usage: vfpu_test [game_vfpu_words.txt]
//   The optional file lists VFPU instruction words (hex, one per line, e.g. extracted from the
//   generated code); each is executed once in a sandbox and the test fails if any of them is
//   reported as unimplemented.
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "runtime/core/runtime.h"

// ------------------------------------------------------------------ stub environment
u8* psp_mem = nullptr;

static int g_vfpuWarnings = 0;
static bool g_quietLog = false;

namespace vc2rt {
void logMsg(LogLevel lvl, const char* tag, const char* fmt, ...) {
    if (lvl == LogLevel::Warn && strcmp(tag, "VFPU") == 0) g_vfpuWarnings++;
    if (g_quietLog) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("    [log %s] %s\n", tag, buf);
}
}  // namespace vc2rt

// ------------------------------------------------------------------ mini assembler
// Register names: single S(m,c,r); column quad C(m,c); row quad R(m,r); matrix M(m) / E(m) (transposed)
static int S(int m, int c, int r) { return m * 4 + c + r * 32; }
static int C(int m, int c, int r = 0) { return m * 4 + c + r * 32; }  // pair: r 0/2, triple: r 0/1 (<<1)
static int R(int m, int r, int c = 0) { return m * 4 + r + 32 + c * 32; }
static int M(int m) { return m * 4; }
static int E(int m) { return m * 4 + 32; }
static int Ct(int m, int c, int r) { return m * 4 + c + (r << 6); }  // triple column starting at row r (0/1)

static u32 sz(int n) { return (u32)(((n - 1) & 1) << 7) | (u32)(((n - 1) >> 1) << 15); }
static u32 op3(u32 base, int n, int vd, int vs, int vt) { return base | sz(n) | ((u32)vt << 16) | ((u32)vs << 8) | (u32)vd; }
static u32 op2(u32 base, int n, int vd, int vs) { return base | sz(n) | ((u32)vs << 8) | (u32)vd; }

enum : u32 {
    VADD = 0x60000000, VSUB = 0x60800000, VDIV = 0x63800000,
    VMUL = 0x64000000, VDOT = 0x64800000, VSCL = 0x65000000, VHDP = 0x66000000, VCRS = 0x66800000, VDET = 0x67000000,
    VCMP = 0x6C000000, VMIN = 0x6D000000, VMAX = 0x6D800000, VSCMP = 0x6E800000, VSGE = 0x6F000000, VSLT = 0x6F800000,
    VMOV = 0xD0000000, VABS = 0xD0010000, VNEG = 0xD0020000, VIDT = 0xD0030000, VSAT0 = 0xD0040000, VSAT1 = 0xD0050000,
    VZERO = 0xD0060000, VONE = 0xD0070000, VRCP = 0xD0100000, VRSQ = 0xD0110000, VSIN = 0xD0120000, VCOS = 0xD0130000,
    VEXP2 = 0xD0140000, VLOG2 = 0xD0150000, VSQRT = 0xD0160000, VASIN = 0xD0170000, VNRCP = 0xD0180000, VNSIN = 0xD01A0000,
    VREXP2 = 0xD01C0000,
    VRNDS = 0xD0200000, VRNDI = 0xD0210000, VRNDF1 = 0xD0220000, VF2H = 0xD0320000, VH2F = 0xD0330000,
    VSBZ = 0xD0360000, VLGB = 0xD0370000, VUC2I = 0xD0380000, VC2I = 0xD0390000, VUS2I = 0xD03A0000, VS2I = 0xD03B0000,
    VI2UC = 0xD03C0000, VI2C = 0xD03D0000, VI2US = 0xD03E0000, VI2S = 0xD03F0000,
    VSRT1 = 0xD0400000, VSRT2 = 0xD0410000, VBFY1 = 0xD0420000, VBFY2 = 0xD0430000, VOCP = 0xD0440000, VSOCP = 0xD0450000,
    VFAD = 0xD0460000, VAVG = 0xD0470000, VSRT3 = 0xD0480000, VSRT4 = 0xD0490000, VSGN = 0xD04A0000,
    VMFVC = 0xD0500000, VMTVC = 0xD0510000, VT4444 = 0xD0590000, VT5551 = 0xD05A0000, VT5650 = 0xD05B0000,
    VCST = 0xD0600000, VF2IN = 0xD2000000, VF2IZ = 0xD2200000, VF2IU = 0xD2400000, VF2ID = 0xD2600000,
    VI2F = 0xD2800000, VCMOV = 0xD2A00000, VWBN = 0xD3000000,
    VPFXS = 0xDC000000, VPFXT = 0xDD000000, VPFXD = 0xDE000000, VIIM = 0xDF000000, VFIM = 0xDF800000,
    VMMUL = 0xF0000000, VTFM2 = 0xF0800000, VTFM3 = 0xF1000000, VTFM4 = 0xF1800000, VMSCL = 0xF2000000,
    VCRSP = 0xF2800000, VMMOV = 0xF3800000, VMIDT = 0xF3830000, VMZERO = 0xF3860000, VMONE = 0xF3870000,
    VROT = 0xF3A00000,
    VNOP = 0xFFFF0000, VSYNC = 0xFFFF0320, VFLUSH = 0xFFFF040D,
};

static u32 lvs(int vt, int rs, int off) { return 0xC8000000 | (rs << 21) | ((vt & 31) << 16) | (off & 0xFFFC) | (vt >> 5); }
static u32 svs(int vt, int rs, int off) { return 0xE8000000 | (rs << 21) | ((vt & 31) << 16) | (off & 0xFFFC) | (vt >> 5); }
static u32 lvq(int vt, int rs, int off) { return 0xD8000000 | (rs << 21) | ((vt & 31) << 16) | (off & 0xFFFC) | ((vt >> 5) & 1); }
static u32 svq(int vt, int rs, int off) { return 0xF8000000 | (rs << 21) | ((vt & 31) << 16) | (off & 0xFFFC) | ((vt >> 5) & 1); }
static u32 lvlq(int vt, int rs, int off) { return 0xD4000000 | (rs << 21) | ((vt & 31) << 16) | (off & 0xFFFC) | ((vt >> 5) & 1); }
static u32 lvrq(int vt, int rs, int off) { return lvlq(vt, rs, off) | 2; }
static u32 svlq(int vt, int rs, int off) { return 0xF4000000 | (rs << 21) | ((vt & 31) << 16) | (off & 0xFFFC) | ((vt >> 5) & 1); }
static u32 svrq(int vt, int rs, int off) { return svlq(vt, rs, off) | 2; }
static u32 mfv(int rt, int reg) { return 0x48600000 | (rt << 16) | reg; }
static u32 mtv(int rt, int reg) { return 0x48E00000 | (rt << 16) | reg; }
static u32 mfvc(int rt, int ctrl) { return 0x48600000 | (rt << 16) | (128 + ctrl); }
static u32 mtvc(int rt, int ctrl) { return 0x48E00000 | (rt << 16) | (128 + ctrl); }

// prefix builders: lanes given as strings like "x", "-y", "|z|", "0", "1", "2", "1/2", "3", "1/3", "1/4", "1/6"
static u32 pfxLane(const char* s, int i) {
    u32 v = 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    static const char* consts[8] = {"0", "1", "2", "1/2", "3", "1/3", "1/4", "1/6"};
    for (int k = 0; k < 8; k++)
        if (!strcmp(s, consts[k])) v = ((u32)(k & 3) << (2 * i)) | ((u32)(k >> 2) << (8 + i)) | (1u << (12 + i));
    if (!(v >> 12)) {
        bool abs = false;
        if (*s == '|') { abs = true; s++; }
        const u32 swz = (u32)(strchr("xyzw", *s) - "xyzw");
        v = (swz << (2 * i)) | ((u32)abs << (8 + i));
    }
    if (neg) v |= 1u << (16 + i);
    return v;
}
static u32 vpfxST(u32 base, const char* a, const char* b, const char* c, const char* d) {
    return base | pfxLane(a, 0) | pfxLane(b, 1) | pfxLane(c, 2) | pfxLane(d, 3);
}
// D prefix: sat per lane 0 none, 1 [0:1], 3 [-1:1]; mask bit per lane
static u32 vpfxd(int s0, int s1, int s2, int s3, int mask) {
    return VPFXD | (u32)s0 | ((u32)s1 << 2) | ((u32)s2 << 4) | ((u32)s3 << 6) | ((u32)mask << 8);
}

// ------------------------------------------------------------------ harness
static PspContext ctx;
static int g_fail = 0, g_pass = 0;
static const u32 kBase = 0x08800000u;

static void reset() {
    memset(&ctx, 0, sizeof ctx);
    ctx.vfpuCtrl[VFPU_CTRL_SPREFIX] = 0xE4;
    ctx.vfpuCtrl[VFPU_CTRL_TPREFIX] = 0xE4;
    ctx.vfpuCtrl[VFPU_CTRL_CC] = 0x3F;
    ctx.r[4] = kBase;
    ctx.r[5] = kBase + 0x100;
}
static void run(u32 insn) {
    ctx.pc += 4;
    psp_vfpu(&ctx, insn);
}
static float* vreg(int reg) {  // single register by name
    return &ctx.v[((reg >> 2) & 7) * 16 + (reg & 3) * 4 + ((reg >> 5) & 3)];
}
static void setQ(int m, int c, float a, float b, float d, float e) {  // column quad C<m><c>0
    float* p = &ctx.v[m * 16 + c * 4];
    p[0] = a; p[1] = b; p[2] = d; p[3] = e;
}
static void setQi(int m, int c, u32 a, u32 b, u32 d, u32 e) {
    u32* p = &ctx.vi[m * 16 + c * 4];
    p[0] = a; p[1] = b; p[2] = d; p[3] = e;
}
static float fsrc(int m, int c, int r) { return ctx.v[m * 16 + c * 4 + r]; }

static void check(bool ok, const char* name, const std::string& detail = "") {
    if (ok) g_pass++;
    else {
        g_fail++;
        printf("FAIL: %s %s\n", name, detail.c_str());
    }
}
static bool approxEq(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps * (1.0f + std::fabs(b)); }
static void checkQ(const char* name, int m, int c, int n, const float* expect, float eps = 1e-6f) {
    bool ok = true;
    std::string got;
    char buf[64];
    for (int i = 0; i < n; i++) {
        float v = fsrc(m, c, i);
        if (!(approxEq(v, expect[i], eps) || (std::isnan(v) && std::isnan(expect[i])))) ok = false;
        snprintf(buf, sizeof buf, "%g(exp %g) ", v, expect[i]);
        got += buf;
    }
    check(ok, name, got);
}
static void checkQi(const char* name, int m, int c, int n, const u32* expect) {
    bool ok = true;
    std::string got;
    char buf[64];
    for (int i = 0; i < n; i++) {
        u32 v = ctx.vi[m * 16 + c * 4 + i];
        if (v != expect[i]) ok = false;
        snprintf(buf, sizeof buf, "%08x(exp %08x) ", v, expect[i]);
        got += buf;
    }
    check(ok, name, got);
}
static void checkU(const char* name, u32 got, u32 exp) {
    char buf[96];
    snprintf(buf, sizeof buf, "got %08x expected %08x", got, exp);
    check(got == exp, name, buf);
}
static void checkF(const char* name, float got, float exp, float eps = 1e-6f) {
    char buf[96];
    snprintf(buf, sizeof buf, "got %.9g expected %.9g", got, exp);
    check(approxEq(got, exp, eps), name, buf);
}
static void memF(u32 addr, float f) { memcpy(PSP_ADDR(addr), &f, 4); }
static float rdF(u32 addr) { float f; memcpy(&f, PSP_ADDR(addr), 4); return f; }

// ------------------------------------------------------------------ tests
static void testLoadStore() {
    reset();
    for (int i = 0; i < 16; i++) memF(kBase + 4 * i, (float)(i + 1));
    run(lvq(C(0, 0), 4, 0));
    { const float e[4] = {1, 2, 3, 4}; checkQ("lv.q C000", 0, 0, 4, e); }
    run(lvq(R(1, 0), 4, 16));  // row vector R100: v[16+0], v[16+4], v[16+8], v[16+12]
    check(fsrc(1, 0, 0) == 5 && fsrc(1, 1, 0) == 6 && fsrc(1, 2, 0) == 7 && fsrc(1, 3, 0) == 8, "lv.q R100 (transposed)");
    run(svq(R(1, 0), 5, 0));
    check(rdF(kBase + 0x100) == 5 && rdF(kBase + 0x10C) == 8, "sv.q R100 round trip");
    run(svq(C(0, 0), 5, 0x20));
    check(memcmp(PSP_ADDR(kBase), PSP_ADDR(kBase + 0x120), 16) == 0, "sv.q C000 round trip");
    run(lvs(S(2, 1, 3), 4, 8));  // S213 = v[2*16 + 1*4 + 3]
    checkF("lv.s S213", ctx.v[32 + 4 + 3], 3.0f);
    run(svs(S(2, 1, 3), 5, 0x40));
    checkF("sv.s S213", rdF(kBase + 0x140), 3.0f);
    // address masking: kernel-mode mirror 0x48800000 maps to the same memory
    ctx.r[6] = 0x48800000u;
    run(lvq(C(3, 0), 6, 0));
    { const float e[4] = {1, 2, 3, 4}; checkQ("lv.q through PSP_ADDR mirror", 3, 0, 4, e); }
    // lvl.q / lvr.q: unaligned quad pair at kBase+8 (words 3,4,5,6)
    setQ(4, 0, -1, -1, -1, -1);
    run(lvlq(C(4, 0), 4, 8 + 12));  // kBase+20: word 1 of its line -> lanes 2..3 from words 5,6
    run(lvrq(C(4, 0), 4, 8));       // kBase+8: word 2 of its line -> lanes 0..1 from words 3,4
    { const float e[4] = {3, 4, 5, 6}; checkQ("lvl.q/lvr.q unaligned pair", 4, 0, 4, e); }
    run(svlq(C(4, 0), 5, 0x60 + 8 + 12));
    run(svrq(C(4, 0), 5, 0x60 + 8));
    check(rdF(kBase + 0x168) == 3 && rdF(kBase + 0x16C) == 4 && rdF(kBase + 0x170) == 5 && rdF(kBase + 0x174) == 6,
          "svl.q/svr.q unaligned pair");
    check(rdF(kBase + 0x164) == 0 && rdF(kBase + 0x178) == 0, "svl.q/svr.q leave neighbours");
}

static void testMoves() {
    reset();
    ctx.r[8] = 0x3F800000u;
    run(mtv(8, S(5, 2, 1)));
    checkU("mtv S521", ctx.vi[5 * 16 + 2 * 4 + 1], 0x3F800000u);
    run(mfv(9, S(5, 2, 1)));
    checkU("mfv S521", ctx.r[9], 0x3F800000u);
    run(mfv(0, 0xFF));  // interlock form, must not touch r0
    checkU("mfv zero, 255 (interlock)", ctx.r[0], 0);
    ctx.r[10] = 0x12;
    run(mtvc(10, VFPU_CTRL_CC));
    run(mfvc(11, VFPU_CTRL_CC));
    checkU("mtvc/mfvc CC", ctx.r[11], 0x12);
    check(VCC(&ctx, 1) == 1 && VCC(&ctx, 4) == 1 && VCC(&ctx, 0) == 0, "VCC() sees mtvc CC bits");
    run(mfvc(12, VFPU_CTRL_REV));
    checkU("mfvc REV", ctx.r[12], 0x7772CEABu);
    // vmfvc / vmtvc
    run(0xD0500000 | (u32)((128 + VFPU_CTRL_CC) << 8) | (u32)S(0, 0, 0));
    checkU("vmfvc S000, CC", ctx.vi[0], 0x12);
    ctx.vi[0] = 0x2D;
    run(VMTVC | (u32)(S(0, 0, 0) << 8) | (u32)(128 + VFPU_CTRL_CC));
    checkU("vmtvc CC, S000", ctx.vfpuCtrl[VFPU_CTRL_CC] & 0x3F, 0x2D);
}

static void testArith() {
    reset();
    setQ(0, 0, 1, 2, 3, 4);
    setQ(0, 1, 5, 6, 7, 8);
    run(op3(VADD, 4, C(1, 0), C(0, 0), C(0, 1)));
    { const float e[4] = {6, 8, 10, 12}; checkQ("vadd.q", 1, 0, 4, e); }
    run(op3(VSUB, 4, C(1, 1), C(0, 0), C(0, 1)));
    { const float e[4] = {-4, -4, -4, -4}; checkQ("vsub.q", 1, 1, 4, e); }
    run(op3(VMUL, 3, C(1, 2), C(0, 0), C(0, 1)));
    { const float e[4] = {5, 12, 21, 0}; checkQ("vmul.t (lane 3 untouched)", 1, 2, 4, e); }
    run(op3(VDIV, 2, C(1, 3), C(0, 1), C(0, 0)));
    { const float e[2] = {5, 3}; checkQ("vdiv.p", 1, 3, 2, e); }
    run(op3(VDOT, 4, S(2, 0, 0), C(0, 0), C(0, 1)));
    checkF("vdot.q", *vreg(S(2, 0, 0)), 70.0f);
    run(op3(VDOT, 3, S(2, 0, 1), C(0, 0), C(0, 1)));
    checkF("vdot.t", *vreg(S(2, 0, 1)), 38.0f);
    run(op3(VHDP, 4, S(2, 0, 2), C(0, 0), C(0, 1)));
    checkF("vhdp.q", *vreg(S(2, 0, 2)), 1 * 5 + 2 * 6 + 3 * 7 + 8.0f);
    *vreg(S(0, 2, 2)) = 0.5f;
    run(op3(VSCL, 4, C(2, 1), C(0, 0), S(0, 2, 2)));
    { const float e[4] = {0.5f, 1, 1.5f, 2}; checkQ("vscl.q", 2, 1, 4, e); }
    run(op3(VDET, 2, S(2, 0, 3), C(0, 0), C(0, 1)));
    checkF("vdet.p", *vreg(S(2, 0, 3)), 1 * 6 - 2 * 5.0f);
    run(op3(VCRS, 3, C(2, 2), C(0, 0), C(0, 1)));
    { const float e[3] = {2 * 7, 3 * 5, 1 * 6}; checkQ("vcrs.t", 2, 2, 3, e); }
    run(op2(VFAD, 4, S(3, 0, 0), C(0, 0)));
    checkF("vfad.q", *vreg(S(3, 0, 0)), 10.0f);
    run(op2(VAVG, 4, S(3, 0, 1), C(0, 0)));
    checkF("vavg.q", *vreg(S(3, 0, 1)), 2.5f);
    setQ(0, 2, 3, -1, 2, 9);
    run(op3(VMIN, 4, C(3, 1), C(0, 0), C(0, 2)));
    { const float e[4] = {1, -1, 2, 4}; checkQ("vmin.q", 3, 1, 4, e); }
    run(op3(VMAX, 4, C(3, 2), C(0, 0), C(0, 2)));
    { const float e[4] = {3, 2, 3, 9}; checkQ("vmax.q", 3, 2, 4, e); }
    run(op3(VSGE, 4, C(3, 3), C(0, 0), C(0, 2)));
    { const float e[4] = {0, 1, 1, 0}; checkQ("vsge.q", 3, 3, 4, e); }
    run(op3(VSLT, 4, C(3, 3), C(0, 0), C(0, 2)));
    { const float e[4] = {1, 0, 0, 1}; checkQ("vslt.q", 3, 3, 4, e); }
    run(op3(VSCMP, 4, C(3, 3), C(0, 0), C(0, 2)));
    { const float e[4] = {-1, 1, 1, -1}; checkQ("vscmp.q", 3, 3, 4, e); }
    setQ(4, 0, -3, 0, 2, -0.0f);
    run(op2(VSGN, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {-1, 0, 1, 0}; checkQ("vsgn.q", 4, 1, 4, e); }
    run(op2(VABS, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {3, 0, 2, 0}; checkQ("vabs.q", 4, 1, 4, e); }
    run(op2(VNEG, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {3, -0.0f, -2, 0}; checkQ("vneg.q", 4, 1, 4, e); }
    setQ(4, 0, -0.5f, 0.25f, 1.5f, -2.0f);
    run(op2(VSAT0, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {0, 0.25f, 1, 0}; checkQ("vsat0.q", 4, 1, 4, e); }
    run(op2(VSAT1, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {-0.5f, 0.25f, 1, -1}; checkQ("vsat1.q", 4, 1, 4, e); }
    run(op2(VOCP, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {1.5f, 0.75f, -0.5f, 3}; checkQ("vocp.q", 4, 1, 4, e); }
    setQ(4, 0, 0.25f, 0.75f, 0, 0);
    run(op2(VSOCP, 2, C(4, 1), C(4, 0, 0)));
    { const float e[4] = {0.75f, 0.25f, 0.25f, 0.75f}; checkQ("vsocp.p", 4, 1, 4, e); }
    setQ(4, 0, 1, 2, 3, 4);
    run(op2(VBFY1, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {3, -1, 7, -1}; checkQ("vbfy1.q", 4, 1, 4, e); }
    run(op2(VBFY2, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {4, 6, -2, -2}; checkQ("vbfy2.q", 4, 1, 4, e); }
    setQ(4, 0, 4, 1, 2, 3);
    run(op2(VSRT1, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {1, 4, 2, 3}; checkQ("vsrt1.q", 4, 1, 4, e); }
    run(op2(VSRT2, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {3, 1, 2, 4}; checkQ("vsrt2.q", 4, 1, 4, e); }
    run(op2(VSRT3, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {4, 1, 3, 2}; checkQ("vsrt3.q", 4, 1, 4, e); }
    run(op2(VSRT4, 4, C(4, 1), C(4, 0)));
    { const float e[4] = {4, 2, 1, 3}; checkQ("vsrt4.q", 4, 1, 4, e); }
}

static void testTranscendental() {
    reset();
    auto un = [](u32 base, float x) {
        *vreg(S(0, 0, 0)) = x;
        run(op2(base, 1, S(0, 1, 0), S(0, 0, 0)));
        return *vreg(S(0, 1, 0));
    };
    check(un(VSIN, 1.0f) == 1.0f, "vsin(1) == 1 exactly");
    check(un(VSIN, 2.0f) == 0.0f, "vsin(2) == 0 exactly");
    check(un(VSIN, -1.0f) == -1.0f, "vsin(-1) == -1 exactly");
    check(un(VCOS, 1.0f) == 0.0f, "vcos(1) == 0 exactly");
    check(un(VCOS, 0.0f) == 1.0f, "vcos(0) == 1 exactly");
    check(un(VCOS, 2.0f) == -1.0f, "vcos(2) == -1 exactly");
    checkF("vsin(0.5)", un(VSIN, 0.5f), 0.70710678f);
    checkF("vsin(1/3)", un(VSIN, 1.0f / 3.0f), 0.5f, 1e-6f);
    checkF("vsin(5.5) (= sin 135 deg)", un(VSIN, 5.5f), 0.70710678f);
    checkF("vsin(-0.5)", un(VSIN, -0.5f), -0.70710678f);
    checkF("vcos(-2.5)", un(VCOS, -2.5f), -0.70710678f);
    checkF("vnsin(0.5)", un(VNSIN, 0.5f), -0.70710678f);
    checkF("vasin(1)", un(VASIN, 1.0f), 1.0f);
    checkF("vasin(0.5)", un(VASIN, 0.5f), 1.0f / 3.0f);
    checkF("vrcp(4)", un(VRCP, 4.0f), 0.25f);
    checkF("vnrcp(4)", un(VNRCP, 4.0f), -0.25f);
    checkF("vrsq(4)", un(VRSQ, 4.0f), 0.5f);
    checkF("vsqrt(9)", un(VSQRT, 9.0f), 3.0f);
    checkF("vexp2(3)", un(VEXP2, 3.0f), 8.0f);
    checkF("vrexp2(3)", un(VREXP2, 3.0f), 0.125f);
    checkF("vlog2(8)", un(VLOG2, 8.0f), 3.0f);
    check(std::isinf(un(VRCP, 0.0f)), "vrcp(0) = inf");
    // vector form applies per lane
    setQ(1, 0, 0, 1, 2, 3);
    run(op2(VSIN, 4, C(1, 1), C(1, 0)));
    { const float e[4] = {0, 1, 0, -1}; checkQ("vsin.q", 1, 1, 4, e); }
    // vrot: C200 = [cos, sin, 0, 0] (vrot.p imm: cos lane 0, sin lane 1) and [s, s, c, s] form
    *vreg(S(0, 0, 0)) = 1.0f / 3.0f;  // 30 degrees
    run(VROT | (u32)((1 << 2 | 0) << 16) | sz(2) | ((u32)S(0, 0, 0) << 8) | (u32)C(2, 0));
    { const float e[2] = {0.8660254f, 0.5f}; checkQ("vrot.p [c,s]", 2, 0, 2, e); }
    run(VROT | (u32)((0x10 | 2 << 2 | 1) << 16) | sz(4) | ((u32)S(0, 0, 0) << 8) | (u32)C(2, 1));
    { const float e[4] = {0, 0.8660254f, -0.5f, 0}; checkQ("vrot.q [0,c,-s,0]", 2, 1, 4, e); }
    run(VROT | (u32)((3 << 2 | 3) << 16) | sz(4) | ((u32)S(0, 0, 0) << 8) | (u32)C(2, 2));
    { const float e[4] = {0.5f, 0.5f, 0.5f, 0.8660254f}; checkQ("vrot.q [s,s,s,c]", 2, 2, 4, e); }
}

static void setMatrices() {
    // A (row-major, logical) loaded so that M000 column j holds A's column j: v[c*4+r] = A[r][c]
    const float A[4][4] = {{1, 2, 0, 0}, {0, 1, 3, 0}, {0, 0, 1, 4}, {5, 0, 0, 1}};
    const float B[4][4] = {{2, 0, 0, 1}, {0, 3, 0, 0}, {1, 0, 4, 0}, {0, 0, 0, 5}};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            ctx.v[0 * 16 + c * 4 + r] = A[r][c];
            ctx.v[1 * 16 + c * 4 + r] = B[r][c];
        }
}
static float mAt(int m, int r, int c) { return ctx.v[m * 16 + c * 4 + r]; }  // logical (row, col) of M<m>

static void testMatrix() {
    reset();
    setMatrices();
    // vmmul.q M200, E000, M100 computes A * B (vmmul multiplies the transpose of its first operand)
    run(op3(VMMUL, 4, M(2), E(0), M(1)));
    const float AB[4][4] = {{2, 6, 0, 1}, {3, 3, 12, 0}, {1, 0, 4, 20}, {10, 0, 0, 10}};
    bool ok = true;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (mAt(2, r, c) != AB[r][c]) ok = false;
    check(ok, "vmmul.q M200, E000, M100 = A*B");
    // vmmul.q M300, M000, M100 = A^T * B ; check one row: (A^T B)[0] = sum_k A[k][0] * B[k] = 1*B0 + 5*B3
    run(op3(VMMUL, 4, M(3), M(0), M(1)));
    check(mAt(3, 0, 0) == 2 && mAt(3, 0, 1) == 0 && mAt(3, 0, 2) == 0 && mAt(3, 0, 3) == 26, "vmmul.q M300, M000, M100 = A^T*B");
    // vmmul.t on the 3x3 upper-left blocks
    run(op3(VMMUL, 3, M(4), E(0), M(1)));
    check(mAt(4, 0, 0) == 2 && mAt(4, 0, 1) == 6 && mAt(4, 1, 2) == 12 && mAt(4, 2, 2) == 4 && mAt(4, 3, 3) == 0,
          "vmmul.t 3x3 block");
    // vtfm4.q C500, E000, C510 = A * x
    setQ(5, 1, 1, 2, 3, 1);
    run(op3(VTFM4, 4, C(5, 0), E(0), C(5, 1)));
    { const float e[4] = {5, 11, 7, 6}; checkQ("vtfm4.q (A*x)", 5, 0, 4, e); }
    // vhtfm4.t: same with implicit w = 1
    setQ(5, 2, 1, 2, 3, 99);
    run(op3(VTFM4, 3, C(5, 3), E(0), C(5, 2)));
    { const float e[4] = {5, 11, 7, 6}; checkQ("vhtfm4.t (A*[x,1])", 5, 3, 4, e); }
    // vtfm3.t with upper-left 3x3 of A
    run(op3(VTFM3, 3, C(6, 0), E(0), C(5, 1)));
    { const float e[3] = {5, 11, 3}; checkQ("vtfm3.t", 6, 0, 3, e); }
    // vtfm2.p
    run(op3(VTFM2, 2, C(6, 1), E(0), C(5, 1)));
    { const float e[2] = {5, 2}; checkQ("vtfm2.p", 6, 1, 2, e); }
    // vhtfm3.p: [1 2 0; 0 1 3; 0 0 1] * [1, 2, 1]
    run(op3(VTFM3, 2, C(6, 2), E(0), C(5, 1)));
    { const float e[3] = {5, 5, 1}; checkQ("vhtfm3.p", 6, 2, 3, e); }
    // vmscl.q M700, M100, S000 ( * 0.5)
    *vreg(S(0, 0, 0)) = 0.5f;
    run(op3(VMSCL, 4, M(7), M(1), S(0, 0, 0)));
    check(mAt(7, 0, 0) == 1 && mAt(7, 3, 3) == 2.5f && mAt(7, 0, 3) == 0.5f, "vmscl.q");
    // vmmov.q M600, E100 (transpose copy)
    run(op2(VMMOV, 4, M(6), E(1)));
    check(mAt(6, 0, 3) == 0 && mAt(6, 3, 0) == 1 && mAt(6, 2, 0) == 0 && mAt(6, 0, 2) == 1, "vmmov.q M600, E100 (transpose)");
    // vmidt / vmzero / vmone
    run(VMIDT | sz(4) | (u32)M(6));
    ok = true;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (mAt(6, r, c) != (r == c ? 1.0f : 0.0f)) ok = false;
    check(ok, "vmidt.q");
    run(VMZERO | sz(2) | (u32)(M(6) | 2 | 64));  // 2x2 at col 2, row 2 (M622)
    check(mAt(6, 2, 2) == 0 && mAt(6, 3, 3) == 0 && mAt(6, 1, 1) == 1 && mAt(6, 2, 3) == 0, "vmzero.p M622");
    run(VMONE | sz(3) | (u32)M(7));
    check(mAt(7, 0, 0) == 1 && mAt(7, 2, 2) == 1 && mAt(7, 1, 2) == 1 && mAt(7, 3, 3) == 2.5f, "vmone.t (4th row/col untouched)");
    // vcrsp.t, vqmul.q
    setQ(0, 0, 1, 0, 0, 0);
    setQ(0, 1, 0, 1, 0, 0);
    run(op3(VCRSP, 3, C(0, 2), C(0, 0), C(0, 1)));
    { const float e[3] = {0, 0, 1}; checkQ("vcrsp.t x cross y = z", 0, 2, 3, e); }
    setQ(0, 0, 1, 2, 3, 0);
    setQ(0, 1, 4, 5, 6, 0);
    run(op3(VCRSP, 3, C(0, 2), C(0, 0), C(0, 1)));
    { const float e[3] = {-3, 6, -3}; checkQ("vcrsp.t general", 0, 2, 3, e); }
    setQ(0, 0, 1, 0, 0, 0);  // i
    setQ(0, 1, 0, 1, 0, 0);  // j
    run(op3(VCRSP, 4, C(0, 2), C(0, 0), C(0, 1)));
    { const float e[4] = {0, 0, 1, 0}; checkQ("vqmul.q i*j = k", 0, 2, 4, e); }
    setQ(0, 0, 1, 2, 3, 4);  // (x,y,z,w)
    setQ(0, 1, 5, 6, 7, 8);
    run(op3(VCRSP, 4, C(0, 2), C(0, 0), C(0, 1)));
    // Hamilton product (w1 v2 + w2 v1 + v1 x v2, w1 w2 - v1.v2)
    { const float e[4] = {4 * 5 + 8 * 1 + (2 * 7 - 3 * 6), 4 * 6 + 8 * 2 + (3 * 5 - 1 * 7), 4 * 7 + 8 * 3 + (1 * 6 - 2 * 5),
                          32 - (5 + 12 + 21)};
      checkQ("vqmul.q general", 0, 2, 4, e); }
    // matrix register addressing of transposed 3x3 at offset: E011 style (row 1 start)
    reset();
    for (int i = 0; i < 16; i++) ctx.v[16 + i] = (float)i;  // M1: v[16 + c*4 + r] = c*4+r
    run(op2(VMMOV, 3, M(2), Ct(1, 1, 1)));  // M111 (3x3 at col 1 row 1) -> M200
    check(mAt(2, 0, 0) == 5 && mAt(2, 1, 0) == 6 && mAt(2, 0, 1) == 9 && mAt(2, 2, 2) == 15, "vmmov.t M200, M111 (offset)");
}

static void testPrefixes() {
    reset();
    setQ(0, 0, 1, -2, 3, -4);
    run(vpfxST(VPFXS, "w", "z", "y", "x"));
    run(op2(VMOV, 4, C(1, 0), C(0, 0)));
    { const float e[4] = {-4, 3, -2, 1}; checkQ("vpfxs [w,z,y,x] + vmov.q", 1, 0, 4, e); }
    run(op2(VMOV, 4, C(1, 1), C(0, 0)));
    { const float e[4] = {1, -2, 3, -4}; checkQ("prefix consumed after one instruction", 1, 1, 4, e); }
    run(vpfxST(VPFXS, "w", "|y|", "2", "-x"));
    run(op2(VMOV, 4, C(1, 0), C(0, 0)));
    { const float e[4] = {-4, 2, 2, -1}; checkQ("vpfxs [w,|y|,2,-x]", 1, 0, 4, e); }
    run(vpfxST(VPFXS, "1/2", "1/3", "-1/4", "1/6"));
    run(op2(VMOV, 4, C(1, 0), C(0, 0)));
    { const float e[4] = {0.5f, 1.0f / 3, -0.25f, 1.0f / 6}; checkQ("vpfxs constants", 1, 0, 4, e); }
    run(vpfxST(VPFXS, "0", "1", "3", "-|x|"));
    run(op2(VMOV, 4, C(1, 0), C(0, 0)));
    { const float e[4] = {0, 1, 3, -1}; checkQ("vpfxs [0,1,3,-|x|]", 1, 0, 4, e); }
    // vpfxt on the second operand
    setQ(0, 1, 10, 20, 30, 40);
    run(vpfxST(VPFXT, "x", "x", "x", "1"));
    run(op3(VADD, 4, C(1, 0), C(0, 0), C(0, 1)));
    { const float e[4] = {11, 8, 13, -3}; checkQ("vpfxt [x,x,x,1] + vadd.q", 1, 0, 4, e); }
    // vpfxd saturation and write mask
    setQ(1, 0, 7, 7, 7, 7);
    setQ(0, 1, 0.5f, 0.5f, 0.5f, 0.5f);
    run(vpfxd(1, 3, 0, 0, 0x4));  // lane0 [0:1], lane1 [-1:1], lane2 masked
    run(op3(VADD, 4, C(1, 0), C(0, 0), C(0, 1)));
    { const float e[4] = {1, -1, 7, -3.5f}; checkQ("vpfxd [0:1,-1:1,M,] + vadd.q", 1, 0, 4, e); }
    // the D mask also applies to vzero
    setQ(1, 1, 9, 9, 9, 9);
    run(vpfxd(0, 0, 0, 0, 0x9));
    run(op2(VZERO, 4, C(1, 1), 0));
    { const float e[4] = {9, 0, 0, 9}; checkQ("vpfxd mask + vzero.q", 1, 1, 4, e); }
    // vmul with broadcast prefixes as the game uses them (vpfxs [y,y,y,y] style)
    setQ(2, 0, 2, 3, 4, 5);
    run(vpfxST(VPFXS, "y", "y", "y", "y"));
    run(op3(VMUL, 4, C(2, 1), C(2, 0), C(2, 0)));
    { const float e[4] = {6, 9, 12, 15}; checkQ("vpfxs [y,y,y,y] + vmul.q", 2, 1, 4, e); }
    // prefixes survive loads, moves and vnop, and are reset by the next arithmetic op
    run(vpfxST(VPFXS, "-x", "-y", "-z", "-w"));
    run(lvq(C(3, 0), 4, 0));
    run(mfv(8, S(0, 0, 0)));
    run(VNOP);
    run(op2(VMOV, 4, C(2, 2), C(2, 0)));
    { const float e[4] = {-2, -3, -4, -5}; checkQ("prefix pending across lv.q/mfv/vnop", 2, 2, 4, e); }
    // mfvc reads the prefix registers
    run(vpfxST(VPFXT, "z", "z", "z", "z"));
    run(mfvc(9, VFPU_CTRL_TPREFIX));
    checkU("mfvc TPREFIX", ctx.r[9], 0xAA);
    run(op2(VMOV, 1, S(7, 0, 0), S(7, 0, 0)));
    run(mfvc(9, VFPU_CTRL_TPREFIX));
    checkU("TPREFIX reset to 0xE4", ctx.r[9], 0xE4);
    // A zero-filled context (as runtime helpers create) behaves as identity prefixes...
    memset(ctx.vfpuCtrl, 0, sizeof ctx.vfpuCtrl);
    run(op2(VMOV, 4, C(2, 3), C(2, 0)));
    { const float e[4] = {2, 3, 4, 5}; checkQ("zeroed vfpuCtrl = identity prefixes", 2, 3, 4, e); }
    // ...while an explicit all-zero prefix is the [x,x,x,x] swizzle
    run(VPFXS | 0);
    run(mfvc(9, VFPU_CTRL_SPREFIX));
    checkU("mfvc SPREFIX after vpfxs [x,x,x,x]", ctx.r[9], 0);
    run(op2(VMOV, 4, C(2, 3), C(2, 0)));
    { const float e[4] = {2, 2, 2, 2}; checkQ("explicit vpfxs [x,x,x,x]", 2, 3, 4, e); }
    // vocp with D saturation (game: vpfxd [0:1,...] + vocp)
    setQ(3, 1, -0.5f, 0.5f, 2, 0);
    run(vpfxd(1, 1, 1, 1, 0));
    run(op2(VOCP, 4, C(3, 2), C(3, 1)));
    { const float e[4] = {1, 0.5f, 0, 1}; checkQ("vpfxd [0:1] + vocp.q", 3, 2, 4, e); }
    // vabs of a single with negate prefix
    *vreg(S(4, 0, 0)) = -0.25f;
    run(vpfxST(VPFXS, "|x|", "y", "z", "w"));
    run(op2(VASIN, 1, S(4, 0, 1), S(4, 0, 0)));
    checkF("vpfxs [|x|] + vasin.s", *vreg(S(4, 0, 1)), (float)(std::asin(0.25) / 1.5707963267948966));
}

static void testCompare() {
    reset();
    setQ(0, 0, 1, 5, 3, 7);
    setQ(0, 1, 2, 4, 3, 8);
    run(op3(VCMP | 2, 4, 0, C(0, 0), C(0, 1)));  // LT
    checkU("vcmp.q LT cc", ctx.vfpuCtrl[VFPU_CTRL_CC], 0x19);
    check(VCC(&ctx, 0) == 1 && VCC(&ctx, 1) == 0 && VCC(&ctx, 3) == 1 && VCC(&ctx, 4) == 1 && VCC(&ctx, 5) == 0, "VCC() after vcmp");
    run(op3(VCMP | 1, 4, 0, C(0, 0), C(0, 0)));  // EQ
    checkU("vcmp.q EQ (all)", ctx.vfpuCtrl[VFPU_CTRL_CC], 0x3F);
    run(op3(VCMP | 6, 2, 0, C(0, 0), C(0, 1)));  // GE on pair: lanes 0,1 = F,T ; bits 2,3 untouched
    checkU("vcmp.p GE keeps bits 2-3", ctx.vfpuCtrl[VFPU_CTRL_CC], 0x1E);
    run(op3(VCMP | 0, 4, 0, C(0, 0), C(0, 1)));  // FL
    checkU("vcmp.q FL", ctx.vfpuCtrl[VFPU_CTRL_CC], 0x00);
    run(op3(VCMP | 4, 1, 0, S(0, 0, 0), S(0, 0, 0)));  // TR single
    checkU("vcmp.s TR", ctx.vfpuCtrl[VFPU_CTRL_CC], 0x31);
    setQ(0, 2, 0, -0.0f, INFINITY, NAN);
    run(op2(VCMP | 8, 4, 0, C(0, 2)));  // EZ
    checkU("vcmp.q EZ", ctx.vfpuCtrl[VFPU_CTRL_CC] & 0xF, 0x3);
    run(op2(VCMP | 9, 4, 0, C(0, 2)));  // EN
    checkU("vcmp.q EN", ctx.vfpuCtrl[VFPU_CTRL_CC] & 0xF, 0x8);
    run(op2(VCMP | 10, 4, 0, C(0, 2)));  // EI
    checkU("vcmp.q EI", ctx.vfpuCtrl[VFPU_CTRL_CC] & 0xF, 0x4);
    run(op2(VCMP | 11, 4, 0, C(0, 2)));  // ES
    checkU("vcmp.q ES", ctx.vfpuCtrl[VFPU_CTRL_CC] & 0xF, 0xC);
    run(op2(VCMP | 12, 4, 0, C(0, 2)));  // NZ
    checkU("vcmp.q NZ", ctx.vfpuCtrl[VFPU_CTRL_CC] & 0xF, 0xC);
    // with prefixes: compare s against constant 3
    run(vpfxST(VPFXT, "3", "3", "3", "3"));
    run(op3(VCMP | 7, 4, 0, C(0, 0), C(0, 1)));  // GT 3 : 1,5,3,7 -> F,T,F,T
    checkU("vpfxt [3,3,3,3] + vcmp.q GT", ctx.vfpuCtrl[VFPU_CTRL_CC], 0x1A);
    // vcmov
    setQ(1, 0, 1, 1, 1, 1);
    setQ(1, 1, 2, 2, 2, 2);
    run(VCMOV | (6u << 16) | sz(4) | ((u32)C(1, 0) << 8) | (u32)C(1, 1));  // vcmovt per-lane (cc 1010)
    { const float e[4] = {2, 1, 2, 1}; checkQ("vcmovt.q per-lane", 1, 1, 4, e); }
    setQ(1, 1, 2, 2, 2, 2);
    run(VCMOV | (1u << 19) | (6u << 16) | sz(4) | ((u32)C(1, 0) << 8) | (u32)C(1, 1));  // vcmovf per-lane
    { const float e[4] = {1, 2, 1, 2}; checkQ("vcmovf.q per-lane", 1, 1, 4, e); }
    setQ(1, 1, 2, 2, 2, 2);
    run(VCMOV | (1u << 16) | sz(4) | ((u32)C(1, 0) << 8) | (u32)C(1, 1));  // vcmovt cc[1] (set)
    { const float e[4] = {1, 1, 1, 1}; checkQ("vcmovt.q cc[1]", 1, 1, 4, e); }
    setQ(1, 1, 2, 2, 2, 2);
    run(VCMOV | (2u << 16) | sz(4) | ((u32)C(1, 0) << 8) | (u32)C(1, 1));  // vcmovt cc[2] (clear)
    { const float e[4] = {2, 2, 2, 2}; checkQ("vcmovt.q cc[2] (no move)", 1, 1, 4, e); }
}

static void testConversions() {
    reset();
    setQ(0, 0, 1.7f, -1.7f, 2.5f, -2.5f);
    run(op2(VF2IZ, 4, C(1, 0), C(0, 0)));
    { const u32 e[4] = {1, (u32)-1, 2, (u32)-2}; checkQi("vf2iz.q", 1, 0, 4, e); }
    run(op2(VF2IN, 4, C(1, 0), C(0, 0)));
    { const u32 e[4] = {2, (u32)-2, 2, (u32)-2}; checkQi("vf2in.q (ties to even)", 1, 0, 4, e); }
    run(op2(VF2IU, 4, C(1, 0), C(0, 0)));
    { const u32 e[4] = {2, (u32)-1, 3, (u32)-2}; checkQi("vf2iu.q", 1, 0, 4, e); }
    run(op2(VF2ID, 4, C(1, 0), C(0, 0)));
    { const u32 e[4] = {1, (u32)-2, 2, (u32)-3}; checkQi("vf2id.q", 1, 0, 4, e); }
    setQ(0, 1, 1.5f, 3e9f, -3e9f, NAN);
    run(op2(VF2IZ | (16u << 16), 4, C(1, 1), C(0, 1)));
    { const u32 e[4] = {98304, 0x7FFFFFFF, 0x80000000, 0x7FFFFFFF}; checkQi("vf2iz.q scale 16 + saturation", 1, 1, 4, e); }
    setQi(0, 2, 256, (u32)-512, 384, 0);
    run(op2(VI2F | (8u << 16), 4, C(1, 2), C(0, 2)));
    { const float e[4] = {1, -2, 1.5f, 0}; checkQ("vi2f.q scale 8", 1, 2, 4, e); }
    // half floats
    setQ(0, 3, 1.0f, -2.0f, 0.5f, 65504.0f);
    run(op2(VF2H, 4, C(2, 0, 0), C(0, 3)));
    { const u32 e[2] = {0xC0003C00u, 0x7BFF3800u}; checkQi("vf2h.q", 2, 0, 2, e); }
    run(op2(VH2F, 2, C(2, 1), C(2, 0)));
    { const float e[4] = {1.0f, -2.0f, 0.5f, 65504.0f}; checkQ("vh2f.p round trip", 2, 1, 4, e); }
    *vreg(S(3, 0, 0)) = 0;
    run(VFIM | ((u32)S(3, 0, 0) << 16) | 0x3E00);  // 1.5 as half
    checkF("vfim.s 1.5", *vreg(S(3, 0, 0)), 1.5f);
    run(VFIM | ((u32)S(3, 0, 1) << 16) | 0x3BFC);  // half 0x3bfc (seen in game) = 0.99805
    checkF("vfim.s 0x3bfc", *vreg(S(3, 0, 1)), 0.998046875f);
    run(VIIM | ((u32)S(3, 0, 2) << 16) | 0xFF85);  // -123
    checkF("viim.s -123", *vreg(S(3, 0, 2)), -123.0f);
    // integer packing
    setQi(0, 0, 0x7FFFFFFFu, 0x40000000u, (u32)-5, 0x00800000u);
    run(op2(VI2UC, 4, S(4, 0, 0), C(0, 0)));
    checkU("vi2uc.q", ctx.vi[64], 0x010080FFu);
    run(op2(VUC2I, 1, C(4, 1), S(4, 0, 0)));
    { const u32 e[4] = {0x7FFFFFFFu, 0x40404040u, 0, 0x00808080u}; checkQi("vuc2i.s", 4, 1, 4, e); }
    run(op2(VI2C, 4, S(4, 0, 1), C(0, 0)));
    checkU("vi2c.q", ctx.vi[65], 0x00FF407Fu);
    *vreg(S(4, 0, 2)) = 0;
    ctx.vi[66] = 0x80FF017Fu;
    run(op2(VC2I, 1, C(4, 2), S(4, 0, 2)));
    { const u32 e[4] = {0x7F000000u, 0x01000000u, 0xFF000000u, 0x80000000u}; checkQi("vc2i.s", 4, 2, 4, e); }
    setQi(0, 1, 0x7FFFFFFFu, (u32)-1, 0x00010000u, 0x40000000u);
    run(op2(VI2S, 4, C(5, 0, 0), C(0, 1)));
    { const u32 e[2] = {0xFFFF7FFFu, 0x40000001u}; checkQi("vi2s.q", 5, 0, 2, e); }
    run(op2(VI2US, 4, C(5, 1, 0), C(0, 1)));
    { const u32 e[2] = {0x0000FFFFu, 0x80000002u}; checkQi("vi2us.q", 5, 1, 2, e); }
    run(op2(VS2I, 2, C(5, 2), C(5, 0, 0)));
    { const u32 e[4] = {0x7FFF0000u, 0xFFFF0000u, 0x00010000u, 0x40000000u}; checkQi("vs2i.p", 5, 2, 4, e); }
    // colour conversion (ABGR8888 -> 565 / 5551 / 4444)
    setQi(0, 2, 0xFF0000FFu, 0x8000FF00u, 0x00FF0000u, 0xFFFFFFFFu);
    run(op2(VT5650, 4, C(6, 0, 0), C(0, 2)));
    { const u32 e[2] = {0x07E0001Fu, 0xFFFFF800u}; checkQi("vt5650.q", 6, 0, 2, e); }
    run(op2(VT5551, 4, C(6, 1, 0), C(0, 2)));
    { const u32 e[2] = {0x83E0801Fu, 0xFFFF7C00u}; checkQi("vt5551.q", 6, 1, 2, e); }
    run(op2(VT4444, 4, C(6, 2, 0), C(0, 2)));
    { const u32 e[2] = {0x80F0F00Fu, 0xFFFF0F00u}; checkQi("vt4444.q", 6, 2, 2, e); }
    // vcst
    run(VCST | (9u << 16) | sz(2) | (u32)C(7, 0));
    { const float e[2] = {3.14159265f, 3.14159265f}; checkQ("vcst.p VFPU_PI", 7, 0, 2, e); }
    // vidt
    run(op2(VIDT, 4, C(7, 2), 0));
    { const float e[4] = {0, 0, 1, 0}; checkQ("vidt.q C720", 7, 2, 4, e); }
    run(op2(VIDT, 4, R(7, 1), 0));
    check(mAt(7, 1, 0) == 0 && mAt(7, 1, 1) == 1 && mAt(7, 1, 2) == 0 && mAt(7, 1, 3) == 0, "vidt.q R701");
    run(op2(VONE, 3, C(7, 3), 0));
    { const float e[3] = {1, 1, 1}; checkQ("vone.t", 7, 3, 3, e); }
    // exponent helpers
    *vreg(S(3, 1, 0)) = 12.0f;  // 1.5 * 2^3
    run(op2(VLGB, 1, S(3, 1, 1), S(3, 1, 0)));
    checkF("vlgb.s 12", *vreg(S(3, 1, 1)), 3.0f);
    run(op2(VSBZ, 1, S(3, 1, 2), S(3, 1, 0)));
    checkF("vsbz.s 12", *vreg(S(3, 1, 2)), 1.5f);
    ctx.vi[3 * 16 + 2 * 4] = 2;  // S320 = integer 2
    run(op3(0x61000000, 1, S(3, 1, 3), S(3, 1, 0), S(3, 2, 0)));  // vsbn: 1.5 * 2^2 -> 6
    checkF("vsbn.s", *vreg(S(3, 1, 3)), 6.0f);
    // random numbers: vrndf1 in [1,2), deterministic for a given seed
    ctx.vi[0] = 1234;
    run(op2(VRNDS, 1, 0, S(0, 0, 0)));
    run(op2(VRNDF1, 4, C(1, 0), 0));
    bool ok = true;
    for (int i = 0; i < 4; i++) ok = ok && fsrc(1, 0, i) >= 1.0f && fsrc(1, 0, i) < 2.0f;
    float first = fsrc(1, 0, 0);
    ctx.vi[0] = 1234;
    run(op2(VRNDS, 1, 0, S(0, 0, 0)));
    run(op2(VRNDF1, 4, C(1, 0), 0));
    check(ok && fsrc(1, 0, 0) == first, "vrnds/vrndf1 range + determinism");
}

static void testMisc() {
    reset();
    int before = g_vfpuWarnings;
    run(VSYNC);
    run(VFLUSH);
    run(VNOP);
    check(g_vfpuWarnings == before, "vsync/vflush/vnop silent");
    g_quietLog = true;
    run(0x60000000 | (3u << 23));  // VFPU0 op 3: undefined
    run(0x60000000 | (3u << 23));
    g_quietLog = false;
    check(g_vfpuWarnings == before + 1, "unknown instruction warns exactly once");
}

static int sweepGameWords(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        printf("cannot open %s\n", path);
        return -1;
    }
    char line[128];
    int count = 0;
    const int before = g_vfpuWarnings;
    while (fgets(line, sizeof line, f)) {
        const char* p = strstr(line, "0x");
        u32 w = (u32)strtoul(p ? p + 2 : line, nullptr, 16);
        if (!w) continue;
        reset();
        for (int i = 1; i < 32; i++) ctx.r[i] = kBase + 0x10000;  // any base register points at valid memory
        for (int i = 0; i < 128; i++) ctx.v[i] = (float)(i % 7) - 3.0f;
        run(w);
        count++;
    }
    fclose(f);
    const int unknown = g_vfpuWarnings - before;
    printf("game word sweep: %d words executed, %d reported unimplemented\n", count, unknown);
    check(unknown == 0, "all game VFPU encodings implemented");
    return unknown;
}

int main(int argc, char** argv) {
    psp_mem = (u8*)VirtualAlloc(nullptr, 0x20000000, MEM_RESERVE, PAGE_NOACCESS);
    if (!psp_mem || !VirtualAlloc(psp_mem + 0x08000000, 0x02000000, MEM_COMMIT, PAGE_READWRITE)) {
        printf("cannot allocate guest memory\n");
        return 2;
    }
    testLoadStore();
    testMoves();
    testArith();
    testTranscendental();
    testMatrix();
    testPrefixes();
    testCompare();
    testConversions();
    testMisc();
    if (argc > 1) sweepGameWords(argv[1]);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
