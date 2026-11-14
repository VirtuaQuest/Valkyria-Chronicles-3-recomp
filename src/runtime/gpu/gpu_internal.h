#pragma once
// Interfaces shared between the GE emulator modules (lists, draw, textures, framebuffers, GL).
#include <algorithm>
#include <cstring>
#include <vector>

#include "../host/gl.h"
#include "ge_state.h"

namespace vc2rt::gpu {

// ------------------------------------------------------------------ ge_lists.cpp
enum : u32 { DIRTY_RASTER = 1, DIRTY_TEXTURE = 2, DIRTY_FRAMEBUF = 4, DIRTY_ALL = 7 };
extern u32 g_dirty;
extern u64 g_frame;            // presented frames
extern u32 g_hashGen;          // bumped whenever the CPU may have touched memory the GE reads
extern u32 g_vaddr, g_iaddr;   // current vertex / index addresses (advanced by PRIM)
extern bool g_tracing;         // VC2_GE_TRACE: log commands and draws of one list
void deliverPendingInterrupts();

// ------------------------------------------------------------------ draw.cpp
void drawInit();
void drawPrim(u32 data);
void drawBezier(u32 data);
void drawSpline(u32 data);
bool bboxVisible(u32 count);
void loadClut();
void blockTransfer();
void flushDraws();             // submit the pending batch

// ------------------------------------------------------------------ renderer.cpp (GL backend)
struct GpuVertex {
    float x, y, z, w;  // GL clip space
    float u, v, q;     // normalized texture coords (q = projective divisor)
    u32 c0;            // primary colour RGBA8
    u32 c1;            // secondary colour RGB8 + fog factor in A
};
static_assert(sizeof(GpuVertex) == 36);

// Everything that is GL state for a batch; compared with memcmp so keep it zero-initialised.
struct DrawState {
    // raster pipeline (GL fixed function)
    u8 prim;  // 0 tris, 1 lines, 2 points
    u8 blend, blendEq, blendSrc, blendDst;  // GL enums encoded as small indices, see renderer.cpp
    u8 logicOp;                             // 0 = off, else 1 + PSP op
    u8 depthTest, depthFunc, depthWrite;
    u8 stencilTest, stencilFunc, stencilRef, stencilMask, sFail, zFail, zPass, stencilWriteMask;
    u8 colorMask;  // bit0 r .. bit3 a
    u8 depthClamp;
    u8 pad0[2];
    u32 blendColor;
    s16 scissor[4];  // x1,y1,x2,y2 (exclusive) in PSP pixels
    // texture
    u32 tex;         // GL texture name (0 = untextured)
    u8 minFilter, magFilter, wrapS, wrapT;
    float lodBias, minLod, maxLod;
    // fragment uniforms
    u8 texFunc, texAlpha, colorDouble, useQ;
    u8 rtt, rttAlphaOne, rttRepeat, flat;
    u8 alphaTest, alphaFunc, alphaRef, alphaMask;
    u8 colorTest, colorFunc, fog, clear;
    u8 srcMul, dstMul, alphaOut, alphaOutValue;  // shader-side blend factors and alpha output
    u8 secondary, depthRange, pad1[2];
    u32 colorRef, colorTestMask, texEnv, fogColor, fixA, fixB;
    float rtScale[4];  // uv * xy + zw  (render-to-texture mapping)
    float minZ, maxZ;
    float pointSize;
};

void rendererInit();
void rendererBeginTarget();    // called after a render target switch (state cache reset)
void rendererFlush();
void rendererSetState(const DrawState& st);  // starts a new batch if the state differs
GpuVertex* rendererAlloc(size_t n);           // space for n vertices in the current batch
GLuint compileProgram(const char* vs, const char* fs, const char* name);
void drawFullscreenQuad();     // uses an attribute-less VAO (gl_VertexID based)
void invalidateGLStateCache();

// ------------------------------------------------------------------ framebuffer.cpp
struct RenderTarget {
    u32 addr = 0, stride = 0, format = 0;
    int width = 0, height = 0;   // PSP pixels
    int scale = 1, samples = 0;
    GLuint fbo = 0, color = 0, depth = 0;            // resolved / single-sample
    GLuint msFbo = 0, msColor = 0, msDepth = 0;      // multisample (when samples > 0)
    bool msDirty = false;
    u64 lastDrawFrame = 0, createdFrame = 0;
    u64 vramHash = 0;            // hash of VRAM contents when we last synced with it
    bool gpuDrawn = false;       // rendered by the GPU since the last VRAM upload
    u32 zbAddr = 0;
    u32 bytes() const { return stride * height * (format == 3 ? 4 : 2); }
};
void fbInit();
RenderTarget* fbCurrent();
RenderTarget* fbBindForDraw();  // makes the target described by the GE registers current
void fbNoteDrawExtent(int x2, int y2);
// Finds a render target whose memory contains the texture at addr.
RenderTarget* fbFindForTexture(u32 addr, u32 bufw, u32 texFmt, int texW, int texH, int& offX, int& offY);
GLuint fbSampleTexture(RenderTarget* rt);  // resolves MSAA and avoids feedback loops
bool fbBlockTransfer(u32 src, u32 srcStride, int sx, int sy, u32 dst, u32 dstStride, int dx, int dy, int w, int h, int bpp);
void fbDownload(RenderTarget* rt, int x, int y, int w, int h);  // GPU -> VRAM
void fbPresent(u32 addr, u32 stride, u32 fmt);
void fbNotifyMemoryWrite(u32 addr, u32 size);
int fbScale();
void decodeFbPixels(const u8* src, u32 stride, u32 fmt, int w, int h, u32* out);

// ------------------------------------------------------------------ texture_cache.cpp
struct TexBinding {
    GLuint tex = 0;
    int w = 1, h = 1;         // PSP dimensions of level 0
    bool rtt = false, rttAlphaOne = false;
    float rtScale[4] = {1, 1, 0, 0};
    int levels = 1;
};
void texInit();
bool texLookup(TexBinding& out);
void texInvalidate(u32 addr, u32 size);
void texNewFrame();
void texClearAll();

// ------------------------------------------------------------------ texture_scaler.cpp
// Edge-aware pixel-art upscaler (xBR family). src/dst are RGBA8, dst is (w*factor)x(h*factor).
void upscaleTexture(const u32* src, int w, int h, int factor, u32* dst);

// ------------------------------------------------------------------ debug.cpp
void debugInit();
void debugFrame(int winW, int winH);  // frame dumps + scripted input, after the game image is drawn
void debugRequestTrace(u64 frame);    // GE-trace this frame (ge_lists.cpp)
void debugRequestDump(u64 frame);     // dump this frame if VC2_DUMP_DIR is set (debug.cpp)
bool savePng(const char* path, const u32* rgba, int w, int h, bool flipY, bool keepAlpha = false);

// ------------------------------------------------------------------ misc helpers
u64 hashMem(const void* p, size_t n);
inline u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | (a << 24); }
inline bool vramAddr(u32 a) { a &= 0x1FFFFFFF; return a >= 0x04000000 && a < 0x04800000; }
// Number of bytes a PSP memory range can be read from (0 if invalid).
u32 validBytes(u32 addr, u32 want);

struct GpuStats {
    u32 prims, verts, batches, textures, texUploads, fbSwitches, transfers, lists, finished, reentered, deferred;
    u64 geMicros, presentMicros, cbMicros, texMicros, fbMicros, vtxMicros;  // host CPU time in list processing / presentation
};
extern GpuStats g_stats;

}  // namespace vc2rt::gpu
