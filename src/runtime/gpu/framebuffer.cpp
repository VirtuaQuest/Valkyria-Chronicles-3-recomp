// Framebuffer manager: GE render targets keyed by VRAM address/stride/format, rendered at
// config().internalScale x native resolution (optionally multisampled), kept coherent with
// VRAM (uploads when the CPU wrote pixels, downloads when PSP-side code needs GPU results),
// block transfers, and presentation to the window (aspect modes, filter, FXAA).
#include <memory>

#include "../host/config.h"
#include "../host/host.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

namespace {

std::vector<std::unique_ptr<RenderTarget>> g_targets;
RenderTarget* g_cur = nullptr;
int g_scale = 1, g_samples = 0;

GLuint g_blitProg = 0, g_fxaaProg = 0;
GLint g_blitSrc = -1, g_blitFlip = -1, g_blitAlphaOne = -1, g_blitTex = -1;
GLint g_fxaaTex = -1, g_fxaaRcp = -1, g_fxaaSrc = -1;
GLuint g_uploadTex = 0, g_displayTex = 0, g_copyTex = 0, g_copyFbo = 0;
int g_copyW = 0, g_copyH = 0;
GLuint g_postFbo = 0, g_postTex = 0;
int g_postW = 0, g_postH = 0;
GLuint g_dlFbo = 0, g_dlTex = 0;
int g_dlW = 0, g_dlH = 0;
std::vector<u32> g_pix;

const char* kBlitVS = R"(#version 330 core
uniform vec4 uSrc;
uniform int uFlip;
out vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    vec2 q = p;
    if (uFlip != 0) q.y = 1.0 - q.y;
    uv = uSrc.xy + q * uSrc.zw;
})";
const char* kBlitFS = R"(#version 330 core
in vec2 uv;
out vec4 o;
uniform sampler2D uTex;
uniform int uAlphaOne;
void main() {
    vec4 c = texture(uTex, uv);
    if (uAlphaOne != 0) c.a = 1.0;
    o = c;
})";

// FXAA (luma edge detection with a short directional search), operating on the internal-
// resolution image before it is scaled to the window.
const char* kFxaaFS = R"(#version 330 core
in vec2 uv;
out vec4 o;
uniform sampler2D uTex;
uniform vec2 uRcp;
uniform vec4 uSrcRect;
float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 tap(vec2 p) { return texture(uTex, clamp(p, uSrcRect.xy, uSrcRect.zw)).rgb; }
void main() {
    vec3 cM = tap(uv);
    float lM = lum(cM);
    float lN = lum(tap(uv + vec2(0.0, uRcp.y)));
    float lS = lum(tap(uv - vec2(0.0, uRcp.y)));
    float lE = lum(tap(uv + vec2(uRcp.x, 0.0)));
    float lW = lum(tap(uv - vec2(uRcp.x, 0.0)));
    float lNE = lum(tap(uv + uRcp));
    float lSW = lum(tap(uv - uRcp));
    float lNW = lum(tap(uv + vec2(-uRcp.x, uRcp.y)));
    float lSE = lum(tap(uv + vec2(uRcp.x, -uRcp.y)));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range = lMax - lMin;
    if (range < max(0.0312, lMax * 0.125)) { o = vec4(cM, 1.0); return; }
    float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    bool horz = edgeH >= edgeV;
    float l1 = horz ? lS : lW, l2 = horz ? lN : lE;
    float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
    float step = horz ? uRcp.y : uRcp.x;
    float lLocal;
    if (g1 >= g2) { step = -step; lLocal = 0.5 * (l1 + lM); } else { lLocal = 0.5 * (l2 + lM); }
    float gScaled = 0.25 * max(g1, g2);
    vec2 cur = uv;
    if (horz) cur.y += step * 0.5; else cur.x += step * 0.5;
    vec2 off = horz ? vec2(uRcp.x, 0.0) : vec2(0.0, uRcp.y);
    vec2 p1 = cur - off, p2 = cur + off;
    float e1 = lum(tap(p1)) - lLocal, e2 = lum(tap(p2)) - lLocal;
    bool d1 = abs(e1) >= gScaled, d2 = abs(e2) >= gScaled;
    for (int i = 0; i < 10 && !(d1 && d2); i++) {
        float s = i < 3 ? 1.0 : (i < 6 ? 1.5 : 2.0);
        if (!d1) { p1 -= off * s; e1 = lum(tap(p1)) - lLocal; d1 = abs(e1) >= gScaled; }
        if (!d2) { p2 += off * s; e2 = lum(tap(p2)) - lLocal; d2 = abs(e2) >= gScaled; }
    }
    float dist1 = horz ? (uv.x - p1.x) : (uv.y - p1.y);
    float dist2 = horz ? (p2.x - uv.x) : (p2.y - uv.y);
    bool isDir1 = dist1 < dist2;
    float distFinal = min(dist1, dist2);
    float edgeLen = dist1 + dist2;
    float pixOff = -distFinal / edgeLen + 0.5;
    bool correct = ((isDir1 ? e1 : e2) < 0.0) != ((lM - lLocal) < 0.0);
    float finalOff = correct ? pixOff : 0.0;
    float lAvg = (1.0 / 12.0) * (2.0 * (lN + lS + lE + lW) + lNE + lNW + lSE + lSW);
    float sub = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    finalOff = max(finalOff, sub * sub * 0.75);
    vec2 fuv = uv;
    if (horz) fuv.y += finalOff * step; else fuv.x += finalOff * step;
    o = vec4(tap(fuv), 1.0);
})";

void makeTex(GLuint& t, int w, int h, GLenum filter) {
    if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

GLuint drawFbo(RenderTarget* rt) { return rt->samples ? rt->msFbo : rt->fbo; }

void destroyTarget(RenderTarget* rt) {
    if (rt->fbo) glDeleteFramebuffers(1, &rt->fbo);
    if (rt->color) glDeleteTextures(1, &rt->color);
    if (rt->depth) glDeleteRenderbuffers(1, &rt->depth);
    if (rt->msFbo) glDeleteFramebuffers(1, &rt->msFbo);
    if (rt->msColor) glDeleteRenderbuffers(1, &rt->msColor);
    if (rt->msDepth) glDeleteRenderbuffers(1, &rt->msDepth);
    rt->fbo = rt->color = rt->depth = rt->msFbo = rt->msColor = rt->msDepth = 0;
}

void allocTarget(RenderTarget* rt) {
    const int W = rt->width * rt->scale, H = rt->height * rt->scale;
    makeTex(rt->color, W, H, GL_LINEAR);
    glGenFramebuffers(1, &rt->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->color, 0);
    if (!rt->samples) {
        glGenRenderbuffers(1, &rt->depth);
        glBindRenderbuffer(GL_RENDERBUFFER, rt->depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt->depth);
    } else {
        glGenFramebuffers(1, &rt->msFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, rt->msFbo);
        glGenRenderbuffers(1, &rt->msColor);
        glBindRenderbuffer(GL_RENDERBUFFER, rt->msColor);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, rt->samples, GL_RGBA8, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rt->msColor);
        glGenRenderbuffers(1, &rt->msDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, rt->msDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, rt->samples, GL_DEPTH24_STENCIL8, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt->msDepth);
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) LOG_ERROR("GPU", "render target %dx%d incomplete (%x)", W, H, st);
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(rt));
    glViewport(0, 0, W, H);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glStencilMask(0xFF);
    glClearColor(0, 0, 0, 0);
    glClearDepth(0.0);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    invalidateGLStateCache();
}

void resolve(RenderTarget* rt) {
    if (!rt->samples || !rt->msDirty) return;
    const int W = rt->width * rt->scale, H = rt->height * rt->scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->fbo);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    rt->msDirty = false;
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? drawFbo(g_cur) : 0);
}

void blitTex(GLuint tex, float u0, float v0, float du, float dv, bool flip, bool alphaOne, GLenum filter) {
    glUseProgram(g_blitProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(g_blitTex, 0);
    glUniform4f(g_blitSrc, u0, v0, du, dv);
    glUniform1i(g_blitFlip, flip ? 1 : 0);
    glUniform1i(g_blitAlphaOne, alphaOne ? 1 : 0);
    drawFullscreenQuad();
}

void neutralState() {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    invalidateGLStateCache();
}

u32 vramOff(u32 addr) { return addr & 0x1FFFFF; }

// Copies a (resolved) target's colour into the scratch texture and returns it.
GLuint snapshot(RenderTarget* rt) {
    resolve(rt);
    const int W = rt->width * rt->scale, H = rt->height * rt->scale;
    if (!g_copyTex || g_copyW != W || g_copyH != H) {
        makeTex(g_copyTex, W, H, GL_LINEAR);
        if (!g_copyFbo) glGenFramebuffers(1, &g_copyFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_copyFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_copyTex, 0);
        g_copyW = W, g_copyH = H;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_copyFbo);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? drawFbo(g_cur) : 0);
    invalidateGLStateCache();
    return g_copyTex;
}

void restoreCurrentBinding() {
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? drawFbo(g_cur) : 0);
    if (g_cur) glViewport(0, 0, g_cur->width * g_cur->scale, g_cur->height * g_cur->scale);
    invalidateGLStateCache();
}

// Draws a region of a texture (normalized uv rect) into a PSP-pixel rectangle of a target.
// Works for multisampled targets too (where glBlitFramebuffer cannot write).
void copyTexToTarget(RenderTarget* dst, GLuint tex, float u0, float v0, float du, float dv, int dx, int dy, int w, int h,
                     GLenum filter) {
    const int s = dst->scale;
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(dst));
    glViewport(dx * s, dy * s, w * s, h * s);
    neutralState();
    blitTex(tex, u0, v0, du, dv, false, false, filter);
    if (dst->samples) dst->msDirty = true;
    restoreCurrentBinding();
}

// Uploads VRAM contents of the target's memory into its GL surface.
void uploadFromVram(RenderTarget* rt) {
    const int w = rt->width, h = rt->height;
    const u32 bpp = rt->format == 3 ? 4 : 2;
    const u32 base = 0x04000000 + rt->addr;
    const u32 avail = validBytes(base, rt->stride * h * bpp);
    g_pix.assign((size_t)rt->stride * h, 0);
    int rows = (int)std::min<u32>(h, avail / (rt->stride * bpp));
    decodeFbPixels(memPtr(base), rt->stride, rt->format, rt->stride, rows, g_pix.data());
    if (!g_uploadTex) glGenTextures(1, &g_uploadTex);
    glBindTexture(GL_TEXTURE_2D, g_uploadTex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rt->stride, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, g_pix.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(rt));
    glViewport(0, 0, w * rt->scale, h * rt->scale);
    neutralState();
    blitTex(g_uploadTex, 0, 0, (float)w / rt->stride, 1, false, false, GL_NEAREST);
    if (rt->samples) rt->msDirty = true;
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? drawFbo(g_cur) : 0);
    if (g_cur) glViewport(0, 0, g_cur->width * g_cur->scale, g_cur->height * g_cur->scale);
    rt->vramHash = hashMem(memPtr(base), std::min<u32>(avail, rt->bytes()));
    rt->gpuDrawn = false;
}

u64 vramHashOf(RenderTarget* rt) {
    const u32 base = 0x04000000 + rt->addr;
    return hashMem(memPtr(base), validBytes(base, rt->bytes()));
}

void resizeTarget(RenderTarget* rt, int newH, int newScale, int newSamples) {
    // keep the old contents: copy (scaled) into the new surface
    RenderTarget old = *rt;
    resolve(&old);
    rt->fbo = rt->color = rt->depth = rt->msFbo = rt->msColor = rt->msDepth = 0;
    rt->height = newH;
    rt->scale = newScale;
    rt->samples = newSamples;
    allocTarget(rt);
    const int rows = std::min(old.height, newH);
    copyTexToTarget(rt, old.color, 0, 0, 1, (float)rows / old.height, 0, 0, rt->width, rows,
                    old.scale == newScale ? GL_NEAREST : GL_LINEAR);
    destroyTarget(&old);
}

void applyConfigChanges() {
    const Config& c = config();
    int scale = std::clamp(c.internalScale, 1, 8);
    int samples = c.msaa >= 8 ? 8 : (c.msaa >= 4 ? 4 : (c.msaa >= 2 ? 2 : 0));
    if (scale == g_scale && samples == g_samples) return;
    flushDraws();
    LOG_INFO("GPU", "render scale %dx (%dx%d), MSAA %d", scale, 480 * scale, 272 * scale, samples);
    g_scale = scale;
    g_samples = samples;
    for (auto& t : g_targets) resizeTarget(t.get(), t->height, scale, samples);
    if (g_cur) {
        glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(g_cur));
        glViewport(0, 0, g_cur->width * g_scale, g_cur->height * g_scale);
    }
    texClearAll();  // render-to-texture copies reference old surfaces
}

RenderTarget* findExact(u32 addr, u32 stride, u32 fmt) {
    for (auto& t : g_targets)
        if (t->addr == addr && t->stride == stride && ((t->format == 3) == (fmt == 3))) return t.get();
    return nullptr;
}

void removeTarget(RenderTarget* rt) {
    for (size_t i = 0; i < g_targets.size(); i++)
        if (g_targets[i].get() == rt) {
            destroyTarget(rt);
            g_targets.erase(g_targets.begin() + i);
            return;
        }
}

}  // namespace

int fbScale() { return g_scale; }
RenderTarget* fbCurrent() { return g_cur; }

void fbInit() {
    g_blitProg = compileProgram(kBlitVS, kBlitFS, "blit");
    g_blitSrc = glGetUniformLocation(g_blitProg, "uSrc");
    g_blitFlip = glGetUniformLocation(g_blitProg, "uFlip");
    g_blitAlphaOne = glGetUniformLocation(g_blitProg, "uAlphaOne");
    g_blitTex = glGetUniformLocation(g_blitProg, "uTex");
    g_fxaaProg = compileProgram(kBlitVS, kFxaaFS, "fxaa");
    g_fxaaTex = glGetUniformLocation(g_fxaaProg, "uTex");
    g_fxaaRcp = glGetUniformLocation(g_fxaaProg, "uRcp");
    g_fxaaSrc = glGetUniformLocation(g_fxaaProg, "uSrcRect");
    g_scale = std::clamp(config().internalScale, 1, 8);
    const int m = config().msaa;
    g_samples = m >= 8 ? 8 : (m >= 4 ? 4 : (m >= 2 ? 2 : 0));
    LOG_INFO("GPU", "GE renderer: internal resolution %dx%d (%dx), MSAA %d", 480 * g_scale, 272 * g_scale, g_scale, g_samples);
}

void decodeFbPixels(const u8* src, u32 stride, u32 fmt, int w, int h, u32* out) {
    for (int y = 0; y < h; y++) {
        u32* o = out + (size_t)y * w;
        if (fmt == 3) {
            const u8* s = src + (size_t)y * stride * 4;
            memcpy(o, s, (size_t)w * 4);
            continue;
        }
        const u8* s = src + (size_t)y * stride * 2;
        for (int x = 0; x < w; x++) {
            u16 c = (u16)(s[x * 2] | (s[x * 2 + 1] << 8));
            u32 r, g, b, a;
            if (fmt == 0) {
                r = (c & 31) << 3 | (c & 31) >> 2;
                g = ((c >> 5) & 63) << 2 | ((c >> 5) & 63) >> 4;
                b = ((c >> 11) & 31) << 3 | ((c >> 11) & 31) >> 2;
                a = 0;
            } else if (fmt == 1) {
                r = (c & 31) << 3 | (c & 31) >> 2;
                g = ((c >> 5) & 31) << 3 | ((c >> 5) & 31) >> 2;
                b = ((c >> 10) & 31) << 3 | ((c >> 10) & 31) >> 2;
                a = (c >> 15) ? 255 : 0;
            } else {
                r = (c & 15) * 17;
                g = ((c >> 4) & 15) * 17;
                b = ((c >> 8) & 15) * 17;
                a = ((c >> 12) & 15) * 17;
            }
            o[x] = rgba(r, g, b, a);
        }
    }
}

RenderTarget* fbBindForDraw() {
    applyConfigChanges();
    const GeState& ge = g_ge;
    const u32 addr = ge.fbAddr(), stride = ge.fbStride(), fmt = ge.fbFormat();
    if (stride == 0) return nullptr;
    // height: the drawing region / scissor decide how much of the buffer is used
    int needH = std::max(ge.scY2(), ge.rgY2()) + 1;
    needH = std::clamp(needH, 1, 512);
    // don't let a stray full-region scissor make tall buffers out of display-sized ones
    if (needH > 272 && stride >= 480 && std::max(ge.scY2(), ge.rgY2()) >= 511) needH = 272;
    const int width = (int)std::min<u32>(stride, 1024);
    RenderTarget* rt = (g_cur && g_cur->addr == addr && g_cur->stride == stride && ((g_cur->format == 3) == (fmt == 3)))
                           ? g_cur
                           : findExact(addr, stride, fmt);
    if (!rt) {
        flushDraws();
        // a different layout at the same address replaces the old target
        for (size_t i = 0; i < g_targets.size(); i++)
            if (g_targets[i]->addr == addr) {
                if (g_cur == g_targets[i].get()) g_cur = nullptr;
                removeTarget(g_targets[i].get());
                break;
            }
        auto t = std::make_unique<RenderTarget>();
        t->addr = addr;
        t->stride = stride;
        t->format = fmt;
        t->width = width;
        t->height = std::max(needH, (stride >= 480) ? 272 : needH);
        t->scale = g_scale;
        t->samples = g_samples;
        t->createdFrame = g_frame;
        t->zbAddr = ge.zbAddr();
        allocTarget(t.get());
        rt = t.get();
        g_targets.push_back(std::move(t));
        uploadFromVram(rt);
        LOG_DEBUG("GPU", "new render target %06x stride %u fmt %u %dx%d", addr, stride, fmt, rt->width, rt->height);
    } else if (needH > rt->height) {
        flushDraws();
        resizeTarget(rt, needH, g_scale, g_samples);
    }
    rt->format = fmt;
    if (rt != g_cur) {
        flushDraws();
        g_cur = rt;
        g_stats.fbSwitches++;
        // the CPU may have written into the buffer since we last looked (e.g. a memset clear)
        if (rt->lastDrawFrame != g_frame || !rt->gpuDrawn) {
            u64 h = vramHashOf(rt);
            if (h != rt->vramHash) uploadFromVram(rt);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(rt));
        glViewport(0, 0, rt->width * rt->scale, rt->height * rt->scale);
        rendererBeginTarget();
    }
    rt->zbAddr = ge.zbAddr();
    return rt;
}

RenderTarget* fbFindForTexture(u32 addr, u32 bufw, u32 texFmt, int texW, int texH, int& offX, int& offY) {
    if (!vramAddr(addr)) return nullptr;
    const u32 off = vramOff(addr);
    RenderTarget* best = nullptr;
    for (auto& t : g_targets) {
        if (!t->gpuDrawn && t->lastDrawFrame == 0) continue;
        if (off < t->addr || off >= t->addr + t->bytes()) continue;
        const u32 bpp = t->format == 3 ? 4 : 2;
        if (texFmt <= 3) {
            if ((texFmt == 3) != (t->format == 3)) continue;
            if (bufw && bufw != t->stride) continue;
        }
        if (!best || t->lastDrawFrame > best->lastDrawFrame) {
            best = t.get();
            const u32 rel = off - t->addr;
            offY = (int)(rel / (t->stride * bpp));
            offX = (int)((rel % (t->stride * bpp)) / bpp);
        }
    }
    (void)texW;
    (void)texH;
    return best;
}

GLuint fbSampleTexture(RenderTarget* rt) {
    flushDraws();
    resolve(rt);
    if (rt != g_cur) return rt->color;
    return snapshot(rt);  // sampling the target being drawn: work on a copy
}

void fbDownload(RenderTarget* rt, int x, int y, int w, int h) {
    flushDraws();
    resolve(rt);
    x = std::clamp(x, 0, rt->width);
    y = std::clamp(y, 0, rt->height);
    w = std::clamp(w, 0, rt->width - x);
    h = std::clamp(h, 0, rt->height - y);
    if (!w || !h) return;
    // downscale to native resolution
    if (!g_dlTex || g_dlW < rt->width || g_dlH < rt->height) {
        g_dlW = std::max(g_dlW, rt->width);
        g_dlH = std::max(g_dlH, rt->height);
        makeTex(g_dlTex, g_dlW, g_dlH, GL_NEAREST);
        if (!g_dlFbo) glGenFramebuffers(1, &g_dlFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_dlFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_dlTex, 0);
    }
    const int s = rt->scale;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dlFbo);
    glBlitFramebuffer(x * s, y * s, (x + w) * s, (y + h) * s, 0, 0, w, h, GL_COLOR_BUFFER_BIT, s > 1 ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_dlFbo);
    std::vector<u32> px((size_t)w * h);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? drawFbo(g_cur) : 0);
    const u32 bpp = rt->format == 3 ? 4 : 2;
    const u32 base = 0x04000000 + rt->addr;
    for (int yy = 0; yy < h; yy++) {
        const u32 row = base + ((y + yy) * rt->stride + x) * bpp;
        if (!validBytes(row, w * bpp)) break;
        u8* d = memPtr(row);
        const u32* sp = px.data() + (size_t)yy * w;
        if (bpp == 4) {
            memcpy(d, sp, (size_t)w * 4);
            continue;
        }
        for (int xx = 0; xx < w; xx++) {
            const u32 c = sp[xx];
            const u32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
            u16 v;
            if (rt->format == 0) v = (u16)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
            else if (rt->format == 1) v = (u16)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15));
            else v = (u16)((r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12));
            d[xx * 2] = (u8)v;
            d[xx * 2 + 1] = (u8)(v >> 8);
        }
    }
    rt->vramHash = vramHashOf(rt);
    texInvalidate(base, rt->bytes());
}

// Copies a rectangle between memory regions; render targets on either side are handled on the GPU.
bool fbBlockTransfer(u32 src, u32 srcStride, int sx, int sy, u32 dst, u32 dstStride, int dx, int dy, int w, int h, int bpp) {
    // which render targets are involved?
    auto findRT = [&](u32 base, u32 stride, int x, int y, int& rx, int& ry) -> RenderTarget* {
        if (!vramAddr(base)) return nullptr;
        const u32 start = vramOff(base) + (y * stride + x) * bpp;
        for (auto& t : g_targets) {
            const u32 tb = t->format == 3 ? 4 : 2;
            if ((u32)bpp != tb || t->stride != stride) continue;
            if (start < t->addr || start >= t->addr + t->bytes()) continue;
            const u32 rel = start - t->addr;
            ry = (int)(rel / (t->stride * tb));
            rx = (int)((rel % (t->stride * tb)) / tb);
            return t.get();
        }
        return nullptr;
    };
    int srx = 0, sry = 0, drx = 0, dry = 0;
    RenderTarget* srt = findRT(src, srcStride, sx, sy, srx, sry);
    RenderTarget* drt = findRT(dst, dstStride, dx, dy, drx, dry);
    if (srt && srt->gpuDrawn && !drt) {
        // GPU result copied to memory (or a texture): bring the pixels down first
        fbDownload(srt, srx, sry, w, h);
    }
    // the memory side of the copy
    for (int y = 0; y < h; y++) {
        const u32 s = src + ((sy + y) * srcStride + sx) * bpp;
        const u32 d = dst + ((dy + y) * dstStride + dx) * bpp;
        if (!validBytes(s, w * bpp) || !validBytes(d, w * bpp)) continue;
        memmove(memPtr(d), memPtr(s), (size_t)w * bpp);
    }
    if (!drt) return srt != nullptr;
    if (srt && srt->gpuDrawn) {
        // render target -> render target (a snapshot when copying within the same surface)
        resolve(srt);
        const GLuint srcTex = (srt == drt) ? snapshot(srt) : srt->color;
        copyTexToTarget(drt, srcTex, (float)srx / srt->width, (float)sry / srt->height, (float)w / srt->width,
                        (float)h / srt->height, drx, dry, w, h, GL_NEAREST);
    } else {
        const int s = drt->scale;
        // memory -> render target: upload the rectangle from the (already copied) destination memory
        std::vector<u32> px((size_t)w * h);
        const u32 base = 0x04000000 + drt->addr;
        const u32 start = base + (dry * drt->stride + drx) * bpp;
        if (validBytes(start, ((h - 1) * drt->stride + w) * bpp))
            decodeFbPixels(memPtr(start), drt->stride, drt->format, w, h, px.data());
        if (!g_uploadTex) glGenTextures(1, &g_uploadTex);
        glBindTexture(GL_TEXTURE_2D, g_uploadTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(drt));
        glViewport(drx * s, dry * s, w * s, h * s);
        neutralState();
        blitTex(g_uploadTex, 0, 0, 1, 1, false, false, GL_NEAREST);
    }
    if (drt->samples) drt->msDirty = true;
    drt->gpuDrawn = true;
    drt->lastDrawFrame = g_frame;
    drt->vramHash = vramHashOf(drt);
    glBindFramebuffer(GL_FRAMEBUFFER, g_cur ? drawFbo(g_cur) : 0);
    if (g_cur) glViewport(0, 0, g_cur->width * g_cur->scale, g_cur->height * g_cur->scale);
    invalidateGLStateCache();
    return true;
}

void fbNotifyMemoryWrite(u32 addr, u32 size) {
    (void)addr;
    (void)size;
}

void fbPresent(u32 addr, u32 stride, u32 fmt) {
    flushDraws();
    applyConfigChanges();
    const Config& cfg = config();
    int winW, winH;
    host::drawableSize(winW, winH);
    GLuint srcTex = 0;
    float u0 = 0, v0 = 0, du = 1, dv = 1;
    bool alphaOne = true;
    int srcW = 480, srcH = 272;  // pixel size of the source image region (for FXAA)
    RenderTarget* rt = nullptr;
    if (addr && vramAddr(addr) && stride) {
        rt = findExact(vramOff(addr), stride, fmt);
        if (rt) {
            // CPU-written content (videos, software-drawn screens) replaces stale GPU pixels
            static const bool noPresentUpload = getenv("VC2_NO_PRESENT_UPLOAD") != nullptr;
            if (!noPresentUpload && vramHashOf(rt) != rt->vramHash) {
                static int n = 0;
                if (n++ < 20) LOG_DEBUG("GPU", "present: VRAM at %06x changed behind the GPU, uploading", rt->addr);
                uploadFromVram(rt);
            }
            resolve(rt);
            srcTex = rt->color;
            du = 480.0f / rt->width;
            dv = std::min(272.0f, (float)rt->height) / rt->height;
            srcW = 480 * rt->scale;
            srcH = std::min(272, rt->height) * rt->scale;
        }
    }
    if (!srcTex && addr && stride) {
        // never rendered by the GE: show memory directly
        g_pix.assign(480 * 272, 0xFF000000);
        const u32 bpp = fmt == 3 ? 4 : 2;
        int rows = (int)std::min<u32>(272, validBytes(addr, stride * 272 * bpp) / (stride * bpp));
        if (rows > 0) decodeFbPixels(memPtr(addr), stride, fmt, 480, rows, g_pix.data());
        if (!g_displayTex) makeTex(g_displayTex, 480, 272, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, g_displayTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, g_pix.data());
        srcTex = g_displayTex;
        srcW = 480, srcH = 272;
    }
    neutralState();
    // optional FXAA at the internal resolution
    if (srcTex && cfg.fxaa) {
        if (!g_postTex || g_postW != srcW || g_postH != srcH) {
            makeTex(g_postTex, srcW, srcH, GL_LINEAR);
            if (!g_postFbo) glGenFramebuffers(1, &g_postFbo);
            glBindFramebuffer(GL_FRAMEBUFFER, g_postFbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_postTex, 0);
            g_postW = srcW, g_postH = srcH;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, g_postFbo);
        glViewport(0, 0, srcW, srcH);
        glUseProgram(g_fxaaProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(g_fxaaTex, 0);
        int tw = rt ? rt->width * rt->scale : 480, th = rt ? rt->height * rt->scale : 272;
        glUniform2f(g_fxaaRcp, 1.0f / tw, 1.0f / th);
        glUniform4f(g_fxaaSrc, u0 + 0.5f / tw, v0 + 0.5f / th, u0 + du - 0.5f / tw, v0 + dv - 0.5f / th);
        GLint srcLoc = glGetUniformLocation(g_fxaaProg, "uSrc");
        glUniform4f(srcLoc, u0, v0, du, dv);
        glUniform1i(glGetUniformLocation(g_fxaaProg, "uFlip"), 0);
        drawFullscreenQuad();
        srcTex = g_postTex;
        u0 = 0, v0 = 0, du = 1, dv = 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, host::windowFramebuffer());
    glViewport(0, 0, winW, winH);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (srcTex) {
        int vx = 0, vy = 0, vw = winW, vh = winH;
        if (cfg.aspect == AspectMode::Keep) {
            const float target = 480.0f / 272.0f;
            vw = winW, vh = (int)lroundf(winW / target);
            if (vh > winH) vh = winH, vw = (int)lroundf(winH * target);
        } else if (cfg.aspect == AspectMode::IntegerScale) {
            int n = std::max(1, std::min(winW / 480, winH / 272));
            vw = 480 * n, vh = 272 * n;
        }
        vx = (winW - vw) / 2;
        vy = (winH - vh) / 2;
        glViewport(vx, vy, vw, vh);
        const GLenum filter = cfg.textureFilter == TexFilter::Nearest ? GL_NEAREST : GL_LINEAR;
        blitTex(srcTex, u0, v0, du, dv, true, alphaOne, filter);
        glViewport(0, 0, winW, winH);
    }
    debugFrame(winW, winH);
    host::overlayFrame();
    debugAfterOverlay(winW, winH);
    host::swapBuffers();
    invalidateGLStateCache();
    if (g_cur) {
        glBindFramebuffer(GL_FRAMEBUFFER, drawFbo(g_cur));
        glViewport(0, 0, g_cur->width * g_cur->scale, g_cur->height * g_cur->scale);
    }
    // forget targets that have not been used for a long time
    for (size_t i = 0; i < g_targets.size();) {
        RenderTarget* t = g_targets[i].get();
        if (t != g_cur && t->lastDrawFrame + 600 < g_frame && t->createdFrame + 600 < g_frame) {
            destroyTarget(t);
            g_targets.erase(g_targets.begin() + i);
            continue;
        }
        i++;
    }
}

}  // namespace vc2rt::gpu
