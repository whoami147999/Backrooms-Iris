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

// GLSL (PC only, optional): the film post-process. Null when the driver has no shaders.
extern PFNGLCREATESHADERPROC iris_glCreateShader;
extern PFNGLSHADERSOURCEPROC iris_glShaderSource;
extern PFNGLCOMPILESHADERPROC iris_glCompileShader;
extern PFNGLGETSHADERIVPROC iris_glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC iris_glGetShaderInfoLog;
extern PFNGLCREATEPROGRAMPROC iris_glCreateProgram;
extern PFNGLATTACHSHADERPROC iris_glAttachShader;
extern PFNGLLINKPROGRAMPROC iris_glLinkProgram;
extern PFNGLGETPROGRAMIVPROC iris_glGetProgramiv;
extern PFNGLUSEPROGRAMPROC iris_glUseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC iris_glGetUniformLocation;
extern PFNGLUNIFORM1IPROC iris_glUniform1i;
extern PFNGLUNIFORM1FPROC iris_glUniform1f;
extern PFNGLUNIFORM2FPROC iris_glUniform2f;
extern PFNGLUNIFORM3FPROC iris_glUniform3f;
extern PFNGLDELETEPROGRAMPROC iris_glDeleteProgram;
extern PFNGLDELETESHADERPROC iris_glDeleteShader;
#endif

namespace iris {

struct GLCaps {
    bool anisotropic = false;
    float max_aniso = 1;
    bool shaders = false;  // GLSL 1.20 available (PC)
};
extern GLCaps g_glcaps;

// PC only: `get_proc` resolves a GL function, `has_ext` checks an extension.
void gl_load_extensions(void* (*get_proc)(const char*), bool (*has_ext)(const char*));

} // namespace iris
