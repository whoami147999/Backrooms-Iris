// Backrooms Iris - renderer: level textures, chunk streaming, entities, items, effects, 2D.
#pragma once
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../core/math.h"
#include "../core/sim.h"
#include "../core/world.h"
#include "chunk_mesh.h"
#include "textures.h"

namespace iris {

struct RenderSettings {
    float view_dist = 34.0f;      // chunks beyond this aren't drawn (fog hides the edge)
    int texture_size = 512;       // 256 on Vita
    int chunk_builds_per_frame = 2;
    bool post_effects = true;     // screen-grab distortion (PC only)
    int mesh_detail = 1;          // 2 on PC: finer baked lighting
    bool film = true;             // film look: bloom, grain, aberration, grading (PC shader; overlays on Vita)
    bool camcorder = false;       // found-footage look: scanlines, tracking noise, REC
    bool atmosphere = true;       // light halos and shafts, dust, flicker
    float particles = 1.0f;       // dust density (0 = none)
    float entity_detail = 1.0f;   // entity / player model tessellation
    bool threaded = true;         // build chunk meshes on a worker thread
    bool mipmaps = true;          // mipmapped world textures
    bool retro = true;            // PS1 look: crisp texels, wobbly vertices + affine textures (PC), pixels + dither (PC)
};

// Parameters for the film post-process (PC).
struct FilmParams {
    float time = 0;
    float grain = 0.05f, aberration = 1.5f, vignette = 0.4f, bloom = 0.8f;
    float saturation = 0.9f, contrast = 0.3f, distort = 0.06f;
    float lift[3] = {0.02f, 0.02f, 0.015f};
    float dim = 0;                // extra darkening (flickering lights)
    bool camcorder = false;
    float pixel_h = 0;            // retro: virtual vertical resolution (0 = full res)
    float dither = 0;             // retro: colour levels per channel with ordered dithering (0 = off)
};

// What's in your hands, for the first-person view model.
struct ViewModel {
    ItemType item = ItemType::None;
    bool light_on = false;
    float bob = 0, kick = 0, swing = 0;   // kick: 1 right after a shot, falling to 0; swing: 0..1 crowbar arc
    float sway_x = 0, sway_y = 0;
    float light = 1;
};

struct Camera {
    Vec3 eye;
    float yaw = 0, pitch = 0, roll = 0;
    float fov_deg = 70;
};

struct DrawEntity {
    uint16_t id;
    EntityType type;
    EntState state;
    Vec2 pos;
    float yaw, anim, speed;
};

struct DrawPlayer {
    int id;
    Vec3 pos;
    float yaw, crouch, anim;
    bool alive;
    std::string name;
    bool light_on = false;
};

struct DrawProp {
    uint16_t id;
    PropKind kind;
    ItemType item;
    Vec2 pos;
    float life;
};

// hallucinations
enum class PhantomKind : uint8_t { Shadow, FakeItem, Figure };
struct Phantom {
    PhantomKind kind;
    Vec3 pos;
    float alpha;
    int variant;
};

class Renderer {
public:
    int draw_calls() const { return last_draws_; } // last finished frame
    int width = 1, height = 1;

    bool init(const RenderSettings& s);
    void shutdown();
    // Make sure textures + chunks belong to this world. Cheap when nothing changed; when the
    // level changes the textures are regenerated, when only the seed changes chunks are dropped.
    void set_world(const World& w);
    LevelId level() const { return level_; }

    // Streams chunks around `eye`. budget < 0 = build everything needed right now.
    void update_chunks(const World& w, Vec3 eye, int budget);
    bool chunks_ready() const { return pending_ == 0; }
    int chunks_loaded() const { return (int)chunks_.size(); }

    void begin_frame(int w, int h);
    void draw_world(const World& w, const Camera& cam, float time, float fog_mult,
                    const std::unordered_set<uint32_t>& picked, const std::vector<DrawEntity>& ents,
                    const std::vector<DrawPlayer>& players, const std::vector<DrawProp>& props,
                    const std::vector<Phantom>& phantoms);
    // Flashlight: brightens the middle of the screen (multiplicative, keeps the dark dark).
    void flashlight_overlay(float strength, float flicker);
    void draw_viewmodel(const ViewModel& vm);
    void night_vision(float time); // brighten everything a lot and turn it green
    // Full-screen distortion of what was just drawn. amount 0 = off.
    void post_warp(float wobble, float ghost, float time);
    // Film look over the finished 3D frame (PC with GLSL). Returns false if it couldn't run.
    bool post_film(const FilmParams& f);
    void set_style(bool film, bool camcorder, bool atmosphere, bool retro);
    // graphics preset changes: view distance and detail now; textures / lighting grids rebuild
    void set_quality(float view_dist, int texture_size, int mesh_detail, float particles, float entity_detail);
    void set_darkness(float d) { darkness_ = d; }  // night on the levels with a day/night cycle (0..1)
    void night_dim(float amount);                  // after the world: darken the whole picture
    // How much a nearby flickering light is darkening the view right now (0..1), set by draw_world.
    float flicker_dim() const { return flicker_dim_; }

    // ---- 2D (pixel coordinates, origin top-left). Colours are 0xRRGGBBAA.
    void begin_2d();
    void rect(float x, float y, float w, float h, uint32_t rgba);
    void sprite(Sprite s, float x, float y, float w, float h, uint32_t rgba, float rot = 0, bool additive = false);
    float text(float x, float y, float px, const std::string& s, uint32_t rgba, int align = 0); // align: 0 left, 1 centre, 2 right
    void tiled(Sprite s, float x, float y, float w, float h, uint32_t rgba, float tile_px, float ox, float oy); // repeating texture
    float text_w(float px, const std::string& s) const { return text_width_px_f(s) * px; }
    float text_raw(float x, float y, float px, const std::string& s, uint32_t rgba, int align = 0);
    void end_2d();
    bool project(Vec3 p, float& sx, float& sy) const;

private:
    struct GpuChunk {
        uint32_t vbo = 0, ibo = 0;
        uint32_t first[MAT_COUNT] = {}, count[MAT_COUNT] = {};
        int kx = 0, kz = 0;
        std::vector<ExitDoor> exits;
        std::vector<uint32_t> items;
        std::vector<Vec3> balloons;
        std::vector<ChunkMesh::LightMark> lights;
    };
    // background chunk building: the worker thread turns (world, chunk) into a ChunkMesh,
    // the main thread uploads it to the GPU
    std::vector<const GpuChunk*> visible_;   // (reused every frame)
    int draws_ = 0, last_draws_ = 0;
    float darkness_ = 0;
    bool in_2d_ = false, batch_add_ = false;  // 2D batching state
    uint32_t batch_tex_ = 0;
    void pre2d(uint32_t tex, bool additive);
    void post2d(uint32_t tex, bool additive);
    struct Job { int kx, kz; uint32_t gen; World world; };
    struct Done { int kx, kz; uint32_t gen; ChunkMesh mesh; };
    std::thread worker_;
    bool threaded_ = false;      // false: the worker couldn't start, chunks build on the main thread
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::deque<Job> jobs_;
    std::deque<Done> done_;
    std::unordered_map<uint64_t, bool> inflight_;
    uint32_t gen_ = 1;
    uint32_t seed_ = 0;
    float fog_density_ = 0.05f;
    void worker_loop();
    float fog_factor(float dist) const { float f = fog_density_ * dist; return std::exp(-f * f); }
    RenderSettings s_;
    LevelId level_ = LevelId::COUNT;
    uint32_t tex_[MAT_COUNT] = {};
    uint32_t spr_[SPR_COUNT] = {};
    std::unordered_map<uint64_t, GpuChunk> chunks_;
    ChunkMesh scratch_;
    int pending_ = 1;
    float view_[16], proj_[16];
    std::vector<Vertex> dyn_;
    uint32_t grab_tex_ = 0;
    int grab_w_ = 0, grab_h_ = 0;
    Camera cam_;
    float flicker_dim_ = 0;
    uint32_t film_prog_ = 0;
    uint32_t retro_prog_ = 0;
    bool retro_failed_ = false;
    int retro_loc_[4] = {};
    void apply_texture_filtering();
    bool use_retro_shader();
    bool film_failed_ = false;
    int film_loc_[16] = {};
    void grab_screen();
    void draw_atmosphere(const World& w, const std::vector<const GpuChunk*>& visible, float time);
    void blob_shadow(Vec2 p, float r, float a);

    static uint64_t key(int kx, int kz) { return ((uint64_t)(uint32_t)kx << 32) | (uint32_t)kz; }
    static float text_width_px_f(const std::string& s) { return s.empty() ? 0.0f : (float)s.size() * 6 - 1; }
    void upload(GpuChunk& g, const ChunkMesh& m);
    void flush_dyn(uint32_t tex, bool additive = false, bool fog = true);
    void push_quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t rgba, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
    void billboard(Vec3 center, float w, float h, uint32_t rgba, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);
    void draw_entity(const World& w, const DrawEntity& e, float time);
    void draw_player(const World& w, const DrawPlayer& p, float time);
};

uint32_t rgba(float r, float g, float b, float a = 1);

} // namespace iris
