#include "renderer.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>

#include "../core/config.h"
#include "../core/hash.h"
#include "../platform/platform.h"
#include "font.h"
#include "gl.h"
#include "models.h"

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

#if !defined(IRIS_GL_VITA) // (PC builds mip levels itself; vitaGL does glGenerateMipmap)
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
#endif

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
        spr_[i] = upload_texture(make_sprite((Sprite)i), false, nearest, i == SPR_NOISE || i == SPR_SKIN || i == SPR_GRIT);
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
#ifdef GL_FOG_HINT
    glHint(GL_FOG_HINT, GL_NICEST); // (vitaGL doesn't have it)
#endif
    glAlphaFunc(GL_GREATER, 0.02f);
    if (!s_.threaded) threaded_ = false;
    else try {
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
    platform_thread_background(1);
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
        platform_boot_stage((std::string("level ") + level_info(w.id()).number + " textures").c_str());
        make_level_textures(w.id(), s_.texture_size, imgs);
        platform_boot_stage((std::string("level ") + level_info(w.id()).number + " texture upload").c_str());
        for (int i = 0; i < MAT_COUNT; i++) tex_[i] = upload_texture(imgs[i], s_.mipmaps, false, true);
        apply_texture_filtering();
        std::printf("[iris] textures uploaded\n");
        platform_boot_stage((std::string("level ") + level_info(w.id()).number + " first draw").c_str());
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
    float light = clampf(w.light_level({e.pos.x, 1.2f, e.pos.z}) * 0.75f + 0.12f, 0.08f, 0.95f);
    float stride = std::min(1.0f, e.speed / 2.2f);
    float breathe = std::sin(time * 1.6f + e.id) * 0.012f;
    const int seg = s_.mesh_detail >= 2 ? 10 : 6;
    ModelBuilder m(dyn_, seg);
    m.place(e.pos, e.yaw);
    m.set_light(light, {0.35f, 0.85f, 0.45f});
    bool stunned = e.state == EntState::Stunned;
    if (stunned) m.push_pose({0, 0.12f, 0}, 0, 0, 1.35f); // knocked over onto its side
    Vec2 f = yaw_forward(e.yaw);
    switch (e.type) {
        case EntityType::Bacteria: {
            // 2.6 m of wet black limbs: arms to its knees, long claws, spines down its back
            Humanoid h;
            float tw = e.state == EntState::Notice ? (hash01(e.id, (int)(time * 30), 0, 1) - 0.5f) * 0.5f : 0.0f;
            h.leg = 1.42f; h.torso = 0.72f; h.neck = 0.22f;
            h.hip_w = 0.075f; h.shoulder_w = 0.16f;
            h.upper_arm = 0.62f; h.forearm = 0.6f;
            h.limb_r = 0.04f; h.arm_r = 0.026f;
            h.chest = {0.13f, 0.17f, 0.08f}; h.belly = {0.08f, 0.15f, 0.065f}; h.head = {0.08f, 0.19f, 0.1f};
            Rgb k{0.035f, 0.035f, 0.045f}, k2{0.07f, 0.07f, 0.09f};
            h.skin = k; h.top = k; h.legs = k; h.feet = k2; h.hands = k2; h.has_hair = false;
            h.phase = e.anim; h.stride = stride;
            h.lean = e.state == EntState::Chase ? 0.38f : 0.1f + breathe;
            h.arm_swing = 0.45f;
            h.head_tilt = 0.25f + tw;
            h.build(m);
            for (int s2 = 0; s2 < 2; s2++) // three long claws per hand
                for (int c = -1; c <= 1; c++) m.cone(h.hand[s2], h.hand[s2] + Vec3{c * 0.035f, -0.26f, 0.04f + std::fabs((float)c) * -0.02f}, 0.012f, k2);
            for (int k3 = 0; k3 < 4; k3++) { // spines
                Vec3 b = h.hip_c + Vec3{0, 0.28f + k3 * 0.16f, -0.05f - k3 * 0.02f + std::sin(h.lean) * (0.28f + k3 * 0.16f)};
                m.cone(b, b + Vec3{0, 0.08f, -0.13f}, 0.02f, k2);
            }
            break;
        }
        case EntityType::Hound: {
            // a person on all fours: grey, starved, ribs showing, black hair over its head and spine
            Rgb sk{0.52f, 0.5f, 0.47f}, dk{0.36f, 0.34f, 0.32f}, hair{0.03f, 0.03f, 0.03f};
            float head_up = e.state == EntState::Stare ? 0.18f : 0.0f;
            float y0 = 0.55f + breathe;
            m.ellipsoid({0, y0, 0.0f}, {0.16f, 0.13f, 0.4f}, sk);
            m.ellipsoid({0, y0 + 0.03f, 0.2f}, {0.2f, 0.16f, 0.22f}, sk);           // ribcage
            for (int r = 0; r < 3; r++) m.ellipsoid({0, y0 + 0.02f, 0.12f + r * 0.08f}, {0.205f, 0.15f, 0.012f}, dk, 0.7f); // ribs
            m.ellipsoid({0, y0, -0.3f}, {0.15f, 0.13f, 0.16f}, sk);                // hips
            for (int s2 = 0; s2 < 4; s2++) { // legs: two segments, front knees back, hind knees forward
                bool front = s2 < 2;
                float side = (s2 & 1) ? 1.0f : -1.0f;
                float ph = e.anim + (s2 == 0 || s2 == 3 ? 0.0f : PI);
                float th = std::sin(ph) * 0.5f * stride;
                float bend = (front ? 1.0f : -1.0f) * (0.35f + std::max(0.0f, -std::cos(ph)) * 0.6f * stride);
                Vec3 top{side * 0.15f, y0 - 0.02f, front ? 0.3f : -0.3f};
                Vec3 knee = top + Vec3{0, -0.27f * std::cos(th - bend * 0.5f), 0.27f * std::sin(th - bend * 0.5f)};
                Vec3 foot = knee + Vec3{0, -std::max(0.05f, knee.y - 0.04f), 0.27f * std::sin(th + bend * 0.5f) * 0.5f};
                m.capsule(top, knee, 0.055f, 0.042f, sk);
                m.capsule(knee, foot, 0.04f, 0.035f, sk);
                m.ellipsoid(foot + Vec3{0, 0, 0.05f}, {0.05f, 0.025f, 0.08f}, dk, 0.6f);
            }
            Vec3 head{0, y0 + 0.08f + head_up, 0.62f};
            m.capsule({0, y0 + 0.04f, 0.38f}, head + Vec3{0, -0.02f, -0.08f}, 0.07f, 0.065f, sk, false);
            m.ellipsoid(head, {0.11f, 0.12f, 0.14f}, sk);
            float jaw = e.state == EntState::Chase ? 0.06f : 0.02f;
            m.ellipsoid(head + Vec3{0, -0.08f - jaw, 0.05f}, {0.08f, 0.035f, 0.1f}, dk, 0.7f);
            m.ellipsoid(head + Vec3{0, 0.06f, -0.03f}, {0.13f, 0.09f, 0.15f}, hair, 0.8f);  // matted hair
            m.ellipsoid({0, y0 + 0.13f, 0.05f}, {0.12f, 0.05f, 0.36f}, hair, 0.8f);
            for (int st = 0; st < 6; st++) { // hanging strands
                float sx = (st % 3 - 1) * 0.08f, sz = st < 3 ? 0.6f : 0.4f;
                Vec3 a{sx, st < 3 ? head.y + 0.02f : y0 + 0.12f, sz};
                m.capsule(a, a + Vec3{sx * 0.6f, -0.32f + std::sin(time * 2 + st) * 0.02f, 0.03f}, 0.018f, 0.008f, hair, false);
            }
            break;
        }
        case EntityType::SkinStealer: {
            // almost a person. The skin doesn't fit: patchy, stretched, raw at the seams
            Humanoid h;
            h.leg = 1.0f; h.torso = 0.62f; h.neck = 0.16f;
            h.upper_arm = 0.42f; h.forearm = 0.44f; h.shoulder_w = 0.21f;
            h.limb_r = 0.058f; h.arm_r = 0.042f;
            h.head = {0.11f, 0.15f, 0.125f};
            h.skin = {0.88f, 0.76f, 0.68f}; h.top = {0.80f, 0.6f, 0.52f}; h.legs = {0.86f, 0.74f, 0.66f};
            h.feet = {0.6f, 0.45f, 0.4f}; h.hands = {0.85f, 0.7f, 0.62f}; h.hair = {0.12f, 0.08f, 0.06f};
            h.phase = e.anim; h.stride = stride; h.lean = 0.18f + breathe;
            h.arm_forward = e.state == EntState::Chase ? 0.5f : 0.05f;
            h.head_tilt = 0.12f;
            h.build(m);
            Rgb raw{0.55f, 0.1f, 0.08f}, hole{0.02f, 0.0f, 0.0f};
            m.push_pose(h.head_c, h.head_tilt);
            for (int s2 = -1; s2 <= 1; s2 += 2) m.ellipsoid(h.head_c + Vec3{s2 * 0.045f, 0.025f, 0.1f}, {0.03f, 0.025f, 0.03f}, hole, 0.6f); // eye holes
            m.ellipsoid(h.head_c + Vec3{0, -0.06f, 0.105f}, {0.065f, 0.012f, 0.025f}, hole, 0.6f);                      // a mouth too wide
            m.pop_pose();
            m.ellipsoid(h.chest_c + Vec3{-0.07f, 0.0f, 0.1f}, {0.06f, 0.11f, 0.03f}, raw, 0.6f); // where the skin is stitched on
            m.ellipsoid(h.chest_c + Vec3{0.09f, -0.15f, 0.1f}, {0.05f, 0.05f, 0.025f}, raw, 0.6f);
            m.ellipsoid((h.shoulder[1] + h.hand[1]) * 0.5f, {0.05f, 0.08f, 0.05f}, raw, 0.6f);
            break;
        }
        case EntityType::Partygoer: {
            // =) a round yellow party guest: white gloves, purple shoes, a party hat
            Humanoid h;
            h.leg = 0.78f; h.torso = 0.55f; h.neck = 0.04f;
            h.hip_w = 0.13f; h.shoulder_w = 0.25f;
            h.upper_arm = 0.3f; h.forearm = 0.28f;
            h.limb_r = 0.075f; h.arm_r = 0.06f;
            h.chest = {0.25f, 0.21f, 0.17f}; h.belly = {0.25f, 0.24f, 0.19f}; h.head = {0.19f, 0.19f, 0.17f};
            Rgb y{0.95f, 0.8f, 0.2f};
            h.skin = y; h.top = y; h.legs = y; h.feet = {0.25f, 0.12f, 0.35f}; h.hands = {0.96f, 0.96f, 0.93f}; h.has_hair = false;
            h.phase = e.anim; h.stride = stride * 0.8f; h.arm_swing = 0.4f;
            h.arm_forward = e.state == EntState::Chase ? 0.9f : 0.0f; // arms out for a hug
            h.build(m);
            Vec3 ht = h.head_c + Vec3{0, h.head.y * 0.85f, 0};
            m.cone(ht, ht + Vec3{0.02f, 0.3f, 0}, 0.11f, {0.85f, 0.15f, 0.2f});
            m.ellipsoid(ht + Vec3{0.02f, 0.31f, 0}, {0.03f, 0.03f, 0.03f}, {1, 0.9f, 0.3f}, 0.6f);
            break;
        }
        case EntityType::Smiler: break;
        default: break;
    }
    if (stunned) m.pop_pose();
    if (!dyn_.empty()) flush_dyn(spr_[SPR_GRIT]);
    if (e.type == EntityType::Partygoer && !stunned) {
        // the =) face, and a balloon on a string
        Vec2 r = yaw_right(e.yaw);
        Vec3 c{e.pos.x + f.x * 0.18f, 1.62f, e.pos.z + f.z * 0.18f};
        Vec3 rr{r.x * 0.15f, 0, r.z * 0.15f}, up{0, 0.15f, 0};
        uint32_t lc = rgba(light, light, light);
        push_quad(c + rr - up, c - rr - up, c - rr + up, c + rr + up, lc);
        flush_dyn(spr_[SPR_PARTY_FACE]);
        Vec3 hand{e.pos.x + r.x * 0.33f, 1.0f, e.pos.z + r.z * 0.33f};
        Vec3 bal{hand.x + std::sin(time * 1.3f + e.id) * 0.1f, 2.45f, hand.z};
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        billboard(bal, 0.38f, 0.46f, rgba(0.85f * light + 0.15f, 0.08f, 0.12f, 1));
        flush_dyn(spr_[SPR_BALLOON]);
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
        billboard(c, size * 2.2f, size * 1.6f, rgba(1, 1, 0.9f, 0.06f * flick));
        flush_dyn(spr_[SPR_GLOW], true, false);
        billboard(c, size, size, rgba(1, 1, 1, 0.9f * flick));
        flush_dyn(spr_[SPR_SMILER], false, false);
        glDepthMask(GL_TRUE);
    } else if (e.type == EntityType::Hound && e.state == EntState::Stare) {
        Vec2 r = yaw_right(e.yaw);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            Vec3 c{e.pos.x + f.x * 0.75f + r.x * 0.05f * s2, 0.66f + 0.18f, e.pos.z + f.z * 0.75f + r.z * 0.05f * s2};
            billboard(c, 0.06f, 0.06f, rgba(1, 0.9f, 0.6f, 0.9f));
        }
        flush_dyn(spr_[SPR_GLOW], true, false);
    } else if (e.type == EntityType::Bacteria && e.state == EntState::Chase) {
        Vec2 r = yaw_right(e.yaw);
        for (int s2 = -1; s2 <= 1; s2 += 2) {
            Vec3 c{e.pos.x + f.x * 0.55f + r.x * 0.035f * s2, 2.45f, e.pos.z + f.z * 0.55f + r.z * 0.035f * s2};
            billboard(c, 0.05f, 0.05f, rgba(1, 1, 1, 0.8f));
        }
        flush_dyn(spr_[SPR_GLOW], true, false);
    }
}

void Renderer::draw_player(const World& w, const DrawPlayer& p, float time) {
    static const float COLORS[4][3] = {{0.95f, 0.55f, 0.15f}, {0.2f, 0.75f, 0.7f}, {0.65f, 0.35f, 0.85f}, {0.45f, 0.8f, 0.3f}};
    const float* c = COLORS[p.id & 3];
    float light = clampf(w.light_level(p.pos + Vec3{0, 1.2f, 0}) * 0.75f + 0.15f, 0.12f, 0.95f);
    if (!p.alive) light *= 0.5f;
    const int seg = s_.mesh_detail >= 2 ? 10 : 6;
    ModelBuilder m(dyn_, seg);
    m.place({p.pos.x, p.pos.z}, p.yaw);
    m.set_light(light, {0.35f, 0.85f, 0.45f});
    if (!p.alive) m.push_pose({0, 0.15f, 0}, -1.5f); // lying on the floor
    Humanoid h;
    h.top = {c[0], c[1], c[2]};
    h.legs = {0.18f, 0.22f, 0.32f};
    h.phase = p.anim;
    h.stride = p.alive ? 0.8f : 0.0f;
    h.crouch = p.crouch;
    h.arm_forward = p.light_on ? 0.35f : 0.0f;
    h.build(m);
    // a backpack, and the flashlight in the right hand
    m.push_pose(h.chest_c, h.lean);
    m.box(h.chest_c + Vec3{0, -0.02f, -0.19f}, {0.15f, 0.2f, 0.08f}, {0.25f, 0.22f, 0.18f});
    m.pop_pose();
    if (p.light_on) m.tube(h.hand[1], h.hand[1] + Vec3{0, 0.02f, 0.2f}, 0.025f, {0.2f, 0.2f, 0.22f});
    if (!p.alive) m.pop_pose();
    flush_dyn(spr_[SPR_GRIT]);
    (void)time;
}

// =================================================================== first-person view models
// Drawn in camera space after the world (own depth, own projection), lit by the light where you stand.
void Renderer::draw_viewmodel(const ViewModel& vm) {
    if (vm.item == ItemType::None) return;
    bool gun = is_gun(vm.item);
    if (!gun && vm.item != ItemType::Crowbar && !(vm.item == ItemType::Flashlight || vm.light_on)) return;
    glClear(GL_DEPTH_BUFFER_BIT);
    Mat4 proj = Mat4::perspective(54.0f * PI / 180.0f, (float)width / (float)height, 0.01f, 6.0f);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(proj.m);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    const int seg = s_.mesh_detail >= 2 ? 12 : 7;
    ModelBuilder m(dyn_, seg);
    m.place_camera();
    m.set_light(clampf(vm.light, 0.25f, 1.1f), {-0.35f, 0.8f, 0.5f});
    const Rgb gunmetal{0.16f, 0.16f, 0.18f}, steel{0.42f, 0.43f, 0.46f}, wood{0.42f, 0.26f, 0.14f}, polymer{0.11f, 0.11f, 0.11f};
    const Rgb skin{0.85f, 0.67f, 0.55f}, sleeve{0.24f, 0.26f, 0.3f}, red{0.6f, 0.08f, 0.07f}, olive{0.28f, 0.3f, 0.22f};
    // hand position: lower right, bobbing with your steps, kicking back when you fire
    float bx = 0.17f + vm.sway_x, by = -0.17f + std::sin(vm.bob) * 0.008f + vm.sway_y, bz = -0.42f + vm.kick * 0.06f;
    Vec3 grip{bx, by, bz};
    float pitch_kick = vm.kick * 0.18f;
    if (gun) {
        m.push_pose(grip, pitch_kick);
        if (vm.item == ItemType::Pistol) {
            m.box(grip + Vec3{0, 0.06f, -0.08f}, {0.017f, 0.025f, 0.11f}, gunmetal);              // slide
            m.box(grip + Vec3{0, 0.085f, -0.17f}, {0.004f, 0.006f, 0.004f}, steel);              // front sight
            m.box(grip + Vec3{0, 0.085f, 0.015f}, {0.012f, 0.006f, 0.004f}, steel);              // rear sight
            m.box(grip + Vec3{0, 0.03f, -0.07f}, {0.015f, 0.012f, 0.09f}, polymer);              // frame
            m.tube(grip + Vec3{0, 0.06f, -0.19f}, grip + Vec3{0, 0.06f, -0.2f}, 0.008f, steel);  // muzzle
            m.push_pose(grip, -0.25f);
            m.box(grip + Vec3{0, -0.02f, 0.005f}, {0.015f, 0.055f, 0.025f}, polymer);           // grip
            m.pop_pose();
            m.capsule(grip + Vec3{0, 0.01f, -0.035f}, grip + Vec3{0, -0.005f, -0.01f}, 0.006f, 0.006f, gunmetal); // trigger guard
        } else if (vm.item == ItemType::Shotgun) {
            m.tube(grip + Vec3{0, 0.065f, 0.0f}, grip + Vec3{0, 0.065f, -0.62f}, 0.017f, gunmetal);
            m.tube(grip + Vec3{0, 0.035f, -0.05f}, grip + Vec3{0, 0.035f, -0.52f}, 0.014f, gunmetal); // magazine tube
            m.box(grip + Vec3{0, 0.045f, 0.03f}, {0.022f, 0.035f, 0.08f}, gunmetal);                  // receiver
            float pump = vm.kick > 0.3f ? 0.05f : 0.0f;
            m.box(grip + Vec3{0, 0.03f, -0.28f + pump}, {0.024f, 0.022f, 0.08f}, wood);              // pump
            m.push_pose(grip + Vec3{0, 0.03f, 0.1f}, 0.25f);
            m.box(grip + Vec3{0, 0.0f, 0.2f}, {0.02f, 0.04f, 0.12f}, wood);                           // stock
            m.pop_pose();
            m.capsule(grip + Vec3{-0.02f, 0.0f, -0.28f}, grip + Vec3{-0.12f, -0.2f, -0.1f}, 0.022f, 0.035f, sleeve); // other hand on the pump
            m.ellipsoid(grip + Vec3{-0.012f, 0.012f, -0.28f}, {0.03f, 0.03f, 0.045f}, skin, 0.8f);
        } else { // rifle
            m.box(grip + Vec3{0, 0.05f, -0.05f}, {0.02f, 0.035f, 0.12f}, olive);                     // receiver
            m.box(grip + Vec3{0, 0.05f, -0.26f}, {0.022f, 0.028f, 0.1f}, polymer);                   // handguard
            m.tube(grip + Vec3{0, 0.055f, -0.36f}, grip + Vec3{0, 0.055f, -0.55f}, 0.008f, gunmetal); // barrel
            m.tube(grip + Vec3{0, 0.11f, 0.0f}, grip + Vec3{0, 0.11f, -0.16f}, 0.018f, polymer);     // scope
            m.box(grip + Vec3{0, 0.09f, -0.08f}, {0.008f, 0.012f, 0.03f}, polymer);
            m.push_pose(grip + Vec3{0, 0.02f, -0.07f}, 0.25f);
            m.box(grip + Vec3{0, -0.04f, -0.07f}, {0.012f, 0.05f, 0.022f}, polymer);                 // magazine
            m.pop_pose();
            m.box(grip + Vec3{0, 0.04f, 0.15f}, {0.02f, 0.035f, 0.08f}, olive);                      // stock
            m.capsule(grip + Vec3{-0.02f, 0.02f, -0.25f}, grip + Vec3{-0.12f, -0.2f, -0.1f}, 0.022f, 0.035f, sleeve);
            m.ellipsoid(grip + Vec3{-0.012f, 0.025f, -0.25f}, {0.03f, 0.03f, 0.045f}, skin, 0.8f);
        }
        // your hand on the grip and the arm running out of frame
        m.ellipsoid(grip + Vec3{0, -0.005f, 0.01f}, {0.03f, 0.035f, 0.045f}, skin, 0.8f);
        m.capsule(grip + Vec3{0, -0.02f, 0.03f}, grip + Vec3{0.06f, -0.2f, 0.25f}, 0.03f, 0.042f, sleeve);
        m.pop_pose();
    } else if (vm.item == ItemType::Crowbar) {
        float sw = vm.swing; // 0..1 through the swing arc
        Vec3 cg = grip + Vec3{0.02f, -0.04f, 0.02f};
        m.push_pose(cg, -0.9f + std::sin(sw * PI) * 1.7f, std::sin(sw * PI) * 0.7f, -0.25f);
        Vec3 g2 = cg;
        Vec3 a = g2 + Vec3{0, -0.04f, 0.03f}, b = g2 + Vec3{0, 0.24f, -0.06f};
        (void)grip;
        m.tube(a, b, 0.012f, red);
        m.tube(b, b + Vec3{0.0f, 0.04f, -0.06f}, 0.011f, red);                 // the hook
        m.tube(b + Vec3{0.0f, 0.04f, -0.06f}, b + Vec3{0.0f, 0.0f, -0.09f}, 0.01f, steel);
        m.tube(a, a + Vec3{0, -0.03f, 0.03f}, 0.011f, steel);                   // the flat end
        m.ellipsoid(g2, {0.03f, 0.035f, 0.045f}, skin, 0.8f);
        m.capsule(g2 + Vec3{0, -0.02f, 0.03f}, g2 + Vec3{0.06f, -0.2f, 0.25f}, 0.03f, 0.042f, sleeve);
        m.pop_pose();
    } else { // flashlight
        Vec3 g = grip + Vec3{0.02f, 0.03f, 0};
        m.tube(g + Vec3{0, 0, 0.08f}, g + Vec3{0, 0, -0.1f}, 0.019f, {0.13f, 0.13f, 0.14f});
        m.capsule(g + Vec3{0, 0, -0.1f}, g + Vec3{0, 0, -0.15f}, 0.022f, 0.03f, {0.2f, 0.2f, 0.22f}, false);
        m.tube(g + Vec3{0, 0, -0.15f}, g + Vec3{0, 0, -0.152f}, 0.029f, vm.light_on ? Rgb{2.0f, 2.0f, 1.8f} : Rgb{0.4f, 0.4f, 0.4f});
        m.ellipsoid(g + Vec3{0, -0.01f, 0}, {0.03f, 0.035f, 0.05f}, skin, 0.8f);
        m.capsule(g + Vec3{0, -0.02f, 0.03f}, g + Vec3{0.06f, -0.2f, 0.25f}, 0.03f, 0.042f, sleeve);
    }
    flush_dyn(spr_[SPR_GRIT]);
    if (gun && vm.kick > 0.6f) { // muzzle flash
        float z = vm.item == ItemType::Pistol ? -0.63f : (vm.item == ItemType::Shotgun ? -1.05f : -0.98f);
        Vec3 c{bx, by + 0.06f, z + 0.42f + bz};
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        float sz = vm.item == ItemType::Shotgun ? 0.16f : 0.1f;
        push_quad(c + Vec3{-sz, -sz, 0}, c + Vec3{sz, -sz, 0}, c + Vec3{sz, sz, 0}, c + Vec3{-sz, sz, 0}, rgba(1, 0.75f, 0.35f, 1));
        flush_dyn(spr_[SPR_GLOW], true, false);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glEnable(GL_CULL_FACE);
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

    // ----- night sky (outdoor levels): stars, and on the moon, the planets
    if (w.id() == LevelId::L7 || w.id() == LevelId::L9 || w.id() == LevelId::L24) { // night skies
        glDepthMask(GL_FALSE);
        glEnable(GL_BLEND);
        const float R = far_z * 0.85f;
        bool moon = w.id() == LevelId::L24;
        for (int i = 0; i < (moon ? 420 : 160); i++) {
            float a = hash01(4242, i, 0, 1) * 2 * PI, e = 0.06f + hash01(4242, i, 1, 1) * 1.45f;
            Vec3 d{std::cos(a) * std::cos(e), std::sin(e), std::sin(a) * std::cos(e)};
            float tw = 0.6f + 0.4f * std::sin(time * (1.0f + hash01(4242, i, 2, 1) * 3) + i);
            float sz = R * (0.004f + hash01(4242, i, 3, 1) * 0.005f);
            billboard(cam.eye + d * R, sz, sz, rgba(1, 1, 1, (moon ? 0.9f : 0.45f) * tw));
        }
        flush_dyn(spr_[SPR_STAR], true, false);
        if (moon) {
            struct Pl { float az, el, size, r, g, b; };
            const Pl P[] = {{0.6f, 0.35f, 0.14f, 0.35f, 0.55f, 1.0f},   // the Earth, huge in the sky
                            {2.2f, 0.55f, 0.05f, 0.95f, 0.45f, 0.3f},   // Mars
                            {3.5f, 0.25f, 0.09f, 0.9f, 0.75f, 0.55f},   // Jupiter
                            {4.6f, 0.7f, 0.07f, 0.95f, 0.85f, 0.55f},   // Saturn
                            {5.5f, 0.15f, 0.04f, 0.6f, 0.85f, 0.95f}};  // Neptune
            for (const Pl& p : P) {
                Vec3 d{std::cos(p.az) * std::cos(p.el), std::sin(p.el), std::sin(p.az) * std::cos(p.el)};
                billboard(cam.eye + d * R, R * p.size, R * p.size, rgba(p.r, p.g, p.b, 1));
            }
            flush_dyn(spr_[SPR_PLANET], false, false);
            Vec3 sun{std::cos(1.8f) * 0.8f, 0.6f, std::sin(1.8f) * 0.8f};
            billboard(cam.eye + sun * R, R * 0.12f, R * 0.12f, rgba(1, 0.97f, 0.9f, 1));
            flush_dyn(spr_[SPR_GLOW], true, false);
        }
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

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
    bool retro_shader = use_retro_shader();
    (void)retro_shader; // (Vita: always false)
    for (int mat = 0; mat < MAT_COUNT; mat++) {
        glBindTexture(GL_TEXTURE_2D, tex_[mat]);
        bool emissive = mat == MAT_LIGHT;
        if (emissive) glFogf(GL_FOG_DENSITY, fog_density_ * 0.45f); // lights cut through the haze a bit
        if (mat == MAT_DECAL || mat == MAT_CROP) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.5f); } // hard-edged details
        if (mat == MAT_CROP) glDisable(GL_CULL_FACE); // wheat is seen from both sides
        for (const GpuChunk* g : visible) {
            if (!g->count[mat]) continue;
            glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo);
            set_pointers(nullptr);
            glDrawElements(GL_TRIANGLES, (GLsizei)g->count[mat], GL_UNSIGNED_SHORT,
                           (const void*)(uintptr_t)(g->first[mat] * sizeof(uint16_t)));
        }
        if (emissive) glFogf(GL_FOG_DENSITY, fog_density_);
        if (mat == MAT_DECAL || mat == MAT_CROP) { glDisable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.02f); }
        if (mat == MAT_CROP) glEnable(GL_CULL_FACE);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
#if !defined(IRIS_GL_VITA)
    if (retro_shader) iris_glUseProgram(0);
#endif

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
        if (p.kind == PropKind::Mark) { // a chalk X on the floor, faintly luminous so you can find it again
            float myaw = (uint8_t)p.item / 255.0f * 2 * PI;
            for (int k = 0; k < 2; k++) {
                float a = myaw + PI / 4 + k * PI / 2;
                Vec3 d{std::cos(a) * 0.35f, 0, std::sin(a) * 0.35f}, s2{-std::sin(a) * 0.035f, 0, std::cos(a) * 0.035f};
                Vec3 c{p.pos.x, 0.012f, p.pos.z};
                push_quad(c - d - s2, c + d - s2, c + d + s2, c - d + s2, rgba(0.95f, 0.95f, 0.9f, 0.85f));
            }
            flush_dyn(spr_[SPR_WHITE], false, true);
            billboard({p.pos.x, 0.1f, p.pos.z}, 0.6f, 0.3f, rgba(0.8f, 0.9f, 1.0f, 0.08f));
            flush_dyn(spr_[SPR_GLOW], true, false);
            continue;
        }
        if (p.kind == PropKind::Glow) { // a glow stick on the floor and a pool of green light
            float yaw = (float)(p.id % 17) * 0.37f;
            Part stick{0, 0, 0, 0.03f, 0.03f, 0.2f, 0.4f, 1.0f, 0.45f, 0};
            add_part(dyn_, stick, p.pos, yaw, 1.6f);
            flush_dyn(spr_[SPR_WHITE]);
            float fade = clampf(p.life / 10.0f, 0, 1);
            float r = GLOW_RADIUS * 0.7f;
            push_quad({p.pos.x - r, 0.025f, p.pos.z + r}, {p.pos.x + r, 0.025f, p.pos.z + r}, {p.pos.x + r, 0.025f, p.pos.z - r},
                      {p.pos.x - r, 0.025f, p.pos.z - r}, rgba(0.3f, 1, 0.4f, 0.35f * fade));
            flush_dyn(spr_[SPR_GLOW], true, false);
            billboard({p.pos.x, 0.25f, p.pos.z}, 1.2f, 0.8f, rgba(0.4f, 1, 0.5f, 0.35f * fade));
            flush_dyn(spr_[SPR_GLOW], true, false);
            continue;
        }
        if (p.kind == PropKind::Lure) { // a little music box, notes drifting up from it
            float light = clampf(w.light_level({p.pos.x, 0.5f, p.pos.z}) + 0.3f, 0.3f, 1.2f);
            Part box{0, 0, 0, 0.18f, 0.11f, 0.13f, 0.5f, 0.28f, 0.15f, 0};
            Part lid{0, 0.11f, -0.06f, 0.18f, 0.015f, 0.13f, 0.65f, 0.4f, 0.22f, -0.9f};
            for (const Part& pt : {box, lid}) add_part(dyn_, pt, p.pos, time * 0.3f, light);
            flush_dyn(spr_[SPR_WHITE]);
            for (int k = 0; k < 4; k++) {
                float ph = std::fmod(time * 0.6f + k * 0.25f, 1.0f);
                billboard({p.pos.x + std::sin(ph * 6 + k) * 0.2f, 0.2f + ph * 1.4f, p.pos.z}, 0.08f, 0.08f, rgba(1, 0.95f, 0.7f, (1 - ph) * 0.9f));
            }
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
    const bool alarm = w.id() == LevelId::LRun; // Level !: everything strobes red
    const float strobe = std::sin(time * 6.0f) > 0 ? 1.0f : 0.2f;
    if (alarm) flicker_dim_ = (1.0f - strobe) * 0.35f;
    auto flicker = [&](const ChunkMesh::LightMark& l) {
        if (alarm) return strobe;
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
uniform float time, grain, aberr, vignette, bloom, sat, contrast, distort, cam, dim, pix, dither;
uniform vec3 lift;
varying vec2 uv;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
float bayer2(vec2 a) { a = floor(a); return fract(dot(a, vec2(0.5, a.y * 0.75))); }
float bayer4(vec2 a) { return bayer2(0.5 * a) * 0.25 + bayer2(a); }
vec3 tap(vec2 p, float bias) { return texture2D(scene, clamp(p, vec2(0.002), vec2(0.998)) * uvmax, bias).rgb; }
void main() {
    vec2 c = uv - 0.5;
    float r2 = dot(c, c);
    vec2 p = 0.5 + c * (1.0 - distort * r2);
    vec2 vres = pix > 0.0 ? vec2(floor(pix * res.x / res.y), pix) : res;
    if (pix > 0.0) p = (floor(p * vres) + 0.5) / vres; // chunky pixels
    if (cam > 0.0) { // a tracking band rolls up the picture; lines inside it jitter sideways
        float band = 1.0 - fract(time * 0.11);
        float k = smoothstep(0.045, 0.0, abs(uv.y - band)) * cam;
        p.x += (hash(vec2(floor(uv.y * res.y * 0.5), floor(time * 24.0))) - 0.5) * 0.02 * k;
        p.x += sin(uv.y * 300.0 + time * 13.0) * 0.0007 * cam;
    }
    vec2 off = c * 2.0 * aberr / (pix > 0.0 ? vres * 2.0 : res);
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
    float n = hash(floor(uv * vres) + fract(time * 13.7) * 91.0) - 0.5;
    col += n * grain * (1.3 - l);
    if (dither > 0.0) col = floor(col * dither + bayer4(uv * vres)) / dither; // PS1-style banding
    gl_FragColor = vec4(col, 1.0);
}
)";
// PS1 world: vertices snap to a coarse screen grid (the wobble), textures are mapped affinely
// (the swim), fog is the same exp2 fog as the fixed-function pipeline.
const char* RETRO_VS = R"(#version 120
uniform vec2 snap;
varying vec4 col;
varying vec2 uvp;
varying vec3 uvw;
varying float fogz;
void main() {
    vec4 p = ftransform();
    if (snap.y > 0.0) {
        vec2 ndc = p.xy / p.w;
        ndc = floor(ndc * snap * 0.5 + 0.5) / (snap * 0.5);
        p.xy = ndc * p.w;
    }
    gl_Position = p;
    col = gl_Color;
    uvp = gl_MultiTexCoord0.xy;
    uvw = vec3(gl_MultiTexCoord0.xy * p.w, p.w);
    fogz = length((gl_ModelViewMatrix * gl_Vertex).xyz);
}
)";
const char* RETRO_FS = R"(#version 120
uniform sampler2D tex;
uniform float affine;
varying vec4 col;
varying vec2 uvp;
varying vec3 uvw;
varying float fogz;
void main() {
    vec2 uv = mix(uvp, uvw.xy / uvw.z, affine);
    vec4 c = texture2D(tex, uv) * col;
    float f = clamp(exp(-pow(gl_Fog.density * fogz, 2.0)), 0.0, 1.0);
    gl_FragColor = vec4(mix(gl_Fog.color.rgb, c.rgb, f), c.a);
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

bool Renderer::use_retro_shader() {
#if defined(IRIS_GL_VITA)
    return false;
#else
    if (!s_.retro || retro_failed_ || !g_glcaps.shaders) return false;
    if (!retro_prog_) {
        GLuint vs = compile(GL_VERTEX_SHADER, RETRO_VS), fs = compile(GL_FRAGMENT_SHADER, RETRO_FS);
        if (!vs || !fs) { retro_failed_ = true; return false; }
        GLuint p = iris_glCreateProgram();
        iris_glAttachShader(p, vs);
        iris_glAttachShader(p, fs);
        iris_glLinkProgram(p);
        GLint ok = 0;
        iris_glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) { retro_failed_ = true; return false; }
        retro_prog_ = p;
        retro_loc_[0] = iris_glGetUniformLocation(p, "tex");
        retro_loc_[1] = iris_glGetUniformLocation(p, "snap");
        retro_loc_[2] = iris_glGetUniformLocation(p, "affine");
    }
    iris_glUseProgram(retro_prog_);
    iris_glUniform1i(retro_loc_[0], 0);
    const float lines = 300.0f; // vertices land on a 300-line grid
    iris_glUniform2f(retro_loc_[1], lines * (float)width / (float)height, lines);
    iris_glUniform1f(retro_loc_[2], 0.12f); // a hint of swim, not a broken ceiling
    return true;
#endif
}

void Renderer::set_style(bool film, bool camcorder, bool atmosphere, bool retro) {
    s_.film = film;
    s_.camcorder = camcorder;
    s_.atmosphere = atmosphere;
    if (s_.retro != retro) { s_.retro = retro; apply_texture_filtering(); }
}

// Retro: texels stay crisp squares up close (nearest), with mip levels picked without blending.
void Renderer::apply_texture_filtering() {
    for (int i = 0; i < MAT_COUNT; i++) {
        if (!tex_[i]) continue;
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_.retro ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, !s_.mipmaps ? (s_.retro ? GL_NEAREST : GL_LINEAR)
                                                                          : (s_.retro ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR));
    }
}

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
        const char* names[] = {"scene", "uvmax", "res", "time", "grain", "aberr", "vignette", "bloom", "sat", "contrast", "distort", "cam", "dim", "lift", "pix", "dither"};
        for (int i = 0; i < 16; i++) film_loc_[i] = iris_glGetUniformLocation(p, names[i]);
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
    iris_glUniform1f(film_loc_[14], f.pixel_h);
    iris_glUniform1f(film_loc_[15], f.dither);
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

void Renderer::night_vision(float time) {
    begin_2d();
    // dst * (1 + src), twice: four times brighter; then multiply by green
    glBlendFunc(GL_DST_COLOR, GL_ONE);
    for (int i = 0; i < 2; i++) {
        push_quad({0, (float)height, 0}, {(float)width, (float)height, 0}, {(float)width, 0, 0}, {0, 0, 0}, rgba(1, 1, 1, 1));
        set_pointers(dyn_.data());
        glBindTexture(GL_TEXTURE_2D, spr_[SPR_WHITE]);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dyn_.size());
        dyn_.clear();
    }
    glBlendFunc(GL_DST_COLOR, GL_ZERO);
    push_quad({0, (float)height, 0}, {(float)width, (float)height, 0}, {(float)width, 0, 0}, {0, 0, 0}, rgba(0.42f, 1.0f, 0.48f, 1));
    set_pointers(dyn_.data());
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)dyn_.size());
    dyn_.clear();
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float ox = hash01(91, (int)(time * 24), 0, 1), oy = hash01(91, (int)(time * 24), 1, 1); // sensor noise
    tiled(SPR_NOISE, 0, 0, (float)width, (float)height, rgba(0.6f, 1, 0.6f, 0.12f), 96, ox, oy);
    end_2d();
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
