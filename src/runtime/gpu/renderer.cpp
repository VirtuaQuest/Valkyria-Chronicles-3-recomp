// OpenGL 3.3 backend: one uber shader pair, a streaming vertex ring buffer, and batching of
// consecutive primitives that share the same DrawState. PSP blending is mapped onto GL blending
// with dual-source factors (the shader premultiplies the source factor and outputs the
// destination factor as the second colour), which covers every GE factor except the doubled
// destination-alpha ones exactly.
#include "../host/config.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

namespace {

const char* kVS = R"(#version 330 core
layout(location = 0) in vec4 aPos;
layout(location = 1) in vec3 aTex;
layout(location = 2) in vec4 aC0;
layout(location = 3) in vec4 aC1;
out vec4 vC0;
out vec4 vC1;
out vec3 vTex;
uniform float uPointSize;
void main() {
    gl_Position = aPos;
    vC0 = aC0;
    vC1 = aC1;
    vTex = aTex;
    gl_PointSize = uPointSize;
}
)";

const char* kFS = R"(#version 330 core
in vec4 vC0;
in vec4 vC1;
in vec3 vTex;
layout(location = 0, index = 0) out vec4 o0;
layout(location = 0, index = 1) out vec4 o1;
uniform sampler2D uTex;
uniform ivec4 uTexCfg;    // enabled, func, alphaUsed, colorDouble
uniform ivec4 uTexCfg2;   // useQ, rtt, rttAlphaOne, rttRepeat
uniform vec4 uRtScale;
uniform vec3 uTexEnv;
uniform ivec4 uAlphaTest; // enabled, func, ref, mask
uniform ivec4 uColorTest; // enabled, func, ref (rgb packed), mask (rgb packed)
uniform ivec4 uMisc;      // fog, clear, secondary, depthRange
uniform vec3 uFogColor;
uniform ivec4 uBlend;     // srcMul, dstMul, alphaOut, alphaOutValue
uniform vec3 uFixA;
uniform vec3 uFixB;
uniform vec2 uDepthRange;

bool cmpTest(int f, int a, int b) {
    if (f == 0) return false;
    if (f == 1) return true;
    if (f == 2) return a == b;
    if (f == 3) return a != b;
    if (f == 4) return a < b;
    if (f == 5) return a <= b;
    if (f == 6) return a > b;
    return a >= b;
}

void main() {
    vec4 p = vC0;
    if (uMisc.y != 0) {   // clear mode: the vertex colour goes straight to the target
        o0 = p;
        o1 = vec4(0.0);
        return;
    }
    vec4 v = p;
    if (uTexCfg.x != 0) {
        vec2 uv = vTex.xy;
        if (uTexCfg2.x != 0) uv = (abs(vTex.z) > 1e-20) ? uv / vTex.z : vec2(0.0);
        if (uTexCfg2.y != 0) {
            if ((uTexCfg2.w & 1) != 0) uv.x = fract(uv.x);
            if ((uTexCfg2.w & 2) != 0) uv.y = fract(uv.y);
            uv = uv * uRtScale.xy + uRtScale.zw;
        }
        vec4 t = texture(uTex, uv);
        if (uTexCfg2.z != 0) t.a = 1.0;
        int fn = uTexCfg.y;
        bool texAlpha = uTexCfg.z != 0;
        if (fn == 3) {
            v = vec4(t.rgb, texAlpha ? t.a : p.a);
        } else {
            if (!texAlpha) t.a = 1.0;
            if (fn == 0) v = p * t;
            else if (fn == 1) v = vec4(mix(p.rgb, t.rgb, t.a), p.a);
            else if (fn == 2) v = vec4(mix(p.rgb, uTexEnv, t.rgb), p.a * t.a);
            else v = vec4(p.rgb + t.rgb, p.a * t.a);
        }
        if (uTexCfg.w != 0) v.rgb *= 2.0;
    }
    if (uMisc.z != 0) v.rgb += vC1.rgb;
    v = clamp(v, 0.0, 1.0);
    if (uAlphaTest.x != 0) {
        int a = int(v.a * 255.0 + 0.5);
        if (!cmpTest(uAlphaTest.y, a & uAlphaTest.w, uAlphaTest.z & uAlphaTest.w)) discard;
    }
    if (uMisc.x != 0) v.rgb = mix(uFogColor, v.rgb, vC1.a);
    if (uColorTest.x != 0) {
        ivec3 c = ivec3(v.rgb * 255.0 + 0.5);
        int cpk = c.r | (c.g << 8) | (c.b << 16);
        bool eq = (cpk & uColorTest.w) == (uColorTest.z & uColorTest.w);
        if (uColorTest.y == 0 || (uColorTest.y == 2 && !eq) || (uColorTest.y == 3 && eq)) discard;
    }
    if (uMisc.w != 0 && (gl_FragCoord.z < uDepthRange.x || gl_FragCoord.z > uDepthRange.y)) discard;
    vec3 src = v.rgb;
    float sa = v.a;
    int sm = uBlend.x;
    if (sm == 1) src *= sa;
    else if (sm == 2) src *= 1.0 - sa;
    else if (sm == 3) src *= 2.0 * sa;
    else if (sm == 4) src *= max(1.0 - 2.0 * sa, 0.0);
    else if (sm == 5) src *= uFixA;
    else if (sm == 6) src *= 2.0;
    vec3 f = vec3(0.0);
    int dm = uBlend.y;
    if (dm == 1) f = v.rgb;
    else if (dm == 2) f = 1.0 - v.rgb;
    else if (dm == 3) f = vec3(sa);
    else if (dm == 4) f = vec3(1.0 - sa);
    else if (dm == 5) f = vec3(min(2.0 * sa, 1.0));
    else if (dm == 6) f = vec3(max(1.0 - 2.0 * sa, 0.0));
    else if (dm == 7) f = uFixB;
    float ao = (uBlend.z == 1) ? float(uBlend.w) / 255.0 : sa;
    o0 = vec4(src, ao);
    o1 = vec4(f, 1.0);
}
)";

GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_emptyVao = 0;
size_t g_vboCap = 0, g_vboHead = 0;  // in vertices

struct Locs {
    GLint tex, texCfg, texCfg2, rtScale, texEnv, alphaTest, colorTest, misc, fogColor, blend, fixA, fixB,
        depthRange, pointSize;
} L;

GpuVertex* g_batchMem = nullptr;
size_t g_batchCount = 0, g_batchCap = 0;
DrawState g_batchState;
bool g_batchValid = false;

DrawState g_applied;
bool g_appliedValid = false;
GLuint g_lastTexParamsTex = 0;

const GLenum kCmp[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
const GLenum kStencilOp[8] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INVERT, GL_INCR, GL_DECR, GL_KEEP, GL_KEEP};
const GLenum kBlendEq[6] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX, GL_FUNC_SUBTRACT};
const GLenum kBlendFactor[11] = {GL_ZERO, GL_ONE, GL_SRC1_COLOR, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_DST_ALPHA,
                                 GL_ONE_MINUS_DST_ALPHA, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR,
                                 GL_ONE_MINUS_CONSTANT_COLOR};
const GLenum kLogic[16] = {GL_CLEAR, GL_AND, GL_AND_REVERSE, GL_COPY, GL_AND_INVERTED, GL_NOOP, GL_XOR, GL_OR,
                           GL_NOR, GL_EQUIV, GL_INVERT, GL_OR_REVERSE, GL_COPY_INVERTED, GL_OR_INVERTED, GL_NAND, GL_SET};

inline void enableIf(GLenum cap, bool on) { on ? glEnable(cap) : glDisable(cap); }

void rgbF(u32 c, float* o) {
    o[0] = (c & 0xFF) / 255.0f;
    o[1] = ((c >> 8) & 0xFF) / 255.0f;
    o[2] = ((c >> 16) & 0xFF) / 255.0f;
}

void applyTextureParams(const DrawState& s) {
    static const GLenum minf[8] = {GL_NEAREST, GL_LINEAR, GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                   GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf[s.minFilter & 7]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s.magFilter ? GL_LINEAR : GL_NEAREST);
    const bool clampAll = s.rtt != 0;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (s.wrapS || clampAll) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (s.wrapT || clampAll) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, s.lodBias);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, s.minLod);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, s.maxLod);
    const int aniso = config().anisotropy;
    const bool linear = (s.minFilter & 1) || s.magFilter;
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, (aniso > 1 && linear && !s.rtt) ? (float)aniso : 1.0f);
}

void applyState(const DrawState& s) {
    static int lastAniso = -1;
    if (config().anisotropy != lastAniso) {  // settings changed at runtime: re-apply sampler state
        lastAniso = config().anisotropy;
        g_appliedValid = false;
        g_lastTexParamsTex = 0;
    }
    const bool full = !g_appliedValid;
    const DrawState& a = g_applied;
    const int scale = fbScale();
    if (full) {
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glEnable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDisable(GL_DITHER);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(L.tex, 0);
    }
    if (full || memcmp(a.scissor, s.scissor, sizeof s.scissor)) {
        int x1 = std::max<int>(0, s.scissor[0]), y1 = std::max<int>(0, s.scissor[1]);
        int x2 = std::max<int>(x1, s.scissor[2]), y2 = std::max<int>(y1, s.scissor[3]);
        glScissor(x1 * scale, y1 * scale, (x2 - x1) * scale, (y2 - y1) * scale);
    }
    const bool logic = s.logicOp != 0;
    if (full || a.blend != s.blend || a.logicOp != s.logicOp) enableIf(GL_BLEND, s.blend && !logic);
    if (s.blend && (full || a.blendEq != s.blendEq || a.blendSrc != s.blendSrc || a.blendDst != s.blendDst)) {
        glBlendEquationSeparate(kBlendEq[s.blendEq], GL_FUNC_ADD);
        glBlendFuncSeparate(kBlendFactor[s.blendSrc], kBlendFactor[s.blendDst], GL_ONE, GL_ZERO);
    }
    if (full || a.logicOp != s.logicOp) {
        enableIf(GL_COLOR_LOGIC_OP, logic);
        if (logic) glLogicOp(kLogic[(s.logicOp - 1) & 15]);
    }
    if (full || a.depthTest != s.depthTest || a.depthFunc != s.depthFunc || a.depthWrite != s.depthWrite) {
        enableIf(GL_DEPTH_TEST, s.depthTest);
        glDepthFunc(kCmp[s.depthFunc & 7]);
        glDepthMask(s.depthWrite ? GL_TRUE : GL_FALSE);
    }
    if (full || a.depthClamp != s.depthClamp) enableIf(GL_DEPTH_CLAMP, s.depthClamp);
    if (full || a.stencilTest != s.stencilTest || a.stencilFunc != s.stencilFunc || a.stencilRef != s.stencilRef ||
        a.stencilMask != s.stencilMask || a.sFail != s.sFail || a.zFail != s.zFail || a.zPass != s.zPass ||
        a.stencilWriteMask != s.stencilWriteMask) {
        enableIf(GL_STENCIL_TEST, s.stencilTest);
        glStencilFunc(kCmp[s.stencilFunc & 7], s.stencilRef, s.stencilMask);
        glStencilOp(kStencilOp[s.sFail & 7], kStencilOp[s.zFail & 7], kStencilOp[s.zPass & 7]);
        glStencilMask(s.stencilTest ? s.stencilWriteMask : 0);
    }
    if (full || a.colorMask != s.colorMask)
        glColorMask((s.colorMask & 1) != 0, (s.colorMask & 2) != 0, (s.colorMask & 4) != 0, (s.colorMask & 8) != 0);
    // texture
    if (s.tex) {
        glBindTexture(GL_TEXTURE_2D, s.tex);
        if (full || g_lastTexParamsTex != s.tex || a.minFilter != s.minFilter || a.magFilter != s.magFilter ||
            a.wrapS != s.wrapS || a.wrapT != s.wrapT || a.lodBias != s.lodBias || a.minLod != s.minLod ||
            a.maxLod != s.maxLod || a.rtt != s.rtt) {
            applyTextureParams(s);
            g_lastTexParamsTex = s.tex;
        }
    }
    // uniforms
    if (full || a.tex != s.tex || a.texFunc != s.texFunc || a.texAlpha != s.texAlpha || a.colorDouble != s.colorDouble)
        glUniform4i(L.texCfg, s.tex ? 1 : 0, s.texFunc, s.texAlpha, s.colorDouble);
    if (full || a.useQ != s.useQ || a.rtt != s.rtt || a.rttAlphaOne != s.rttAlphaOne || a.rttRepeat != s.rttRepeat)
        glUniform4i(L.texCfg2, s.useQ, s.rtt, s.rttAlphaOne, s.rttRepeat);
    if (full || memcmp(a.rtScale, s.rtScale, sizeof s.rtScale)) glUniform4fv(L.rtScale, 1, s.rtScale);
    if (full || a.texEnv != s.texEnv) {
        float c[3];
        rgbF(s.texEnv, c);
        glUniform3fv(L.texEnv, 1, c);
    }
    if (full || a.alphaTest != s.alphaTest || a.alphaFunc != s.alphaFunc || a.alphaRef != s.alphaRef || a.alphaMask != s.alphaMask)
        glUniform4i(L.alphaTest, s.alphaTest, s.alphaFunc, s.alphaRef, s.alphaMask);
    if (full || a.colorTest != s.colorTest || a.colorFunc != s.colorFunc || a.colorRef != s.colorRef || a.colorTestMask != s.colorTestMask)
        glUniform4i(L.colorTest, s.colorTest, s.colorFunc, (int)(s.colorRef & 0xFFFFFF), (int)(s.colorTestMask & 0xFFFFFF));
    if (full || a.fog != s.fog || a.clear != s.clear || a.secondary != s.secondary || a.depthRange != s.depthRange)
        glUniform4i(L.misc, s.fog, s.clear, s.secondary, s.depthRange);
    if (full || a.fogColor != s.fogColor) {
        float c[3];
        rgbF(s.fogColor, c);
        glUniform3fv(L.fogColor, 1, c);
    }
    if (full || a.srcMul != s.srcMul || a.dstMul != s.dstMul || a.alphaOut != s.alphaOut || a.alphaOutValue != s.alphaOutValue)
        glUniform4i(L.blend, s.srcMul, s.dstMul, s.alphaOut, s.alphaOutValue);
    if (full || a.fixA != s.fixA) {
        float c[3];
        rgbF(s.fixA, c);
        glUniform3fv(L.fixA, 1, c);
    }
    if (full || a.fixB != s.fixB) {
        float c[3];
        rgbF(s.fixB, c);
        glUniform3fv(L.fixB, 1, c);
    }
    if (full || a.minZ != s.minZ || a.maxZ != s.maxZ) glUniform2f(L.depthRange, s.minZ, s.maxZ);
    if (full || a.pointSize != s.pointSize) {
        glUniform1f(L.pointSize, s.pointSize);
        glLineWidth(s.pointSize);
    }
    g_applied = s;
    g_appliedValid = true;
}

}  // namespace

GLuint compileProgram(const char* vs, const char* fs, const char* name) {
    auto sh = [&](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            LOG_ERROR("GPU", "%s %s shader compile failed:\n%s", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        }
        return s;
    };
    GLuint p = glCreateProgram();
    GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        LOG_ERROR("GPU", "%s program link failed:\n%s", name, log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

void rendererInit() {
    g_prog = compileProgram(kVS, kFS, "ge");
#define LOC(field, nm) L.field = glGetUniformLocation(g_prog, nm)
    LOC(tex, "uTex");
    LOC(texCfg, "uTexCfg");
    LOC(texCfg2, "uTexCfg2");
    LOC(rtScale, "uRtScale");
    LOC(texEnv, "uTexEnv");
    LOC(alphaTest, "uAlphaTest");
    LOC(colorTest, "uColorTest");
    LOC(misc, "uMisc");
    LOC(fogColor, "uFogColor");
    LOC(blend, "uBlend");
    LOC(fixA, "uFixA");
    LOC(fixB, "uFixB");
    LOC(depthRange, "uDepthRange");
    LOC(pointSize, "uPointSize");
#undef LOC
    glGenVertexArrays(1, &g_vao);
    glGenVertexArrays(1, &g_emptyVao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    g_vboCap = (16u << 20) / sizeof(GpuVertex);
    glBufferData(GL_ARRAY_BUFFER, g_vboCap * sizeof(GpuVertex), nullptr, GL_STREAM_DRAW);
    const GLsizei st = sizeof(GpuVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, st, (void*)offsetof(GpuVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, st, (void*)offsetof(GpuVertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void*)offsetof(GpuVertex, c0));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void*)offsetof(GpuVertex, c1));
    glBindVertexArray(0);
    rendererAlloc(65536);
    g_batchCount = 0;
}

void invalidateGLStateCache() {
    g_appliedValid = false;
    g_lastTexParamsTex = 0;
}

void rendererBeginTarget() { invalidateGLStateCache(); }

void rendererSetState(const DrawState& st) {
    if (g_batchValid && memcmp(&st, &g_batchState, sizeof st) == 0) return;
    rendererFlush();
    g_batchState = st;
    g_batchValid = true;
}

GpuVertex* rendererAlloc(size_t n) {
    if (g_batchCount + n > g_batchCap) {
        g_batchCap = std::max<size_t>(g_batchCap * 2, g_batchCount + n + 4096);
        GpuVertex* nb = (GpuVertex*)realloc(g_batchMem, g_batchCap * sizeof(GpuVertex));
        if (!nb) fatal("out of memory for GE vertex batch");
        g_batchMem = nb;
    }
    GpuVertex* p = g_batchMem + g_batchCount;
    g_batchCount += n;
    return p;
}

void rendererFlush() {
    if (!g_batchCount) return;
    RenderTarget* rt = fbCurrent();
    if (!rt) {
        g_batchCount = 0;
        return;
    }
    const DrawState& s = g_batchState;
    applyState(s);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    size_t n = g_batchCount;
    size_t off = 0;
    while (off < n) {
        size_t chunk = std::min(n - off, g_vboCap);
        if (s.prim == 0) chunk -= chunk % 3;
        else if (s.prim == 1) chunk -= chunk % 2;
        if (g_vboHead + chunk > g_vboCap) {
            glBufferData(GL_ARRAY_BUFFER, g_vboCap * sizeof(GpuVertex), nullptr, GL_STREAM_DRAW);
            g_vboHead = 0;
        }
        void* p = glMapBufferRange(GL_ARRAY_BUFFER, g_vboHead * sizeof(GpuVertex), chunk * sizeof(GpuVertex),
                                   GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
        if (!p) break;
        memcpy(p, g_batchMem + off, chunk * sizeof(GpuVertex));
        glUnmapBuffer(GL_ARRAY_BUFFER);
        const GLenum mode = s.prim == 0 ? GL_TRIANGLES : (s.prim == 1 ? GL_LINES : GL_POINTS);
        glDrawArrays(mode, (GLint)g_vboHead, (GLsizei)chunk);
        g_vboHead += chunk;
        off += chunk;
    }
    rt->gpuDrawn = true;
    rt->lastDrawFrame = g_frame;
    if (rt->samples) rt->msDirty = true;
    g_stats.batches++;
    g_batchCount = 0;
}

void drawFullscreenQuad() {
    glBindVertexArray(g_emptyVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

}  // namespace vc2rt::gpu
