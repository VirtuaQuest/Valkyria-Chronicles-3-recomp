// sceGe_user: thin HLE layer over the GPU backend.
#include "../gpu/gpu.h"
#include "hle.h"

namespace vc2rt {
namespace {
inline void gp(PspContext* c) { gpu::setCallerGp(c->r[28]); }
void sceGeEdramGetAddr(PspContext* c) { RET(0x04000000); }
void sceGeEdramGetSize(PspContext* c) { RET(0x00200000); }
void sceGeListEnQueue(PspContext* c) {
    gp(c);
    RET(gpu::listEnqueue(A0, A1, (s32)A2, A3, false));
}
void sceGeListEnQueueHead(PspContext* c) {
    gp(c);
    RET(gpu::listEnqueue(A0, A1, (s32)A2, A3, true));
}
void sceGeListUpdateStallAddr(PspContext* c) {
    gp(c);
    RET(gpu::listUpdateStall(A0, A1));
}
void sceGeListSync(PspContext* c) { RET(gpu::listSync(A0, A1)); }
void sceGeDrawSync(PspContext* c) { RET(gpu::drawSync(A0)); }
void sceGeSetCallback(PspContext* c) {
    gp(c);
    RET(gpu::setCallback(A0));
}
void sceGeUnsetCallback(PspContext* c) {
    gpu::unsetCallback((int)A0);
    RET(0);
}
void sceGeBreak(PspContext* c) { RET(gpu::geBreak(A0, A1)); }
void sceGeContinue(PspContext* c) {
    gp(c);
    RET(gpu::geContinue());
}
void sceGeGetCmd(PspContext* c) { RET(gpu::getCmd(A0)); }
void sceGeGetMtx(PspContext* c) { RET(gpu::getMtx(A0, A1)); }
void sceGeSaveContext(PspContext* c) { RET(gpu::saveContextTo(A0)); }
void sceGeRestoreContext(PspContext* c) { RET(gpu::restoreContextFrom(A0)); }
void sceGeEdramSetAddrTranslation(PspContext* c) {
    static u32 cur = 0x400;
    u32 old = cur;
    cur = A0;
    RET(old);
}

const HleFunction kGe[] = {
    {0xE47E40E4, "sceGeEdramGetAddr", sceGeEdramGetAddr},
    {0x1F6752AD, "sceGeEdramGetSize", sceGeEdramGetSize},
    {0xAB49E76A, "sceGeListEnQueue", sceGeListEnQueue},
    {0x1C0D95A6, "sceGeListEnQueueHead", sceGeListEnQueueHead},
    {0xE0D68148, "sceGeListUpdateStallAddr", sceGeListUpdateStallAddr},
    {0x03444EB4, "sceGeListSync", sceGeListSync},
    {0xB287BD61, "sceGeDrawSync", sceGeDrawSync},
    {0xA4FC06A4, "sceGeSetCallback", sceGeSetCallback},
    {0x05DB22CE, "sceGeUnsetCallback", sceGeUnsetCallback},
    {0xB448EC0D, "sceGeBreak", sceGeBreak},
    {0x4C06E472, "sceGeContinue", sceGeContinue},
    {0xDC93CFEF, "sceGeGetCmd", sceGeGetCmd},
    {0x57C8945B, "sceGeGetMtx", sceGeGetMtx},
    {0x438A385A, "sceGeSaveContext", sceGeSaveContext},
    {0x0BF608FB, "sceGeRestoreContext", sceGeRestoreContext},
    {0xB77905EA, "sceGeEdramSetAddrTranslation", sceGeEdramSetAddrTranslation},
};
}  // namespace
HLE_MODULE("sceGe_user", kGe);
}  // namespace vc2rt
