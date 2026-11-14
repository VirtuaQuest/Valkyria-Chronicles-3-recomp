#pragma once
// GE (PSP Graphics Engine) register file, command numbers and decoded-state helpers.
#include <cstring>

#include "../core/runtime.h"

namespace vc2rt::gpu {

// ------------------------------------------------------------------ command numbers
enum GeCmd : u8 {
    CMD_NOP = 0x00, CMD_VADDR = 0x01, CMD_IADDR = 0x02, CMD_PRIM = 0x04, CMD_BEZIER = 0x05,
    CMD_SPLINE = 0x06, CMD_BBOX = 0x07, CMD_JUMP = 0x08, CMD_BJUMP = 0x09, CMD_CALL = 0x0A,
    CMD_RET = 0x0B, CMD_END = 0x0C, CMD_SIGNAL = 0x0E, CMD_FINISH = 0x0F, CMD_BASE = 0x10,
    CMD_VTYPE = 0x12, CMD_OFFSETADDR = 0x13, CMD_ORIGIN = 0x14, CMD_REGION1 = 0x15, CMD_REGION2 = 0x16,
    CMD_LIGHTING = 0x17, CMD_LIGHT0 = 0x18, CMD_DEPTHCLIP = 0x1C, CMD_CULLENABLE = 0x1D,
    CMD_TEXENABLE = 0x1E, CMD_FOGENABLE = 0x1F, CMD_DITHERENABLE = 0x20, CMD_BLENDENABLE = 0x21,
    CMD_ATESTENABLE = 0x22, CMD_ZTESTENABLE = 0x23, CMD_STESTENABLE = 0x24, CMD_AAENABLE = 0x25,
    CMD_PATCHCULL = 0x26, CMD_CTESTENABLE = 0x27, CMD_LOGICOPENABLE = 0x28,
    CMD_BONENUM = 0x2A, CMD_BONEDATA = 0x2B, CMD_MORPH0 = 0x2C, CMD_PATCHDIV = 0x36,
    CMD_PATCHPRIM = 0x37, CMD_PATCHFACING = 0x38, CMD_WORLDNUM = 0x3A, CMD_WORLDDATA = 0x3B,
    CMD_VIEWNUM = 0x3C, CMD_VIEWDATA = 0x3D, CMD_PROJNUM = 0x3E, CMD_PROJDATA = 0x3F,
    CMD_TGENNUM = 0x40, CMD_TGENDATA = 0x41, CMD_VPXSCALE = 0x42, CMD_VPYSCALE = 0x43,
    CMD_VPZSCALE = 0x44, CMD_VPXCENTER = 0x45, CMD_VPYCENTER = 0x46, CMD_VPZCENTER = 0x47,
    CMD_TEXSCALEU = 0x48, CMD_TEXSCALEV = 0x49, CMD_TEXOFFU = 0x4A, CMD_TEXOFFV = 0x4B,
    CMD_OFFSETX = 0x4C, CMD_OFFSETY = 0x4D, CMD_SHADEMODE = 0x50, CMD_REVNORMAL = 0x51,
    CMD_MATUPDATE = 0x53, CMD_MATEMISSIVE = 0x54, CMD_MATAMBIENT = 0x55, CMD_MATDIFFUSE = 0x56,
    CMD_MATSPECULAR = 0x57, CMD_MATALPHA = 0x58, CMD_MATSPECCOEF = 0x5B, CMD_AMBIENTCOLOR = 0x5C,
    CMD_AMBIENTALPHA = 0x5D, CMD_LIGHTMODE = 0x5E, CMD_LTYPE0 = 0x5F, CMD_LPOS0 = 0x63,
    CMD_LDIR0 = 0x6F, CMD_LATT0 = 0x7B, CMD_LCONV0 = 0x87, CMD_LCUTOFF0 = 0x8B, CMD_LCOLOR0 = 0x8F,
    CMD_CULL = 0x9B, CMD_FBPTR = 0x9C, CMD_FBWIDTH = 0x9D, CMD_ZBPTR = 0x9E, CMD_ZBWIDTH = 0x9F,
    CMD_TEXADDR0 = 0xA0, CMD_TEXBUFW0 = 0xA8, CMD_CLUTADDR = 0xB0, CMD_CLUTADDRUPPER = 0xB1,
    CMD_TRXSRC = 0xB2, CMD_TRXSRCW = 0xB3, CMD_TRXDST = 0xB4, CMD_TRXDSTW = 0xB5,
    CMD_TEXSIZE0 = 0xB8, CMD_TEXMAPMODE = 0xC0, CMD_TEXSHADELS = 0xC1, CMD_TEXMODE = 0xC2,
    CMD_TEXFORMAT = 0xC3, CMD_LOADCLUT = 0xC4, CMD_CLUTFORMAT = 0xC5, CMD_TEXFILTER = 0xC6,
    CMD_TEXWRAP = 0xC7, CMD_TEXLEVEL = 0xC8, CMD_TEXFUNC = 0xC9, CMD_TEXENVCOLOR = 0xCA,
    CMD_TEXFLUSH = 0xCB, CMD_TEXSYNC = 0xCC, CMD_FOG1 = 0xCD, CMD_FOG2 = 0xCE, CMD_FOGCOLOR = 0xCF,
    CMD_TEXLODSLOPE = 0xD0, CMD_FBPIXFMT = 0xD2, CMD_CLEARMODE = 0xD3, CMD_SCISSOR1 = 0xD4,
    CMD_SCISSOR2 = 0xD5, CMD_MINZ = 0xD6, CMD_MAXZ = 0xD7, CMD_CTEST = 0xD8, CMD_CREF = 0xD9,
    CMD_CMASK = 0xDA, CMD_ATEST = 0xDB, CMD_STEST = 0xDC, CMD_SOP = 0xDD, CMD_ZTEST = 0xDE,
    CMD_BLENDMODE = 0xDF, CMD_FIXA = 0xE0, CMD_FIXB = 0xE1, CMD_DITH0 = 0xE2, CMD_LOGICOP = 0xE6,
    CMD_ZWRITEDISABLE = 0xE7, CMD_MASKRGB = 0xE8, CMD_MASKALPHA = 0xE9, CMD_TRXKICK = 0xEA,
    CMD_TRXSRCPOS = 0xEB, CMD_TRXDSTPOS = 0xEC, CMD_TRXSIZE = 0xEE,
    CMD_VSCX = 0xF0, CMD_VSCY = 0xF1, CMD_VSCZ = 0xF2, CMD_VTCS = 0xF3, CMD_VTCT = 0xF4,
    CMD_VTCQ = 0xF5, CMD_VCV = 0xF6, CMD_VAP = 0xF7, CMD_VFC = 0xF8, CMD_VSCV = 0xF9,
};

enum PrimType { PRIM_POINTS = 0, PRIM_LINES, PRIM_LINE_STRIP, PRIM_TRIANGLES, PRIM_TRI_STRIP, PRIM_TRI_FAN, PRIM_RECTS };

// vertex type bits
enum : u32 {
    VT_TC_MASK = 3u << 0, VT_COL_MASK = 7u << 2, VT_NRM_MASK = 3u << 5, VT_POS_MASK = 3u << 7,
    VT_WT_MASK = 3u << 9, VT_IDX_MASK = 3u << 11, VT_WTCOUNT_MASK = 7u << 14, VT_MORPH_MASK = 7u << 18,
    VT_THROUGH = 1u << 23,
};

// ------------------------------------------------------------------ float24 helpers
inline float f24(u32 v) {
    u32 b = v << 8;
    float f;
    memcpy(&f, &b, 4);
    return f;
}
inline u32 toF24(float f) {
    u32 b;
    memcpy(&b, &f, 4);
    return b >> 8;
}

// ------------------------------------------------------------------ the register file
struct GeState {
    u32 cmd[256];         // last value written per command (full 32-bit command word)
    float world[12], view[12], proj[16], tgen[12], bone[96];
    float morph[8];
    // derived
    u32 offsetAddr;
    // CLUT memory (loaded by CMD_LOADCLUT)
    alignas(16) u8 clut[4096];
    u64 clutHash;
    u32 clutLoadedBytes;

    u32 r(u32 c) const { return cmd[c] & 0xFFFFFF; }
    u32 base() const { return (cmd[CMD_BASE] & 0x0F0000) << 8; }
    u32 relAddr(u32 data) const { return (offsetAddr + (base() | data)) & 0x0FFFFFFF; }

    bool through() const { return (r(CMD_VTYPE) & VT_THROUGH) != 0; }
    bool clearMode() const { return r(CMD_CLEARMODE) & 1; }
    // framebuffer
    u32 fbAddr() const { return r(CMD_FBPTR) & 0x1FFFF0; }
    u32 fbStride() const { return r(CMD_FBWIDTH) & 0x7FC; }
    u32 fbFormat() const { return r(CMD_FBPIXFMT) & 3; }
    u32 zbAddr() const { return r(CMD_ZBPTR) & 0x1FFFF0; }
    u32 zbStride() const { return r(CMD_ZBWIDTH) & 0x7FC; }
    // scissor (inclusive)
    int scX1() const { return r(CMD_SCISSOR1) & 0x3FF; }
    int scY1() const { return (r(CMD_SCISSOR1) >> 10) & 0x3FF; }
    int scX2() const { return r(CMD_SCISSOR2) & 0x3FF; }
    int scY2() const { return (r(CMD_SCISSOR2) >> 10) & 0x3FF; }
    int rgX2() const { return r(CMD_REGION2) & 0x3FF; }
    int rgY2() const { return (r(CMD_REGION2) >> 10) & 0x3FF; }
    // texture
    u32 texAddr(int l) const { return (r(CMD_TEXADDR0 + l) & 0xFFFFF0) | ((r(CMD_TEXBUFW0 + l) << 8) & 0x0F000000); }
    u32 texBufw(int l) const { return r(CMD_TEXBUFW0 + l) & 0x7FF; }
    int texW(int l) const { return 1 << std::min<u32>(r(CMD_TEXSIZE0 + l) & 0xF, 9); }
    int texH(int l) const { return 1 << std::min<u32>((r(CMD_TEXSIZE0 + l) >> 8) & 0xF, 9); }
    u32 texFormat() const { return r(CMD_TEXFORMAT) & 0xF; }
    bool texSwizzled() const { return r(CMD_TEXMODE) & 1; }
    int texMaxLevel() const { return (r(CMD_TEXMODE) >> 16) & 7; }
    bool clutShared() const { return (r(CMD_TEXMODE) & 0x100) == 0; }
    u32 clutAddr() const { return (r(CMD_CLUTADDR) & 0xFFFFF0) | ((r(CMD_CLUTADDRUPPER) << 8) & 0x0F000000); }
    u32 clutFormat() const { return r(CMD_CLUTFORMAT) & 3; }
    u32 clutShift() const { return (r(CMD_CLUTFORMAT) >> 2) & 0x1F; }
    u32 clutMask() const { return (r(CMD_CLUTFORMAT) >> 8) & 0xFF; }
    u32 clutStart() const { return ((r(CMD_CLUTFORMAT) >> 16) & 0x1F) << 4; }
    // misc
    float vpSX() const { return f24(r(CMD_VPXSCALE)); }
    float vpSY() const { return f24(r(CMD_VPYSCALE)); }
    float vpSZ() const { return f24(r(CMD_VPZSCALE)); }
    float vpCX() const { return f24(r(CMD_VPXCENTER)); }
    float vpCY() const { return f24(r(CMD_VPYCENTER)); }
    float vpCZ() const { return f24(r(CMD_VPZCENTER)); }
    float offX() const { return (float)(r(CMD_OFFSETX) & 0xFFFF) / 16.0f; }
    float offY() const { return (float)(r(CMD_OFFSETY) & 0xFFFF) / 16.0f; }
};

extern GeState g_ge;

// Context save/restore layout (our own, 512 words, used by sceGeListEnQueue's context option).
void saveContext(u32* dst);
void restoreContext(const u32* src);

}  // namespace vc2rt::gpu
