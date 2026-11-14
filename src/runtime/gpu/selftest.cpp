// GE self-test (VC2_GE_SELFTEST=dir): before the game starts, builds synthetic display lists in
// unused kernel RAM and renders a few scenes that exercise transform/lighting, depth, culling,
// textures (CLUT4, 16-bit, swizzled), fog, blending, alpha test, stencil and clear mode. The
// results are read back and written as PNGs for visual verification. All touched memory and GE
// state is restored afterwards.
#include <cmath>
#include <string>

#include "gpu.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

void geInitLists();

namespace {

constexpr u32 kScratch = 0x08200000;   // kernel RAM, unused by the game
constexpr u32 kScratchSize = 0x00100000;
constexpr u32 kFb = 0x04100000, kZb = 0x04190000;

struct Builder {
    u32 pc = kScratch;          // command stream
    u32 data = kScratch + 0x40000;  // vertex / texture data
    void cmd(u32 c, u32 v) {
        wr32(pc, (c << 24) | (v & 0xFFFFFF));
        pc += 4;
    }
    void cmdf(u32 c, float f) { cmd(c, toF24(f)); }
    u32 alloc(u32 bytes, u32 align = 16) {
        data = (data + align - 1) & ~(align - 1);
        u32 a = data;
        data += bytes;
        return a;
    }
    void addr(u32 cmdNo, u32 a) {
        cmd(CMD_BASE, (a >> 8) & 0x0F0000);
        cmd(cmdNo, a & 0xFFFFFF);
    }
    void matrix(u32 numCmd, const float* m, int n) {
        cmd(numCmd, 0);
        for (int i = 0; i < n; i++) cmdf(numCmd + 1, m[i]);
    }
};

void put16(u32& a, u16 v) {
    wr16(a, v);
    a += 2;
}
void putf(u32& a, float f) {
    u32 b;
    memcpy(&b, &f, 4);
    wr32(a, b);
    a += 4;
}

void setupTarget(Builder& b) {
    b.cmd(CMD_FBPTR, kFb & 0xFFFFFF);
    b.cmd(CMD_FBWIDTH, ((kFb >> 24) << 16) | 512);
    b.cmd(CMD_FBPIXFMT, 3);
    b.cmd(CMD_ZBPTR, kZb & 0xFFFFFF);
    b.cmd(CMD_ZBWIDTH, ((kZb >> 24) << 16) | 512);
    b.cmd(CMD_SCISSOR1, 0);
    b.cmd(CMD_SCISSOR2, (271 << 10) | 479);
    b.cmd(CMD_REGION1, 0);
    b.cmd(CMD_REGION2, (271 << 10) | 479);
    b.cmd(CMD_OFFSETX, (2048 - 240) << 4);
    b.cmd(CMD_OFFSETY, (2048 - 136) << 4);
    b.cmdf(CMD_VPXSCALE, 240);
    b.cmdf(CMD_VPYSCALE, -136);
    b.cmdf(CMD_VPZSCALE, -32767.5f);
    b.cmdf(CMD_VPXCENTER, 2048);
    b.cmdf(CMD_VPYCENTER, 2048);
    b.cmdf(CMD_VPZCENTER, 32767.5f);
    b.cmd(CMD_MINZ, 0);
    b.cmd(CMD_MAXZ, 0xFFFF);
    b.cmd(CMD_MASKRGB, 0);
    b.cmd(CMD_MASKALPHA, 0);
    b.cmd(CMD_DEPTHCLIP, 1);
}

// Clear colour+depth+stencil with a through-mode sprite.
void clear(Builder& b, u32 color, u16 depth) {
    u32 v = b.alloc(2 * 12), a = v;
    for (int i = 0; i < 2; i++) {
        wr32(a, color);
        a += 4;
        put16(a, (u16)(i ? 480 : 0));
        put16(a, (u16)(i ? 272 : 0));
        put16(a, depth);
        put16(a, 0);
    }
    b.cmd(CMD_CLEARMODE, 0x701);
    b.cmd(CMD_VTYPE, (1 << 23) | (7 << 2) | (2 << 7));
    b.addr(CMD_VADDR, v);
    b.cmd(CMD_PRIM, (6 << 16) | 2);
    b.cmd(CMD_CLEARMODE, 0);
}

void perspective(float* m, float fovy, float aspect, float zn, float zf) {
    const float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 16 * 4);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1;
    m[14] = 2 * zf * zn / (zn - zf);
}

void rotYX(float* m43, float ay, float ax, float tx, float ty, float tz) {
    const float cy = cosf(ay), sy = sinf(ay), cx = cosf(ax), sx = sinf(ax);
    // columns: x axis, y axis, z axis, translation (R = Ry * Rx)
    float c[3][3] = {{cy, 0, -sy}, {sy * sx, cx, cy * sx}, {sy * cx, -sx, cy * cx}};
    for (int k = 0; k < 3; k++) m43[k] = c[0][k], m43[3 + k] = c[1][k], m43[6 + k] = c[2][k];
    m43[9] = tx, m43[10] = ty, m43[11] = tz;
}

// Unit cube (24 verts, 36 indices) with normals, per-face colours and UVs. float everything.
void cube(Builder& b, u32& vaddr, u32& iaddr) {
    static const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const u32 col[6] = {0xFF4040FF, 0xFF40FF40, 0xFFFF4040, 0xFF40FFFF, 0xFFFF40FF, 0xFFFFFF40};
    vaddr = b.alloc(24 * 36);
    u32 a = vaddr;
    for (int f = 0; f < 6; f++) {
        const float* N = n[f];
        float u[3], v[3];
        if (fabsf(N[1]) > 0.5f) u[0] = 1, u[1] = 0, u[2] = 0;
        else u[0] = -N[2], u[1] = 0, u[2] = N[0];
        v[0] = N[1] * u[2] - N[2] * u[1];
        v[1] = N[2] * u[0] - N[0] * u[2];
        v[2] = N[0] * u[1] - N[1] * u[0];
        const float cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int k = 0; k < 4; k++) {
            putf(a, (cs[k][0] + 1) * 0.5f);  // uv
            putf(a, (cs[k][1] + 1) * 0.5f);
            wr32(a, col[f]);
            a += 4;
            putf(a, N[0]), putf(a, N[1]), putf(a, N[2]);
            for (int j = 0; j < 3; j++) putf(a, (N[j] + cs[k][0] * u[j] + cs[k][1] * v[j]) * 0.5f);
        }
    }
    iaddr = b.alloc(36 * 2);
    u32 ia = iaddr;
    for (int f = 0; f < 6; f++) {
        const u16 q = (u16)(f * 4);
        const u16 idx[6] = {q, (u16)(q + 1), (u16)(q + 2), q, (u16)(q + 2), (u16)(q + 3)};
        for (u16 x : idx) put16(ia, x);
    }
}

// 64x64 CLUT4 checker texture (swizzled) and a 16-entry palette.
void checkerTexture(Builder& b, u32& tex, u32& clut) {
    clut = b.alloc(64, 64);
    for (int i = 0; i < 16; i++) wr32(clut + i * 4, i == 0 ? 0xFFFFFFFF : (i == 1 ? 0xFF202020 : 0x80FF8000 + i));
    // linear 4bpp image, 32 bytes per row
    u8 lin[64 * 32];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x += 2) {
            auto px = [&](int xx) { return (((xx >> 3) ^ (y >> 3)) & 1) ? 1 : 0; };
            u8 v = (u8)(px(x) | (px(x + 1) << 4));
            if (y >= 28 && y < 36 && x >= 28 && x < 36) v = 0x22;  // a translucent centre block
            lin[y * 32 + x / 2] = v;
        }
    tex = b.alloc(sizeof lin, 16);
    // swizzle: 16-byte x 8-row blocks
    const int rowBytes = 32, bxCount = rowBytes / 16;
    u32 o = tex;
    for (int by = 0; by < 64 / 8; by++)
        for (int bx = 0; bx < bxCount; bx++)
            for (int j = 0; j < 8; j++) {
                memcpy(memPtr(o), lin + (by * 8 + j) * rowBytes + bx * 16, 16);
                o += 16;
            }
}

void runList(u32 start, u32 end) {
    wr32(end, CMD_FINISH << 24);
    wr32(end + 4, CMD_END << 24);
    listEnqueue(start, 0, -1, 0, false);
    drawSync(0);
}

void dumpTarget(const std::string& dir, const char* name) {
    flushDraws();
    int ox, oy;
    RenderTarget* rt = fbFindForTexture(kFb, 512, 3, 512, 512, ox, oy);
    if (!rt) {
        LOG_ERROR("SELFTEST", "%s: no render target", name);
        return;
    }
    fbDownload(rt, 0, 0, 480, 272);
    std::vector<u32> px(480 * 272);
    decodeFbPixels(memPtr(kFb), 512, 3, 480, 272, px.data());
    for (auto& p : px) p |= 0xFF000000;
    std::string path = dir + "\\selftest_" + name + ".png";
    savePng(path.c_str(), px.data(), 480, 272, false);
    LOG_INFO("SELFTEST", "wrote %s", path.c_str());
}

}  // namespace

void runSelfTest() {
    const char* dirEnv = getenv("VC2_GE_SELFTEST");
    if (!dirEnv) return;
    std::string dir = dirEnv;
    std::vector<u8> savedRam(kScratchSize), savedVram(0x100000);
    memcpy(savedRam.data(), memPtr(kScratch), kScratchSize);
    memcpy(savedVram.data(), memPtr(0x04100000), 0x100000);
    memset(memPtr(kScratch), 0, kScratchSize);

    // ---------------- scene 1: lit, textured, culled perspective cubes + fog + sprites
    {
        Builder b;
        setupTarget(b);
        clear(b, 0xFF603010, 0);
        float proj[16], view[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, world[12];
        perspective(proj, 1.0f, 480.0f / 272.0f, 0.5f, 100.0f);
        b.matrix(CMD_PROJNUM, proj, 16);
        b.matrix(CMD_VIEWNUM, view, 12);
        u32 cv, ci, tex, clut;
        cube(b, cv, ci);
        checkerTexture(b, tex, clut);
        // depth test (the viewport maps near to 65535: GEQUAL keeps nearer fragments)
        b.cmd(CMD_ZTESTENABLE, 1);
        b.cmd(CMD_ZTEST, 7);
        b.cmd(CMD_ZWRITEDISABLE, 0);
        b.cmd(CMD_CULLENABLE, 1);
        b.cmd(CMD_CULL, 0);  // GU_CCW front faces
        b.cmd(CMD_SHADEMODE, 1);
        // light 0: directional from the upper left, white
        b.cmd(CMD_LIGHTING, 1);
        b.cmd(CMD_LIGHT0, 1);
        b.cmd(CMD_LTYPE0, 0x000);
        b.cmdf(CMD_LPOS0, -0.5f), b.cmdf(CMD_LPOS0 + 1, 0.7f), b.cmdf(CMD_LPOS0 + 2, 0.6f);
        b.cmd(CMD_LCOLOR0 + 0, 0x000000);
        b.cmd(CMD_LCOLOR0 + 1, 0xFFFFFF);
        b.cmd(CMD_LCOLOR0 + 2, 0xFFFFFF);
        b.cmd(CMD_AMBIENTCOLOR, 0x404040);
        b.cmd(CMD_AMBIENTALPHA, 0xFF);
        b.cmd(CMD_MATUPDATE, 3);  // vertex colour = ambient + diffuse
        b.cmd(CMD_MATEMISSIVE, 0);
        // texture: CLUT4 swizzled, modulate
        b.cmd(CMD_TEXENABLE, 1);
        b.cmd(CMD_TEXMODE, 1);  // swizzled
        b.cmd(CMD_TEXFORMAT, 4);
        b.cmd(CMD_CLUTFORMAT, 3 | (0xFF << 8));
        b.cmd(CMD_CLUTADDR, clut & 0xFFFFFF);
        b.cmd(CMD_CLUTADDRUPPER, (clut >> 8) & 0x0F0000);
        b.cmd(CMD_LOADCLUT, 2);
        b.cmd(CMD_TEXADDR0, tex & 0xFFFFFF);
        b.cmd(CMD_TEXBUFW0, ((tex >> 8) & 0x0F0000) | 64);
        b.cmd(CMD_TEXSIZE0, (6 << 8) | 6);
        b.cmd(CMD_TEXFUNC, 0x100);  // modulate, texture alpha
        b.cmd(CMD_TEXFILTER, 0x101);
        b.cmd(CMD_TEXWRAP, 0);
        b.cmdf(CMD_TEXSCALEU, 2), b.cmdf(CMD_TEXSCALEV, 2), b.cmdf(CMD_TEXOFFU, 0), b.cmdf(CMD_TEXOFFV, 0);
        b.cmd(CMD_TEXMAPMODE, 0);
        b.cmd(CMD_TEXFLUSH, 0);
        const u32 vt = 3 | (7 << 2) | (3 << 5) | (3 << 7) | (2 << 11);  // float uv, 8888, float nrm/pos, 16-bit idx
        b.cmd(CMD_VTYPE, vt);
        // cube 1 (left): textured + lit
        rotYX(world, 0.6f, 0.4f, -1.6f, 0.0f, -5.0f);
        b.matrix(CMD_WORLDNUM, world, 12);
        b.addr(CMD_VADDR, cv);
        b.addr(CMD_IADDR, ci);
        b.cmd(CMD_PRIM, (3 << 16) | 36);
        // cube 2 (right, behind & intersecting a third): untextured, fogged
        b.cmd(CMD_TEXENABLE, 0);
        b.cmd(CMD_FOGENABLE, 1);
        b.cmd(CMD_FOGCOLOR, 0x808080);
        b.cmdf(CMD_FOG1, 9.0f);          // fog end (view z is negative in front of the camera)
        b.cmdf(CMD_FOG2, 1.0f / 6.0f);   // slope: z=-3 -> 1 (clear), z=-9 -> 0 (full fog)
        rotYX(world, -0.5f, 0.3f, 1.8f, 0.2f, -7.0f);
        b.matrix(CMD_WORLDNUM, world, 12);
        b.addr(CMD_VADDR, cv);
        b.addr(CMD_IADDR, ci);
        b.cmd(CMD_PRIM, (3 << 16) | 36);
        rotYX(world, 0.2f, 0.9f, 1.2f, -0.4f, -5.5f);
        b.matrix(CMD_WORLDNUM, world, 12);
        b.addr(CMD_VADDR, cv);
        b.addr(CMD_IADDR, ci);
        b.cmd(CMD_PRIM, (3 << 16) | 36);
        b.cmd(CMD_FOGENABLE, 0);
        b.cmd(CMD_LIGHTING, 0);
        b.cmd(CMD_ZTESTENABLE, 0);
        b.cmd(CMD_CULLENABLE, 0);
        // through-mode sprite with the CLUT texture, alpha blended, nearest filter, texel exact
        {
            u32 sv = b.alloc(2 * 12), a = sv;
            const u16 sp[2][4] = {{0, 0, 16, 16}, {64, 64, 16 + 128, 16 + 128}};  // u v x y
            for (int i = 0; i < 2; i++) {  // 10-byte vertices: u16 uv, s16 xyz
                put16(a, sp[i][0]), put16(a, sp[i][1]);
                put16(a, sp[i][2]), put16(a, sp[i][3]), put16(a, 0);
            }
            b.cmd(CMD_TEXENABLE, 1);
            b.cmd(CMD_TEXFILTER, 0);
            b.cmd(CMD_TEXFUNC, 0x103);  // replace with alpha
            b.cmd(CMD_BLENDENABLE, 1);
            b.cmd(CMD_BLENDMODE, 2 | (3 << 4));  // src alpha, 1 - src alpha
            b.cmd(CMD_VTYPE, (1 << 23) | 2 | (2 << 7));
            b.addr(CMD_VADDR, sv);
            b.cmd(CMD_PRIM, (6 << 16) | 2);
        }
        // additive blended gouraud triangle strip in through mode (fixed colours)
        {
            u32 tv = b.alloc(4 * 12), a = tv;
            const u32 cols[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
            const s16 pos[4][2] = {{300, 180}, {460, 180}, {300, 260}, {460, 260}};
            for (int i = 0; i < 4; i++) {
                wr32(a, cols[i]);
                a += 4;
                put16(a, (u16)pos[i][0]), put16(a, (u16)pos[i][1]), put16(a, 0);
                put16(a, 0);
            }
            b.cmd(CMD_TEXENABLE, 0);
            b.cmd(CMD_BLENDMODE, 10 | (10 << 4));  // FIXA, FIXB
            b.cmd(CMD_FIXA, 0xFFFFFF);
            b.cmd(CMD_FIXB, 0x808080);
            b.cmd(CMD_VTYPE, (1 << 23) | (7 << 2) | (2 << 7));
            b.addr(CMD_VADDR, tv);
            b.cmd(CMD_PRIM, (4 << 16) | 4);
            b.cmd(CMD_BLENDENABLE, 0);
        }
        runList(kScratch, b.pc);
        dumpTarget(dir, "scene1");
    }
    // ---------------- scene 2: stencil masking, alpha test, colour test, logic op, line/point prims
    {
        Builder b;
        setupTarget(b);
        clear(b, 0xFF000000, 0);
        u32 rv = b.alloc(16 * 12);
        auto rect = [&](u32 color, int x0, int y0, int x1, int y1) {
            u32 a = b.alloc(2 * 12), v = a;
            for (int i = 0; i < 2; i++) {
                wr32(a, color);
                a += 4;
                put16(a, (u16)(i ? x1 : x0)), put16(a, (u16)(i ? y1 : y0)), put16(a, 0);
                put16(a, 0);
            }
            b.cmd(CMD_VTYPE, (1 << 23) | (7 << 2) | (2 << 7));
            b.addr(CMD_VADDR, v);
            b.cmd(CMD_PRIM, (6 << 16) | 2);
        };
        (void)rv;
        // write stencil 1 into a centre box (colour masked)
        b.cmd(CMD_STESTENABLE, 1);
        b.cmd(CMD_STEST, 1 | (1 << 8) | (0xFF << 16));  // always, ref 1
        b.cmd(CMD_SOP, 2 | (2 << 8) | (2 << 16));       // replace
        b.cmd(CMD_MASKRGB, 0xFFFFFF);
        rect(0xFFFFFFFF, 120, 60, 360, 212);
        b.cmd(CMD_MASKRGB, 0);
        // draw a full red rect only where stencil == 1
        b.cmd(CMD_STEST, 2 | (1 << 8) | (0xFF << 16));  // equal 1
        b.cmd(CMD_SOP, 0);
        rect(0xFF0000FF, 0, 0, 480, 272);
        b.cmd(CMD_STESTENABLE, 0);
        // alpha test: only alpha > 0x80 passes: left half green passes, right half blue rejected
        b.cmd(CMD_ATESTENABLE, 1);
        b.cmd(CMD_ATEST, 6 | (0x80 << 8) | (0xFF << 16));
        rect(0xC000FF00, 20, 20, 100, 100);
        rect(0x40FF0000, 380, 20, 460, 100);
        b.cmd(CMD_ATESTENABLE, 0);
        // colour test: reject pure yellow
        b.cmd(CMD_CTESTENABLE, 1);
        b.cmd(CMD_CTEST, 3);  // pass if != ref
        b.cmd(CMD_CREF, 0x00FFFF);
        b.cmd(CMD_CMASK, 0xFFFFFF);
        rect(0xFF00FFFF, 20, 180, 100, 260);   // yellow: rejected
        rect(0xFFFF00FF, 380, 180, 460, 260);  // magenta: drawn
        b.cmd(CMD_CTESTENABLE, 0);
        // logic op XOR inverting a band
        b.cmd(CMD_LOGICOPENABLE, 1);
        b.cmd(CMD_LOGICOP, 6);
        rect(0xFFFFFFFF, 200, 0, 280, 272);
        b.cmd(CMD_LOGICOPENABLE, 0);
        // lines and points
        {
            u32 lv = b.alloc(6 * 12), a = lv;
            const s16 p[6][2] = {{10, 130}, {470, 140}, {10, 150}, {470, 160}, {240, 120}, {250, 120}};
            for (int i = 0; i < 6; i++) {
                wr32(a, 0xFF00FFFF);
                a += 4;
                put16(a, (u16)p[i][0]), put16(a, (u16)p[i][1]), put16(a, 0);
                put16(a, 0);
            }
            b.cmd(CMD_VTYPE, (1 << 23) | (7 << 2) | (2 << 7));
            b.addr(CMD_VADDR, lv);
            b.cmd(CMD_PRIM, (1 << 16) | 4);
            b.cmd(CMD_PRIM, (0 << 16) | 2);
        }
        runList(kScratch, b.pc);
        dumpTarget(dir, "scene2");
    }

    // ---------------- scene 3: render-to-texture, block transfer, bezier patch, skinning, morphing
    {
        constexpr u32 kRtt = 0x041E0000;
        Builder b;
        // offscreen 128x128 target: gradient + white diagonal
        b.cmd(CMD_FBPTR, kRtt & 0xFFFFFF);
        b.cmd(CMD_FBWIDTH, ((kRtt >> 24) << 16) | 128);
        b.cmd(CMD_FBPIXFMT, 3);
        b.cmd(CMD_SCISSOR1, 0);
        b.cmd(CMD_SCISSOR2, (127 << 10) | 127);
        b.cmd(CMD_REGION2, (127 << 10) | 127);
        b.cmd(CMD_MASKRGB, 0);
        b.cmd(CMD_MASKALPHA, 0);
        {
            u32 v = b.alloc(4 * 12), a = v;
            const u32 cols[4] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFF00FFFF};
            const s16 pos[4][2] = {{0, 0}, {128, 0}, {0, 128}, {128, 128}};
            for (int i = 0; i < 4; i++) {
                wr32(a, cols[i]);
                a += 4;
                put16(a, (u16)pos[i][0]), put16(a, (u16)pos[i][1]), put16(a, 0);
                put16(a, 0);
            }
            b.cmd(CMD_CLEARMODE, 0);
            b.cmd(CMD_VTYPE, (1 << 23) | (7 << 2) | (2 << 7));
            b.addr(CMD_VADDR, v);
            b.cmd(CMD_PRIM, (4 << 16) | 4);
            u32 l = b.alloc(2 * 12);
            a = l;
            for (int i = 0; i < 2; i++) {
                wr32(a, 0xFFFFFFFF);
                a += 4;
                put16(a, (u16)(i ? 128 : 0)), put16(a, (u16)(i ? 128 : 0)), put16(a, 0);
                put16(a, 0);
            }
            b.addr(CMD_VADDR, l);
            b.cmd(CMD_PRIM, (1 << 16) | 2);
        }
        // main target: show the offscreen buffer as a texture, scaled 2x with bilinear filtering
        setupTarget(b);
        clear(b, 0xFF202020, 0);
        {
            u32 sv = b.alloc(2 * 10), a = sv;
            const u16 sp[2][4] = {{0, 0, 10, 10}, {128, 128, 266, 266}};
            for (int i = 0; i < 2; i++) put16(a, sp[i][0]), put16(a, sp[i][1]), put16(a, sp[i][2]), put16(a, sp[i][3]), put16(a, 0);
            b.cmd(CMD_TEXENABLE, 1);
            b.cmd(CMD_TEXMODE, 0);
            b.cmd(CMD_TEXFORMAT, 3);
            b.cmd(CMD_TEXADDR0, kRtt & 0xFFFFFF);
            b.cmd(CMD_TEXBUFW0, ((kRtt >> 8) & 0x0F0000) | 128);
            b.cmd(CMD_TEXSIZE0, (7 << 8) | 7);
            b.cmd(CMD_TEXFUNC, 0x003);  // replace, no alpha
            b.cmd(CMD_TEXFILTER, 0x101);
            b.cmd(CMD_TEXWRAP, 0x101);
            b.cmd(CMD_TEXFLUSH, 0);
            b.cmd(CMD_VTYPE, (1 << 23) | 2 | (2 << 7));
            b.addr(CMD_VADDR, sv);
            b.cmd(CMD_PRIM, (6 << 16) | 2);
            b.cmd(CMD_TEXENABLE, 0);
        }
        // block transfer: copy the top-left 64x64 of the shown texture to (300, 10)
        b.cmd(CMD_TRXSRC, kFb & 0xFFFFFF);
        b.cmd(CMD_TRXSRCW, ((kFb >> 8) & 0xFF0000) | 512);
        b.cmd(CMD_TRXDST, kFb & 0xFFFFFF);
        b.cmd(CMD_TRXDSTW, ((kFb >> 8) & 0xFF0000) | 512);
        b.cmd(CMD_TRXSRCPOS, (10 << 10) | 10);
        b.cmd(CMD_TRXDSTPOS, (10 << 10) | 300);
        b.cmd(CMD_TRXSIZE, (63 << 10) | 63);
        b.cmd(CMD_TRXKICK, 1);
        // bezier patch (4x4 control points, float positions + colours) in perspective
        {
            float proj[16], view[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            float world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 2.2f, -1.3f, -5.0f};
            perspective(proj, 1.0f, 480.0f / 272.0f, 0.5f, 100.0f);
            b.matrix(CMD_PROJNUM, proj, 16);
            b.matrix(CMD_VIEWNUM, view, 12);
            b.matrix(CMD_WORLDNUM, world, 12);
            u32 cp = b.alloc(16 * 16), a = cp;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    const u32 c = rgba(i * 80, j * 80, 255 - i * 60, 255);
                    wr32(a, c);
                    a += 4;
                    putf(a, (i - 1.5f) * 0.6f), putf(a, (j - 1.5f) * 0.6f);
                    putf(a, (i == 1 || i == 2) && (j == 1 || j == 2) ? 1.2f : 0.0f);
                }
            b.cmd(CMD_LIGHTING, 0);
            b.cmd(CMD_ZTESTENABLE, 0);
            b.cmd(CMD_CULLENABLE, 0);
            b.cmd(CMD_PATCHDIV, (8 << 8) | 8);
            b.cmd(CMD_PATCHPRIM, 0);
            b.cmd(CMD_VTYPE, (7 << 2) | (3 << 7));
            b.addr(CMD_VADDR, cp);
            b.cmd(CMD_BEZIER, (4 << 8) | 4);
        }
        // skinning: one quad, two bones (left half follows bone 0, right half bone 1 moved up)
        {
            float bone0[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            float bone1[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0.6f, 0};
            float world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, -2.2f, -1.0f, -5.0f};
            b.matrix(CMD_WORLDNUM, world, 12);
            b.cmd(CMD_BONENUM, 0);
            for (int i = 0; i < 12; i++) b.cmdf(CMD_BONEDATA, bone0[i]);
            for (int i = 0; i < 12; i++) b.cmdf(CMD_BONEDATA, bone1[i]);
            // vertex: 2 float weights, 8888 colour, float pos -> 8 + 4 + 12 = 24
            u32 sv = b.alloc(4 * 24), a = sv;
            const float P[4][2] = {{-0.8f, -0.4f}, {0.8f, -0.4f}, {-0.8f, 0.4f}, {0.8f, 0.4f}};
            for (int i = 0; i < 4; i++) {
                const float w1 = P[i][0] > 0 ? 1.0f : 0.0f;
                putf(a, 1.0f - w1), putf(a, w1);
                wr32(a, 0xFF00C0FF);
                a += 4;
                putf(a, P[i][0]), putf(a, P[i][1]), putf(a, 0);
            }
            b.cmd(CMD_VTYPE, (3 << 9) | (1 << 14) | (7 << 2) | (3 << 7));
            b.addr(CMD_VADDR, sv);
            b.cmd(CMD_PRIM, (4 << 16) | 4);
        }
        // morphing: two targets, weights 0.25 / 0.75 between a small and a large triangle
        {
            float world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0.0f, 1.0f, -5.0f};
            b.matrix(CMD_WORLDNUM, world, 12);
            b.cmdf(CMD_MORPH0, 0.25f);
            b.cmdf(CMD_MORPH0 + 1, 0.75f);
            // vertex per target: 8888 colour + float pos = 16
            u32 mv = b.alloc(3 * 32), a = mv;
            const float T[3][2] = {{0, 0.5f}, {-0.5f, -0.3f}, {0.5f, -0.3f}};
            for (int i = 0; i < 3; i++)
                for (int m = 0; m < 2; m++) {
                    wr32(a, m ? 0xFF0000FF : 0xFFFFFFFF);
                    a += 4;
                    const float s = m ? 1.0f : 0.2f;
                    putf(a, T[i][0] * s), putf(a, T[i][1] * s), putf(a, 0);
                }
            b.cmd(CMD_VTYPE, (1 << 18) | (7 << 2) | (3 << 7));
            b.addr(CMD_VADDR, mv);
            b.cmd(CMD_PRIM, (3 << 16) | 3);
            b.cmdf(CMD_MORPH0, 1.0f);
            b.cmdf(CMD_MORPH0 + 1, 0.0f);
        }
        runList(kScratch, b.pc);
        dumpTarget(dir, "scene3");
    }

    // ---------------- scene 4: texture formats. Row 1: 5650 5551 4444 8888 CLUT8(shift/mask)
    //                  Row 2: DXT1 DXT3 DXT5 CLUT16-index, mipmapped quad in perspective
    {
        Builder b;
        setupTarget(b);
        clear(b, 0xFF404040, 0);
        b.cmd(CMD_TEXENABLE, 1);
        b.cmd(CMD_TEXFUNC, 0x103);
        b.cmd(CMD_TEXFILTER, 0);
        b.cmd(CMD_TEXWRAP, 0);
        b.cmd(CMD_BLENDENABLE, 1);
        b.cmd(CMD_BLENDMODE, 2 | (3 << 4));
        auto sprite = [&](u32 tex, u32 fmt, int log2w, int x, int y, int size) {
            u32 sv = b.alloc(2 * 10), a = sv;
            const int tw = 1 << log2w;
            put16(a, 0), put16(a, 0), put16(a, (u16)x), put16(a, (u16)y), put16(a, 0);
            put16(a, (u16)tw), put16(a, (u16)tw), put16(a, (u16)(x + size)), put16(a, (u16)(y + size)), put16(a, 0);
            b.cmd(CMD_TEXMODE, 0);
            b.cmd(CMD_TEXFORMAT, fmt);
            b.cmd(CMD_TEXADDR0, tex & 0xFFFFFF);
            b.cmd(CMD_TEXBUFW0, ((tex >> 8) & 0x0F0000) | tw);
            b.cmd(CMD_TEXSIZE0, (log2w << 8) | log2w);
            b.cmd(CMD_TEXFLUSH, 0);
            b.cmd(CMD_VTYPE, (1 << 23) | 2 | (2 << 7));
            b.addr(CMD_VADDR, sv);
            b.cmd(CMD_PRIM, (6 << 16) | 2);
        };
        // 16x16 gradient images: r = x, g = y, b = 255 - x, alpha = left half opaque, right 50%
        for (u32 fmt = 0; fmt < 4; fmt++) {
            const u32 bpp = fmt == 3 ? 4 : 2;
            u32 t = b.alloc(16 * 16 * bpp), a = t;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    const u32 r = x * 17, g = y * 17, bl = 255 - x * 17, al = x < 8 ? 255 : 128;
                    if (fmt == 0) put16(a, (u16)((r >> 3) | ((g >> 2) << 5) | ((bl >> 3) << 11)));
                    else if (fmt == 1) put16(a, (u16)((r >> 3) | ((g >> 3) << 5) | ((bl >> 3) << 10) | ((al >> 7) << 15)));
                    else if (fmt == 2) put16(a, (u16)((r >> 4) | ((g >> 4) << 4) | ((bl >> 4) << 8) | ((al >> 4) << 12)));
                    else wr32(a, rgba(r, g, bl, al)), a += 4;
                }
            sprite(t, fmt, 4, 10 + fmt * 90, 10, 80);
        }
        // CLUT8: indices x*16+y; palette shift 4 mask 0x0F start 16 -> uses entries 16..31 (= x)
        {
            u32 pal = b.alloc(256 * 4, 64);
            for (int i = 0; i < 256; i++) wr32(pal + i * 4, i >= 16 && i < 32 ? rgba((i - 16) * 17, 255 - (i - 16) * 17, 0, 255) : 0xFFFF00FF);
            u32 t = b.alloc(16 * 16), a = t;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) wr8(a++, (u8)(x * 16 + y));
            b.cmd(CMD_CLUTADDR, pal & 0xFFFFFF);
            b.cmd(CMD_CLUTADDRUPPER, (pal >> 8) & 0x0F0000);
            b.cmd(CMD_CLUTFORMAT, 3 | (4 << 2) | (0x0F << 8) | (1 << 16));
            b.cmd(CMD_LOADCLUT, 32);
            sprite(t, 5, 4, 370, 10, 80);
        }
        // DXT: 16x16, every 4x4 block uses endpoints red/blue (565) with index = x&3
        for (int k = 0; k < 3; k++) {
            const u32 blockBytes = k == 0 ? 8 : 16;
            u32 t = b.alloc(16 * blockBytes), a = t;
            for (int blk = 0; blk < 16; blk++) {
                const u32 lines = 0xE4E4E4E4;  // indices 0,1,2,3 per row
                if (k == 0) {
                    wr32(a, lines), wr16(a + 4, 0xF800), wr16(a + 6, 0x001F);  // red > blue: 4-colour mode
                } else {
                    wr32(a, lines), wr16(a + 4, 0xF800), wr16(a + 6, 0x001F);
                    if (k == 1) {
                        for (int r = 0; r < 4; r++) wr16(a + 8 + r * 2, (u16)(0xF000 | (0x0A00) | (0x0050) | (0x000F - r * 4)));
                    } else {
                        // DXT5: alpha endpoints 255 and 0, index row = 0,1,6,7... -> 255, 0, 0, 255
                        wr32(a + 8, 0x00E3E3E3 & 0x00FFFFFF);
                        wr16(a + 12, 0x0E3E);
                        wr8(a + 14, 255), wr8(a + 15, 0);
                    }
                }
                a += blockBytes;
            }
            sprite(t, 8 + k, 4, 10 + k * 90, 110, 80);
        }
        // CLUT16 index texture (16-bit indices, low byte used)
        {
            u32 pal = b.alloc(16 * 4, 64);
            for (int i = 0; i < 16; i++) wr32(pal + i * 4, rgba(0, i * 17, 255 - i * 17, 255));
            u32 t = b.alloc(16 * 16 * 2), a = t;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) put16(a, (u16)(0x1200 | y));
            b.cmd(CMD_CLUTADDR, pal & 0xFFFFFF);
            b.cmd(CMD_CLUTADDRUPPER, (pal >> 8) & 0x0F0000);
            b.cmd(CMD_CLUTFORMAT, 3 | (0x0F << 8));
            b.cmd(CMD_LOADCLUT, 2);
            sprite(t, 6, 4, 280, 110, 80);
        }
        b.cmd(CMD_BLENDENABLE, 0);
        // mipmapped 32x32 checker floor in perspective: level 0 red/white, 1 green, 2 blue, 3 yellow...
        {
            const u32 lvCol[6] = {0, 0xFF00FF00, 0xFFFF0000, 0xFF00FFFF, 0xFFFF00FF, 0xFFFFFFFF};
            u32 lvAddr[6];
            for (int l = 0; l < 6; l++) {
                const int s = 32 >> l;
                u32 t = b.alloc(s * s * 4), a = t;
                lvAddr[l] = t;
                for (int y = 0; y < s; y++)
                    for (int x = 0; x < s; x++) {
                        wr32(a, l == 0 ? ((((x >> 2) ^ (y >> 2)) & 1) ? 0xFFFFFFFF : 0xFF0000FF) : lvCol[l]);
                        a += 4;
                    }
            }
            b.cmd(CMD_TEXFORMAT, 3);
            b.cmd(CMD_TEXMODE, 5 << 16);  // max level 5
            for (int l = 0; l < 6; l++) {
                const int s = 32 >> l, lg = 5 - l;
                b.cmd(CMD_TEXADDR0 + l, lvAddr[l] & 0xFFFFFF);
                b.cmd(CMD_TEXBUFW0 + l, ((lvAddr[l] >> 8) & 0x0F0000) | s);
                b.cmd(CMD_TEXSIZE0 + l, (lg << 8) | lg);
            }
            b.cmd(CMD_TEXFILTER, 0x107);  // linear-mipmap-linear
            b.cmd(CMD_TEXLEVEL, 0);
            b.cmd(CMD_TEXFLUSH, 0);
            float proj[16], view[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            float world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1.9f, -1.0f, -3.0f};
            perspective(proj, 1.0f, 480.0f / 272.0f, 0.5f, 100.0f);
            b.matrix(CMD_PROJNUM, proj, 16);
            b.matrix(CMD_VIEWNUM, view, 12);
            b.matrix(CMD_WORLDNUM, world, 12);
            b.cmdf(CMD_TEXSCALEU, 8), b.cmdf(CMD_TEXSCALEV, 8), b.cmdf(CMD_TEXOFFU, 0), b.cmdf(CMD_TEXOFFV, 0);
            b.cmd(CMD_TEXMAPMODE, 0);
            b.cmd(CMD_TEXFUNC, 0x003);
            b.cmd(CMD_TEXWRAP, 0);
            u32 fv = b.alloc(4 * 20), a = fv;
            const float Q[4][3] = {{-1.2f, 0, 0.8f}, {1.2f, 0, 0.8f}, {-1.2f, 0, -20}, {1.2f, 0, -20}};
            for (int i = 0; i < 4; i++) {
                putf(a, (float)(i & 1)), putf(a, (float)(i >> 1));
                putf(a, Q[i][0]), putf(a, Q[i][1]), putf(a, Q[i][2]);
            }
            b.cmd(CMD_VTYPE, 3 | (3 << 7));
            b.addr(CMD_VADDR, fv);
            b.cmd(CMD_PRIM, (4 << 16) | 4);
        }
        runList(kScratch, b.pc);
        dumpTarget(dir, "scene4");
    }

    // restore memory and GE state for the game
    memcpy(memPtr(kScratch), savedRam.data(), kScratchSize);
    memcpy(memPtr(0x04100000), savedVram.data(), 0x100000);
    geInitLists();
    texClearAll();
}

}  // namespace vc2rt::gpu
