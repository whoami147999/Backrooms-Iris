#include "renderer.h"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "../core/config.h"
#include "gl.h"
#include "textures.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace iris {
namespace {

const float FOG_COLOR[4] = {0.24f, 0.21f, 0.12f, 1.0f};

// Halve an RGBA8 image with a box filter (for mip levels).
Image half(const Image& src) {
    Image d;
    d.w = std::max(1, src.w / 2);
    d.h = std::max(1, src.h / 2);
    d.rgba.resize((size_t)d.w * d.h * 4);
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++)
            for (int c = 0; c < 4; c++) {
                int sx = std::min(x * 2, src.w - 1), sy = std::min(y * 2, src.h - 1);
                int sx1 = std::min(sx + 1, src.w - 1), sy1 = std::min(sy + 1, src.h - 1);
                int sum = src.rgba[((size_t)sy * src.w + sx) * 4 + c] + src.rgba[((size_t)sy * src.w + sx1) * 4 + c] +
                          src.rgba[((size_t)sy1 * src.w + sx) * 4 + c] + src.rgba[((size_t)sy1 * src.w + sx1) * 4 + c];
                d.rgba[((size_t)y * d.w + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
            }
    return d;
}

uint32_t upload_texture(const Image& img) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
#if defined(IRIS_GL_VITA)
    glGenerateMipmap(GL_TEXTURE_2D);
#else
    // build the mip chain on the CPU: works on every GL version, no extension needed
    Image cur = img;
    int level = 0;
    while (cur.w > 1 || cur.h > 1) {
        cur = half(cur);
        glTexImage2D(GL_TEXTURE_2D, ++level, GL_RGBA, cur.w, cur.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur.rgba.data());
    }
    if (g_glcaps.anisotropic)
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(8.0f, g_glcaps.max_aniso));
#endif
    return t;
}

} // namespace

bool Renderer::init(const RenderSettings& s) {
    s_ = s;
    tex_[MAT_WALL] = upload_texture(make_wallpaper());
    tex_[MAT_FLOOR] = upload_texture(make_carpet());
    tex_[MAT_CEIL] = upload_texture(make_ceiling_tiles());
    tex_[MAT_LIGHT] = upload_texture(make_light_panel());

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glShadeModel(GL_SMOOTH);

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glFogf(GL_FOG_DENSITY, s_.fog_density);
    glFogfv(GL_FOG_COLOR, FOG_COLOR);
    glHint(GL_FOG_HINT, GL_NICEST);
    return true;
}

void Renderer::shutdown() {
    for (auto& kv : chunks_) {
        glDeleteBuffers(1, &kv.second.vbo);
        glDeleteBuffers(1, &kv.second.ibo);
    }
    chunks_.clear();
    glDeleteTextures(MAT_COUNT, tex_);
}

void Renderer::upload(GpuChunk& g, const ChunkMesh& m) {
    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ibo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m.verts.size() * sizeof(Vertex)), m.verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m.idx.size() * sizeof(uint16_t)), m.idx.data(), GL_STATIC_DRAW);
    for (int i = 0; i < MAT_COUNT; i++) {
        g.first[i] = m.first[i];
        g.count[i] = m.count[i];
    }
}

void Renderer::update_chunks(const Level0& level, Vec3 eye, int budget) {
    const float load = s_.view_dist + CHUNK_SIZE * 0.75f;
    int ekx = floor_to_int(eye.x / CHUNK_SIZE), ekz = floor_to_int(eye.z / CHUNK_SIZE);
    int r = (int)(load / CHUNK_SIZE) + 1;

    // distance from the eye to a chunk's square footprint
    auto dist = [&](int kx, int kz) {
        float x0 = kx * CHUNK_SIZE, z0 = kz * CHUNK_SIZE;
        float dx = std::max(std::max(x0 - eye.x, eye.x - (x0 + CHUNK_SIZE)), 0.0f);
        float dz = std::max(std::max(z0 - eye.z, eye.z - (z0 + CHUNK_SIZE)), 0.0f);
        return std::sqrt(dx * dx + dz * dz);
    };

    // unload chunks well outside the radius (hysteresis avoids thrashing at the edge)
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (dist(it->second.kx, it->second.kz) > load + CHUNK_SIZE) {
            glDeleteBuffers(1, &it->second.vbo);
            glDeleteBuffers(1, &it->second.ibo);
            it = chunks_.erase(it);
        } else ++it;
    }

    struct Want { float d; int kx, kz; };
    std::vector<Want> want;
    for (int kz = ekz - r; kz <= ekz + r; kz++)
        for (int kx = ekx - r; kx <= ekx + r; kx++) {
            float d = dist(kx, kz);
            if (d <= load && !chunks_.count(key(kx, kz))) want.push_back({d, kx, kz});
        }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.d < b.d; });
    int built = 0;
    for (const Want& w : want) {
        if (budget >= 0 && built >= budget) break;
        build_chunk_mesh(level, w.kx, w.kz, scratch_);
        GpuChunk g;
        g.kx = w.kx;
        g.kz = w.kz;
        upload(g, scratch_);
        chunks_[key(w.kx, w.kz)] = g;
        built++;
    }
}

void Renderer::draw_world(Vec3 eye, float yaw, float pitch, int width, int height) {
    glViewport(0, 0, width, height);
    glClearColor(FOG_COLOR[0], FOG_COLOR[1], FOG_COLOR[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    Mat4 proj = Mat4::perspective(s_.fov_deg * PI / 180.0f, (float)width / (float)std::max(1, height), 0.05f, s_.view_dist + 20.0f);
    Mat4 view = Mat4::fps_view(eye, yaw, pitch);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_FOG);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    Vec2 fwd = yaw_forward(yaw);
    const float half_diag = CHUNK_SIZE * 0.7072f;
    drawn_ = 0;
    for (int mat = 0; mat < MAT_COUNT; mat++) {
        glBindTexture(GL_TEXTURE_2D, tex_[mat]);
        for (auto& kv : chunks_) {
            const GpuChunk& g = kv.second;
            if (!g.count[mat]) continue;
            float cx = (g.kx + 0.5f) * CHUNK_SIZE - eye.x, cz = (g.kz + 0.5f) * CHUNK_SIZE - eye.z;
            float d = std::sqrt(cx * cx + cz * cz);
            if (d - half_diag > s_.view_dist) continue;
            // behind the camera (generous, so looking up/down never culls what you see)
            if (cx * fwd.x + cz * fwd.z < -half_diag) continue;
            glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
            glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void*)offsetof(Vertex, x));
            glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), (const void*)offsetof(Vertex, u));
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), (const void*)offsetof(Vertex, r));
            glDrawElements(GL_TRIANGLES, (GLsizei)g.count[mat], GL_UNSIGNED_SHORT,
                           (const void*)(uintptr_t)(g.first[mat] * sizeof(uint16_t)));
            if (mat == 0) drawn_++;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

void Renderer::draw_hud(float stamina, bool exhausted, int width, int height) {
    if (stamina >= 0.999f) return; // only show while it matters
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float bw = width * 0.18f, bh = std::max(3.0f, height * 0.006f);
    float x = (width - bw) * 0.5f, y = height * 0.92f;
    float fill = bw * clampf(stamina, 0, 1);
    float a = 0.55f;
    float verts[] = {
        x - 2, y - 2, x + bw + 2, y - 2, x + bw + 2, y + bh + 2, x - 2, y + bh + 2,
        x, y, x + fill, y, x + fill, y + bh, x, y + bh,
    };
    float cols[] = {
        0, 0, 0, a * 0.6f, 0, 0, 0, a * 0.6f, 0, 0, 0, a * 0.6f, 0, 0, 0, a * 0.6f,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    for (int i = 4; i < 8; i++) {
        float* c = &cols[i * 4];
        if (exhausted) { c[0] = 0.75f; c[1] = 0.25f; c[2] = 0.18f; }
        else { c[0] = 0.92f; c[1] = 0.86f; c[2] = 0.62f; }
        c[3] = a;
    }
    unsigned short quad[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, verts);
    glColorPointer(4, GL_FLOAT, 0, cols);
    glDisable(GL_CULL_FACE);
    glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, quad);
    glEnable(GL_CULL_FACE);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_BLEND);
}

} // namespace iris
