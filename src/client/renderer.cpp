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
        build_chunk_mesh(job.world, job.kx, job.kz, d.mesh);
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
            build_chunk_mesh(w, wn.kx, wn.kz, scratch_);
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
// the box's top (pivot) by `swing` radians.
struct Part { float x, y, z, w, h, d; float r, g, b; float swing; };

static void add_part(std::vector<Vertex>& out, const Part& p, Vec2 pos, float yaw, float light) {
    float cy = std::cos(yaw), sy = std::sin(yaw);
    float cs = std::cos(p.swing), ss = std::sin(p.swing);
    float hx = p.w * 0.5f, hz = p.d * 0.5f;
    auto xf = [&](float lx, float ly, float lz) {
        // swing around the pivot at the top of the part (rotation about local x)
        float py = p.y + p.h; // pivot
        float ry = ly - py, rz = lz - p.z;
        float sy2 = ry * cs - rz * ss, sz2 = ry * ss + rz * cs;
        ly = py + sy2;
        lz = p.z + sz2;
        // local x = right, local z = forward; world: right = (cos, -sin), forward = (-sin, -cos)
        float wx = lx * cy - lz * sy;
        float wz = -lx * sy - lz * cy;
        return Vec3{pos.x + wx, ly, pos.z + wz};
    };
    float x0 = p.x - hx, x1 = p.x + hx, y0 = p.y, y1 = p.y + p.h, z0 = p.z - hz, z1 = p.z + hz;
    Vec3 c[8] = {xf(x0, y0, z0), xf(x1, y0, z0), xf(x1, y1, z0), xf(x0, y1, z0),
                 xf(x0, y0, z1), xf(x1, y0, z1), xf(x1, y1, z1), xf(x0, y1, z1)};
    const int F[6][4] = {{1, 0, 3, 2}, {4, 5, 6, 7}, {0, 4, 7, 3}, {5, 1, 2, 6}, {3, 7, 6, 2}, {0, 1, 5, 4}};
    const float shade[6] = {0.75f, 0.85f, 0.7f, 0.8f, 1.0f, 0.5f};
    for (int f = 0; f < 6; f++) {
        float k = shade[f] * light;
        uint8_t r = (uint8_t)clampf(p.r * k * 255, 0, 255), g = (uint8_t)clampf(p.g * k * 255, 0, 255), b = (uint8_t)clampf(p.b * k * 255, 0, 255);
        Vertex v[4];
        const float uv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (int i = 0; i < 4; i++) {
            Vec3 q = c[F[f][i]];
            v[i] = {q.x, q.y, q.z, uv[i][0], uv[i][1], r, g, b, 255};
        }
        out.insert(out.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
    }
}

void Renderer::draw_entity(const World& w, const DrawEntity& e, float time) {
    float light = clampf(w.light_level({e.pos.x, 1.2f, e.pos.z}) * 0.9f + 0.10f, 0.08f, 1.3f);
    float sw = std::sin(e.anim) * std::min(1.0f, e.speed / 2.0f);
    std::vector<Part> parts;
    switch (e.type) {
        case EntityType::Bacteria: {
            // tall, thin, black; twitches when it notices you
            float tw = e.state == EntState::Notice ? (hash01(e.id, (int)(time * 30), 0, 1) - 0.5f) * 0.12f : 0.0f;
            float k = 0.05f;
            parts = {
                {-0.09f, 0, 0, 0.07f, 1.35f, 0.07f, k, k, k, sw * 0.9f},
                {0.09f, 0, 0, 0.07f, 1.35f, 0.07f, k, k, k, -sw * 0.9f},
                {tw, 1.3f, 0.05f, 0.24f, 0.85f, 0.14f, k, k, k, 0.15f},
                {-0.19f + tw, 0.85f, 0.05f, 0.05f, 1.25f, 0.05f, k, k, k, -sw * 0.7f},
                {0.19f + tw, 0.85f, 0.05f, 0.05f, 1.25f, 0.05f, k, k, k, sw * 0.7f},
                {tw * 2, 2.12f, 0.1f, 0.17f, 0.38f, 0.17f, k * 1.4f, k * 1.4f, k * 1.4f, 0.2f},
            };
            break;
        }
        case EntityType::Hound: {
            // a person on all fours, grey skin, a mat of black hair; head comes up when staring
            float sk[3] = {0.55f, 0.52f, 0.48f};
            float head_up = e.state == EntState::Stare ? 0.18f : 0.0f;
            parts = {
                {0, 0.52f, 0, 0.36f, 0.26f, 0.95f, sk[0], sk[1], sk[2], 0},
                {-0.15f, 0, 0.38f, 0.09f, 0.56f, 0.09f, sk[0], sk[1], sk[2], sw},
                {0.15f, 0, 0.38f, 0.09f, 0.56f, 0.09f, sk[0], sk[1], sk[2], -sw},
                {-0.15f, 0, -0.38f, 0.09f, 0.56f, 0.09f, sk[0], sk[1], sk[2], -sw},
                {0.15f, 0, -0.38f, 0.09f, 0.56f, 0.09f, sk[0], sk[1], sk[2], sw},
                {0, 0.55f + head_up, 0.58f, 0.26f, 0.26f, 0.26f, sk[0], sk[1], sk[2], 0},
                {0, 0.72f + head_up, 0.52f, 0.32f, 0.12f, 0.36f, 0.03f, 0.03f, 0.03f, 0},
                {0, 0.62f, 0.15f, 0.3f, 0.18f, 0.5f, 0.03f, 0.03f, 0.03f, 0},
            };
            break;
        }
        case EntityType::SkinStealer: {
            float sk[3] = {0.82f, 0.62f, 0.55f};
            parts = {
                {-0.12f, 0, 0, 0.12f, 1.0f, 0.12f, sk[0], sk[1], sk[2], sw * 0.8f},
                {0.12f, 0, 0, 0.12f, 1.0f, 0.12f, sk[0], sk[1], sk[2], -sw * 0.8f},
                {0, 0.98f, 0, 0.42f, 0.75f, 0.22f, sk[0] * 0.9f, sk[1] * 0.85f, sk[2] * 0.85f, 0.1f},
                {-0.27f, 0.75f, 0, 0.09f, 1.0f, 0.09f, sk[0], sk[1], sk[2], -sw},
                {0.27f, 0.75f, 0, 0.09f, 1.0f, 0.09f, sk[0], sk[1], sk[2], sw},
                {0, 1.73f, 0.02f, 0.26f, 0.32f, 0.26f, sk[0], sk[1], sk[2], 0},
                {-0.06f, 1.88f, 0.14f, 0.06f, 0.05f, 0.02f, 0.02f, 0.0f, 0.0f, 0},
                {0.06f, 1.88f, 0.14f, 0.06f, 0.05f, 0.02f, 0.02f, 0.0f, 0.0f, 0},
                {0, 1.77f, 0.14f, 0.14f, 0.03f, 0.02f, 0.3f, 0.05f, 0.05f, 0},
            };
            break;
        }
        case EntityType::Partygoer: {
            float y[3] = {0.92f, 0.78f, 0.22f};
            parts = {
                {-0.11f, 0, 0, 0.13f, 0.85f, 0.13f, y[0], y[1], y[2], sw * 0.5f},
                {0.11f, 0, 0, 0.13f, 0.85f, 0.13f, y[0], y[1], y[2], -sw * 0.5f},
                {0, 0.83f, 0, 0.42f, 0.62f, 0.24f, y[0], y[1], y[2], 0},
                {-0.27f, 0.62f, 0, 0.1f, 0.8f, 0.1f, y[0], y[1], y[2], -sw * 0.4f},
                {0.27f, 0.62f, 0, 0.1f, 0.8f, 0.1f, y[0], y[1], y[2], sw * 0.4f - 0.3f},
                {0, 1.45f, 0, 0.34f, 0.34f, 0.3f, y[0], y[1], y[2], 0},
                {0, 1.79f, 0, 0.16f, 0.22f, 0.16f, 0.85f, 0.15f, 0.2f, 0}, // party hat
            };
            break;
        }
        case EntityType::Smiler: break;
        default: break;
    }
    if (e.state == EntState::Stunned) // knocked down: everything slumps toward the floor
        for (Part& p : parts) { p.y *= 0.35f; p.h *= 0.55f; p.swing = 0.6f; }
    if (!parts.empty()) {
        for (const Part& p : parts) add_part(dyn_, p, e.pos, e.yaw, light);
        flush_dyn(spr_[SPR_WHITE]);
    }
    Vec2 f = yaw_forward(e.yaw);
    if (e.type == EntityType::Partygoer) {
        // the =) face, and a balloon on a string
        Vec2 r = yaw_right(e.yaw);
        Vec3 c{e.pos.x + f.x * 0.155f, 1.62f, e.pos.z + f.z * 0.155f};
        Vec3 rr{r.x * 0.16f, 0, r.z * 0.16f}, up{0, 0.16f, 0};
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
        billboard(c, size, size, rgba(1, 1, 1, 0.9f * flick));
        flush_dyn(spr_[SPR_SMILER], false, false);
        glDepthMask(GL_TRUE);
    } else if (e.type == EntityType::Hound && e.state == EntState::Stare) {
        // eyes catch the light when it's staring back at you
        Vec2 r = yaw_right(e.yaw);
        for (int s = -1; s <= 1; s += 2) {
            Vec3 c{e.pos.x + f.x * 0.72f + r.x * 0.06f * s, 0.85f, e.pos.z + f.z * 0.72f + r.z * 0.06f * s};
            billboard(c, 0.07f, 0.07f, rgba(1, 0.9f, 0.6f, 0.9f));
        }
        flush_dyn(spr_[SPR_GLOW], true, false);
    }
}

void Renderer::draw_player(const World& w, const DrawPlayer& p, float time) {
    static const float COLORS[4][3] = {{0.95f, 0.55f, 0.15f}, {0.2f, 0.75f, 0.7f}, {0.65f, 0.35f, 0.85f}, {0.45f, 0.8f, 0.3f}};
    const float* c = COLORS[p.id & 3];
    float light = clampf(w.light_level(p.pos + Vec3{0, 1.2f, 0}) * 0.9f + 0.15f, 0.12f, 1.3f);
    if (!p.alive) light *= 0.4f;
    float sw = std::sin(p.anim) * 0.6f;
    float crouch = p.crouch * 0.45f;
    float down = p.alive ? 0.0f : 1.4f; // dead players lie on the floor
    std::vector<Part> parts = {
        {-0.11f, 0, 0, 0.13f, 0.85f - crouch, 0.14f, 0.2f, 0.2f, 0.22f, sw},
        {0.11f, 0, 0, 0.13f, 0.85f - crouch, 0.14f, 0.2f, 0.2f, 0.22f, -sw},
        {0, 0.83f - crouch, 0, 0.42f, 0.62f, 0.25f, c[0], c[1], c[2], 0},
        {-0.27f, 0.82f - crouch, 0, 0.1f, 0.62f, 0.1f, c[0], c[1], c[2], -sw * 0.8f},
        {0.27f, 0.82f - crouch, 0, 0.1f, 0.62f, 0.1f, c[0], c[1], c[2], sw * 0.8f},
        {0, 1.45f - crouch, 0, 0.26f, 0.28f, 0.26f, 0.85f, 0.7f, 0.6f, 0},
        {0, 1.55f - crouch, 0.13f, 0.2f, 0.08f, 0.02f, 0.1f, 0.1f, 0.12f, 0},
    };
    if (down > 0) for (auto& part : parts) { part.y *= 0.15f; part.h *= 0.4f; }
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
    // exit signs: glowing green, visible through the fog: the one thing to follow
    glDepthMask(GL_FALSE);
    for (const GpuChunk* g : visible)
        for (const Vec3& ex : g->exits) {
            for (int side = -1; side <= 1; side += 2) {
                float z = ex.z + side * 0.145f;
                Vec3 a{ex.x - 0.36f, 2.4f, z}, b{ex.x + 0.36f, 2.4f, z}, c{ex.x + 0.36f, 2.66f, z}, d{ex.x - 0.36f, 2.66f, z};
                if (side > 0) std::swap(a, b), std::swap(c, d); // keep the text readable from both sides
                push_quad(a, b, c, d, rgba(1, 1, 1));
            }
            flush_dyn(spr_[SPR_EXIT_SIGN], false, false);
            float ed = (Vec2{cam.eye.x, cam.eye.z} - Vec2{ex.x, ex.z}).len();
            billboard({ex.x, 2.53f, ex.z}, 2.2f, 1.2f, rgba(0.3f, 1, 0.4f, 0.22f * std::max(0.35f, fog_factor(ed))));
            flush_dyn(spr_[SPR_GLOW], true, false);
        }
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
    int tw = 1, th = 1;
    while (tw < width) tw <<= 1;
    while (th < height) th <<= 1;
    if (!grab_tex_ || tw != grab_w_ || th != grab_h_) {
        if (grab_tex_) glDeleteTextures(1, &grab_tex_);
        glGenTextures(1, &grab_tex_);
        glBindTexture(GL_TEXTURE_2D, grab_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        std::vector<uint8_t> zero((size_t)tw * th * 4, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
        grab_w_ = tw;
        grab_h_ = th;
    }
    glBindTexture(GL_TEXTURE_2D, grab_tex_);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
    float umax = (float)width / tw, vmax = (float)height / th;

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
