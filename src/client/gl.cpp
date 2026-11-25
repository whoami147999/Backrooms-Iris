#include "gl.h"

#include <cstdio>
#include <cstdlib>

#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#if !defined(IRIS_GL_VITA)
PFNGLGENBUFFERSPROC iris_glGenBuffers = nullptr;
PFNGLDELETEBUFFERSPROC iris_glDeleteBuffers = nullptr;
PFNGLBINDBUFFERPROC iris_glBindBuffer = nullptr;
PFNGLBUFFERDATAPROC iris_glBufferData = nullptr;
PFNGLCREATESHADERPROC iris_glCreateShader = nullptr;
PFNGLSHADERSOURCEPROC iris_glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC iris_glCompileShader = nullptr;
PFNGLGETSHADERIVPROC iris_glGetShaderiv = nullptr;
PFNGLGETSHADERINFOLOGPROC iris_glGetShaderInfoLog = nullptr;
PFNGLCREATEPROGRAMPROC iris_glCreateProgram = nullptr;
PFNGLATTACHSHADERPROC iris_glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC iris_glLinkProgram = nullptr;
PFNGLGETPROGRAMIVPROC iris_glGetProgramiv = nullptr;
PFNGLUSEPROGRAMPROC iris_glUseProgram = nullptr;
PFNGLGETUNIFORMLOCATIONPROC iris_glGetUniformLocation = nullptr;
PFNGLUNIFORM1IPROC iris_glUniform1i = nullptr;
PFNGLUNIFORM1FPROC iris_glUniform1f = nullptr;
PFNGLUNIFORM2FPROC iris_glUniform2f = nullptr;
PFNGLUNIFORM3FPROC iris_glUniform3f = nullptr;
PFNGLDELETEPROGRAMPROC iris_glDeleteProgram = nullptr;
PFNGLDELETESHADERPROC iris_glDeleteShader = nullptr;
#endif

namespace iris {

GLCaps g_glcaps;

void gl_load_extensions(void* (*get_proc)(const char*), bool (*has_ext)(const char*)) {
#if !defined(IRIS_GL_VITA)
    auto load = [&](const char* a, const char* b) {
        void* p = get_proc(a);
        if (!p) p = get_proc(b);
        return p;
    };
    iris_glGenBuffers = (PFNGLGENBUFFERSPROC)load("glGenBuffers", "glGenBuffersARB");
    iris_glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)load("glDeleteBuffers", "glDeleteBuffersARB");
    iris_glBindBuffer = (PFNGLBINDBUFFERPROC)load("glBindBuffer", "glBindBufferARB");
    iris_glBufferData = (PFNGLBUFFERDATAPROC)load("glBufferData", "glBufferDataARB");
    if (!iris_glGenBuffers || !iris_glBindBuffer || !iris_glBufferData || !iris_glDeleteBuffers) {
        std::fprintf(stderr, "[iris] your OpenGL driver has no vertex buffer objects (need GL 1.5+)\n");
        std::exit(1);
    }
#define IRIS_LOAD(name) iris_##name = (decltype(iris_##name))get_proc(#name)
    IRIS_LOAD(glCreateShader); IRIS_LOAD(glShaderSource); IRIS_LOAD(glCompileShader); IRIS_LOAD(glGetShaderiv);
    IRIS_LOAD(glGetShaderInfoLog); IRIS_LOAD(glCreateProgram); IRIS_LOAD(glAttachShader); IRIS_LOAD(glLinkProgram);
    IRIS_LOAD(glGetProgramiv); IRIS_LOAD(glUseProgram); IRIS_LOAD(glGetUniformLocation); IRIS_LOAD(glUniform1i);
    IRIS_LOAD(glUniform1f); IRIS_LOAD(glUniform2f); IRIS_LOAD(glUniform3f); IRIS_LOAD(glDeleteProgram); IRIS_LOAD(glDeleteShader);
#undef IRIS_LOAD
    g_glcaps.shaders = iris_glCreateShader && iris_glShaderSource && iris_glCompileShader && iris_glGetShaderiv &&
                       iris_glCreateProgram && iris_glAttachShader && iris_glLinkProgram && iris_glGetProgramiv &&
                       iris_glUseProgram && iris_glGetUniformLocation && iris_glUniform1i && iris_glUniform1f &&
                       iris_glUniform2f && iris_glUniform3f && iris_glGetShaderInfoLog;
    if (has_ext("GL_EXT_texture_filter_anisotropic") || has_ext("GL_ARB_texture_filter_anisotropic")) {
        g_glcaps.anisotropic = true;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &g_glcaps.max_aniso);
    }
#else
    (void)get_proc;
    (void)has_ext;
#endif
}

} // namespace iris
