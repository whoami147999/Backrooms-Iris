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
