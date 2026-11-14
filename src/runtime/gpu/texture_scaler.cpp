// Edge-aware texture upscaler in the xBR family.
//
// For every source pixel and each of its four corners, the xBR level-2 rules decide whether an
// edge runs through that corner (and whether it is a 45 degree, shallow or steep edge) and which
// neighbour colour lies across it. Instead of fixed per-factor blend tables, the edge is turned
// into a line in pixel-local coordinates and every output sub-pixel is blended by its
// anti-aliased coverage, which works for any integer factor (2x..4x here).
#include <cmath>

#include "gpu_internal.h"

namespace vc2rt::gpu {

namespace {

struct YUVA {
    float y, u, v, a;
};

inline YUVA toYuv(u32 c) {
    const float r = (float)(c & 0xFF), g = (float)((c >> 8) & 0xFF), b = (float)((c >> 16) & 0xFF);
    return {0.299f * r + 0.587f * g + 0.114f * b, -0.169f * r - 0.331f * g + 0.5f * b, 0.5f * r - 0.419f * g - 0.081f * b,
            (float)(c >> 24)};
}

inline float dist(const YUVA& p, const YUVA& q) {
    return 48.0f * fabsf(p.y - q.y) + 7.0f * fabsf(p.u - q.u) + 6.0f * fabsf(p.v - q.v) + 32.0f * fabsf(p.a - q.a);
}

constexpr float kEqThreshold = 15.0f * 48.0f / 4.0f;
inline bool eq(const YUVA& p, const YUVA& q) { return dist(p, q) < kEqThreshold; }

inline u32 lerpColor(u32 a, u32 b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    u32 r = 0;
    for (int s = 0; s < 32; s += 8) {
        const float ca = (float)((a >> s) & 0xFF), cb = (float)((b >> s) & 0xFF);
        r |= (u32)lroundf(ca + (cb - ca) * t) << s;
    }
    return r;
}

enum EdgeKind : u8 { EDGE_NONE = 0, EDGE_45 = 1, EDGE_SHALLOW = 2, EDGE_STEEP = 4 };

}  // namespace

void upscaleTexture(const u32* src, int w, int h, int N, u32* dst) {
    if (N <= 1) {
        memcpy(dst, src, (size_t)w * h * 4);
        return;
    }
    std::vector<YUVA> yuv((size_t)w * h);
    for (size_t i = 0; i < yuv.size(); i++) yuv[i] = toYuv(src[i]);
    auto P = [&](int x, int y) -> int { return std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1); };
    // corner rotations: local axes (dx, dy) point from E towards the corner being examined
    static const int kDir[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
    const int W = w * N;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const u32 ec = src[P(x, y)];
            u32* blockOut = dst + (size_t)y * N * W + (size_t)x * N;
            for (int sy = 0; sy < N; sy++)
                for (int sx = 0; sx < N; sx++) blockOut[(size_t)sy * W + sx] = ec;
            for (int c = 0; c < 4; c++) {
                const int ax = kDir[c][0], ay = kDir[c][1];
                // neighbourhood in the rotated frame (names follow the xBR convention for the
                // bottom-right corner): F = +x, H = +y, I = diagonal, D = -x, B = -y, ...
                auto at = [&](int lx, int ly) -> int { return P(x + lx * ax, y + ly * ay); };
                const YUVA& E = yuv[at(0, 0)];
                const YUVA& F = yuv[at(1, 0)];
                const YUVA& H = yuv[at(0, 1)];
                const YUVA& I = yuv[at(1, 1)];
                const YUVA& D = yuv[at(-1, 0)];
                const YUVA& B = yuv[at(0, -1)];
                const YUVA& C = yuv[at(1, -1)];
                const YUVA& G = yuv[at(-1, 1)];
                const YUVA& F4 = yuv[at(2, 0)];
                const YUVA& I4 = yuv[at(2, 1)];
                const YUVA& H5 = yuv[at(0, 2)];
                const YUVA& I5 = yuv[at(1, 2)];
                if (eq(E, F) || eq(E, H)) continue;
                const float wd1 = dist(E, C) + dist(E, G) + dist(I, F4) + dist(I, H5) + 4.0f * dist(H, F);
                const float wd2 = dist(H, D) + dist(H, I5) + dist(F, I4) + dist(F, B) + 4.0f * dist(E, I);
                if (!(wd1 < wd2)) continue;
                const bool cond = (!eq(F, B) && !eq(H, D)) || (eq(E, I) && !eq(F, I4) && !eq(H, I5)) || eq(E, G) || eq(E, C);
                if (!cond) continue;
                u8 kind = EDGE_45;
                const float dFG = dist(F, G), dHC = dist(H, C);
                if (dFG * 2.0f <= dHC && !eq(E, G) && !eq(D, G)) kind |= EDGE_SHALLOW;
                if (dHC * 2.0f <= dFG && !eq(E, C) && !eq(B, C)) kind |= EDGE_STEEP;
                const u32 X = dist(E, F) <= dist(E, H) ? src[at(1, 0)] : src[at(0, 1)];
                for (int sy = 0; sy < N; sy++)
                    for (int sx = 0; sx < N; sx++) {
                        // sub-pixel centre in the rotated local frame, origin at the far side
                        float px = (sx + 0.5f) / N, py = (sy + 0.5f) / N;
                        if (ax < 0) px = 1.0f - px;
                        if (ay < 0) py = 1.0f - py;
                        float d = (px + py - 1.5f) * 0.70710678f;  // 45 degree line
                        if (kind & EDGE_SHALLOW) d = std::max(d, (px * 0.5f + py - 1.0f) * 0.89442719f);
                        if (kind & EDGE_STEEP) d = std::max(d, (px + py * 0.5f - 1.0f) * 0.89442719f);
                        const float cov = std::clamp(0.5f + d * (float)N, 0.0f, 1.0f);
                        if (cov <= 0.0f) continue;
                        u32& o = blockOut[(size_t)sy * W + sx];
                        o = lerpColor(o, X, cov);
                    }
            }
        }
}

}  // namespace vc2rt::gpu
