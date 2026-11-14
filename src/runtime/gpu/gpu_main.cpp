// GE emulator entry points: initialisation, display framebuffer selection and presentation.
#include "gpu.h"
#include "gpu_internal.h"

namespace vc2rt::gpu {

namespace {
u32 g_dispAddr = 0, g_dispStride = 512, g_dispFmt = 3;
}

void geInitLists();
void runSelfTest();

void init() {
    geInitLists();
    rendererInit();
    fbInit();
    texInit();
    drawInit();
    debugInit();
    runSelfTest();
    LOG_INFO("GPU", "OpenGL GE renderer ready");
}

void shutdown() { texClearAll(); }

void setDisplayFramebuffer(u32 addr, u32 stride, u32 fmt) {
    g_dispAddr = addr;
    g_dispStride = stride;
    g_dispFmt = fmt & 3;
}

void present() {
    deliverPendingInterrupts();
    g_frame++;
    const u64 t0 = hostMicros();
    fbPresent(g_dispAddr, g_dispStride, g_dispFmt);
    g_stats.presentMicros += hostMicros() - t0;
    texNewFrame();
    g_hashGen++;
}

}  // namespace vc2rt::gpu
