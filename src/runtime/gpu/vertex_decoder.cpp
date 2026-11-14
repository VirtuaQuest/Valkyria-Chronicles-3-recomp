// Vertex decoding. Layout: [weights][texcoord][colour][normal][position], each component aligned
// to its own element size, the vertex padded to its largest alignment, repeated per morph target.
#include "vertex_decoder.h"

#include <algorithm>

namespace vc2rt::gpu {

namespace {
const u8 kTcSize[4] = {0, 2, 4, 8}, kTcAlign[4] = {0, 1, 2, 4};
const u8 kColSize[8] = {0, 0, 0, 0, 2, 2, 2, 4}, kColAlign[8] = {0, 0, 0, 0, 2, 2, 2, 4};
const u8 kNrmSize[4] = {0, 3, 6, 12}, kNrmAlign[4] = {0, 1, 2, 4};
const u8 kPosSize[4] = {3, 3, 6, 12}, kPosAlign[4] = {1, 1, 2, 4};
const u8 kWtSize[4] = {0, 1, 2, 4};

inline int alignUp(int v, int a) { return a ? (v + a - 1) & ~(a - 1) : v; }

inline void decodeColor(int fmt, const u8* p, float* c) {
    switch (fmt) {
        case 4: {  // BGR565 (r in low bits)
            u16 v = (u16)(p[0] | (p[1] << 8));
            c[0] = (v & 31) / 31.0f;
            c[1] = ((v >> 5) & 63) / 63.0f;
            c[2] = ((v >> 11) & 31) / 31.0f;
            c[3] = 1.0f;
            break;
        }
        case 5: {  // ABGR1555
            u16 v = (u16)(p[0] | (p[1] << 8));
            c[0] = (v & 31) / 31.0f;
            c[1] = ((v >> 5) & 31) / 31.0f;
            c[2] = ((v >> 10) & 31) / 31.0f;
            c[3] = (v >> 15) ? 1.0f : 0.0f;
            break;
        }
        case 6: {  // ABGR4444
            u16 v = (u16)(p[0] | (p[1] << 8));
            c[0] = (v & 15) / 15.0f;
            c[1] = ((v >> 4) & 15) / 15.0f;
            c[2] = ((v >> 8) & 15) / 15.0f;
            c[3] = ((v >> 12) & 15) / 15.0f;
            break;
        }
        case 7:
            c[0] = p[0] / 255.0f;
            c[1] = p[1] / 255.0f;
            c[2] = p[2] / 255.0f;
            c[3] = p[3] / 255.0f;
            break;
        default: c[0] = c[1] = c[2] = c[3] = 1.0f; break;
    }
}

inline float rdf(const u8* p) {
    float f;
    memcpy(&f, p, 4);
    return f;
}
inline s16 rds16(const u8* p) { return (s16)(p[0] | (p[1] << 8)); }
inline u16 rdu16(const u8* p) { return (u16)(p[0] | (p[1] << 8)); }

// One morph target, unweighted.
void decodeOne(const VertexLayout& L, const u8* p, DecodedVertex& v) {
    for (int i = 0; i < L.wtCount; i++) {
        const u8* w = p + L.wtOff + i * kWtSize[L.wtFmt];
        switch (L.wtFmt) {
            case 1: v.wt[i] = w[0] / 128.0f; break;
            case 2: v.wt[i] = rdu16(w) / 32768.0f; break;
            case 3: v.wt[i] = rdf(w); break;
        }
    }
    if (L.tcFmt) {
        const u8* t = p + L.tcOff;
        switch (L.tcFmt) {
            case 1:
                v.uv[0] = t[0] / 128.0f;
                v.uv[1] = t[1] / 128.0f;
                break;
            case 2:
                if (L.through) {
                    v.uv[0] = rdu16(t);
                    v.uv[1] = rdu16(t + 2);
                } else {
                    v.uv[0] = rdu16(t) / 32768.0f;
                    v.uv[1] = rdu16(t + 2) / 32768.0f;
                }
                break;
            case 3:
                v.uv[0] = rdf(t);
                v.uv[1] = rdf(t + 4);
                break;
        }
    }
    if (L.colFmt) decodeColor(L.colFmt, p + L.colOff, v.col);
    if (L.nrmFmt) {
        const u8* n = p + L.nrmOff;
        for (int i = 0; i < 3; i++) {
            switch (L.nrmFmt) {
                case 1: v.nrm[i] = (s8)n[i] / 128.0f; break;
                case 2: v.nrm[i] = rds16(n + i * 2) / 32768.0f; break;
                case 3: v.nrm[i] = rdf(n + i * 4); break;
            }
        }
    }
    const u8* q = p + L.posOff;
    if (L.through) {
        switch (L.posFmt) {
            case 2:
                v.pos[0] = rds16(q);
                v.pos[1] = rds16(q + 2);
                v.pos[2] = rdu16(q + 4);
                break;
            case 3: {
                v.pos[0] = rdf(q);
                v.pos[1] = rdf(q + 4);
                float z = rdf(q + 8);
                v.pos[2] = z >= 65535.0f ? 65535.0f : (z > 0.0f ? (float)(int)z : 0.0f);
                break;
            }
            default: v.pos[0] = v.pos[1] = v.pos[2] = 0; break;
        }
    } else {
        for (int i = 0; i < 3; i++) {
            switch (L.posFmt) {
                case 1: v.pos[i] = (s8)q[i] / 128.0f; break;
                case 2: v.pos[i] = rds16(q + i * 2) / 32768.0f; break;
                case 3: v.pos[i] = rdf(q + i * 4); break;
                default: v.pos[i] = 0; break;
            }
        }
    }
}
}  // namespace

void computeLayout(u32 vtype, VertexLayout& L) {
    L = VertexLayout{};
    L.vtype = vtype;
    L.through = (vtype & VT_THROUGH) != 0;
    L.tcFmt = vtype & 3;
    L.colFmt = (vtype >> 2) & 7;
    L.nrmFmt = (vtype >> 5) & 3;
    L.posFmt = (vtype >> 7) & 3;
    L.wtFmt = (vtype >> 9) & 3;
    L.wtCount = L.wtFmt ? (int)((vtype >> 14) & 7) + 1 : 0;
    L.morphCount = (int)((vtype >> 18) & 7) + 1;
    int size = 0, biggest = 0;
    if (L.wtFmt) {
        size = alignUp(size, kWtSize[L.wtFmt]);
        L.wtOff = size;
        size += kWtSize[L.wtFmt] * L.wtCount;
        biggest = std::max<int>(biggest, kWtSize[L.wtFmt]);
    }
    if (L.tcFmt) {
        size = alignUp(size, kTcAlign[L.tcFmt]);
        L.tcOff = size;
        size += kTcSize[L.tcFmt];
        biggest = std::max<int>(biggest, kTcAlign[L.tcFmt]);
    }
    if (L.colFmt >= 4) {
        size = alignUp(size, kColAlign[L.colFmt]);
        L.colOff = size;
        size += kColSize[L.colFmt];
        biggest = std::max<int>(biggest, kColAlign[L.colFmt]);
    } else {
        L.colFmt = 0;
    }
    if (L.nrmFmt) {
        size = alignUp(size, kNrmAlign[L.nrmFmt]);
        L.nrmOff = size;
        size += kNrmSize[L.nrmFmt];
        biggest = std::max<int>(biggest, kNrmAlign[L.nrmFmt]);
    }
    size = alignUp(size, kPosAlign[L.posFmt]);
    L.posOff = size;
    size += kPosSize[L.posFmt];
    biggest = std::max<int>(biggest, kPosAlign[L.posFmt]);
    size = alignUp(size, biggest);
    L.oneSize = size;
    L.stride = size * L.morphCount;
}

void decodeVertex(const VertexLayout& L, const u8* p, DecodedVertex& out) {
    out.uv[0] = out.uv[1] = 0;
    out.nrm[0] = out.nrm[1] = 0;
    out.nrm[2] = 0;
    out.col[0] = out.col[1] = out.col[2] = out.col[3] = 1.0f;
    if (L.morphCount == 1) {
        decodeOne(L, p, out);
        return;
    }
    DecodedVertex acc{};
    for (int m = 0; m < L.morphCount; m++) {
        DecodedVertex t;
        t.uv[0] = t.uv[1] = 0;
        t.nrm[0] = t.nrm[1] = t.nrm[2] = 0;
        t.col[0] = t.col[1] = t.col[2] = t.col[3] = 0;
        for (int i = 0; i < 8; i++) t.wt[i] = 0;
        decodeOne(L, p + m * L.oneSize, t);
        const float w = g_ge.morph[m];
        for (int i = 0; i < 3; i++) acc.pos[i] += t.pos[i] * w, acc.nrm[i] += t.nrm[i] * w;
        for (int i = 0; i < 2; i++) acc.uv[i] += t.uv[i] * w;
        for (int i = 0; i < 4; i++) acc.col[i] += t.col[i] * w;
        for (int i = 0; i < L.wtCount; i++) acc.wt[i] += t.wt[i] * w;
    }
    for (int i = 0; i < 4; i++) acc.col[i] = std::clamp(acc.col[i], 0.0f, 1.0f);
    if (!L.colFmt) acc.col[0] = acc.col[1] = acc.col[2] = acc.col[3] = 1.0f;
    out = acc;
}

}  // namespace vc2rt::gpu
