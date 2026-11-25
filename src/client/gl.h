// Backrooms Iris - one OpenGL include for every platform.
//
// The renderer sticks to the subset that exists everywhere we ship:
//   - desktop OpenGL 2.1 compatibility (Linux Mesa/NVIDIA, Windows opengl32)
//   - vitaGL on PS Vita
// i.e. fixed-function pipeline + vertex/index buffer objects + fog. Shaders (VHS, grain,
// flicker) arrive in milestone 3 behind this same header.
#pragma once

#if defined(__vita__)
#include <vitaGL.h>
#define IRIS_GL_VITA 1
#else
#include <SDL_opengl.h> // brings in the platform GL 1.1 headers + glext typedefs

// GL 1.5+ entry points aren't exported by opengl32.dll on Windows, so they are loaded at
// runtime on every PC platform (same code path on Linux and Windows).
extern PFNGLGENBUFFERSPROC iris_glGenBuffers;
extern PFNGLDELETEBUFFERSPROC iris_glDeleteBuffers;
extern PFNGLBINDBUFFERPROC iris_glBindBuffer;
extern PFNGLBUFFERDATAPROC iris_glBufferData;
#define glGenBuffers iris_glGenBuffers
#define glDeleteBuffers iris_glDeleteBuffers
#define glBindBuffer iris_glBindBuffer
#define glBufferData iris_glBufferData
#endif

namespace iris {

struct GLCaps {
    bool anisotropic = false;
    float max_aniso = 1;
};
extern GLCaps g_glcaps;

// PC only: `get_proc` resolves a GL function, `has_ext` checks an extension.
void gl_load_extensions(void* (*get_proc)(const char*), bool (*has_ext)(const char*));

} // namespace iris
