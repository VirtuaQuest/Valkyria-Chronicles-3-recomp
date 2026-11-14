#include "gl.h"

#include <SDL3/SDL.h>

#include "../core/runtime.h"

#define VC2_GL_DEFINE(type, name) type p_##name = nullptr;
VC2_GL_FUNCS(VC2_GL_DEFINE)
#undef VC2_GL_DEFINE

namespace vc2rt {

bool glLoadFunctions() {
    bool ok = true;
#define VC2_GL_LOAD(type, name)                                          \
    p_##name = (type)SDL_GL_GetProcAddress(#name);                       \
    if (!p_##name && strcmp(#name, "glDebugMessageCallback") != 0) {     \
        LOG_ERROR("GL", "missing OpenGL entry point %s", #name);         \
        ok = false;                                                      \
    }
    VC2_GL_FUNCS(VC2_GL_LOAD)
#undef VC2_GL_LOAD
    return ok;
}

}  // namespace vc2rt
