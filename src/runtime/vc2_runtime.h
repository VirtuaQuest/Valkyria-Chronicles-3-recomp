/* Entry point exported by vc3runtime.dll and called from the generated game executable. */
#ifndef VC2_RUNTIME_H
#define VC2_RUNTIME_H

#include "psp_recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VC2_RUNTIME_ABI 1

typedef struct Vc2GameImage {
    u32 abiVersion;
    u8** mem;  /* the executable's psp_mem pointer; the runtime fills it in */
    const PspFuncEntry* funcs;
    u32 funcCount;
    const PspImportEntry* imports;
    u32 importCount;
    const PspModuleEntry* modules;
    u32 moduleCount;
} Vc2GameImage;

PSP_API int vc2_run(const Vc2GameImage* image, int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif
