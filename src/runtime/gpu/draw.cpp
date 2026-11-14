// Primitive submission: vertex decode, software transform & lighting (exactly per vertex like the
// GE does), primitive assembly (strips/fans/sprites/lines/points), culling, near-plane clipping,
// and translation of the GE raster state into a DrawState for the GL backend.
#include <cmath>

#include "../host/config.h"
#include "gpu_internal.h"
#include "vertex_decoder.h"

namespace vc2rt::gpu {

namespace {

struct TVert {
    float clip[4];   // transform: PSP clip coords. through: screen x,y, depth z, w = 1
    float uv[3];
    float c0[4];
    float c1[3];
    float fog;
    bool outside;
};

// ---------------------------------------------------------------- per-draw transform setup
struct XformState {
    bool through, lighting, fog, skin, texGen;
    float wvp[16];         // proj * view * world, column-major
    float wv[16];          // view * world
    float viewDir[3];
    int uvMode, projMode;
    float uvScale[2], uvOff[2];
    float sx, sy, sz, cx, cy, cz, offx, offy;
    bool depthClip;
    float fogEnd, fogSlope;
    // lighting
    struct Light {
        bool on, directional, spot, specular, powDiffuse;
        float pos[3], dir[3], att[3], cutoff, conv;
        float amb[3], dif[3], spe[3];
    } lights[4];
    float matEmissive[3], matAmbient[4], matDiffuse[3], matSpecular[3], specCoef;
    float ambient[4];
    int matUpdate;
    bool sepSpecular;
    bool reverseNormals;
};

void m43to44(const float* m, float* o) {
    o[0] = m[0], o[1] = m[1], o[2] = m[2], o[3] = 0;
    o[4] = m[3], o[5] = m[4], o[6] = m[5], o[7] = 0;
    o[8] = m[6], o[9] = m[7], o[10] = m[8], o[11] = 0;
    o[12] = m[9], o[13] = m[10], o[14] = m[11], o[15] = 1;
}
void mul44(const float* a, const float* b, float* o) {  // o = a * b (column-major)
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    memcpy(o, r, sizeof r);
}
inline void rgbOf(u32 c, float* o) {
    o[0] = (c & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = ((c >> 16) & 0xFF) / 255.0f;
}
inline void norm3(float* v) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 0) {
        l = 1.0f / sqrtf(l);
        v[0] *= l, v[1] *= l, v[2] *= l;
    }
}
inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float pspPow(float x, float e) { return e <= 0.0f ? 1.0f : (x > 0.0f ? powf(x, e) : 0.0f); }

void setupXform(XformState& X, const VertexLayout& L) {
    const GeState& ge = g_ge;
    X.through = L.through;
    X.skin = !L.through && L.wtFmt != 0;
    X.lighting = !L.through && (ge.r(CMD_LIGHTING) & 1);
    X.fog = !L.through && (ge.r(CMD_FOGENABLE) & 1);
    X.uvMode = ge.r(CMD_TEXMAPMODE) & 3;
    if (X.uvMode == 3) X.uvMode = 0;
    X.projMode = (ge.r(CMD_TEXMAPMODE) >> 8) & 3;
    X.uvScale[0] = f24(ge.r(CMD_TEXSCALEU));
    X.uvScale[1] = f24(ge.r(CMD_TEXSCALEV));
    X.uvOff[0] = f24(ge.r(CMD_TEXOFFU));
    X.uvOff[1] = f24(ge.r(CMD_TEXOFFV));
    X.depthClip = ge.r(CMD_DEPTHCLIP) & 1;
    X.reverseNormals = ge.r(CMD_REVNORMAL) & 1;
    if (L.through) return;
    float w[16], v[16];
    m43to44(ge.world, w);
    m43to44(ge.view, v);
    mul44(v, w, X.wv);
    mul44(ge.proj, X.wv, X.wvp);
    X.viewDir[0] = ge.view[2], X.viewDir[1] = ge.view[5], X.viewDir[2] = ge.view[8];
    if (X.viewDir[0] == 0 && X.viewDir[1] == 0 && X.viewDir[2] == 0) X.viewDir[2] = 1;
    norm3(X.viewDir);
    X.sx = ge.vpSX(), X.sy = ge.vpSY(), X.sz = ge.vpSZ();
    X.cx = ge.vpCX(), X.cy = ge.vpCY(), X.cz = ge.vpCZ();
    X.offx = ge.offX(), X.offy = ge.offY();
    X.fogEnd = f24(ge.r(CMD_FOG1));
    X.fogSlope = f24(ge.r(CMD_FOG2));
    if (!std::isfinite(X.fogEnd)) X.fogEnd = std::signbit(X.fogEnd) ? -1e30f : 1e30f;
    if (!std::isfinite(X.fogSlope)) X.fogSlope = std::signbit(X.fogSlope) ? -262144.0f : 262144.0f;

    const bool needLight = X.lighting || X.uvMode == 2;
    if (needLight) {
        for (int i = 0; i < 4; i++) {
            auto& l = X.lights[i];
            l.on = X.lighting && (ge.r(CMD_LIGHT0 + i) & 1);
            u32 lt = ge.r(CMD_LTYPE0 + i);
            int comp = lt & 3, type = (lt >> 8) & 3;
            l.directional = type == 0;
            l.spot = type >= 2;
            l.specular = comp == 1;
            l.powDiffuse = comp == 2;
            for (int k = 0; k < 3; k++) {
                l.pos[k] = f24(ge.r(CMD_LPOS0 + i * 3 + k));
                l.dir[k] = f24(ge.r(CMD_LDIR0 + i * 3 + k));
                l.att[k] = f24(ge.r(CMD_LATT0 + i * 3 + k));
            }
            if (l.directional) norm3(l.pos);
            norm3(l.dir);
            l.conv = f24(ge.r(CMD_LCONV0 + i));
            l.cutoff = f24(ge.r(CMD_LCUTOFF0 + i));
            rgbOf(ge.r(CMD_LCOLOR0 + i * 3 + 0), l.amb);
            rgbOf(ge.r(CMD_LCOLOR0 + i * 3 + 1), l.dif);
            rgbOf(ge.r(CMD_LCOLOR0 + i * 3 + 2), l.spe);
        }
        rgbOf(ge.r(CMD_MATEMISSIVE), X.matEmissive);
        rgbOf(ge.r(CMD_MATAMBIENT), X.matAmbient);
        X.matAmbient[3] = (ge.r(CMD_MATALPHA) & 0xFF) / 255.0f;
        rgbOf(ge.r(CMD_MATDIFFUSE), X.matDiffuse);
        rgbOf(ge.r(CMD_MATSPECULAR), X.matSpecular);
        X.specCoef = f24(ge.r(CMD_MATSPECCOEF));
        rgbOf(ge.r(CMD_AMBIENTCOLOR), X.ambient);
        X.ambient[3] = (ge.r(CMD_AMBIENTALPHA) & 0xFF) / 255.0f;
        X.matUpdate = L.colFmt ? (ge.r(CMD_MATUPDATE) & 7) : 0;
        X.sepSpecular = ge.r(CMD_LIGHTMODE) & 1;
    }
}

void lightVertex(const XformState& X, const float* wpos, const float* n, const float* vcol, float* c0, float* c1) {
    const float* mac = (X.matUpdate & 1) ? vcol : X.matAmbient;
    const float* mdc = (X.matUpdate & 2) ? vcol : X.matDiffuse;
    const float* msc = (X.matUpdate & 4) ? vcol : X.matSpecular;
    float s0[4], s1[3] = {0, 0, 0};
    for (int k = 0; k < 3; k++) s0[k] = X.matEmissive[k] + mac[k] * X.ambient[k];
    s0[3] = mac[3] * X.ambient[3];
    for (int i = 0; i < 4; i++) {
        const auto& l = X.lights[i];
        if (!l.on) continue;
        float L[3];
        float scale = 1.0f;
        if (l.directional) {
            L[0] = l.pos[0], L[1] = l.pos[1], L[2] = l.pos[2];
        } else {
            L[0] = l.pos[0] - wpos[0], L[1] = l.pos[1] - wpos[1], L[2] = l.pos[2] - wpos[2];
            float d2 = dot3(L, L), d = sqrtf(d2);
            if (d > 0) {
                L[0] /= d, L[1] /= d, L[2] /= d;
            } else {
                L[0] = 0, L[1] = 0, L[2] = 1;
            }
            float den = l.att[0] + l.att[1] * d + l.att[2] * d2;
            scale = den > 0 ? std::clamp(1.0f / den, 0.0f, 1.0f) : 0.0f;
            if (l.spot) {
                float a = dot3(l.dir, L);
                scale = (a >= l.cutoff) ? scale * pspPow(a, l.conv) : 0.0f;
            }
        }
        float ld = dot3(L, n);
        if (l.powDiffuse) ld = pspPow(ld, X.specCoef);
        float dfac = std::max(ld, 0.0f);
        for (int k = 0; k < 3; k++) s0[k] += (l.amb[k] * mac[k] + l.dif[k] * mdc[k] * dfac) * scale;
        if (l.specular && ld >= 0.0f) {
            float H[3] = {L[0] + X.viewDir[0], L[1] + X.viewDir[1], L[2] + X.viewDir[2]};
            norm3(H);
            float sp = pspPow(dot3(H, n), X.specCoef);
            if (sp > 0)
                for (int k = 0; k < 3; k++) s1[k] += l.spe[k] * msc[k] * sp * scale;
        }
    }
    if (X.sepSpecular) {
        for (int k = 0; k < 3; k++) c1[k] = std::min(s1[k], 1.0f);
    } else {
        for (int k = 0; k < 3; k++) s0[k] += s1[k];
        c1[0] = c1[1] = c1[2] = 0;
    }
    for (int k = 0; k < 4; k++) c0[k] = std::clamp(s0[k], 0.0f, 1.0f);
}

void transformVertex(const XformState& X, const DecodedVertex& d, bool hasColor, TVert& t) {
    const GeState& ge = g_ge;
    t.outside = false;
    t.fog = 1.0f;
    t.c1[0] = t.c1[1] = t.c1[2] = 0;
    float vcol[4];
    if (hasColor) memcpy(vcol, d.col, sizeof vcol);
    else {
        u32 ma = ge.r(CMD_MATAMBIENT);
        rgbOf(ma, vcol);
        vcol[3] = (ge.r(CMD_MATALPHA) & 0xFF) / 255.0f;
    }
    if (X.through) {
        t.clip[0] = d.pos[0], t.clip[1] = d.pos[1], t.clip[2] = d.pos[2], t.clip[3] = 1.0f;
        t.uv[0] = d.uv[0], t.uv[1] = d.uv[1], t.uv[2] = 1.0f;
        memcpy(t.c0, vcol, sizeof vcol);
        return;
    }
    float p[3] = {d.pos[0], d.pos[1], d.pos[2]};
    float n[3] = {d.nrm[0], d.nrm[1], d.nrm[2]};
    if (X.skin) {
        float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
        const int nw = (int)(((ge.r(CMD_VTYPE) >> 14) & 7) + 1);
        for (int i = 0; i < nw; i++) {
            const float w = d.wt[i];
            if (w == 0.0f) continue;
            const float* b = ge.bone + i * 12;
            for (int k = 0; k < 3; k++) {
                sp[k] += (b[k] * p[0] + b[3 + k] * p[1] + b[6 + k] * p[2] + b[9 + k]) * w;
                sn[k] += (b[k] * n[0] + b[3 + k] * n[1] + b[6 + k] * n[2]) * w;
            }
        }
        memcpy(p, sp, sizeof p);
        memcpy(n, sn, sizeof n);
    }
    if (X.reverseNormals) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
    const float* m = X.wvp;
    for (int r = 0; r < 4; r++) t.clip[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];

    // screen-range test (the GE drops primitives with a vertex outside the 4096x4096 space)
    const float w = t.clip[3];
    if (w != 0.0f) {
        const float iw = 1.0f / w;
        const float scx = t.clip[0] * iw * X.sx + X.cx;
        const float scy = t.clip[1] * iw * X.sy + X.cy;
        const float scz = t.clip[2] * iw * X.sz + X.cz;
        const bool xyIn = scx >= 0.0f && scy >= 0.0f && scx < 4096.0f && scy < 4096.0f;
        if (X.depthClip) {
            if (!(t.clip[2] < -w) && !xyIn) t.outside = true;
        } else if (!xyIn || !(scz >= 0.0f && scz < 65536.0f)) {
            t.outside = true;
        }
    } else {
        t.outside = !X.depthClip;
    }

    const bool needWorld = X.lighting || X.uvMode == 2;
    float wpos[3] = {0, 0, 0}, wn[3] = {0, 0, 1};
    if (needWorld) {
        const float* W = ge.world;
        for (int k = 0; k < 3; k++) {
            wpos[k] = W[k] * p[0] + W[3 + k] * p[1] + W[6 + k] * p[2] + W[9 + k];
            wn[k] = W[k] * n[0] + W[3 + k] * n[1] + W[6 + k] * n[2];
        }
        norm3(wn);
    }
    if (X.fog) {
        const float* v = X.wv;
        float vz = v[2] * p[0] + v[6] * p[1] + v[10] * p[2] + v[14];
        t.fog = std::clamp((vz + X.fogEnd) * X.fogSlope, 0.0f, 1.0f);
    }
    // texture coordinates
    switch (X.uvMode) {
        case 0:
            t.uv[0] = d.uv[0] * X.uvScale[0] + X.uvOff[0];
            t.uv[1] = d.uv[1] * X.uvScale[1] + X.uvOff[1];
            t.uv[2] = 1.0f;
            break;
        case 1: {
            float src[3];
            switch (X.projMode) {
                case 0: memcpy(src, p, sizeof src); break;
                case 1: src[0] = d.uv[0], src[1] = d.uv[1], src[2] = 0; break;
                case 2: memcpy(src, n, sizeof src), norm3(src); break;
                default: memcpy(src, n, sizeof src); break;
            }
            const float* T = ge.tgen;
            for (int k = 0; k < 3; k++) t.uv[k] = T[k] * src[0] + T[3 + k] * src[1] + T[6 + k] * src[2] + T[9 + k];
            break;
        }
        case 2: {
            const int ls[2] = {(int)(ge.r(CMD_TEXSHADELS) & 3), (int)((ge.r(CMD_TEXSHADELS) >> 8) & 3)};
            for (int k = 0; k < 2; k++) {
                const auto& l = X.lights[ls[k]];
                float L[3];
                if (l.directional) memcpy(L, l.pos, sizeof L);
                else L[0] = l.pos[0] - wpos[0], L[1] = l.pos[1] - wpos[1], L[2] = l.pos[2] - wpos[2];
                norm3(L);
                if (l.specular) {
                    L[0] += X.viewDir[0], L[1] += X.viewDir[1], L[2] += X.viewDir[2];
                    norm3(L);
                }
                t.uv[k] = (dot3(L, wn) + 1.0f) * 0.5f;
            }
            t.uv[2] = 1.0f;
            break;
        }
    }
    if (X.lighting) lightVertex(X, wpos, wn, vcol, t.c0, t.c1);
    else memcpy(t.c0, vcol, sizeof vcol);
}

// ---------------------------------------------------------------- vertex fetch
VertexLayout g_layout;
u32 g_layoutType = 0xFFFFFFFF;
std::vector<TVert> g_tv;          // transformed vertices (indexed by vertex number - minIndex)
std::vector<u32> g_indices;       // per primitive vertex reference
XformState g_x;

// Fetches `count` vertices for the current PRIM: g_indices[i] indexes g_tv.
bool fetchVertices(u32 count) {
    const GeState& ge = g_ge;
    const u32 vtype = ge.r(CMD_VTYPE);
    if (vtype != g_layoutType) {
        computeLayout(vtype, g_layout);
        g_layoutType = vtype;
    }
    const VertexLayout& L = g_layout;
    const int idxFmt = (vtype >> 11) & 3;
    g_indices.resize(count);
    u32 minI = 0, maxI = count ? count - 1 : 0;
    if (idxFmt) {
        const int isz = idxFmt == 1 ? 1 : (idxFmt == 2 ? 2 : 4);
        if (!validBytes(g_iaddr, count * isz)) return false;
        const u8* ip = memPtr(g_iaddr);
        minI = 0xFFFFFFFF, maxI = 0;
        for (u32 i = 0; i < count; i++) {
            u32 v = idxFmt == 1 ? ip[i] : (u32)(ip[i * isz] | (ip[i * isz + 1] << 8));
            g_indices[i] = v;
            minI = std::min(minI, v);
            maxI = std::max(maxI, v);
        }
        g_iaddr += count * isz;
        if (count == 0) minI = maxI = 0;
    } else {
        for (u32 i = 0; i < count; i++) g_indices[i] = i;
    }
    const u32 nv = maxI - minI + 1;
    const u32 base = g_vaddr + minI * L.stride;
    if (!idxFmt) g_vaddr += count * L.stride;
    if (!validBytes(base, nv * L.stride)) return false;
    setupXform(g_x, L);
    g_tv.resize(nv);
    const u8* vp = memPtr(base);
    const bool hasColor = L.colFmt != 0;
    for (u32 i = 0; i < nv; i++) {
        DecodedVertex d;
        decodeVertex(L, vp + i * L.stride, d);
        transformVertex(g_x, d, hasColor, g_tv[i]);
    }
    if (minI)
        for (auto& v : g_indices) v -= minI;
    return true;
}

// ---------------------------------------------------------------- output
RenderTarget* g_rt = nullptr;
DrawState g_state;
bool g_stateValid = false;
TexBinding g_tex;
float g_ndcX = 1, g_ndcY = 1;   // 2/fbW, 2/fbH
float g_uvNormX = 1, g_uvNormY = 1;  // 1/texW, 1/texH (through mode texel normalization)

inline u32 packColor(const float* c, float a) {
    auto q = [](float v) {
        const int i = (int)(v * 255.0f + 0.5f);
        return (u32)(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    return q(c[0]) | (q(c[1]) << 8) | (q(c[2]) << 16) | (q(a) << 24);
}

// PSP clip coords -> GL clip coords for the current render target.
inline void toGL(const TVert& t, GpuVertex& o) {
    const XformState& X = g_x;
    if (X.through) {
        o.x = t.clip[0] * g_ndcX - 1.0f;
        o.y = t.clip[1] * g_ndcY - 1.0f;
        o.z = t.clip[2] * (2.0f / 65535.0f) - 1.0f;
        o.w = 1.0f;
        o.u = t.uv[0] * g_uvNormX;
        o.v = t.uv[1] * g_uvNormY;
        o.q = 1.0f;
    } else {
        const float w = t.clip[3];
        o.x = (t.clip[0] * X.sx + w * (X.cx - X.offx)) * g_ndcX - w;
        o.y = (t.clip[1] * X.sy + w * (X.cy - X.offy)) * g_ndcY - w;
        o.z = (t.clip[2] * X.sz + w * X.cz) * (2.0f / 65535.0f) - w;
        o.w = w;
        o.u = t.uv[0];
        o.v = t.uv[1];
        o.q = t.uv[2];
    }
    o.c0 = packColor(t.c0, t.c0[3]);
    o.c1 = packColor(t.c1, t.fog);
}

inline void copyFlat(GpuVertex* v, int n, const TVert& prov) {
    u32 c0 = packColor(prov.c0, prov.c0[3]);
    u32 c1base = packColor(prov.c1, 0) & 0x00FFFFFF;
    for (int i = 0; i < n; i++) {
        v[i].c0 = c0;
        v[i].c1 = c1base | (v[i].c1 & 0xFF000000);
    }
}

bool g_flat = false;
int g_cullMode = -1;  // -1 = no culling, 0/1 = GE cull face

// Screen-space orientation (y down) of a triangle from GL-space vertices.
inline float orient(const GpuVertex& a, const GpuVertex& b, const GpuVertex& c) {
    const float ax = a.x / a.w, ay = a.y / a.w, bx = b.x / b.w, by = b.y / b.w, cx = c.x / c.w, cy = c.y / c.w;
    // GL y is up for our mapping of PSP y (row 0 at y=-1), i.e. GL y == PSP y direction: same sign.
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

std::vector<GpuVertex> g_poly;

TVert lerpT(const TVert& a, const TVert& b, float t) {
    TVert r;
    for (int i = 0; i < 4; i++) r.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
    for (int i = 0; i < 3; i++) r.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    for (int i = 0; i < 4; i++) r.c0[i] = a.c0[i] + (b.c0[i] - a.c0[i]) * t;
    for (int i = 0; i < 3; i++) r.c1[i] = a.c1[i] + (b.c1[i] - a.c1[i]) * t;
    r.fog = a.fog + (b.fog - a.fog) * t;
    r.outside = false;
    return r;
}

void emitTriangle(const TVert& a, const TVert& b, const TVert& c) {
    const XformState& X = g_x;
    if (!X.through) {
        if (a.outside || b.outside || c.outside) return;
        if (a.clip[3] < 0 && b.clip[3] < 0 && c.clip[3] < 0) return;
        const bool na = a.clip[2] < -a.clip[3], nb = b.clip[2] < -b.clip[3], nc = c.clip[2] < -c.clip[3];
        if (!X.depthClip) {
            if (!(a.clip[3] > 0 && b.clip[3] > 0 && c.clip[3] > 0)) return;
        } else if (na || nb || nc) {
            if (na && nb && nc) return;
            // clip against the near plane z + w >= 0
            const TVert* in[3] = {&a, &b, &c};
            TVert poly[4];
            int np = 0;
            for (int i = 0; i < 3; i++) {
                const TVert& p = *in[i];
                const TVert& q = *in[(i + 1) % 3];
                const float dp = p.clip[2] + p.clip[3], dq = q.clip[2] + q.clip[3];
                if (dp >= 0) poly[np++] = p;
                if ((dp >= 0) != (dq >= 0)) poly[np++] = lerpT(p, q, dp / (dp - dq));
            }
            GpuVertex gv[4];
            for (int i = 0; i < np; i++) toGL(poly[i], gv[i]);
            if (g_cullMode >= 0 && np >= 3) {
                float o = orient(gv[0], gv[1], gv[2]);
                if (g_cullMode == 0 ? !(o < 0) : !(o > 0)) return;
            }
            for (int i = 1; i + 1 < np; i++) {
                GpuVertex* out = rendererAlloc(3);
                out[0] = gv[0], out[1] = gv[i], out[2] = gv[i + 1];
                if (g_flat) copyFlat(out, 3, c);
            }
            return;
        }
    }
    GpuVertex gv[3];
    toGL(a, gv[0]);
    toGL(b, gv[1]);
    toGL(c, gv[2]);
    if (g_cullMode >= 0) {
        float o = orient(gv[0], gv[1], gv[2]);
        if (g_cullMode == 0 ? !(o < 0) : !(o > 0)) return;
    }
    GpuVertex* out = rendererAlloc(3);
    out[0] = gv[0], out[1] = gv[1], out[2] = gv[2];
    if (g_flat) copyFlat(out, 3, c);
}

void emitSprite(const TVert& A, const TVert& B) {
    // screen-space corners (PSP framebuffer pixels), depth and normalized texcoords
    float ax, ay, bx, by, z, au, av, bu, bv;
    if (g_x.through) {
        ax = A.clip[0], ay = A.clip[1], bx = B.clip[0], by = B.clip[1], z = B.clip[2];
        au = A.uv[0] * g_uvNormX, av = A.uv[1] * g_uvNormY, bu = B.uv[0] * g_uvNormX, bv = B.uv[1] * g_uvNormY;
    } else {
        if (A.outside || B.outside || A.clip[3] == 0 || B.clip[3] == 0) return;
        const float ia = 1.0f / A.clip[3], ib = 1.0f / B.clip[3];
        ax = A.clip[0] * ia * g_x.sx + g_x.cx - g_x.offx;
        ay = A.clip[1] * ia * g_x.sy + g_x.cy - g_x.offy;
        bx = B.clip[0] * ib * g_x.sx + g_x.cx - g_x.offx;
        by = B.clip[1] * ib * g_x.sy + g_x.cy - g_x.offy;
        z = std::clamp(B.clip[2] * ib * g_x.sz + g_x.cz, 0.0f, 65535.0f);
        // projective texcoords collapse to plain ones on a sprite
        const float qa = (A.uv[2] != 0) ? A.uv[2] : 1.0f, qb = (B.uv[2] != 0) ? B.uv[2] : 1.0f;
        au = A.uv[0] / qa, av = A.uv[1] / qa, bu = B.uv[0] / qb, bv = B.uv[1] / qb;
    }
    float u1 = bu, v1 = av, u3 = au, v3 = bv;  // corners (B.x, A.y) and (A.x, B.y)
    if ((ax < bx && ay > by) || (ax > bx && ay < by)) {
        std::swap(u1, u3);
        std::swap(v1, v3);
    }
    const float gax = ax * g_ndcX - 1.0f, gay = ay * g_ndcY - 1.0f, gbx = bx * g_ndcX - 1.0f, gby = by * g_ndcY - 1.0f;
    const float gz = z * (2.0f / 65535.0f) - 1.0f;
    const u32 c0 = packColor(B.c0, B.c0[3]), c1 = packColor(B.c1, B.fog);
    GpuVertex* o = rendererAlloc(6);
    auto put = [&](GpuVertex& v, float x, float y, float u, float t) {
        v.x = x, v.y = y, v.z = gz, v.w = 1.0f;
        v.u = u, v.v = t, v.q = 1.0f;
        v.c0 = c0, v.c1 = c1;
    };
    put(o[0], gbx, gby, bu, bv);
    put(o[1], gbx, gay, u1, v1);
    put(o[2], gax, gay, au, av);
    o[3] = o[0];
    o[4] = o[2];
    put(o[5], gax, gby, u3, v3);
}

void emitLine(const TVert& a, const TVert& b) {
    if (!g_x.through && (a.outside || b.outside)) return;
    if (!g_x.through && (a.clip[3] <= 0 || b.clip[3] <= 0)) return;
    GpuVertex* out = rendererAlloc(2);
    toGL(a, out[0]);
    toGL(b, out[1]);
    if (g_flat) copyFlat(out, 2, b);
}

void emitPoint(const TVert& a) {
    if (!g_x.through && (a.outside || a.clip[3] <= 0)) return;
    GpuVertex* out = rendererAlloc(1);
    toGL(a, out[0]);
}

// ---------------------------------------------------------------- raster state translation
enum BlendIdx : u8 {
    BF_ZERO, BF_ONE, BF_SRC1_COLOR, BF_DST_COLOR, BF_INV_DST_COLOR, BF_DST_ALPHA, BF_INV_DST_ALPHA,
    BF_SRC_ALPHA, BF_INV_SRC_ALPHA, BF_CONST, BF_INV_CONST,
};

void computeBlend(DrawState& s) {
    const GeState& ge = g_ge;
    const u32 bm = ge.r(CMD_BLENDMODE);
    const u32 A = bm & 0xF, B = (bm >> 4) & 0xF, eq = (bm >> 8) & 7;
    s.blend = 1;
    s.blendEq = (u8)std::min<u32>(eq, 5);
    s.fixA = ge.r(CMD_FIXA);
    s.fixB = ge.r(CMD_FIXB);
    // modes GL blending can only approximate: report each once
    if (eq == 5 || A == 8 || A == 9 || B == 6 || B == 8 || B == 9) {
        static std::vector<u32> seen;
        const u32 key = bm & 0x7FF;
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            LOG_WARN("GE", "approximated blend mode A=%u B=%u eq=%u", A, B, eq);
        }
    }
    if (eq == 3 || eq == 4) {  // min / max ignore the factors
        s.srcMul = 0;
        s.dstMul = 0;
        s.blendSrc = BF_ONE;
        s.blendDst = BF_ONE;
        return;
    }
    // source factor: computed in the shader when it only depends on the fragment
    switch (A) {
        case 0: s.blendSrc = BF_DST_COLOR; break;
        case 1: s.blendSrc = BF_INV_DST_COLOR; break;
        case 2: s.srcMul = 1; break;
        case 3: s.srcMul = 2; break;
        case 4: s.blendSrc = BF_DST_ALPHA; break;
        case 5: s.blendSrc = BF_INV_DST_ALPHA; break;
        case 6: s.srcMul = 3; break;
        case 7: s.srcMul = 4; break;
        case 8: s.blendSrc = BF_DST_ALPHA; s.srcMul = 6; break;      // 2*dstA ~ (2*src)*dstA
        case 9: s.blendSrc = BF_INV_DST_ALPHA; break;                // approximated
        default:
            if ((s.fixA & 0xFFFFFF) == 0xFFFFFF) s.blendSrc = BF_ONE;
            else if ((s.fixA & 0xFFFFFF) == 0) s.blendSrc = BF_ZERO;
            else s.srcMul = 5;
            break;
    }
    if (s.srcMul) s.blendSrc = (A == 8) ? BF_DST_ALPHA : BF_ONE;
    switch (B) {
        case 0: s.dstMul = 1; break;
        case 1: s.dstMul = 2; break;
        case 2: s.blendDst = BF_SRC_ALPHA; break;
        case 3: s.blendDst = BF_INV_SRC_ALPHA; break;
        case 4: s.blendDst = BF_DST_ALPHA; break;
        case 5: s.blendDst = BF_INV_DST_ALPHA; break;
        case 6: s.dstMul = 5; break;
        case 7: s.dstMul = 6; break;
        case 8: s.blendDst = BF_DST_ALPHA; break;     // approximated (2*dstA)
        case 9: s.blendDst = BF_INV_DST_ALPHA; break; // approximated
        default:
            if ((s.fixB & 0xFFFFFF) == 0xFFFFFF) s.blendDst = BF_ONE;
            else if ((s.fixB & 0xFFFFFF) == 0) s.blendDst = BF_ZERO;
            else s.dstMul = 7;
            break;
    }
    // SRC_ALPHA as a GL dst factor reads the fragment alpha we output; keep alpha intact then
    if (s.dstMul) s.blendDst = BF_SRC1_COLOR;
    else if (B == 2) s.dstMul = 3, s.blendDst = BF_SRC1_COLOR;
    else if (B == 3) s.dstMul = 4, s.blendDst = BF_SRC1_COLOR;
}

void computeState(DrawState& s, u32 primKind) {
    const GeState& ge = g_ge;
    memset(&s, 0, sizeof s);
    s.prim = (u8)primKind;
    s.pointSize = (float)fbScale();
    const bool clear = ge.clearMode();
    const bool through = ge.through();
    const u32 fbFmt = ge.fbFormat();
    // scissor
    int x1 = ge.scX1(), y1 = ge.scY1(), x2 = ge.scX2() + 1, y2 = ge.scY2() + 1;
    s.scissor[0] = (s16)x1, s.scissor[1] = (s16)y1, s.scissor[2] = (s16)x2, s.scissor[3] = (s16)y2;
    s.depthClamp = 1;
    const u32 pmskc = ge.r(CMD_MASKRGB), pmska = ge.r(CMD_MASKALPHA) & 0xFF;
    u8 cmask = 0;
    if ((pmskc & 0xFF) != 0xFF) cmask |= 1;
    if (((pmskc >> 8) & 0xFF) != 0xFF) cmask |= 2;
    if (((pmskc >> 16) & 0xFF) != 0xFF) cmask |= 4;
    if (clear) {
        const u32 cm = ge.r(CMD_CLEARMODE);
        s.clear = 1;
        if (!(cm & 0x100)) cmask &= ~7;
        if ((cm & 0x200) && fbFmt != 0 && pmska != 0xFF) {
            cmask |= 8;
            s.alphaOut = 2;
            s.stencilTest = 1;
            s.stencilFunc = 1;  // always
            s.sFail = s.zFail = s.zPass = 2;  // replace
            s.stencilMask = 0xFF;
            s.stencilWriteMask = (u8)~pmska;
        }
        if (cm & 0x400) {
            s.depthTest = 1;
            s.depthFunc = 1;
            s.depthWrite = 1;
        }
        s.colorMask = cmask;
        return;
    }
    // texturing
    if ((ge.r(CMD_TEXENABLE) & 1) && g_tex.tex) {
        s.tex = g_tex.tex;
        const u32 tf = ge.r(CMD_TEXFILTER);
        u32 minf = tf & 7;
        s.magFilter = (tf >> 8) & 1;
        if (g_tex.levels <= 1 && minf >= 4) minf &= 1;
        s.minFilter = (u8)minf;
        const u32 tw = ge.r(CMD_TEXWRAP);
        s.wrapS = tw & 1;
        s.wrapT = (tw >> 8) & 1;
        const u32 tl = ge.r(CMD_TEXLEVEL);
        const float bias = (float)(s8)((tl >> 16) & 0xFF) / 16.0f;
        s.minLod = 0, s.maxLod = 1000;
        if ((tl & 3) == 1 || (tl & 3) == 3) {
            s.minLod = s.maxLod = std::clamp(bias, 0.0f, (float)(g_tex.levels - 1));
        } else {
            s.lodBias = bias;
        }
        const u32 tfn = ge.r(CMD_TEXFUNC);
        s.texFunc = (u8)(tfn & 7);
        s.texAlpha = (tfn >> 8) & 1;
        s.colorDouble = (tfn >> 16) & 1;
        s.texEnv = ge.r(CMD_TEXENVCOLOR);
        s.useQ = !through && (ge.r(CMD_TEXMAPMODE) & 3) == 1;
        s.rtt = g_tex.rtt;
        s.rttAlphaOne = g_tex.rttAlphaOne;
        if (g_tex.rtt) {
            memcpy(s.rtScale, g_tex.rtScale, sizeof s.rtScale);
            s.rttRepeat = (u8)((!s.wrapS ? 1 : 0) | (!s.wrapT ? 2 : 0));
        }
    }
    s.secondary = !through && (ge.r(CMD_LIGHTING) & 1) && (ge.r(CMD_LIGHTMODE) & 1);
    // alpha test
    if (ge.r(CMD_ATESTENABLE) & 1) {
        const u32 at = ge.r(CMD_ATEST);
        if ((at & 7) != 1) {
            s.alphaTest = 1;
            s.alphaFunc = at & 7;
            s.alphaRef = (at >> 8) & 0xFF;
            s.alphaMask = (at >> 16) & 0xFF;
        }
    }
    if (ge.r(CMD_CTESTENABLE) & 1) {
        const u32 ct = ge.r(CMD_CTEST) & 3;
        if (ct != 1) {
            s.colorTest = 1;
            s.colorFunc = (u8)ct;
            s.colorRef = ge.r(CMD_CREF);
            s.colorTestMask = ge.r(CMD_CMASK);
        }
    }
    if (!through && (ge.r(CMD_FOGENABLE) & 1)) {
        s.fog = 1;
        s.fogColor = ge.r(CMD_FOGCOLOR);
    }
    if (!through) {
        const u32 mn = ge.r(CMD_MINZ) & 0xFFFF, mx = ge.r(CMD_MAXZ) & 0xFFFF;
        if (mn > 0 || mx < 0xFFFF) {
            s.depthRange = 1;
            s.minZ = mn / 65535.0f;
            s.maxZ = mx / 65535.0f;
        }
    }
    if (ge.r(CMD_BLENDENABLE) & 1) computeBlend(s);
    if ((ge.r(CMD_LOGICOPENABLE) & 1) && (ge.r(CMD_LOGICOP) & 0xF) != 3) s.logicOp = (u8)(1 + (ge.r(CMD_LOGICOP) & 0xF));
    static const bool noZTest = getenv("VC2_DEBUG_NO_ZTEST") != nullptr;  // diagnostic only
    if ((ge.r(CMD_ZTESTENABLE) & 1) && !noZTest) {
        s.depthTest = 1;
        s.depthFunc = ge.r(CMD_ZTEST) & 7;
        s.depthWrite = !(ge.r(CMD_ZWRITEDISABLE) & 1);
    }
    if ((ge.r(CMD_STESTENABLE) & 1) && fbFmt != 0) {
        const u32 st = ge.r(CMD_STEST), so = ge.r(CMD_SOP);
        s.stencilTest = 1;
        s.stencilFunc = st & 7;
        s.stencilRef = (st >> 8) & 0xFF;
        s.stencilMask = (st >> 16) & 0xFF;
        s.sFail = so & 7;
        s.zFail = (so >> 8) & 7;
        s.zPass = (so >> 16) & 7;
        s.stencilWriteMask = (u8)~pmska;
        if (pmska != 0xFF) {
            // the framebuffer alpha *is* the stencil: mirror the stencil result into it
            switch (s.zPass) {
                case 1: cmask |= 8, s.alphaOut = 1, s.alphaOutValue = 0; break;
                case 2: cmask |= 8, s.alphaOut = 1, s.alphaOutValue = s.stencilRef; break;
                case 3:
                case 4:
                case 5: break;  // INVERT/INCR/DECR: keep alpha (GL stencil still tracks it)
                default: break;
            }
            if (fbFmt == 1 && s.alphaOut == 1) s.alphaOutValue = (s.alphaOutValue & 0x80) ? 0xFF : 0;
        }
    }
    s.colorMask = cmask;
}

u32 primKind(u32 prim) { return prim == PRIM_POINTS ? 2 : (prim <= PRIM_LINE_STRIP ? 1 : 0); }

void prepareDraw(u32 prim) {
    u64 t0 = hostMicros();
    RenderTarget* rt = fbBindForDraw();
    g_stats.fbMicros += hostMicros() - t0;
    if (rt != g_rt) {
        g_rt = rt;
        g_dirty |= DIRTY_RASTER | DIRTY_TEXTURE;  // re-resolve render-to-texture sources too
    }
    const GeState& ge = g_ge;
    if (g_dirty & DIRTY_TEXTURE) {
        g_tex = TexBinding{};
        t0 = hostMicros();
        if ((ge.r(CMD_TEXENABLE) & 1) && !ge.clearMode()) texLookup(g_tex);
        g_stats.texMicros += hostMicros() - t0;
        g_dirty &= ~DIRTY_TEXTURE;
        g_dirty |= DIRTY_RASTER;
    }
    const u32 kind = primKind(prim);
    if ((g_dirty & DIRTY_RASTER) || !g_stateValid || g_state.prim != kind) {
        // the texture binding depends on texenable which is a raster command
        if ((ge.r(CMD_TEXENABLE) & 1) && !ge.clearMode() && !g_tex.tex) texLookup(g_tex);
        computeState(g_state, kind);
        g_stateValid = true;
        g_dirty &= ~DIRTY_RASTER;
    }
    g_ndcX = 2.0f / (float)std::max(1, rt ? rt->width : 480);
    g_ndcY = 2.0f / (float)std::max(1, rt ? rt->height : 272);
    g_uvNormX = 1.0f / (float)g_tex.w;
    g_uvNormY = 1.0f / (float)g_tex.h;
    g_flat = (ge.r(CMD_SHADEMODE) & 1) == 0;
    g_cullMode = -1;
    if ((ge.r(CMD_CULLENABLE) & 1) && !ge.clearMode()) g_cullMode = (int)(ge.r(CMD_CULL) & 1);
}

}  // namespace

void drawInit() {}

void flushDraws() { rendererFlush(); }

void drawPrim(u32 data) {
    const u32 count = data & 0xFFFF;
    u32 prim = (data >> 16) & 7;
    if (prim == 7) prim = PRIM_TRIANGLES;  // "keep previous" - continuation is not tracked
    if (count == 0) return;
    g_stats.prims++;
    g_stats.verts += count;
    prepareDraw(prim);
    if (!g_rt) return;
    const u64 tv0 = hostMicros();
    struct VtxTimer {
        u64 t;
        ~VtxTimer() { g_stats.vtxMicros += hostMicros() - t; }
    } vtxTimer{tv0};
    if (!fetchVertices(count)) {
        static int warned = 0;
        if (warned++ < 10) LOG_WARN("GE", "PRIM with invalid vertex/index data (v=%08x i=%08x)", g_vaddr, g_iaddr);
        return;
    }
    const GeState& ge = g_ge;
    if (ge.clearMode()) {
        // the clear colour's alpha is the stencil value written
        const TVert& last = g_tv[g_indices[count - 1]];
        u8 a = (u8)std::clamp((int)lroundf(last.c0[3] * 255.0f), 0, 255);
        g_state.stencilRef = a;  // part of the batch state: a change starts a new batch
    }
    rendererSetState(g_state);
    const auto& I = g_indices;
    {
        // Debug: VC2_TRACE_ON_TEX=<addr> traces (and dumps, with VC2_DUMP_DIR) the frame after the
        // first draw that samples this texture address - a state-triggered capture that does not
        // depend on scripted input timing.
        static const u32 trigTex = getenv("VC2_TRACE_ON_TEX") ? (u32)strtoul(getenv("VC2_TRACE_ON_TEX"), nullptr, 16) : 0;
        static int fired = 0;
        static const u64 trigAfter = getenv("VC2_TRACE_ON_TEX_AFTER") ? strtoull(getenv("VC2_TRACE_ON_TEX_AFTER"), nullptr, 10) : 0;
        if (trigTex && fired < 3 && g_frame >= trigAfter && (ge.r(CMD_TEXENABLE) & 1) &&
            (ge.texAddr(0) & 0x0FFFFFFF) == (trigTex & 0x0FFFFFFF)) {
            static u64 lastFrame = ~0ull;
            if (g_frame != lastFrame && (fired == 0 || g_frame >= lastFrame + 120)) {
                lastFrame = g_frame;
                fired++;
                debugRequestTrace(g_frame + 1);
                debugRequestDump(g_frame + 1);
                LOG_INFO("GPU", "texture %08x drawn at frame %llu: tracing frame %llu", trigTex,
                         (unsigned long long)g_frame, (unsigned long long)g_frame + 1);
            }
        }
    }
    if (g_tracing) {
        float zmin = 1e9f, zmax = -1e9f;
        for (u32 i = 0; i < count; i++) {
            const TVert& t = g_tv[I[i]];
            float z = (!g_x.through && t.clip[3] != 0) ? t.clip[2] / t.clip[3] : t.clip[2];
            zmin = std::min(zmin, z), zmax = std::max(zmax, z);
        }
        LOG_INFO("GETRACE", "  state zte %u zfunc %u zmask %u ndcZ %.4f..%.4f minz %04x maxz %04x vscz %06x vtz %06x "
                 "ate %u atst %06x ste %u ctest %u cmask %06x amask %02x lte %u cull %u logic %u depthTest(gl) %u func %u write %u",
                 ge.r(CMD_ZTESTENABLE) & 1, ge.r(CMD_ZTEST) & 7, ge.r(CMD_ZWRITEDISABLE) & 1, zmin, zmax,
                 ge.r(0xD6) & 0xFFFF, ge.r(0xD7) & 0xFFFF, ge.r(0x44) & 0xFFFFFF, ge.r(0x47) & 0xFFFFFF,
                 ge.r(CMD_ATESTENABLE) & 1, ge.r(CMD_ATEST), ge.r(CMD_STESTENABLE) & 1, ge.r(0x27) & 1,
                 ge.r(0xE8) & 0xFFFFFF, ge.r(0xE9) & 0xFF, ge.r(0x17) & 1, ge.r(0x1D) & 1, ge.r(CMD_LOGICOPENABLE) & 1,
                 g_state.depthTest, g_state.depthFunc, g_state.depthWrite);
    }
    if (g_tracing) {
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (u32 i = 0; i < count; i++) {
            const TVert& t = g_tv[I[i]];
            u0 = std::min(u0, t.uv[0]), v0 = std::min(v0, t.uv[1]), u1 = std::max(u1, t.uv[0]), v1 = std::max(v1, t.uv[1]);
            float sx = t.clip[0], sy = t.clip[1];
            if (!g_x.through && t.clip[3] != 0) {
                sx = t.clip[0] / t.clip[3] * g_x.sx + g_x.cx - g_x.offx;
                sy = t.clip[1] / t.clip[3] * g_x.sy + g_x.cy - g_x.offy;
            }
            x0 = std::min(x0, sx), y0 = std::min(y0, sy), x1 = std::max(x1, sx), y1 = std::max(y1, sy);
        }
        const TVert& l = g_tv[I[count - 1]];
        LOG_INFO("GETRACE", "  draw prim %u n %u vtype %06x bbox (%.1f,%.1f)-(%.1f,%.1f) uv (%.4f,%.4f)-(%.4f,%.4f) col %08x tex %08x fmt %u %dx%d func %x blend %u:%x atest %06x clear %u fb %06x filt %x wrap %x",
                 prim, count, ge.r(CMD_VTYPE), x0, y0, x1, y1, u0, v0, u1, v1, packColor(l.c0, l.c0[3]),
                 (ge.r(CMD_TEXENABLE) & 1) ? ge.texAddr(0) : 0, ge.texFormat(), ge.texW(0), ge.texH(0), ge.r(CMD_TEXFUNC),
                 ge.r(CMD_BLENDENABLE) & 1, ge.r(CMD_BLENDMODE), (ge.r(CMD_ATESTENABLE) & 1) ? ge.r(CMD_ATEST) : 0,
                 ge.clearMode() ? 1 : 0, ge.fbAddr(), ge.r(CMD_TEXFILTER), ge.r(CMD_TEXWRAP));
    }
    const auto& T = g_tv;
    int maxY = 0;
    switch (prim) {
        case PRIM_POINTS:
            for (u32 i = 0; i < count; i++) emitPoint(T[I[i]]);
            break;
        case PRIM_LINES:
            for (u32 i = 0; i + 1 < count; i += 2) emitLine(T[I[i]], T[I[i + 1]]);
            break;
        case PRIM_LINE_STRIP:
            for (u32 i = 0; i + 1 < count; i++) emitLine(T[I[i]], T[I[i + 1]]);
            break;
        case PRIM_TRIANGLES:
            for (u32 i = 0; i + 2 < count; i += 3) emitTriangle(T[I[i]], T[I[i + 1]], T[I[i + 2]]);
            break;
        case PRIM_TRI_STRIP:
            for (u32 i = 0; i + 2 < count; i++) {
                if (i & 1) emitTriangle(T[I[i + 1]], T[I[i]], T[I[i + 2]]);
                else emitTriangle(T[I[i]], T[I[i + 1]], T[I[i + 2]]);
            }
            break;
        case PRIM_TRI_FAN:
            for (u32 i = 1; i + 1 < count; i++) emitTriangle(T[I[0]], T[I[i]], T[I[i + 1]]);
            break;
        case PRIM_RECTS:
            for (u32 i = 0; i + 1 < count; i += 2) emitSprite(T[I[i]], T[I[i + 1]]);
            break;
    }
    (void)maxY;
}

bool bboxVisible(u32 count) {
    const u32 vtype = g_ge.r(CMD_VTYPE);
    VertexLayout L;
    computeLayout(vtype, L);
    if (L.through) return true;
    if (!validBytes(g_vaddr, count * L.stride)) return true;
    XformState X;
    setupXform(X, L);
    const u8* vp = memPtr(g_vaddr);
    // all vertices beyond one clip plane -> invisible
    int outMask = 0x3F;
    for (u32 i = 0; i < count; i++) {
        DecodedVertex d;
        decodeVertex(L, vp + i * L.stride, d);
        float c[4];
        const float* m = X.wvp;
        for (int r = 0; r < 4; r++) c[r] = m[r] * d.pos[0] + m[4 + r] * d.pos[1] + m[8 + r] * d.pos[2] + m[12 + r];
        int mask = 0;
        if (c[0] < -c[3]) mask |= 1;
        if (c[0] > c[3]) mask |= 2;
        if (c[1] < -c[3]) mask |= 4;
        if (c[1] > c[3]) mask |= 8;
        if (c[2] < -c[3]) mask |= 16;
        if (c[2] > c[3]) mask |= 32;
        outMask &= mask;
        if (!outMask) return true;
    }
    return outMask == 0;
}

// ---------------------------------------------------------------- CLUT and block transfers
void loadClut() {
    const GeState& ge = g_ge;
    u32 blocks = ge.r(CMD_LOADCLUT) & 0x3F;
    if ((ge.r(CMD_LOADCLUT) & 0x7F) == 0x40) blocks = 0x40;
    const u32 bytes = blocks * 32;
    const u32 addr = ge.clutAddr();
    if (!bytes) return;
    if (!validBytes(addr, bytes)) {
        memset(g_ge.clut, 0, bytes);
    } else {
        // if the palette lives in a GPU-rendered framebuffer, bring it down first
        int ox, oy;
        RenderTarget* rt = vramAddr(addr) ? fbFindForTexture(addr, 0, 0xFF, 0, 0, ox, oy) : nullptr;
        if (rt && rt->gpuDrawn) {
            flushDraws();
            fbDownload(rt, 0, 0, rt->width, rt->height);
        }
        memcpy(g_ge.clut, memPtr(addr), bytes);
    }
    g_ge.clutLoadedBytes = bytes;
    g_ge.clutHash = hashMem(g_ge.clut, bytes);
    g_dirty |= DIRTY_TEXTURE;
}

void blockTransfer() {
    const GeState& ge = g_ge;
    const u32 src = (ge.r(CMD_TRXSRC) & 0xFFFFF0) | ((ge.r(CMD_TRXSRCW) & 0xFF0000) << 8);
    const u32 dst = (ge.r(CMD_TRXDST) & 0xFFFFF0) | ((ge.r(CMD_TRXDSTW) & 0xFF0000) << 8);
    u32 srcStride = ge.r(CMD_TRXSRCW) & 0x7F8, dstStride = ge.r(CMD_TRXDSTW) & 0x7F8;
    if (srcStride > 0x400) srcStride = 0;
    if (dstStride > 0x400) dstStride = 0;
    const int sx = ge.r(CMD_TRXSRCPOS) & 0x3FF, sy = (ge.r(CMD_TRXSRCPOS) >> 10) & 0x3FF;
    const int dx = ge.r(CMD_TRXDSTPOS) & 0x3FF, dy = (ge.r(CMD_TRXDSTPOS) >> 10) & 0x3FF;
    const int w = (ge.r(CMD_TRXSIZE) & 0x3FF) + 1, h = ((ge.r(CMD_TRXSIZE) >> 10) & 0x3FF) + 1;
    const int bpp = (ge.r(CMD_TRXKICK) & 1) ? 4 : 2;
    g_stats.transfers++;
    flushDraws();
    // copies memory and mirrors the copy on any render targets involved
    fbBlockTransfer(src, srcStride, sx, sy, dst, dstStride, dx, dy, w, h, bpp);
    const u32 dstStart = dst + (dy * dstStride + dx) * bpp;
    const u32 dstBytes = ((h - 1) * dstStride + w) * bpp;
    texInvalidate(dstStart, dstBytes);
    g_dirty |= DIRTY_TEXTURE;
}

// ---------------------------------------------------------------- bezier / spline patches
namespace {
std::vector<DecodedVertex> g_cp;
std::vector<u8> g_patchBuf;

void bernstein(float t, float* b) {
    const float it = 1.0f - t;
    b[0] = it * it * it;
    b[1] = 3 * t * it * it;
    b[2] = 3 * t * t * it;
    b[3] = t * t * t;
}

// Cox-de Boor uniform cubic B-spline basis over a knot vector.
float bsplineBasis(int i, int k, float t, const std::vector<float>& knots) {
    if (k == 0) {
        if (knots[i] <= t && t < knots[i + 1]) return 1.0f;
        return 0.0f;
    }
    float a = 0, b = 0;
    float d1 = knots[i + k] - knots[i], d2 = knots[i + k + 1] - knots[i + 1];
    if (d1 > 0) a = (t - knots[i]) / d1 * bsplineBasis(i, k - 1, t, knots);
    if (d2 > 0) b = (knots[i + k + 1] - t) / d2 * bsplineBasis(i + 1, k - 1, t, knots);
    return a + b;
}

void lerpDecoded(DecodedVertex& out, const DecodedVertex& v, float w, bool first) {
    if (first) memset(&out, 0, sizeof out);
    for (int i = 0; i < 3; i++) out.pos[i] += v.pos[i] * w, out.nrm[i] += v.nrm[i] * w;
    for (int i = 0; i < 2; i++) out.uv[i] += v.uv[i] * w;
    for (int i = 0; i < 4; i++) out.col[i] += v.col[i] * w;
}

// Builds a tessellated grid (nu x nv points) and draws it as triangles.
void drawPatchGrid(const std::vector<DecodedVertex>& grid, int nu, int nv, bool hasColor, bool genUV) {
    const VertexLayout& L = g_layout;
    setupXform(g_x, L);
    g_tv.resize(grid.size());
    for (size_t i = 0; i < grid.size(); i++) {
        DecodedVertex d = grid[i];
        if (genUV) {
            const int u = (int)(i % nu), v = (int)(i / nu);
            d.uv[0] = (float)u / (float)(nu - 1);
            d.uv[1] = (float)v / (float)(nv - 1);
        }
        if (L.nrmFmt == 0) {
            // compute a normal from the grid
            const int u = (int)(i % nu), v = (int)(i / nu);
            const auto& a = grid[v * nu + std::min(u + 1, nu - 1)];
            const auto& b = grid[v * nu + std::max(u - 1, 0)];
            const auto& c = grid[std::min(v + 1, nv - 1) * nu + u];
            const auto& e = grid[std::max(v - 1, 0) * nu + u];
            float du[3] = {a.pos[0] - b.pos[0], a.pos[1] - b.pos[1], a.pos[2] - b.pos[2]};
            float dv[3] = {c.pos[0] - e.pos[0], c.pos[1] - e.pos[1], c.pos[2] - e.pos[2]};
            d.nrm[0] = du[1] * dv[2] - du[2] * dv[1];
            d.nrm[1] = du[2] * dv[0] - du[0] * dv[2];
            d.nrm[2] = du[0] * dv[1] - du[1] * dv[0];
            norm3(d.nrm);
            if (g_ge.r(CMD_PATCHFACING) & 1) d.nrm[0] = -d.nrm[0], d.nrm[1] = -d.nrm[1], d.nrm[2] = -d.nrm[2];
        }
        transformVertex(g_x, d, hasColor, g_tv[i]);
    }
    const u32 pp = g_ge.r(CMD_PATCHPRIM) & 3;
    for (int v = 0; v + 1 < nv; v++)
        for (int u = 0; u + 1 < nu; u++) {
            const TVert& a = g_tv[v * nu + u];
            const TVert& b = g_tv[v * nu + u + 1];
            const TVert& c = g_tv[(v + 1) * nu + u];
            const TVert& d = g_tv[(v + 1) * nu + u + 1];
            if (pp == 0) {
                emitTriangle(a, c, b);
                emitTriangle(b, c, d);
            } else if (pp == 1) {
                emitLine(a, b);
                emitLine(a, c);
            } else {
                emitPoint(a);
            }
        }
}

bool fetchControlPoints(int count) {
    const u32 vtype = g_ge.r(CMD_VTYPE);
    if (vtype != g_layoutType) {
        computeLayout(vtype, g_layout);
        g_layoutType = vtype;
    }
    const VertexLayout& L = g_layout;
    const int idxFmt = (vtype >> 11) & 3;
    g_cp.resize(count);
    std::vector<u32> idx(count);
    if (idxFmt) {
        const int isz = idxFmt == 1 ? 1 : (idxFmt == 2 ? 2 : 4);
        if (!validBytes(g_iaddr, count * isz)) return false;
        const u8* ip = memPtr(g_iaddr);
        for (int i = 0; i < count; i++) idx[i] = idxFmt == 1 ? ip[i] : (u32)(ip[i * isz] | (ip[i * isz + 1] << 8));
        g_iaddr += count * isz;
    } else {
        for (int i = 0; i < count; i++) idx[i] = i;
    }
    u32 maxI = 0;
    for (u32 v : idx) maxI = std::max(maxI, v);
    if (!validBytes(g_vaddr, (maxI + 1) * L.stride)) return false;
    for (int i = 0; i < count; i++) decodeVertex(L, memPtr(g_vaddr) + idx[i] * L.stride, g_cp[i]);
    if (!idxFmt) g_vaddr += count * L.stride;
    return true;
}
}  // namespace

void drawBezier(u32 data) {
    const int ucount = data & 0xFF, vcount = (data >> 8) & 0xFF;
    if (ucount < 4 || vcount < 4) return;
    prepareDraw(PRIM_TRIANGLES);
    if (!g_rt || !fetchControlPoints(ucount * vcount)) return;
    rendererSetState(g_state);
    const int divU = std::max<int>(1, g_ge.r(CMD_PATCHDIV) & 0x7F), divV = std::max<int>(1, (g_ge.r(CMD_PATCHDIV) >> 8) & 0x7F);
    const int pu = (ucount - 1) / 3, pv = (vcount - 1) / 3;
    const bool genUV = g_layout.tcFmt == 0;
    for (int py = 0; py < pv; py++)
        for (int px = 0; px < pu; px++) {
            const int nu = divU + 1, nv = divV + 1;
            std::vector<DecodedVertex> grid(nu * nv);
            for (int j = 0; j < nv; j++) {
                float bv[4];
                bernstein((float)j / divV, bv);
                for (int i = 0; i < nu; i++) {
                    float bu[4];
                    bernstein((float)i / divU, bu);
                    DecodedVertex& out = grid[j * nu + i];
                    bool first = true;
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++) {
                            const DecodedVertex& cp = g_cp[(py * 3 + r) * ucount + px * 3 + c];
                            lerpDecoded(out, cp, bu[c] * bv[r], first);
                            first = false;
                        }
                    if (genUV) {
                        out.uv[0] = (px + (float)i / divU) / pu;
                        out.uv[1] = (py + (float)j / divV) / pv;
                    }
                }
            }
            drawPatchGrid(grid, nu, nv, g_layout.colFmt != 0, false);
        }
}

void drawSpline(u32 data) {
    const int ucount = data & 0xFF, vcount = (data >> 8) & 0xFF;
    const int utype = (data >> 16) & 3, vtype = (data >> 18) & 3;
    if (ucount < 4 || vcount < 4) return;
    prepareDraw(PRIM_TRIANGLES);
    if (!g_rt || !fetchControlPoints(ucount * vcount)) return;
    rendererSetState(g_state);
    const int divU = std::max<int>(1, g_ge.r(CMD_PATCHDIV) & 0x7F), divV = std::max<int>(1, (g_ge.r(CMD_PATCHDIV) >> 8) & 0x7F);
    auto knots = [](int n, int type) {
        // n control points, degree 3: n + 4 uniform knots, domain [0, n - 3].
        // type bit0: the first edge is open (curve starts at the first point), bit1: last edge.
        std::vector<float> k(n + 4);
        for (int i = 0; i < n + 4; i++) k[i] = (float)(i - 3);
        if (type & 1) k[0] = k[1] = k[2] = k[3] = 0.0f;
        if (type & 2) k[n] = k[n + 1] = k[n + 2] = k[n + 3] = (float)(n - 3);
        return k;
    };
    std::vector<float> ku = knots(ucount, utype), kv = knots(vcount, vtype);
    const int segU = (ucount - 3) * divU, segV = (vcount - 3) * divV;
    const int nu = segU + 1, nv = segV + 1;
    std::vector<DecodedVertex> grid(nu * nv);
    const float tu1 = (float)(ucount - 3), tv1 = (float)(vcount - 3);
    for (int j = 0; j < nv; j++) {
        float tv = std::min((float)j / segV * tv1, tv1 - 1e-4f);
        float bv[64];
        for (int r = 0; r < vcount && r < 64; r++) bv[r] = bsplineBasis(r, 3, tv, kv);
        for (int i = 0; i < nu; i++) {
            float tu = std::min((float)i / segU * tu1, tu1 - 1e-4f);
            DecodedVertex& out = grid[j * nu + i];
            bool first = true;
            for (int c = 0; c < ucount && c < 64; c++) {
                float bu = bsplineBasis(c, 3, tu, ku);
                if (bu == 0) continue;
                for (int r = 0; r < vcount && r < 64; r++) {
                    if (bv[r] == 0) continue;
                    lerpDecoded(out, g_cp[r * ucount + c], bu * bv[r], first);
                    first = false;
                }
            }
            if (first) memset(&out, 0, sizeof out);
            if (g_layout.tcFmt == 0) {
                out.uv[0] = (float)i / segU;
                out.uv[1] = (float)j / segV;
            }
        }
    }
    drawPatchGrid(grid, nu, nv, g_layout.colFmt != 0, false);
}

}  // namespace vc2rt::gpu
