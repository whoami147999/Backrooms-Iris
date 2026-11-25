// Backrooms Iris - renderer: textures, chunk streaming, fog, simple HUD.
#pragma once
#include <cstdint>
#include <unordered_map>

#include "../core/level0.h"
#include "../core/math.h"
#include "chunk_mesh.h"

namespace iris {

struct RenderSettings {
    float fov_deg = 68.0f;
    float view_dist = 34.0f;     // chunks beyond this aren't drawn (fog hides the edge)
    float fog_density = 0.055f;
    int chunk_builds_per_frame = 2;
};

class Renderer {
public:
    bool init(const RenderSettings& s);
    void shutdown();

    // Streams chunks around `eye`. budget < 0 = build everything needed right now.
    void update_chunks(const Level0& level, Vec3 eye, int budget);
    void draw_world(Vec3 eye, float yaw, float pitch, int width, int height);
    void draw_hud(float stamina, bool exhausted, int width, int height);

    int chunks_loaded() const { return (int)chunks_.size(); }
    int chunks_drawn() const { return drawn_; }

private:
    struct GpuChunk {
        uint32_t vbo = 0, ibo = 0;
        uint32_t first[MAT_COUNT] = {}, count[MAT_COUNT] = {};
        int kx = 0, kz = 0;
    };
    RenderSettings s_;
    uint32_t tex_[MAT_COUNT] = {};
    std::unordered_map<uint64_t, GpuChunk> chunks_;
    ChunkMesh scratch_;
    int drawn_ = 0;

    static uint64_t key(int kx, int kz) { return ((uint64_t)(uint32_t)kx << 32) | (uint32_t)kz; }
    void upload(GpuChunk& g, const ChunkMesh& m);
};

} // namespace iris
