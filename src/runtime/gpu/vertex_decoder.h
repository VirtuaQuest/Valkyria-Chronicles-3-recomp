#pragma once
// PSP vertex format decoding (all component formats, morphing) into a canonical float vertex.
#include "ge_state.h"

namespace vc2rt::gpu {

struct VertexLayout {
    u32 vtype = 0;
    int stride = 0;      // bytes for one complete vertex (all morph targets)
    int oneSize = 0;     // bytes for one morph target
    int morphCount = 1;
    int wtFmt = 0, wtCount = 0, wtOff = 0;
    int tcFmt = 0, tcOff = 0;
    int colFmt = 0, colOff = 0;
    int nrmFmt = 0, nrmOff = 0;
    int posFmt = 0, posOff = 0;
    bool through = false;
};

struct DecodedVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float col[4];  // 0..1 RGBA
    float wt[8];
};

void computeLayout(u32 vtype, VertexLayout& L);
// Decodes one vertex at p (morphing with g_ge.morph weights when the format has morph targets).
void decodeVertex(const VertexLayout& L, const u8* p, DecodedVertex& out);

}  // namespace vc2rt::gpu
