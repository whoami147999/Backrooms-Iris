#include "renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>

#include "../core/config.h"
#include "../core/hash.h"
#include "font.h"
#include "gl.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace iris {

uint32_t rgba(float r, float g, float b, float a) {
    auto c = [](float v) { return (uint32_t)(clampf(v, 0, 1) * 255.0f + 0.5f); };
    return (c(r) << 24) | (c(g) << 16) | (c(b) << 8) | c(a);
}

namespace {

void unpack(uint32_t c, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    r = (uint8_t)(c >> 24); g = (uint8_t)(c >> 16); b = (uint8_t)(c >> 8); a = (uint8_t)c;
}

Image half(const Image& src) {
    Image d(std::max(1, src.w / 2), std::max(1, src.h / 2));
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

uint32_t upload_texture(const Image& img, bool mips, bool nearest, bool repeat) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : (mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    if (mips) {
#if defined(IRIS_GL_VITA)
        glGenerateMipmap(GL_TEXTURE_2D);
#else
        Image cur = img;
        int level = 0;
        while (cur.w > 1 || cur.h > 1) {
            cur = half(cur);
            glTexImage2D(GL_TEXTURE_2D, ++level, GL_RGBA, cur.w, cur.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur.rgba.data());
        }
        if (g_glcaps.anisotropic)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(8.0f, g_glcaps.max_aniso));
#endif
    }
    return t;
}

void set_pointers(const Vertex* base) {
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const char*)base + offsetof(Vertex, x));
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), (const char*)base + offsetof(Vertex, u));
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), (const char*)base + offsetof(Vertex, r));
}

} // namespace

// =================================================================== setup
bool Renderer::init(const RenderSettings& s) {
    s_ = s;
    for (int i = 0; i < SPR_COUNT; i++) {
        bool nearest = i == SPR_FONT || i == SPR_PARTY_FACE || i == SPR_EXIT_SIGN;
        spr_[i] = upload_texture(make_sprite((Sprite)i), false, nearest, i == SPR_NOISE || i == SPR_SKIN);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glShadeModel(GL_SMOOTH);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glHint(GL_FOG_HINT, GL_NICEST);
    glAlphaFunc(GL_GREATER, 0.02f);
    try {
        worker_ = std::thread(&Renderer::worker_loop, this);
        threaded_ = true;
    } catch (const std::exception& e) {
        std::printf("[iris] chunk worker thread failed (%s): building on the main thread\n", e.what());
        threaded_ = false;
    }
    std::printf("[iris] renderer ready (%s)\n", threaded_ ? "worker thread" : "single thread");
    return true;
}

void Renderer::worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = jobs_.front();
            jobs_.pop_front();
        }
        Done d;
        d.kx = job.kx;
        d.kz = job.kz;
        d.gen = job.gen;
        build_chunk_mesh(job.world, job.kx, job.kz, d.mesh, s_.mesh_detail);
        std::lock_guard<std::mutex> lk(mu_);
        done_.push_back(std::move(d));
    }
}

void Renderer::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    for (auto& kv : chunks_) {
        glDeleteBuffers(1, &kv.second.vbo);
        glDeleteBuffers(1, &kv.second.ibo);
    }
    chunks_.clear();
    if (tex_[0]) glDeleteTextures(MAT_COUNT, tex_);
    glDeleteTextures(SPR_COUNT, spr_);
    if (grab_tex_) glDeleteTextures(1, &grab_tex_);
}

void Renderer::set_world(const World& w) {
    if (level_ == w.id() && seed_ == w.seed()) return;
    for (auto& kv : chunks_) {
        glDeleteBuffers(1, &kv.second.vbo);
        glDeleteBuffers(1, &kv.second.ibo);
    }
    chunks_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        jobs_.clear();
        done_.clear();
        gen_++;
    }
    inflight_.clear();
    if (level_ != w.id()) {
        if (tex_[0]) glDeleteTextures(MAT_COUNT, tex_);
        std::printf("[iris] textures: level %s\n", level_info(w.id()).number);
        Image imgs[MAT_COUNT];
        make_level_textures(w.id(), s_.texture_size, imgs);
        for (int i = 0; i < MAT_COUNT; i++) tex_[i] = upload_texture(imgs[i], true, false, true);
        std::printf("[iris] textures uploaded\n");
    }
    level_ = w.id();
    seed_ = w.seed();
    pending_ = 1;
}

void Renderer::upload(GpuChunk& g, const ChunkMesh& m) {
    glGenBuffers(1, &g.vbo);
    glGenBuffers(1, &g.ibo);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(m.verts.size() * sizeof(Vertex)), m.verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(m.idx.size() * sizeof(uint16_t)), m.idx.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    for (int i = 0; i < MAT_COUNT; i++) { g.first[i] = m.first[i]; g.count[i] = m.count[i]; }
    g.exits = m.exits;
    g.items = m.items;
    g.balloons = m.balloons;
    g.lights = m.lights;
}

void Renderer::update_chunks(const World& w, Vec3 eye, int budget) {
    const float load = s_.view_dist + CHUNK_SIZE * 0.75f;
    int ekx = floor_to_int(eye.x / CHUNK_SIZE), ekz = floor_to_int(eye.z / CHUNK_SIZE);
    int r = (int)(load / CHUNK_SIZE) + 1;
    auto dist = [&](int kx, int kz) {
        float x0 = kx * CHUNK_SIZE, z0 = kz * CHUNK_SIZE;
        float dx = std::max(std::max(x0 - eye.x, eye.x - (x0 + CHUNK_SIZE)), 0.0f);
        float dz = std::max(std::max(z0 - eye.z, eye.z - (z0 + CHUNK_SIZE)), 0.0f);
        return std::sqrt(dx * dx + dz * dz);
    };
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
    if (budget < 0 || !threaded_) {
        // build right now on this thread: everything (level load, screenshots), or the closest
        // few per frame when there's no worker thread
        int n = 0;
        for (const Want& wn : want) {
            if (budget >= 0 && n++ >= std::max(1, budget)) break;
            build_chunk_mesh(w, wn.kx, wn.kz, scratch_, s_.mesh_detail);
            GpuChunk g;
            g.kx = wn.kx;
            g.kz = wn.kz;
            upload(g, scratch_);
            chunks_[key(wn.kx, wn.kz)] = std::move(g);
        }
        pending_ = budget < 0 ? 0 : (int)want.size();
        return;
    }
    // hand the closest missing chunks to the worker (a few at a time so it stays responsive)
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const Want& wn : want) {
            if (inflight_.size() >= 3) break;
            uint64_t k = key(wn.kx, wn.kz);
            if (inflight_.count(k)) continue;
            inflight_[k] = true;
            jobs_.push_back({wn.kx, wn.kz, gen_, w});
        }
    }
    cv_.notify_one();
    // upload what's finished
    int uploaded = 0;
    for (;;) {
        Done d;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (done_.empty() || uploaded >= std::max(1, budget)) break;
            d = std::move(done_.front());
            done_.pop_front();
        }
        uint64_t k = key(d.kx, d.kz);
        if (d.gen != gen_) continue;
        inflight_.erase(k);
        if (chunks_.count(k)) continue;
        GpuChunk g;
        g.kx = d.kx;
        g.kz = d.kz;
        upload(g, d.mesh);
        chunks_[k] = std::move(g);
        uploaded++;
    }
    pending_ = (int)want.size();
}

// =================================================================== dynamic geometry
void Renderer::push_quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t col, float u0, float v0, float u1, float v1) {
    uint8_t r, g, bl, al;
    unpack(col, r, g, bl, al);
    Vertex va{a.x, a.y, a.z, u0, v1, r, g, bl, al}, vb{b.x, b.y, b.z, u1, v1, r, g, bl, al};
    Vertex vc{c.x, c.y, c.z, u1, v0, r, g, bl, al}, vd{d.x, d.y, d.z, u0, v0, r, g, bl, al};
    dyn_.insert(dyn_.end(), {va, vb, vc, va, vc, vd});
}

void Renderer::billboard(Vec3 c, float w, float h, uint32_t col, float u0, float v0, float u1, float v1) {
    Vec2 r2 = yaw_right(cam_.yaw);
    Vec3 r{r2.x * w * 0.5f, 0, r2.z * w * 0.5f};
    Vec3 up{0, h * 0.5f, 0};
    push_quad(c - r - up, c + r - up, c + r + up, c - r + up, col, u0, v0, u1, v1);
}

void Renderer::flush_dyn(uint32_t tex, bool additive, bool fog) {
    if (dyn_.empty()) return;
    glBindTexture(GL_TEXTURE_2D, tex);
    if (additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    if (!fog) glDisable(GL_FOG);
    set_pointers(dyn_.data());
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dyn_.size());
    if (additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!fog) glEnable(GL_FOG);
    dyn_.clear();
}

// A box in an entity's local space: x right, y up, z forward. Optional limb swing around
// a pivot (by default the top of the box) by `swing` radians about local x. Parts of the same
// limb (hand, claws) share the limb's pivot so they move together.
struct Part {
    float x, y, z, w, h, d;
    float r, g, b;
    float swing;
    float pivot = -1;   // pivot height (y); < 0 = top of this box
    float pz = -99;     // pivot z; -99 = this box's z
};

static void add_part(std::vector<Vertex>& out, const Part& p, Vec2 pos, float yaw, float light) {
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cs = std::cos(p.swing), ss = std::sin(p.swing);
    float hx = p.w * 0.5f, hz = p.d * 0.5f;
    float py = p.pivot >= 0 ? p.pivot : p.y + p.h;
    float pz = p.pz > -98 ? p.pz : p.z;
    auto xf = [&](float lx, float ly, float lz) {
        float ry = ly - py, rz = lz - pz;
        float sy2 = ry * cs - rz * ss, sz2 = ry * ss + rz * cs;
        ly = py + sy2;
        lz = pz + sz2;
        // local x = right, local z = forward; world: right = (cos, -sin), forward = (-sin, -cos)
        float wx = lx * cy - lz * sy;
        float wz = -lx * sy - lz * cy;
        return Vec3{pos.x + wx, ly, pos.z + wz};
    };
    float x0 = p.x - hx, x1 = p.x + hx, y0 = p.y, y1 = p.y + p.h, z0 = p.z - hz, z1 = p.z + hz;
    Vec3 c[8] = {xf(x0, y0, z0), xf(x1, y0, z0), xf(x1, y1, z0), xf(x0, y1, z0),
                 xf(x0, y0, z1), xf(x1, y0, z1), xf(x1, y1, z1), xf(x0, y1, z1)};
    const int F[6][4] = {{1, 0, 3, 2}, {4, 5, 6, 7}, {0, 4, 7, 3}, {5, 1, 2, 6}, {3, 7, 6, 2}, {0, 1, 5, 4}};
    const float shade[6] = {0.72f, 0.88f, 0.68f, 0.8f, 1.0f, 0.45f};
    for (int f = 0; f < 6; f++) {
        float k = shade[f] * light;
        uint8_t r = (uint8_t)clampf(p.r * k * 255, 0, 255), g = (uint8_t)clampf(p.g * k * 255, 0, 255), b = (uint8_t)clampf(p.b * k * 255, 0, 255);
        // a soft gradient: darker toward the feet (cheap ambient occlusion)
        Vertex v[4];
        const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (int i = 0; i < 4; i++) {
            Vec3 q = c[F[f][i]];
            float ao = clampf(0.55f + q.y * 0.45f, 0.55f, 1.0f);
            v[i] = {q.x, q.y, q.z, uv[i][0], uv[i][1], (uint8_t)(r * ao), (uint8_t)(g * ao), (uint8_t)(b * ao), 255};
        }
        out.insert(out.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
    }
}

void Renderer::draw_entity(const World& w, const DrawEntity& e, float time) {
    float light = clampf(w.light_level({e.pos.x, 1.2f, e.pos.z}) * 0.75f + 0.10f, 0.08f, 0.95f);
    float sw = std::sin(e.anim) * std::min(1.0f, e.speed / 2.0f);
    float breathe = std::sin(time * 1.6f + e.id) * 0.012f;
    std::vector<Part> parts;
    switch (e.type) {
        case EntityType::Bacteria: {
            // 2.6 m of black, wet, thin limbs. Arms to its knees, long claws, spines down its back.
            // Twitches when it notices you.
            float tw = e.state == EntState::Notice ? (hash01(e.id, (int)(time * 30), 0, 1) - 0.5f) * 0.14f : 0.0f;
            float k = 0.045f, k2 = 0.09f;
            float lean = e.state == EntState::Chase ? 0.35f : 0.12f; // hunches forward when it runs
            float arm = 2.05f;                                        // shoulder height (arm pivot)
            parts = {
                // legs: thigh-to-ankle sticks, knobbly knees, splayed feet
                {-0.1f, 0.05f, 0, 0.065f, 1.3f, 0.065f, k, k, k, sw * 0.9f},
                {0.1f, 0.05f, 0, 0.065f, 1.3f, 0.065f, k, k, k, -sw * 0.9f},
                {-0.1f, 0.62f, 0.02f, 0.09f, 0.1f, 0.09f, k2, k2, k2, sw * 0.9f, 1.35f},
                {0.1f, 0.62f, 0.02f, 0.09f, 0.1f, 0.09f, k2, k2, k2, -sw * 0.9f, 1.35f},
                {-0.11f, 0, 0.07f, 0.08f, 0.06f, 0.2f, k, k, k, sw * 0.9f, 1.35f},
                {0.11f, 0, 0.07f, 0.08f, 0.06f, 0.2f, k, k, k, -sw * 0.9f, 1.35f},
                // pelvis, a thin torso with ribs showing, a long neck, a narrow head
                {tw, 1.3f, 0, 0.26f, 0.14f, 0.12f, k, k, k, 0},
                {tw, 1.42f, 0.03f, 0.22f, 0.62f, 0.13f, k, k, k, lean, 1.42f, 0.03f},
                {tw, 1.62f, 0.1f, 0.24f, 0.03f, 0.02f, k2, k2, k2, lean, 1.42f, 0.03f},
                {tw, 1.72f, 0.1f, 0.25f, 0.03f, 0.02f, k2, k2, k2, lean, 1.42f, 0.03f},
                {tw, 1.82f, 0.1f, 0.25f, 0.03f, 0.02f, k2, k2, k2, lean, 1.42f, 0.03f},
                {tw, 2.0f, 0.04f, 0.34f, 0.07f, 0.12f, k, k, k, lean, 1.42f, 0.03f},     // shoulders
                {tw * 1.5f, 2.04f, 0.08f, 0.05f, 0.28f, 0.05f, k, k, k, lean + 0.2f, 1.42f, 0.03f},
                {tw * 2, 2.28f, 0.14f, 0.15f, 0.36f, 0.17f, k * 1.3f, k * 1.3f, k * 1.3f, lean + 0.25f, 1.42f, 0.03f},
                {tw * 2, 2.36f, 0.23f, 0.11f, 0.03f, 0.01f, 0.0f, 0.0f, 0.0f, lean + 0.25f, 1.42f, 0.03f}, // a slit of a mouth
                // spines down its back
                {tw, 1.95f, -0.05f, 0.03f, 0.14f, 0.03f, k, k, k, lean - 0.5f, 1.42f, 0.03f},
                {tw, 1.78f, -0.05f, 0.03f, 0.16f, 0.03f, k, k, k, lean - 0.6f, 1.42f, 0.03f},
                {tw, 1.6f, -0.05f, 0.03f, 0.13f, 0.03f, k, k, k, lean - 0.5f, 1.42f, 0.03f},
                // arms down to the knees, each ending in three claws
                {-0.21f + tw, 0.75f, 0.06f, 0.045f, 1.3f, 0.045f, k, k, k, -sw * 0.7f + lean, arm},
                {0.21f + tw, 0.75f, 0.06f, 0.045f, 1.3f, 0.045f, k, k, k, sw * 0.7f + lean, arm},
            };
            for (int side = -1; side <= 1; side += 2)
                for (int c = -1; c <= 1; c++)
                    parts.push_back({side * 0.21f + c * 0.025f + tw, 0.45f, 0.08f, 0.012f, 0.32f, 0.012f, k2, k2, k2,
                                     side * sw * 0.7f + lean, arm});
            break;
        }
        case EntityType::Hound: {
            // a person on all fours: grey, starved, ribs showing, a mat of black hair over its head
            // and back. The head comes up when it's staring you down.
            float sk[3] = {0.52f, 0.5f, 0.47f}, dk[3] = {0.36f, 0.34f, 0.32f};
            float head_up = e.state == EntState::Stare ? 0.2f : 0.0f;
            float lunge = e.state == EntState::Chase ? 0.08f : 0.0f;
            parts = {
                {0, 0.5f + breathe, 0, 0.36f, 0.24f, 0.92f, sk[0], sk[1], sk[2], 0},              // body
                {0, 0.48f, 0.38f, 0.42f, 0.2f, 0.22f, sk[0], sk[1], sk[2], 0},                    // shoulders
                {0, 0.53f, -0.38f, 0.32f, 0.17f, 0.18f, dk[0], dk[1], dk[2], 0},                  // hips
                // ribs (dark lines along the flanks)
                {-0.185f, 0.56f, 0.12f, 0.01f, 0.13f, 0.03f, dk[0], dk[1], dk[2], 0},
                {-0.185f, 0.56f, 0.0f, 0.01f, 0.13f, 0.03f, dk[0], dk[1], dk[2], 0},
                {-0.185f, 0.56f, -0.12f, 0.01f, 0.13f, 0.03f, dk[0], dk[1], dk[2], 0},
                {0.185f, 0.56f, 0.12f, 0.01f, 0.13f, 0.03f, dk[0], dk[1], dk[2], 0},
                {0.185f, 0.56f, 0.0f, 0.01f, 0.13f, 0.03f, dk[0], dk[1], dk[2], 0},
                {0.185f, 0.56f, -0.12f, 0.01f, 0.13f, 0.03f, dk[0], dk[1], dk[2], 0},
                // long front arms with big hands, bent back legs with feet
                {-0.18f, 0.05f, 0.42f + lunge, 0.08f, 0.5f, 0.08f, sk[0], sk[1], sk[2], sw},
                {0.18f, 0.05f, 0.42f + lunge, 0.08f, 0.5f, 0.08f, sk[0], sk[1], sk[2], -sw},
                {-0.18f, 0, 0.47f + lunge, 0.12f, 0.05f, 0.16f, sk[0], sk[1], sk[2], sw, 0.55f, 0.42f + lunge},
                {0.18f, 0, 0.47f + lunge, 0.12f, 0.05f, 0.16f, sk[0], sk[1], sk[2], -sw, 0.55f, 0.42f + lunge},
                {-0.15f, 0.05f, -0.4f, 0.09f, 0.5f, 0.1f, sk[0], sk[1], sk[2], -sw},
                {0.15f, 0.05f, -0.4f, 0.09f, 0.5f, 0.1f, sk[0], sk[1], sk[2], sw},
                {-0.15f, 0, -0.34f, 0.09f, 0.05f, 0.18f, dk[0], dk[1], dk[2], -sw, 0.55f, -0.4f},
                {0.15f, 0, -0.34f, 0.09f, 0.05f, 0.18f, dk[0], dk[1], dk[2], sw, 0.55f, -0.4f},
                // neck, head, a hanging jaw
                {0, 0.55f + head_up * 0.5f, 0.55f, 0.13f, 0.14f, 0.16f, sk[0], sk[1], sk[2], 0},
                {0, 0.55f + head_up, 0.66f, 0.24f, 0.24f, 0.26f, sk[0], sk[1], sk[2], 0},
                {0, 0.5f + head_up, 0.72f, 0.18f, 0.06f, 0.18f, dk[0] * 0.6f, dk[1] * 0.5f, dk[2] * 0.5f, 0},
                // hair: a black mat over the head and down the spine, strands hanging
                {0, 0.74f + head_up, 0.62f, 0.3f, 0.1f, 0.34f, 0.025f, 0.025f, 0.025f, 0},
                {0, 0.72f, 0.25f, 0.28f, 0.1f, 0.5f, 0.025f, 0.025f, 0.025f, 0},
                {-0.15f, 0.5f + head_up, 0.62f, 0.03f, 0.28f, 0.2f, 0.025f, 0.025f, 0.025f, 0},
                {0.15f, 0.5f + head_up, 0.62f, 0.03f, 0.28f, 0.2f, 0.025f, 0.025f, 0.025f, 0},
                {-0.16f, 0.45f, 0.3f, 0.03f, 0.3f, 0.3f, 0.025f, 0.025f, 0.025f, 0},
                {0.16f, 0.45f, 0.3f, 0.03f, 0.3f, 0.3f, 0.025f, 0.025f, 0.025f, 0},
            };
            break;
        }
        case EntityType::SkinStealer: {
            // almost a person. The skin doesn't fit: patchy, stretched, raw at the seams. Arms too
            // long, neck too long, holes where the eyes should be.
            float sk[3] = {0.80f, 0.62f, 0.54f}, pale[3] = {0.88f, 0.78f, 0.7f}, raw[3] = {0.55f, 0.12f, 0.1f};
            parts = {
                {-0.12f, 0, 0, 0.12f, 1.0f, 0.12f, sk[0], sk[1], sk[2], sw * 0.8f},
                {0.12f, 0, 0, 0.12f, 1.0f, 0.12f, pale[0], pale[1], pale[2], -sw * 0.8f},
                {-0.12f, 0.35f, 0.062f, 0.08f, 0.12f, 0.005f, raw[0], raw[1], raw[2], sw * 0.8f, 1.0f},   // a raw patch on the shin
                {0, 0.98f, 0, 0.4f, 0.24f, 0.22f, pale[0], pale[1], pale[2], 0},                          // hips
                {0, 1.2f + breathe, 0, 0.44f, 0.55f, 0.22f, sk[0] * 0.92f, sk[1] * 0.86f, sk[2] * 0.86f, 0.08f, 1.2f},
                {0.12f, 1.35f, 0.112f, 0.14f, 0.22f, 0.006f, pale[0], pale[1], pale[2], 0.08f, 1.2f},      // mismatched patch
                {-0.1f, 1.25f, 0.113f, 0.03f, 0.4f, 0.006f, raw[0], raw[1], raw[2], 0.08f, 1.2f},          // a stitched seam
                {-0.29f, 0.6f, 0, 0.08f, 1.15f, 0.08f, sk[0], sk[1], sk[2], -sw, 1.72f},                   // arms past the knees
                {0.29f, 0.6f, 0, 0.08f, 1.15f, 0.08f, pale[0], pale[1], pale[2], sw, 1.72f},
                {-0.29f, 0.47f, 0.01f, 0.1f, 0.14f, 0.06f, sk[0], sk[1], sk[2], -sw, 1.72f},               // hands
                {0.29f, 0.47f, 0.01f, 0.1f, 0.14f, 0.06f, pale[0], pale[1], pale[2], sw, 1.72f},
                {0, 1.73f, 0.01f, 0.1f, 0.2f, 0.1f, sk[0], sk[1], sk[2], 0},                               // a long neck
                {0, 1.9f, 0.03f, 0.24f, 0.32f, 0.25f, pale[0], pale[1], pale[2], 0},                       // head
                {-0.06f, 2.06f, 0.155f, 0.065f, 0.06f, 0.01f, 0.01f, 0.0f, 0.0f, 0},                       // empty eye holes
                {0.06f, 2.06f, 0.155f, 0.065f, 0.06f, 0.01f, 0.01f, 0.0f, 0.0f, 0},
                {0, 1.97f, 0.155f, 0.17f, 0.025f, 0.01f, raw[0] * 0.5f, raw[1] * 0.5f, raw[2] * 0.5f, 0},  // the mouth, too wide
                {0, 2.17f, 0.0f, 0.26f, 0.06f, 0.27f, 0.12f, 0.08f, 0.06f, 0},                             // matted hair
            };
            break;
        }
        case EntityType::Partygoer: {
            // =) a yellow party guest: round body, white gloves, a hat, a balloon (drawn below)
            float y[3] = {0.94f, 0.8f, 0.2f}, y2[3] = {0.82f, 0.68f, 0.14f};
            parts = {
                {-0.12f, 0, 0, 0.14f, 0.82f, 0.14f, y[0], y[1], y[2], sw * 0.5f},
                {0.12f, 0, 0, 0.14f, 0.82f, 0.14f, y[0], y[1], y[2], -sw * 0.5f},
                {-0.12f, 0, 0.04f, 0.16f, 0.08f, 0.24f, 0.15f, 0.1f, 0.25f, sw * 0.5f, 0.82f},           // shoes
                {0.12f, 0, 0.04f, 0.16f, 0.08f, 0.24f, 0.15f, 0.1f, 0.25f, -sw * 0.5f, 0.82f},
                {0, 0.8f, 0, 0.46f, 0.66f + breathe, 0.28f, y[0], y[1], y[2], 0},
                {0, 0.95f, 0.145f, 0.3f, 0.36f, 0.01f, y2[0], y2[1], y2[2], 0},                         // belly
                {0, 1.44f, 0, 0.52f, 0.1f, 0.3f, y[0], y[1], y[2], 0},                                    // shoulders
                {-0.3f, 0.66f, 0, 0.1f, 0.8f, 0.1f, y[0], y[1], y[2], -sw * 0.4f, 1.46f},
                {0.3f, 0.66f, 0, 0.1f, 0.8f, 0.1f, y[0], y[1], y[2], sw * 0.4f - 0.3f, 1.46f},
                {-0.3f, 0.56f, 0.01f, 0.13f, 0.13f, 0.13f, 0.95f, 0.95f, 0.92f, -sw * 0.4f, 1.46f},     // gloves
                {0.3f, 0.56f, 0.01f, 0.13f, 0.13f, 0.13f, 0.95f, 0.95f, 0.92f, sw * 0.4f - 0.3f, 1.46f},
                {0, 1.5f, 0, 0.36f, 0.36f, 0.32f, y[0], y[1], y[2], 0},
                {0, 1.86f, 0, 0.2f, 0.06f, 0.2f, 0.85f, 0.15f, 0.2f, 0},                                 // party hat, stacked
                {0, 1.92f, 0, 0.14f, 0.09f, 0.14f, 0.2f, 0.6f, 0.9f, 0},
                {0, 2.01f, 0, 0.08f, 0.09f, 0.08f, 0.85f, 0.15f, 0.2f, 0},
                {0, 2.1f, 0, 0.05f, 0.05f, 0.05f, 1.0f, 0.9f, 0.3f, 0},
            };
            break;
        }
        case EntityType::Smiler: break;
        default: break;
    }
    if (e.state == EntState::Stunned) // knocked down: everything slumps toward the floor
        for (Part& p : parts) { p.y *= 0.35f; p.h *= 0.55f; p.swing = 0.6f; p.pivot = -1; p.pz = -99; }
    if (!parts.empty()) {
        for (const Part& p : parts) add_part(dyn_, p, e.pos, e.yaw, light);
        flush_dyn(spr_[SPR_WHITE]);
    }
    Vec2 f = yaw_forward(e.yaw);
    if (e.type == EntityType::Partygoer) {
        // the =) face, and a balloon on a string
        Vec2 r = yaw_right(e.yaw);
        Vec3 c{e.pos.x + f.x * 0.165f, 1.68f, e.pos.z + f.z * 0.165f};
        Vec3 rr{r.x * 0.17f, 0, r.z * 0.17f}, up{0, 0.17f, 0};
        uint32_t lc = rgba(light, light, light);
        // the entity faces you, so its right is your left: wind the quad from its left
        push_quad(c + rr - up, c - rr - up, c - rr + up, c + rr + up, lc);
        flush_dyn(spr_[SPR_PARTY_FACE]);
        Vec3 hand{e.pos.x + r.x * 0.3f, 1.0f, e.pos.z + r.z * 0.3f};
        Vec3 bal{hand.x + std::sin(time * 1.3f + e.id) * 0.1f, 2.45f, hand.z};
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        billboard(bal, 0.38f, 0.46f, rgba(0.85f * light + 0.15f, 0.08f, 0.12f, 1));
        flush_dyn(spr_[SPR_BALLOON]);
        // the string
        push_quad({hand.x - 0.005f, hand.y, hand.z}, {hand.x + 0.005f, hand.y, hand.z}, {bal.x + 0.005f, bal.y - 0.22f, bal.z}, {bal.x - 0.005f, bal.y - 0.22f, bal.z}, rgba(0.8f, 0.8f, 0.8f));
        flush_dyn(spr_[SPR_WHITE]);
        glDisable(GL_BLEND);
    } else if (e.type == EntityType::Smiler) {
        // just the face: full bright, no fog, so it's the only thing you see in the dark
        float charge = e.state == EntState::Charge ? 1.0f : 0.0f;
        float flick = 0.85f + 0.15f * std::sin(time * 11 + e.id);
        float size = 1.35f + charge * 0.6f;
        Vec3 c{e.pos.x, 1.6f + std::sin(time * 1.7f + e.id) * 0.05f, e.pos.z};
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        billboard(c, size * 2.2f, size * 1.6f, rgba(1, 1, 0.9f, 0.06f * flick)); // a faint glow around it
        flush_dyn(spr_[SPR_GLOW], true, false);
        billboard(c, size, size, rgba(1, 1, 1, 0.9f * flick));
        flush_dyn(spr_[SPR_SMILER], false, false);
        glDepthMask(GL_TRUE);
    } else if (e.type == EntityType::Hound && e.state == EntState::Stare) {
        // eyes catch the light when it's staring back at you
        Vec2 r = yaw_right(e.yaw);
        for (int s = -1; s <= 1; s += 2) {
            Vec3 c{e.pos.x + f.x * 0.8f + r.x * 0.065f * s, 0.84f, e.pos.z + f.z * 0.8f + r.z * 0.065f * s};
            billboard(c, 0.07f, 0.07f, rgba(1, 0.9f, 0.6f, 0.9f));
        }
        flush_dyn(spr_[SPR_GLOW], true, false);
    } else if (e.type == EntityType::Bacteria && e.state == EntState::Chase) {
        // in the chase, two pin-pricks of light where its eyes would be
        Vec2 r = yaw_right(e.yaw);
        for (int s = -1; s <= 1; s += 2) {
            Vec3 c{e.pos.x + f.x * 0.4f + r.x * 0.04f * s, 2.38f, e.pos.z + f.z * 0.4f + r.z * 0.04f * s};
            billboard(c, 0.05f, 0.05f, rgba(1, 1, 1, 0.8f));
        }
        flush_dyn(spr_[SPR_GLOW], true, false);
    }
}

void Renderer::draw_player(const World& w, const DrawPlayer& p, float time) {
    static const float COLORS[4][3] = {{0.95f, 0.55f, 0.15f}, {0.2f, 0.75f, 0.7f}, {0.65f, 0.35f, 0.85f}, {0.45f, 0.8f, 0.3f}};
    const float* c = COLORS[p.id & 3];
    float light = clampf(w.light_level(p.pos + Vec3{0, 1.2f, 0}) * 0.75f + 0.15f, 0.12f, 0.95f);
    if (!p.alive) light *= 0.4f;
    float sw = std::sin(p.anim) * 0.6f;
    float cr = p.crouch * 0.45f;
    float down = p.alive ? 0.0f : 1.4f; // dead players lie on the floor
    const float jeans[3] = {0.18f, 0.22f, 0.32f}, boot[3] = {0.12f, 0.1f, 0.09f}, skin[3] = {0.85f, 0.68f, 0.56f};
    float sh = 1.43f - cr; // shoulder pivot
    std::vector<Part> parts = {
        {-0.11f, 0.06f, 0, 0.14f, 0.8f - cr, 0.15f, jeans[0], jeans[1], jeans[2], sw},
        {0.11f, 0.06f, 0, 0.14f, 0.8f - cr, 0.15f, jeans[0], jeans[1], jeans[2], -sw},
        {-0.11f, 0, 0.03f, 0.15f, 0.08f, 0.22f, boot[0], boot[1], boot[2], sw, 0.86f - cr},
        {0.11f, 0, 0.03f, 0.15f, 0.08f, 0.22f, boot[0], boot[1], boot[2], -sw, 0.86f - cr},
        {0, 0.84f - cr, 0, 0.44f, 0.62f, 0.26f, c[0], c[1], c[2], 0},                       // jacket
        {0, 0.84f - cr, 0.132f, 0.03f, 0.55f, 0.005f, c[0] * 0.6f, c[1] * 0.6f, c[2] * 0.6f, 0}, // zip
        {0, 0.95f - cr, -0.2f, 0.32f, 0.42f, 0.14f, 0.25f, 0.22f, 0.18f, 0},               // backpack
        {-0.28f, 0.88f - cr, 0, 0.11f, 0.58f, 0.11f, c[0], c[1], c[2], -sw * 0.8f, sh},
        {0.28f, 0.88f - cr, 0, 0.11f, 0.58f, 0.11f, c[0], c[1], c[2], sw * 0.8f, sh},
        {-0.28f, 0.78f - cr, 0.01f, 0.1f, 0.11f, 0.1f, skin[0], skin[1], skin[2], -sw * 0.8f, sh},
        {0.28f, 0.78f - cr, 0.01f, 0.1f, 0.11f, 0.1f, skin[0], skin[1], skin[2], sw * 0.8f, sh},
        {0, 1.45f - cr, 0, 0.12f, 0.06f, 0.12f, skin[0], skin[1], skin[2], 0},               // neck
        {0, 1.5f - cr, 0, 0.26f, 0.28f, 0.26f, skin[0], skin[1], skin[2], 0},                // head
        {0, 1.74f - cr, -0.01f, 0.28f, 0.08f, 0.28f, 0.2f, 0.14f, 0.1f, 0},                  // hair
        {-0.06f, 1.64f - cr, 0.131f, 0.04f, 0.035f, 0.005f, 0.1f, 0.1f, 0.12f, 0},           // eyes
        {0.06f, 1.64f - cr, 0.131f, 0.04f, 0.035f, 0.005f, 0.1f, 0.1f, 0.12f, 0},
    };
    if (p.light_on) parts.push_back({0.28f, 0.7f - cr, 0.12f, 0.06f, 0.06f, 0.24f, 0.2f, 0.2f, 0.22f, 1.3f, sh, 0}); // flashlight raised
    if (down > 0) for (auto& part : parts) { part.y *= 0.15f; part.h *= 0.4f; part.pivot = -1; part.pz = -99; part.swing = 0; }
    for (const Part& part : parts) add_part(dyn_, part, {p.pos.x, p.pos.z}, p.yaw, light);
    flush_dyn(spr_[SPR_WHITE]);
    (void)time;
}

// =================================================================== frame
void Renderer::begin_frame(int w, int h) {
    width = std::max(1, w);
    height = std::max(1, h);
    glViewport(0, 0, width, height);
}

bool Renderer::project(Vec3 p, float& sx, float& sy) const {
    float v[4] = {p.x, p.y, p.z, 1}, e[4], c[4];
    for (int r = 0; r < 4; r++) e[r] = view_[r] * v[0] + view_[4 + r] * v[1] + view_[8 + r] * v[2] + view_[12 + r] * v[3];
    for (int r = 0; r < 4; r++) c[r] = proj_[r] * e[0] + proj_[4 + r] * e[1] + proj_[8 + r] * e[2] + proj_[12 + r] * e[3];
    if (c[3] <= 0.05f) return false;
    sx = (c[0] / c[3] * 0.5f + 0.5f) * width;
    sy = (1 - (c[1] / c[3] * 0.5f + 0.5f)) * height;
    return true;
}

void Renderer::draw_world(const World& w, const Camera& cam, float time, float fog_mult,
                          const std::unordered_set<uint32_t>& picked, const std::vector<DrawEntity>& ents,
                          const std::vector<DrawPlayer>& players, const std::vector<DrawProp>& props,
                          const std::vector<Phantom>& phantoms) {
    cam_ = cam;
    const LevelInfo& li = w.info();
    glClearColor(li.fog[0], li.fog[1], li.fog[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float far_z = li.outdoor ? s_.view_dist + 60.0f : s_.view_dist + 20.0f;
    Mat4 proj = Mat4::perspective(cam.fov_deg * PI / 180.0f, (float)width / (float)height, 0.08f, far_z);
    Mat4 view = Mat4::fps_view(cam.eye, cam.yaw, cam.pitch);
    if (cam.roll != 0) { // tilt the horizon (sanity effects): rotate around the view axis
        float c = std::cos(cam.roll), s = std::sin(cam.roll);
        Mat4 r = Mat4::identity();
        r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c;
        Mat4 out{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                float sum = 0;
                for (int k = 0; k < 4; k++) sum += r.m[k * 4 + j] * view.m[i * 4 + k];
                out.m[i * 4 + j] = sum;
            }
        view = out;
    }
    std::copy(proj.m, proj.m + 16, proj_);
    std::copy(view.m, view.m + 16, view_);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(view.m);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_FOG);
    glFogfv(GL_FOG_COLOR, li.fog);
#if defined(IRIS_GL_VITA)
    fog_density_ = li.fog_density_vita * fog_mult;
#else
    fog_density_ = li.fog_density * fog_mult;
#endif
    glFogf(GL_FOG_DENSITY, fog_density_);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    // ----- static chunks
    Vec2 fwd = yaw_forward(cam.yaw);
    const float half_diag = CHUNK_SIZE * 0.7072f;
    std::vector<const GpuChunk*> visible;
    for (auto& kv : chunks_) {
        const GpuChunk& g = kv.second;
        float cx = (g.kx + 0.5f) * CHUNK_SIZE - cam.eye.x, cz = (g.kz + 0.5f) * CHUNK_SIZE - cam.eye.z;
        float d = std::sqrt(cx * cx + cz * cz);
        if (d - half_diag > s_.view_dist) continue;
        if (cx * fwd.x + cz * fwd.z < -half_diag) continue; // behind the camera
        visible.push_back(&g);
    }
    for (int mat = 0; mat < MAT_COUNT; mat++) {
        glBindTexture(GL_TEXTURE_2D, tex_[mat]);
        bool emissive = mat == MAT_LIGHT;
        if (emissive) glFogf(GL_FOG_DENSITY, fog_density_ * 0.45f); // lights cut through the haze a bit
        for (const GpuChunk* g : visible) {
            if (!g->count[mat]) continue;
            glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
            set_pointers(nullptr);
            glDrawElements(GL_TRIANGLES, (GLsizei)g->count[mat], GL_UNSIGNED_SHORT,
                           (const void*)(uintptr_t)(g->first[mat] * sizeof(uint16_t)));
        }
        if (emissive) glFogf(GL_FOG_DENSITY, fog_density_);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // ----- entities and other players (opaque; box models are drawn without culling)
    glDisable(GL_CULL_FACE);
    for (const DrawEntity& e : ents)
        if (e.type != EntityType::Smiler) draw_entity(w, e, time);
    for (const DrawPlayer& p : players) draw_player(w, p, time);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    for (const DrawPlayer& p : players) {
        if (!p.light_on || !p.alive) continue;
        Vec2 f = yaw_forward(p.yaw), r = yaw_right(p.yaw);
        Vec3 hand{p.pos.x + f.x * 0.35f + r.x * 0.25f, 1.25f, p.pos.z + f.z * 0.35f + r.z * 0.25f};
        billboard(hand, 0.25f, 0.25f, rgba(1, 0.97f, 0.85f, 0.9f));
        flush_dyn(spr_[SPR_GLOW], true, false);
        Vec3 tip{hand.x + f.x * 7, 1.0f, hand.z + f.z * 7}; // (not "far": that is a macro on Windows)
        Vec3 sx{r.x * 0.15f, 0, r.z * 0.15f}, sx2{r.x * 1.6f, 0, r.z * 1.6f};
        push_quad(hand - sx, hand + sx, tip + sx2, tip - sx2, rgba(1, 0.95f, 0.8f, 0.10f));
        flush_dyn(spr_[SPR_GLOW], true, true);
    }
    glDisable(GL_BLEND);

    // ----- transparent stuff
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);

    // soft contact shadows under everything that walks
    glDepthMask(GL_FALSE);
    for (const DrawEntity& e : ents)
        if (e.type != EntityType::Smiler) blob_shadow(e.pos, e.type == EntityType::Hound ? 0.75f : 0.55f, e.state == EntState::Stunned ? 0.35f : 0.5f);
    for (const DrawPlayer& p : players) blob_shadow({p.pos.x, p.pos.z}, 0.5f, 0.45f);
    glDepthMask(GL_TRUE);

    // items: little bobbing billboards with a faint glow so you can spot them nearby
    auto item_billboard = [&](ItemType t, Vec2 p, uint32_t id) {
        float bob = std::sin(time * 2.2f + (float)(id & 255)) * 0.04f;
        float l = clampf(w.light_level({p.x, 0.5f, p.z}) * 0.8f + 0.35f, 0.3f, 1.2f);
        billboard({p.x, 0.26f + bob, p.z}, 0.36f, 0.36f, rgba(l, l, l));
        flush_dyn(spr_[item_sprite(t)]);
        float d = (Vec2{cam.eye.x, cam.eye.z} - p).len();
        float pulse = (0.22f + 0.08f * std::sin(time * 3 + (float)(id & 63))) * fog_factor(d);
        if (pulse < 0.01f) return;
        billboard({p.x, 0.25f, p.z}, 0.9f, 0.9f, t == ItemType::MemoryJuice ? rgba(0.8f, 0.3f, 1, pulse) : rgba(1, 0.95f, 0.7f, pulse));
        flush_dyn(spr_[SPR_GLOW], true, false);
    };
    for (const GpuChunk* g : visible) {
        for (uint32_t id : g->items) {
            if (picked.count(id)) continue;
            int cx, cz;
            World::item_cell(id, cx, cz);
            item_billboard(w.item(cx, cz), w.item_pos(cx, cz), id);
        }
        for (const Vec3& b : g->balloons) { // Level Fun: balloons drifting under the ceiling
            static const float BC[5][3] = {{0.95f, 0.2f, 0.3f}, {0.2f, 0.55f, 0.95f}, {0.98f, 0.8f, 0.1f}, {0.3f, 0.85f, 0.4f}, {0.8f, 0.3f, 0.9f}};
            int k = (int)(std::fabs(b.x * 7 + b.z * 13)) % 5;
            float sway = std::sin(time * 0.8f + b.x) * 0.05f;
            billboard({b.x + sway, b.y + std::sin(time * 1.1f + b.z) * 0.06f, b.z}, 0.38f, 0.46f, rgba(BC[k][0], BC[k][1], BC[k][2]));
            flush_dyn(spr_[SPR_BALLOON]);
        }
    }
    // props: dropped items and burning firesalt
    for (const DrawProp& p : props) {
        float d = (Vec2{cam.eye.x, cam.eye.z} - p.pos).len();
        if (d > s_.view_dist) continue;
        if (p.kind == PropKind::Item) { item_billboard(p.item, p.pos, p.id); continue; }
        if (p.kind == PropKind::Tape) { // a VHS tape on the floor, with a faint glint so it can be found
            float light = clampf(w.light_level({p.pos.x, 0.5f, p.pos.z}) + 0.25f, 0.2f, 1.3f);
            float yaw = (uint8_t)p.item / 255.0f * 2 * PI;
            Part case_{0, 0, 0, 0.19f, 0.026f, 0.105f, 0.06f, 0.06f, 0.07f, 0};
            Part label{0, 0.026f, -0.01f, 0.13f, 0.004f, 0.06f, 0.85f, 0.82f, 0.7f, 0};
            Part reel1{-0.045f, 0.026f, 0.03f, 0.04f, 0.003f, 0.03f, 0.25f, 0.25f, 0.27f, 0};
            Part reel2{0.045f, 0.026f, 0.03f, 0.04f, 0.003f, 0.03f, 0.25f, 0.25f, 0.27f, 0};
            for (const Part& pt : {case_, label, reel1, reel2}) add_part(dyn_, pt, p.pos, yaw, light);
            flush_dyn(spr_[SPR_WHITE]);
            float pulse = 0.5f + 0.5f * std::sin(time * 2.5f + p.id);
            billboard({p.pos.x, 0.12f, p.pos.z}, 0.55f, 0.55f, rgba(0.6f, 0.85f, 1.0f, 0.10f + 0.12f * pulse));
            flush_dyn(spr_[SPR_GLOW], true, false);
            continue;
        }
        if (p.kind == PropKind::Breaker) { // a grey fuse box on the wall, red light until it's switched on
            static const float NX[4] = {1, -1, 0, 0}, NZ[4] = {0, 0, 1, -1};
            int side = (int)(uint8_t)p.item & 3;
            Vec2 n{NX[side], NZ[side]};
            float yaw = std::atan2(-n.x, -n.z); // its front faces out of the wall
            float light = clampf(w.light_level({p.pos.x + n.x * 0.5f, 1.4f, p.pos.z + n.z * 0.5f}) + 0.12f, 0.12f, 1.3f);
            bool on = p.life > 0.5f;
            Vec2 at = p.pos + n * 0.07f;
            Part box{0, 1.12f, 0, 0.38f, 0.52f, 0.13f, 0.48f, 0.5f, 0.5f, 0};
            Part door{0, 1.15f, 0.067f, 0.34f, 0.46f, 0.01f, 0.40f, 0.42f, 0.42f, 0};
            Part handle{0.1f, on ? 1.42f : 1.30f, 0.09f, 0.04f, 0.12f, 0.04f, 0.08f, 0.08f, 0.08f, 0};
            Part conduit{-0.12f, 1.64f, -0.02f, 0.05f, 1.2f, 0.05f, 0.35f, 0.36f, 0.36f, 0};
            for (const Part& pt : {box, door, handle, conduit}) add_part(dyn_, pt, at, yaw, light);
            flush_dyn(spr_[SPR_WHITE]);
            float blink = on ? 1.0f : (std::sin(time * 6 + p.id) > 0 ? 1.0f : 0.3f);
            Vec3 led{at.x + n.x * 0.08f, 1.55f, at.z + n.z * 0.08f};
            Vec2 rt = yaw_right(yaw);
            led.x -= rt.x * 0.1f;
            led.z -= rt.z * 0.1f;
            billboard(led, 0.09f, 0.09f, on ? rgba(0.3f, 1, 0.4f, blink) : rgba(1, 0.15f, 0.1f, blink));
            billboard(led, 0.5f, 0.5f, on ? rgba(0.3f, 1, 0.4f, 0.25f) : rgba(1, 0.15f, 0.1f, 0.22f * blink));
            flush_dyn(spr_[SPR_GLOW], true, false);
            continue;
        }
        float fl = 0.85f + 0.15f * std::sin(time * 17 + p.id) * std::sin(time * 7.3f);
        float fade = clampf(p.life / 3.0f, 0, 1); // dies down at the end
        billboard({p.pos.x, 0.22f, p.pos.z}, 0.35f * fl, 0.5f * fl, rgba(1, 1, 1, fade));
        flush_dyn(spr_[SPR_FLAME], true, false);
        // a pool of orange light on the floor around it
        float r = FLARE_RADIUS * 0.8f * fl;
        push_quad({p.pos.x - r, 0.03f, p.pos.z + r}, {p.pos.x + r, 0.03f, p.pos.z + r}, {p.pos.x + r, 0.03f, p.pos.z - r}, {p.pos.x - r, 0.03f, p.pos.z - r},
                  rgba(1, 0.45f, 0.12f, 0.45f * fade));
        flush_dyn(spr_[SPR_GLOW], true, false);
        billboard({p.pos.x, 1.0f, p.pos.z}, 3.0f * fl, 3.0f * fl, rgba(1, 0.5f, 0.15f, 0.25f * fade));
        flush_dyn(spr_[SPR_GLOW], true, false);
    }
    // exit signs: a small green sign over the door. On hard and nightmare it barely glows.
    glDepthMask(GL_FALSE);
    const bool dim_signs = w.difficulty() >= Difficulty::Hard;
    for (const GpuChunk* g : visible)
        for (const ExitDoor& ex : g->exits) {
            Vec3 n{ex.normal.x, 0, ex.normal.z}, t{n.z, 0, -n.x};
            Vec3 c = Vec3{ex.pos.x, 0, ex.pos.z} + n * 0.08f;
            float y0 = 2.26f, y1 = 2.46f;
            float lum = dim_signs ? 0.45f : 1.0f;
            push_quad(c - t * 0.3f + Vec3{0, y0, 0}, c + t * 0.3f + Vec3{0, y0, 0}, c + t * 0.3f + Vec3{0, y1, 0}, c - t * 0.3f + Vec3{0, y1, 0},
                      rgba(lum, lum, lum));
            flush_dyn(spr_[SPR_EXIT_SIGN], false, false);
            if (!dim_signs) {
                float ed = (Vec2{cam.eye.x, cam.eye.z} - ex.pos).len();
                billboard(c + n * 0.15f + Vec3{0, 2.36f, 0}, 1.2f, 0.7f, rgba(0.3f, 1, 0.4f, 0.16f * std::max(0.3f, fog_factor(ed))));
                flush_dyn(spr_[SPR_GLOW], true, false);
            }
        }
    if (s_.atmosphere) draw_atmosphere(w, visible, time);
    // smilers last: their faces float in the dark
    for (const DrawEntity& e : ents)
        if (e.type == EntityType::Smiler) draw_entity(w, e, time);
    // hallucinations
    for (const Phantom& ph : phantoms) {
        if (ph.alpha <= 0.01f) continue;
        switch (ph.kind) {
            case PhantomKind::Shadow:
                billboard(ph.pos + Vec3{0, 1.0f, 0}, 0.9f, 2.0f, rgba(1, 1, 1, ph.alpha));
                flush_dyn(spr_[SPR_SHADOW]);
                break;
            case PhantomKind::FakeItem: {
                Sprite s = ph.variant == 0 ? SPR_ALMOND : (ph.variant == 1 ? SPR_ENERGY : SPR_JUICE);
                billboard(ph.pos + Vec3{0, 0.26f, 0}, 0.36f, 0.36f, rgba(1, 1, 1, ph.alpha));
                flush_dyn(spr_[s]);
                break;
            }
            case PhantomKind::Figure: {
                DrawPlayer fake{ph.variant, ph.pos, std::atan2(-(cam.eye.x - ph.pos.x), -(cam.eye.z - ph.pos.z)), 0, 0, true, ""};
                glDisable(GL_BLEND);
                draw_player(w, fake, time);
                glEnable(GL_BLEND);
                break;
            }
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

void Renderer::post_warp(float wobble, float ghost, float time) {
#if defined(IRIS_GL_VITA)
    (void)wobble; (void)ghost; (void)time; // overlays only on Vita (keeps the GPU budget sane)
#else
    if (!s_.post_effects || (wobble <= 0.001f && ghost <= 0.001f)) return;
    grab_screen();
    float umax = (float)width / grab_w_, vmax = (float)height / grab_h_;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    // a warped grid: wavy, breathing, slightly swimming
    const int N = 24;
    auto pass = [&](float ox, float oy, float alpha) {
        dyn_.clear();
        uint8_t a = (uint8_t)(clampf(alpha, 0, 1) * 255);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                Vertex q[4];
                for (int k = 0; k < 4; k++) {
                    int gx = x + (k == 1 || k == 2), gy = y + (k >= 2);
                    float u = (float)gx / N, v = (float)gy / N;
                    float edge = std::sin(u * PI) * std::sin(v * PI); // keep the screen border fixed
                    float du = std::sin(v * 9 + time * 1.7f) * 0.012f * wobble * edge + std::sin(v * 23 + time * 3.1f) * 0.004f * wobble * edge;
                    float dv = std::cos(u * 7 + time * 1.3f) * 0.012f * wobble * edge;
                    float breathe = 1.0f + std::sin(time * 1.1f) * 0.015f * wobble;
                    float su = (u - 0.5f) * breathe + 0.5f + du + ox, sv = (v - 0.5f) * breathe + 0.5f + dv + oy;
                    q[k] = {u, v, 0, clampf(su, 0, 1) * umax, clampf(sv, 0, 1) * vmax, 255, 255, 255, a};
                }
                dyn_.insert(dyn_.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
            }
        set_pointers(dyn_.data());
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dyn_.size());
        dyn_.clear();
    };
    pass(0, 0, 1.0f);
    if (ghost > 0.001f) { // double vision
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        pass(std::sin(time * 0.7f) * 0.02f * ghost, std::cos(time * 0.5f) * 0.006f * ghost, 0.45f * ghost);
        glDisable(GL_BLEND);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_DEPTH_TEST);
#endif
}

void Renderer::grab_screen() {
#if !defined(IRIS_GL_VITA)
    int tw = 1, th = 1;
    while (tw < width) tw <<= 1;
    while (th < height) th <<= 1;
    if (!grab_tex_ || tw != grab_w_ || th != grab_h_) {
        if (grab_tex_) glDeleteTextures(1, &grab_tex_);
        glGenTextures(1, &grab_tex_);
        glBindTexture(GL_TEXTURE_2D, grab_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE); // blurry levels for bloom, rebuilt on every copy
        std::vector<uint8_t> zero((size_t)tw * th * 4, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
        grab_w_ = tw;
        grab_h_ = th;
    }
    glBindTexture(GL_TEXTURE_2D, grab_tex_);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
#endif
}

void Renderer::blob_shadow(Vec2 p, float r, float a) {
    push_quad({p.x - r, 0.015f, p.z + r}, {p.x + r, 0.015f, p.z + r}, {p.x + r, 0.015f, p.z - r}, {p.x - r, 0.015f, p.z - r}, rgba(0, 0, 0, a));
    flush_dyn(spr_[SPR_GLOW], false, true);
}

// Light halos, soft shafts under the lamps, flickering tubes and dust in the air.
void Renderer::draw_atmosphere(const World& w, const std::vector<const GpuChunk*>& visible, float time) {
    const LevelInfo& li = w.info();
    const float* lc = li.light_color;
    const float bright_level = li.ambient[0] > 0.4f ? 0.25f : 1.0f; // daylight-ish levels: halos would wash out
    Vec2 eye{cam_.eye.x, cam_.eye.z};
    Vec2 fwd = yaw_forward(cam_.yaw);
    glDisable(GL_ALPHA_TEST);
    flicker_dim_ = 0;
    // flicker: mostly on, now and then a stutter of dark frames
    auto flicker = [&](const ChunkMesh::LightMark& l) {
        if (l.state != LightState::Flicker) return 1.0f;
        int seed = (int)(l.pos.x * 7.0f) * 131 + (int)(l.pos.z * 7.0f);
        float slow = std::sin(time * 0.9f + seed) * 0.5f + 0.5f;           // when it's having a bad moment
        if (slow < 0.65f) return 1.0f;
        return hash01((uint32_t)seed, (int)(time * 18), 0, 7) < 0.45f ? 0.08f : 1.0f;
    };
    // batched: one draw for every halo, one for every shaft, one for tubes that are out
    std::vector<Vertex> halos, shafts, outs;
    for (const GpuChunk* g : visible)
        for (const auto& l : g->lights) {
            if (l.state == LightState::Dead || l.state == LightState::None) continue;
            Vec2 lp{l.pos.x, l.pos.z};
            Vec2 d = lp - eye;
            float dist = d.len();
            if (dist > s_.view_dist) continue;
            if (dist > 3.0f && (d.x * fwd.x + d.z * fwd.z) < -2.0f) continue; // behind us
            float k = flicker(l);
            float fade = std::max(0.0f, fog_factor(dist));
            if (l.state == LightState::Flicker && dist < 7.0f) flicker_dim_ = std::max(flicker_dim_, (1.0f - k) * 0.35f * (1.0f - dist / 7.0f));
            if (k < 0.5f) { // the tube cuts out: cover the baked bright panel
                if (l.sx > 0) {
                    float y = l.pos.y - 0.004f;
                    push_quad({l.pos.x - l.sx, y, l.pos.z + l.sz}, {l.pos.x + l.sx, y, l.pos.z + l.sz}, {l.pos.x + l.sx, y, l.pos.z - l.sz},
                              {l.pos.x - l.sx, y, l.pos.z - l.sz}, rgba(0.05f, 0.05f, 0.05f, 0.9f));
                    outs.insert(outs.end(), dyn_.end() - 6, dyn_.end());
                    dyn_.clear();
                }
                continue;
            }
            float big = l.sx < 0 ? 1.6f : 1.0f + (l.sx + l.sz) * 0.9f;
            // halo around the fixture
            billboard({l.pos.x, l.pos.y - (l.sx < 0 ? -0.15f : 0.06f), l.pos.z}, big, big * (l.sx < 0 ? 0.6f : 0.75f),
                      rgba(lc[0], lc[1], lc[2], 0.20f * k * fade * bright_level));
            halos.insert(halos.end(), dyn_.end() - 6, dyn_.end());
            dyn_.clear();
            // a shaft of light down to the floor, always turned toward the camera
            if (l.sx > 0 && !li.outdoor) {
                Vec2 r = d.len() > 0.01f ? Vec2{-d.z / dist, d.x / dist} : Vec2{1, 0};
                float top = std::max(l.sx, l.sz) * 1.2f, bot = 1.3f + top;
                Vec3 t0{l.pos.x - r.x * top, l.pos.y, l.pos.z - r.z * top}, t1{l.pos.x + r.x * top, l.pos.y, l.pos.z + r.z * top};
                Vec3 b0{l.pos.x - r.x * bot, 0.0f, l.pos.z - r.z * bot}, b1{l.pos.x + r.x * bot, 0.0f, l.pos.z + r.z * bot};
                float a = 0.075f * k * fade * bright_level * clampf(dist / 2.5f, 0, 1); // fades out when you stand in it
                push_quad(b0, b1, t1, t0, rgba(lc[0], lc[1], lc[2], a));
                shafts.insert(shafts.end(), dyn_.end() - 6, dyn_.end());
                dyn_.clear();
            }
        }
    dyn_ = outs;
    flush_dyn(spr_[SPR_WHITE], false, true);
    dyn_ = halos;
    flush_dyn(spr_[SPR_GLOW], true, false);
    dyn_ = shafts;
    flush_dyn(spr_[SPR_CONE], true, false);
    // dust drifting in the air around you (catches the light)
    if (!li.outdoor) {
        float amb = clampf(w.light_level(cam_.eye) * 0.6f, 0.05f, 0.7f);
        const float R = 5.0f;
        for (int i = 0; i < 70; i++) {
            float hx = hash01(911, i, 0, 1), hz = hash01(911, i, 1, 1), hy = hash01(911, i, 2, 1);
            float x = cam_.eye.x + std::fmod(hx * 2 * R + time * (0.05f + hx * 0.08f) + 1000.0f * R, 2 * R) - R;
            float z = cam_.eye.z + std::fmod(hz * 2 * R + std::sin(time * 0.3f + i) * 0.4f + 1000.0f * R, 2 * R) - R;
            float y = std::fmod(hy * li.ceil_h + time * 0.04f * (hy - 0.5f) + 100.0f * li.ceil_h, li.ceil_h);
            float tw = 0.5f + 0.5f * std::sin(time * 1.7f + i * 2.3f);
            billboard({x, y, z}, 0.022f, 0.022f, rgba(1, 0.96f, 0.85f, amb * (0.35f + 0.65f * tw)));
        }
        flush_dyn(spr_[SPR_GLOW], true, true);
    }
    glEnable(GL_ALPHA_TEST);
}

// ------------------------------------------------------------------ film post-process (PC)
#if !defined(IRIS_GL_VITA)
namespace {
const char* FILM_VS = R"(#version 120
varying vec2 uv;
void main() { uv = gl_MultiTexCoord0.xy; gl_Position = ftransform(); }
)";
const char* FILM_FS = R"(#version 120
uniform sampler2D scene;
uniform vec2 uvmax;
uniform vec2 res;
uniform float time, grain, aberr, vignette, bloom, sat, contrast, distort, cam, dim;
uniform vec3 lift;
varying vec2 uv;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
vec3 tap(vec2 p, float bias) { return texture2D(scene, clamp(p, vec2(0.002), vec2(0.998)) * uvmax, bias).rgb; }
void main() {
    vec2 c = uv - 0.5;
    float r2 = dot(c, c);
    vec2 p = 0.5 + c * (1.0 - distort * r2);
    if (cam > 0.0) { // a tracking band rolls up the picture; lines inside it jitter sideways
        float band = 1.0 - fract(time * 0.11);
        float k = smoothstep(0.045, 0.0, abs(uv.y - band)) * cam;
        p.x += (hash(vec2(floor(uv.y * res.y * 0.5), floor(time * 24.0))) - 0.5) * 0.02 * k;
        p.x += sin(uv.y * 300.0 + time * 13.0) * 0.0007 * cam;
    }
    vec2 off = c * 2.0 * aberr / res;
    vec3 col = vec3(tap(p + off, 0.0).r, tap(p, 0.0).g, tap(p - off, 0.0).b);
    vec3 b1 = tap(p, 2.5), b2 = tap(p, 4.0), b3 = tap(p, 5.5);
    vec3 glow = max(b1 - 0.55, 0.0) * 0.5 + max(b2 - 0.42, 0.0) * 0.7 + max(b3 - 0.3, 0.0) * 0.9;
    col += glow * bloom;
    col = lift + col * (1.0 - lift);
    col = clamp(col, 0.0, 1.0);
    col = mix(col, col * col * (3.0 - 2.0 * col), contrast);
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, sat);
    col *= 1.0 - dim;
    if (cam > 0.0) {
        col *= 0.90 + 0.10 * sin(uv.y * res.y * 1.5708);
        col = mix(col, vec3(l) * vec3(1.06, 1.0, 0.88), 0.18 * cam);
        float k = smoothstep(0.045, 0.0, abs(uv.y - (1.0 - fract(time * 0.11))));
        col += (hash(uv * res + time) - 0.5) * 0.25 * k * cam;
    }
    col *= 1.0 - vignette * smoothstep(0.06, 0.5, r2);
    float n = hash(uv * res + fract(time * 13.7) * 91.0) - 0.5;
    col += n * grain * (1.3 - l);
    gl_FragColor = vec4(col, 1.0);
}
)";
uint32_t compile(uint32_t type, const char* src) {
    GLuint s = iris_glCreateShader(type);
    iris_glShaderSource(s, 1, &src, nullptr);
    iris_glCompileShader(s);
    GLint ok = 0;
    iris_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        iris_glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::printf("[iris] film shader: %s\n", log);
        return 0;
    }
    return s;
}
} // namespace
#endif

bool Renderer::post_film(const FilmParams& f) {
#if defined(IRIS_GL_VITA)
    (void)f;
    return false;
#else
    if (!s_.film || film_failed_ || !g_glcaps.shaders) return false;
    if (!film_prog_) {
        GLuint vs = compile(GL_VERTEX_SHADER, FILM_VS), fs = compile(GL_FRAGMENT_SHADER, FILM_FS);
        if (!vs || !fs) { film_failed_ = true; return false; }
        GLuint p = iris_glCreateProgram();
        iris_glAttachShader(p, vs);
        iris_glAttachShader(p, fs);
        iris_glLinkProgram(p);
        GLint ok = 0;
        iris_glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { std::printf("[iris] film shader didn't link: film look off\n"); film_failed_ = true; return false; }
        film_prog_ = p;
        const char* names[] = {"scene", "uvmax", "res", "time", "grain", "aberr", "vignette", "bloom", "sat", "contrast", "distort", "cam", "dim", "lift"};
        for (int i = 0; i < 14; i++) film_loc_[i] = iris_glGetUniformLocation(p, names[i]);
        std::printf("[iris] film look on (GLSL)\n");
    }
    grab_screen();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, grab_tex_);
    iris_glUseProgram(film_prog_);
    iris_glUniform1i(film_loc_[0], 0);
    iris_glUniform2f(film_loc_[1], (float)width / grab_w_, (float)height / grab_h_);
    iris_glUniform2f(film_loc_[2], (float)width, (float)height);
    iris_glUniform1f(film_loc_[3], std::fmod(f.time, 1000.0f));
    iris_glUniform1f(film_loc_[4], f.grain);
    iris_glUniform1f(film_loc_[5], f.aberration * (float)height / 540.0f);
    iris_glUniform1f(film_loc_[6], f.vignette);
    iris_glUniform1f(film_loc_[7], f.bloom);
    iris_glUniform1f(film_loc_[8], f.saturation);
    iris_glUniform1f(film_loc_[9], f.contrast);
    iris_glUniform1f(film_loc_[10], f.distort);
    iris_glUniform1f(film_loc_[11], f.camcorder ? 1.0f : 0.0f);
    iris_glUniform1f(film_loc_[12], f.dim);
    iris_glUniform3f(film_loc_[13], f.lift[0], f.lift[1], f.lift[2]);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    dyn_.clear();
    Vertex q[4] = {{0, 0, 0, 0, 0, 255, 255, 255, 255}, {1, 0, 0, 1, 0, 255, 255, 255, 255},
                   {1, 1, 0, 1, 1, 255, 255, 255, 255}, {0, 1, 0, 0, 1, 255, 255, 255, 255}};
    dyn_.insert(dyn_.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
    set_pointers(dyn_.data());
    glDrawArrays(GL_TRIANGLES, 0, 6);
    dyn_.clear();
    iris_glUseProgram(0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_DEPTH_TEST);
    return true;
#endif
}

void Renderer::flashlight_overlay(float strength, float flicker) {
    if (strength <= 0.01f) return;
    begin_2d();
    // dst * (1 + src) per pass: brightens what's in the beam without lifting pure black.
    // Stacking passes lets the beam pull real detail out of a nearly black level.
    glBlendFunc(GL_DST_COLOR, GL_ONE);
    glBindTexture(GL_TEXTURE_2D, spr_[SPR_BEAM]);
    float sz = (float)std::max(width, height) * 0.95f;
    int passes = strength > 1.2f ? 3 : (strength > 1.0f ? 2 : 1);
    float k = clampf((strength > 1.0f ? 1.0f : strength) * flicker, 0, 1);
    for (int i = 0; i < passes; i++) {
        float grow = 1.0f - i * 0.18f; // each pass a bit tighter: a hot centre, soft edges
        float w2 = sz * grow / 2;
        push_quad({width * 0.5f - w2, height * 0.52f + w2, 0}, {width * 0.5f + w2, height * 0.52f + w2, 0},
                  {width * 0.5f + w2, height * 0.52f - w2, 0}, {width * 0.5f - w2, height * 0.52f - w2, 0}, rgba(k, k, k, 1));
        set_pointers(dyn_.data());
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dyn_.size());
        dyn_.clear();
    }
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    end_2d();
}

// =================================================================== 2D
void Renderer::begin_2d() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
}

void Renderer::end_2d() {
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

void Renderer::rect(float x, float y, float w, float h, uint32_t c) { sprite(SPR_WHITE, x, y, w, h, c); }

void Renderer::tiled(Sprite s, float x, float y, float w, float h, uint32_t c, float tile_px, float ox, float oy) {
    push_quad({x, y + h, 0}, {x + w, y + h, 0}, {x + w, y, 0}, {x, y, 0}, c, ox, oy, ox + w / tile_px, oy + h / tile_px);
    flush_dyn(spr_[s], false, true);
}

void Renderer::sprite(Sprite s, float x, float y, float w, float h, uint32_t c, float rot, bool additive) {
    float cx = x + w * 0.5f, cy = y + h * 0.5f, co = std::cos(rot), si = std::sin(rot);
    auto P = [&](float lx, float ly) { return Vec3{cx + lx * co - ly * si, cy + lx * si + ly * co, 0}; };
    Vec3 a = P(-w / 2, h / 2), b = P(w / 2, h / 2), cc = P(w / 2, -h / 2), d = P(-w / 2, -h / 2);
    // push_quad maps a/b to v1 (bottom); in screen space y grows down, so flip v
    push_quad(a, b, cc, d, c, 0, 0, 1, 1);
    for (size_t i = dyn_.size() - 6; i < dyn_.size(); i++) dyn_[i].v = 1 - dyn_[i].v;
    flush_dyn(spr_[s], additive, true);
}

float Renderer::text(float x, float y, float px, const std::string& s, uint32_t c, int align) {
    // a soft drop shadow keeps text readable on bright levels (pool tiles, the city)
    uint8_t ca = (uint8_t)c;
    if (ca > 40 && (c >> 8) != 0) text_raw(x + std::max(1.0f, px * 0.5f), y + std::max(1.0f, px * 0.5f), px, s, (uint32_t)(ca * 0.55f), align);
    return text_raw(x, y, px, s, c, align);
}

float Renderer::text_raw(float x, float y, float px, const std::string& s, uint32_t c, int align) {
    float w = text_width_px_f(s) * px;
    if (align == 1) x -= w * 0.5f;
    else if (align == 2) x -= w;
    x = std::floor(x);
    y = std::floor(y);
    for (size_t i = 0; i < s.size(); i++) {
        int ch = (unsigned char)s[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        ch -= 32;
        if (ch <= 0 || ch >= FONT_COLS * FONT_ROWS) continue;
        float u0 = (float)(ch % FONT_COLS) * FONT_CELL / FONT_ATLAS_W, v0 = (float)(ch / FONT_COLS) * FONT_CELL / FONT_ATLAS_H;
        float u1 = u0 + 5.0f / FONT_ATLAS_W, v1 = v0 + 7.0f / FONT_ATLAS_H;
        float gx = x + i * 6 * px;
        Vec3 a{gx, y + 7 * px, 0}, b{gx + 5 * px, y + 7 * px, 0}, cc{gx + 5 * px, y, 0}, d{gx, y, 0};
        uint8_t r, g, bl, al;
        unpack(c, r, g, bl, al);
        Vertex va{a.x, a.y, 0, u0, v1, r, g, bl, al}, vb{b.x, b.y, 0, u1, v1, r, g, bl, al};
        Vertex vc{cc.x, cc.y, 0, u1, v0, r, g, bl, al}, vd{d.x, d.y, 0, u0, v0, r, g, bl, al};
        dyn_.insert(dyn_.end(), {va, vb, vc, va, vc, vd});
    }
    flush_dyn(spr_[SPR_FONT], false, true);
    return w;
}

} // namespace iris
