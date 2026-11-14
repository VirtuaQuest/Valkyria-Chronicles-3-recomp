// Texture cache: decodes every GE texture format (16/32-bit, CLUT4/8/16/32 with shift/mask/
// offset, swizzled layouts, DXT1/3/5, mipmap chains) to RGBA8, caches GL textures keyed by
// address + format + palette, detects content changes by hashing, optionally upscales, and maps
// textures that live inside GPU-rendered framebuffers onto the render target surfaces.
#include <unordered_map>

#include "../host/config.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

// ------------------------------------------------------------------ helpers
u64 hashMem(const void* data, size_t n) {
    const u8* p = (const u8*)data;
    const u64 P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
    u64 a = P1 ^ n, b = P2, c = P3, d = P1 + P2;
    auto rot = [](u64 v, int r) { return (v << r) | (v >> (64 - r)); };
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        u64 w[4];
        memcpy(w, p + i, 32);
        a = rot(a + w[0] * P2, 31) * P1;
        b = rot(b + w[1] * P2, 31) * P1;
        c = rot(c + w[2] * P2, 31) * P1;
        d = rot(d + w[3] * P2, 31) * P1;
    }
    u64 h = rot(a, 1) + rot(b, 7) + rot(c, 12) + rot(d, 18);
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        h = rot(h ^ (w * P2), 27) * P1 + P3;
    }
    for (; i < n; i++) h = rot(h ^ (p[i] * P3), 11) * P1;
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    return h;
}

u32 validBytes(u32 addr, u32 want) {
    const u32 a = addr & 0x1FFFFFFF;
    u32 end = 0;
    if (a >= kRamBase && a < kRamBase + kRamSize) end = kRamBase + kRamSize;
    else if (a >= kVramBase && a < kVramBase + 0x00800000) end = kVramBase + 0x00800000;
    else if (a >= kScratchpadBase && a < kScratchpadBase + 0x4000) end = kScratchpadBase + 0x4000;
    else return 0;
    return std::min(want, end - a);
}

namespace {

struct TexEntry {
    u32 addr = 0, fmt = 0, w = 0, h = 0, bufw = 0;
    int levels = 1, upscale = 1;
    u64 dataHash = 0, clutHash = 0;
    u32 hashGen = 0;
    u64 lastFrame = 0;
    u32 bytes = 0;  // bytes of texel data covered (all levels)
    GLuint tex = 0;
    u32 changes = 0;
    u64 lastChange = 0;
    bool dynamic = false;
};

std::unordered_map<u64, TexEntry> g_cache;
std::vector<u8> g_unswz;
std::vector<u32> g_rgba, g_scaled, g_pal;

inline u32 c5650(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 63, b = (c >> 11) & 31;
    return rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}
inline u32 c5551(u16 c) {
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), (c >> 15) ? 255 : 0);
}
inline u32 c4444(u16 c) { return rgba((c & 15) * 17, ((c >> 4) & 15) * 17, ((c >> 8) & 15) * 17, ((c >> 12) & 15) * 17); }

int bitsPerTexel(u32 fmt) {
    switch (fmt) {
        case 0: case 1: case 2: case 6: return 16;
        case 3: case 7: return 32;
        case 4: return 4;
        case 5: return 8;
        case 8: return 4;   // DXT1: 8 bytes per 4x4
        case 9: case 10: return 8;  // DXT3/5: 16 bytes per 4x4
        default: return 16;
    }
}

// The GE fetches texture rows in 16-byte units: the buffer width is aligned down to a 16-byte
// multiple (DXT excepted), and anything narrower than 16 bytes reads as 16 bytes.
u32 alignedBufw(u32 fmt, u32 bufw) {
    if (fmt >= 8) return bufw ? bufw : 128 / bitsPerTexel(fmt);
    const u32 unit = 128 / bitsPerTexel(fmt);
    bufw &= 0x7FF & ~(unit - 1);
    return bufw ? bufw : unit;
}

u32 levelBytes(u32 fmt, u32 bufw, int h) {
    if (fmt >= 8) {
        const u32 blockBytes = fmt == 8 ? 8 : 16;
        return ((bufw + 3) / 4) * ((h + 3) / 4) * blockBytes;
    }
    return bufw * h * bitsPerTexel(fmt) / 8;
}

// Builds the RGBA palette for the current CLUT registers.
void buildPalette(int entries) {
    const GeState& ge = g_ge;
    const u32 cf = ge.clutFormat();
    g_pal.resize(entries);
    const u32 shift = ge.clutShift(), mask = ge.clutMask();
    const u32 start = ge.clutStart() & (cf == 3 ? 0xFF : 0x1FF);
    for (int i = 0; i < entries; i++) {
        u32 idx = ((i >> shift) & mask) | start;
        if (cf == 3) {
            idx &= 0xFF;
            u32 v;
            memcpy(&v, ge.clut + idx * 4, 4);
            g_pal[i] = v;
        } else {
            idx &= 0x1FF;
            u16 v;
            memcpy(&v, ge.clut + idx * 2, 2);
            g_pal[i] = cf == 0 ? c5650(v) : (cf == 1 ? c5551(v) : c4444(v));
        }
    }
}

const u8* unswizzle(const u8* src, u32 rowBytes, int h) {
    const int hh = (h + 7) & ~7;
    g_unswz.resize((size_t)rowBytes * hh);
    const u32 bxCount = rowBytes / 16;
    for (int by = 0; by < hh / 8; by++)
        for (u32 bx = 0; bx < bxCount; bx++) {
            const u8* blk = src + ((size_t)by * bxCount + bx) * 128;
            for (int j = 0; j < 8; j++) memcpy(&g_unswz[((size_t)by * 8 + j) * rowBytes + bx * 16], blk + j * 16, 16);
        }
    return g_unswz.data();
}

void decodeDXT(u32 fmt, const u8* src, u32 bufw, int w, int h, u32* out) {
    const u32 blockBytes = fmt == 8 ? 8 : 16;
    const u32 bpr = (bufw + 3) / 4;
    for (int by = 0; by < (h + 3) / 4; by++)
        for (int bx = 0; bx < (w + 3) / 4; bx++) {
            const u8* b = src + ((size_t)by * bpr + bx) * blockBytes;
            // PSP block layout: colour block = 4 bytes of 2-bit indices (one per row), then two RGB565
            u32 lines;
            memcpy(&lines, b, 4);
            u16 c1 = (u16)(b[4] | (b[5] << 8)), c2 = (u16)(b[6] | (b[7] << 8));
            auto expand = [](u16 c, int& r, int& g, int& bl) {
                bl = (c << 3) & 0xF8;
                g = (c >> 3) & 0xFC;
                r = (c >> 8) & 0xF8;
            };
            int r1, g1, b1, r2, g2, b2;
            expand(c1, r1, g1, b1);
            expand(c2, r2, g2, b2);
            const int alpha = fmt == 8 ? 255 : 0;
            u32 cols[4];
            cols[0] = rgba(r1, g1, b1, alpha);
            cols[1] = rgba(r2, g2, b2, alpha);
            if (c1 > c2 || fmt != 8) {
                cols[2] = rgba((2 * r1 + r2) / 3, (2 * g1 + g2) / 3, (2 * b1 + b2) / 3, alpha);
                cols[3] = rgba((r1 + 2 * r2) / 3, (g1 + 2 * g2) / 3, (b1 + 2 * b2) / 3, alpha);
            } else {
                cols[2] = rgba((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2, alpha);
                cols[3] = 0;
            }
            u8 a5[8] = {};
            u64 a5bits = 0;
            if (fmt == 10) {
                const u8 al1 = b[14], al2 = b[15];
                a5[0] = al1, a5[1] = al2;
                if (al1 > al2) {
                    for (int n = 1; n <= 6; n++) a5[n + 1] = (u8)(((al1 * ((7 - n) << 8)) / 7 + (al2 * (n << 8)) / 7 + 31) >> 8);
                } else {
                    for (int n = 1; n <= 4; n++) a5[n + 1] = (u8)(((al1 * ((5 - n) << 8)) / 5 + (al2 * (n << 8)) / 5 + 31) >> 8);
                    a5[6] = 0, a5[7] = 255;
                }
                u32 lo;
                u16 hi;
                memcpy(&lo, b + 8, 4);
                memcpy(&hi, b + 12, 2);
                a5bits = ((u64)hi << 32) | lo;
            }
            for (int y = 0; y < 4; y++) {
                const int py = by * 4 + y;
                if (py >= h) break;
                u32 cl = (lines >> (8 * y)) & 0xFF;
                u16 a3 = 0;
                if (fmt == 9) memcpy(&a3, b + 8 + y * 2, 2);
                for (int x = 0; x < 4; x++) {
                    const int px = bx * 4 + x;
                    u32 c = cols[(cl >> (2 * x)) & 3];
                    if (fmt == 9) c |= (u32)((a3 >> (4 * x)) & 0xF) << 28;
                    else if (fmt == 10) c |= (u32)a5[(a5bits >> (12 * y + 3 * x)) & 7] << 24;
                    if (px < w) out[(size_t)py * w + px] = c;
                }
            }
        }
}

// Decodes one mip level to RGBA8 (w x h).
void decodeLevel(u32 fmt, u32 addr, u32 bufw, int w, int h, bool swizzled, u32* out) {
    const u32 need = levelBytes(fmt, bufw, h);
    const u32 have = validBytes(addr, need);
    if (have < need) {
        // partially outside memory: decode what exists, rest black
        std::fill(out, out + (size_t)w * h, 0);
        if (have == 0) return;
    }
    const u8* src = memPtr(addr);
    std::vector<u8> padded;
    if (have < need) {
        padded.assign(need, 0);
        memcpy(padded.data(), src, have);
        src = padded.data();
    }
    if (fmt >= 8) {
        decodeDXT(fmt, src, bufw, w, h, out);
        return;
    }
    const int bits = bitsPerTexel(fmt);
    const u32 rowBytes = bufw * bits / 8;
    if (swizzled && rowBytes >= 16) src = unswizzle(src, rowBytes, h);
    switch (fmt) {
        case 0:
        case 1:
        case 2:
            for (int y = 0; y < h; y++) {
                const u16* s = (const u16*)(src + (size_t)y * rowBytes);
                u32* o = out + (size_t)y * w;
                if (fmt == 0) for (int x = 0; x < w; x++) o[x] = c5650(s[x]);
                else if (fmt == 1) for (int x = 0; x < w; x++) o[x] = c5551(s[x]);
                else for (int x = 0; x < w; x++) o[x] = c4444(s[x]);
            }
            break;
        case 3:
            for (int y = 0; y < h; y++) memcpy(out + (size_t)y * w, src + (size_t)y * rowBytes, (size_t)w * 4);
            break;
        case 4: {
            buildPalette(16);
            for (int y = 0; y < h; y++) {
                const u8* s = src + (size_t)y * rowBytes;
                u32* o = out + (size_t)y * w;
                for (int x = 0; x < w; x++) o[x] = g_pal[(s[x >> 1] >> ((x & 1) * 4)) & 15];
            }
            break;
        }
        case 5: {
            buildPalette(256);
            for (int y = 0; y < h; y++) {
                const u8* s = src + (size_t)y * rowBytes;
                u32* o = out + (size_t)y * w;
                for (int x = 0; x < w; x++) o[x] = g_pal[s[x]];
            }
            break;
        }
        case 6:
        case 7: {
            // 16/32-bit indices: the transform (shift/mask/start) is applied per texel
            const GeState& ge = g_ge;
            const u32 cf = ge.clutFormat(), shift = ge.clutShift(), mask = ge.clutMask();
            const u32 start = ge.clutStart() & (cf == 3 ? 0xFF : 0x1FF);
            for (int y = 0; y < h; y++) {
                u32* o = out + (size_t)y * w;
                for (int x = 0; x < w; x++) {
                    u32 idx;
                    if (fmt == 6) idx = ((const u16*)(src + (size_t)y * rowBytes))[x];
                    else memcpy(&idx, src + (size_t)y * rowBytes + x * 4, 4);
                    idx = ((idx >> shift) & mask) | start;
                    if (cf == 3) {
                        u32 v;
                        memcpy(&v, ge.clut + (idx & 0xFF) * 4, 4);
                        o[x] = v;
                    } else {
                        u16 v;
                        memcpy(&v, ge.clut + (idx & 0x1FF) * 2, 2);
                        o[x] = cf == 0 ? c5650(v) : (cf == 1 ? c5551(v) : c4444(v));
                    }
                }
            }
            break;
        }
    }
}

u64 entryKey(u32 addr, u32 fmt, u32 sizeReg, u32 bufw, bool swz, int levels, u64 clutKey) {
    u64 k = ((u64)addr << 32) ^ ((u64)fmt << 28) ^ ((u64)sizeReg << 12) ^ (u64)bufw ^ ((u64)swz << 27) ^ ((u64)levels << 24);
    return k ^ (clutKey * 0x9E3779B97F4A7C15ull);
}

}  // namespace

void texInit() {}

void texClearAll() {
    for (auto& [k, e] : g_cache)
        if (e.tex) glDeleteTextures(1, &e.tex);
    g_cache.clear();
}

void texInvalidate(u32 addr, u32 size) {
    const u32 a0 = addr & 0x0FFFFFFF, a1 = a0 + size;
    for (auto& [k, e] : g_cache) {
        const u32 b0 = e.addr & 0x0FFFFFFF, b1 = b0 + e.bytes;
        if (b0 < a1 && a0 < b1) e.hashGen = 0;
    }
}

void texNewFrame() {
    if ((g_frame & 63) != 0) return;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->second.lastFrame + 900 < g_frame) {
            if (it->second.tex) glDeleteTextures(1, &it->second.tex);
            it = g_cache.erase(it);
        } else {
            ++it;
        }
    }
}

bool texLookup(TexBinding& out) {
    const GeState& ge = g_ge;
    const u32 addr = ge.texAddr(0) & 0x0FFFFFFF;
    const u32 fmt = ge.texFormat();
    if (fmt > 10) return false;
    const int w = ge.texW(0), h = ge.texH(0);
    const u32 bufw = alignedBufw(fmt, ge.texBufw(0));
    out.w = w, out.h = h;

    // render-to-texture
    if (vramAddr(addr)) {
        int ox = 0, oy = 0;
        RenderTarget* rt = fbFindForTexture(addr, bufw, fmt, w, h, ox, oy);
        if (rt && rt->gpuDrawn) {
            if (fmt <= 3) {
                out.tex = fbSampleTexture(rt);
                out.rtt = true;
                out.rttAlphaOne = rt->format == 0;
                out.rtScale[0] = (float)w / rt->width;
                out.rtScale[1] = (float)h / rt->height;
                out.rtScale[2] = (float)ox / rt->width;
                out.rtScale[3] = (float)oy / rt->height;
                out.levels = 1;
                g_stats.textures++;
                return true;
            }
            // palette lookups into a rendered buffer: let the CPU decoder see the pixels
            fbDownload(rt, 0, 0, rt->width, rt->height);
        }
    }

    const bool swz = ge.texSwizzled();
    int levels = 1;
    const u32 minf = ge.r(CMD_TEXFILTER) & 7;
    if (minf >= 4 && ge.texMaxLevel() > 0) {
        const int maxL = std::min(ge.texMaxLevel(), 7);
        for (int l = 1; l <= maxL; l++) {
            if (ge.texW(l) != std::max(1, w >> l) || ge.texH(l) != std::max(1, h >> l)) break;
            if (!(ge.texAddr(l) & 0x0FFFFFFF)) break;
            levels = l + 1;
        }
    }
    const bool clut = fmt >= 4 && fmt <= 7;
    const u64 clutKey = clut ? (g_ge.clutHash ^ ((u64)(ge.r(CMD_CLUTFORMAT)) << 40)) : 0;
    const u64 key = entryKey(addr, fmt, ge.r(CMD_TEXSIZE0), bufw, swz, levels, clutKey);
    TexEntry& e = g_cache[key];
    const int upscale = std::clamp(config().textureUpscale, 1, 4);
    bool rebuild = e.tex == 0 || e.upscale != upscale;
    if (!rebuild && e.hashGen != g_hashGen) {
        u64 hsh = 0;
        for (int l = 0; l < levels; l++) {
            const u32 la = ge.texAddr(l) & 0x0FFFFFFF;
            const u32 lb = alignedBufw(fmt, ge.texBufw(l));
            const u32 n = levelBytes(fmt, lb, std::max(1, h >> l));
            hsh = hsh * 31 + hashMem(memPtr(la), validBytes(la, n));
        }
        e.hashGen = g_hashGen;
        if (hsh != e.dataHash) {
            rebuild = true;
            e.dataHash = hsh;
        }
    }
    e.lastFrame = g_frame;
    if (rebuild) {
        flushDraws();  // the GL texture may be referenced by the pending batch
        if (!e.tex) glGenTextures(1, &e.tex);
        e.addr = addr, e.fmt = fmt, e.w = w, e.h = h, e.bufw = bufw, e.levels = levels, e.upscale = upscale;
        u64 hsh = 0;
        e.bytes = 0;
        glBindTexture(GL_TEXTURE_2D, e.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        // textures rewritten every few frames (videos, scrolling text) skip the costly upscaler
        if (e.dataHash != 0) {
            e.changes = (g_frame - e.lastChange < 120) ? e.changes + 1 : 1;
            e.lastChange = g_frame;
            if (e.changes >= 3) e.dynamic = true;
        }
        const bool doScale = upscale > 1 && !e.dynamic && w * h <= 512 * 512;
        static const char* dumpDir = getenv("VC2_DUMP_TEX");
        for (int l = 0; l < levels; l++) {
            const int lw = std::max(1, w >> l), lh = std::max(1, h >> l);
            const u32 la = ge.texAddr(l) & 0x0FFFFFFF;
            const u32 lb = alignedBufw(fmt, ge.texBufw(l));
            const u32 n = levelBytes(fmt, lb, lh);
            hsh = hsh * 31 + hashMem(memPtr(la), validBytes(la, n));
            if (l == 0) e.bytes = n;
            g_rgba.resize((size_t)lw * lh);
            decodeLevel(fmt, la, lb, lw, lh, swz, g_rgba.data());
            if (dumpDir && l == 0) {
                char path[1024];
                snprintf(path, sizeof path, "%s\\tex_%08x_f%u_%dx%d_%llu.png", dumpDir, addr, fmt, lw, lh, (unsigned long long)g_frame);
                savePng(path, g_rgba.data(), lw, lh, false, true);
            }
            if (doScale) {
                // every PSP mip level is upscaled by the same factor, so the chain still halves
                g_scaled.resize((size_t)lw * lh * upscale * upscale);
                upscaleTexture(g_rgba.data(), lw, lh, upscale, g_scaled.data());
                if (dumpDir) {
                    char path[1024];
                    snprintf(path, sizeof path, "%s\\tex_%08x_f%u_%dx%d_%llu_x%d.png", dumpDir, addr, fmt, lw, lh, (unsigned long long)g_frame, upscale);
                    savePng(path, g_scaled.data(), lw * upscale, lh * upscale, false, true);
                }
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw * upscale, lh * upscale, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_scaled.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba.data());
            }
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
        e.dataHash = hsh;
        e.hashGen = g_hashGen;
        e.clutHash = clutKey;
        g_stats.texUploads++;
        invalidateGLStateCache();
    }
    out.tex = e.tex;
    out.levels = levels;
    g_stats.textures++;
    return true;
}

}  // namespace vc2rt::gpu
