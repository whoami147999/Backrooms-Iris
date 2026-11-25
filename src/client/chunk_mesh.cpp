#include "chunk_mesh.h"

#include <algorithm>
#include <cmath>

#include "../core/config.h"
#include "../core/hash.h"

namespace iris {
namespace {

constexpr float TEX_M = 1.25f;      // metres covered by one repeat of the wallpaper
constexpr float TEX_FLOOR = CELL;   // carpet and ceiling textures cover one whole cell
constexpr float LIGHT_R = 7.5f;     // light radius
constexpr float AMBIENT = 0.17f;

struct Light { Vec3 pos; float power; };

struct Builder {
    const Level0& L;
    std::vector<Light> lights;
    std::vector<Vertex> v[MAT_COUNT];
    std::vector<uint16_t> i[MAT_COUNT];

    explicit Builder(const Level0& l) : L(l) {}

    // Baked lighting: ambient + every panel in range, attenuated, Lambert, and dimmed hard
    // when a wall is in the way (so dead-light rooms are actually dark).
    float shade(Vec3 p, Vec3 n) const {
        float b = AMBIENT;
        Vec2 sample{p.x + n.x * 0.06f, p.z + n.z * 0.06f};
        for (const Light& l : lights) {
            Vec3 d = l.pos - p;
            float dist = d.len();
            if (dist >= LIGHT_R || dist < 1e-4f) continue;
            float att = 1.0f - dist / LIGHT_R;
            att *= att;
            float lam = n.y < -0.5f ? 0.30f : std::max(0.0f, n.dot(d) / dist);
            if (lam <= 0) continue;
            if (L.segment_blocked(sample, {l.pos.x, l.pos.z})) att *= 0.10f;
            b += l.power * att * lam * 1.35f;
        }
        return b;
    }

    static Vertex vert(Vec3 p, float u, float t, float bright) {
        // soft roll-off instead of a hard clip, then a warm fluorescent tint
        float c = bright / (1.0f + bright * 0.30f) * 1.12f;
        auto to8 = [](float f) { return (uint8_t)std::min(255.0f, std::max(0.0f, f * 255.0f)); };
        return {p.x, p.y, p.z, u, t, to8(c), to8(c * 0.95f), to8(c * 0.82f), 255};
    }

    // Grid of (su+1)*(sv+1) vertices spanning o + a*s + b*t. CCW when seen from `n`.
    template <typename UV, typename AO>
    void grid(Material m, Vec3 o, Vec3 a, Vec3 b, int su, int sv, Vec3 n, UV uv, AO ao) {
        auto& vs = v[m];
        auto& is = i[m];
        uint16_t base = (uint16_t)vs.size();
        for (int y = 0; y <= sv; y++) {
            for (int x = 0; x <= su; x++) {
                Vec3 p = o + a * ((float)x / su) + b * ((float)y / sv);
                float uu, tt;
                uv(p, uu, tt);
                vs.push_back(vert(p, uu, tt, shade(p, n) * ao(p)));
            }
        }
        for (int y = 0; y < sv; y++) {
            for (int x = 0; x < su; x++) {
                uint16_t q = (uint16_t)(base + y * (su + 1) + x);
                uint16_t r = (uint16_t)(q + 1), s = (uint16_t)(q + su + 1), t = (uint16_t)(s + 1);
                is.insert(is.end(), {q, r, t, q, t, s});
            }
        }
    }

    // A vertical face of a box. `along` is the horizontal direction of the face (unit),
    // used for the wallpaper u coordinate so the pattern continues across neighbours.
    void wall_face(Vec3 o, Vec3 a, Vec3 n, int segs) {
        Vec3 ad = a * (1.0f / a.len());
        grid(MAT_WALL, o, a, Vec3{0, CEIL_H, 0}, segs, 3, n,
             [&](Vec3 p, float& u, float& t) { u = p.dot(ad) / TEX_M; t = (CEIL_H - p.y) / CEIL_H; },
             [&](Vec3 p) { return p.y < 0.01f ? 0.80f : (p.y > CEIL_H - 0.01f ? 0.90f : 1.0f); });
    }

    // 4 sides of an axis-aligned box centred at (cx, cz)
    void box(float cx, float cz, float hx, float hz, int segs) {
        float x0 = cx - hx, x1 = cx + hx, z0 = cz - hz, z1 = cz + hz;
        wall_face({x1, 0, z1}, {0, 0, z0 - z1}, {1, 0, 0}, segs);   // +X
        wall_face({x0, 0, z0}, {0, 0, z1 - z0}, {-1, 0, 0}, segs);  // -X
        wall_face({x0, 0, z1}, {x1 - x0, 0, 0}, {0, 0, 1}, segs);   // +Z
        wall_face({x1, 0, z0}, {x0 - x1, 0, 0}, {0, 0, -1}, segs);  // -Z
    }

    // smooth value noise over the world (period-free), 0.78..1.05
    float damp(Vec3 p) const {
        float fx = p.x / 3.1f, fz = p.z / 3.1f;
        int ix = floor_to_int(fx), iz = floor_to_int(fz);
        float tx = fx - ix, tz = fz - iz;
        tx = tx * tx * (3 - 2 * tx);
        tz = tz * tz * (3 - 2 * tz);
        auto h = [&](int a, int b) { return hash01(L.seed(), a, b, 977); };
        float n = lerpf(lerpf(h(ix, iz), h(ix + 1, iz), tx), lerpf(h(ix, iz + 1), h(ix + 1, iz + 1), tx), tz);
        return 0.78f + 0.27f * n;
    }

    // floor/ceiling darkening where they meet walls (cheap ambient occlusion)
    float corner_ao(Vec3 p) const {
        float gxf = p.x / CELL, gzf = p.z / CELL;
        int gx = (int)std::lround(gxf), gz = (int)std::lround(gzf);
        bool on_x = std::fabs(gxf - gx) < 1e-3f, on_z = std::fabs(gzf - gz) < 1e-3f;
        if (on_x && on_z) return L.post(gx, gz) ? 0.72f : 1.0f;
        if (on_x) return L.wall_x(gx, floor_to_int(gzf)) ? 0.80f : 1.0f;
        if (on_z) return L.wall_z(floor_to_int(gxf), gz) ? 0.80f : 1.0f;
        return 1.0f;
    }
};

} // namespace

void build_chunk_mesh(const Level0& L, int kx, int kz, ChunkMesh& out) {
    Builder B(L);
    const int c0x = kx * CHUNK_CELLS, c0z = kz * CHUNK_CELLS;
    const int m = 4; // light search margin in cells (> LIGHT_R / CELL)
    for (int cz = c0z - m; cz < c0z + CHUNK_CELLS + m; cz++) {
        for (int cx = c0x - m; cx < c0x + CHUNK_CELLS + m; cx++) {
            LightState s = L.light(cx, cz);
            if (s == LightState::On || s == LightState::Flicker)
                B.lights.push_back({{(cx + 0.5f) * CELL, CEIL_H - 0.05f, (cz + 0.5f) * CELL}, s == LightState::On ? 1.0f : 0.75f});
        }
    }

    const float hw = WALL_T * 0.5f, hp = POST_T * 0.5f, hl = PILLAR_T * 0.5f;
    for (int cz = c0z; cz < c0z + CHUNK_CELLS; cz++) {
        for (int cx = c0x; cx < c0x + CHUNK_CELLS; cx++) {
            float x0 = cx * CELL, z0 = cz * CELL, x1 = x0 + CELL, z1 = z0 + CELL;
            auto floor_uv = [](Vec3 p, float& u, float& t) { u = p.x / TEX_FLOOR; t = p.z / TEX_FLOOR; };
            auto ao = [&](Vec3 p) { return B.corner_ao(p); };
            // world-space damp patches so the carpet never visibly repeats
            auto floor_ao = [&](Vec3 p) { return B.corner_ao(p) * B.damp(p); };
            // floor (seen from above) and ceiling (seen from below)
            B.grid(MAT_FLOOR, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, -CELL}, 2, 2, {0, 1, 0}, floor_uv, floor_ao);
            B.grid(MAT_CEIL, {x0, CEIL_H, z0}, {CELL, 0, 0}, {0, 0, CELL}, 2, 2, {0, -1, 0}, floor_uv, ao);

            // light panel, 1.2 x 0.6 m, a hair below the ceiling
            LightState ls = L.light(cx, cz);
            if (ls != LightState::None) {
                float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f, y = CEIL_H - 0.004f;
                uint8_t c = ls == LightState::Dead ? 70 : 255;
                auto& vs = B.v[MAT_LIGHT];
                auto& is = B.i[MAT_LIGHT];
                uint16_t b0 = (uint16_t)vs.size();
                vs.push_back({cxm - 0.6f, y, czm - 0.3f, 0, 0, c, c, c, 255});
                vs.push_back({cxm + 0.6f, y, czm - 0.3f, 1, 0, c, c, c, 255});
                vs.push_back({cxm + 0.6f, y, czm + 0.3f, 1, 1, c, c, c, 255});
                vs.push_back({cxm - 0.6f, y, czm + 0.3f, 0, 1, c, c, c, 255});
                is.insert(is.end(), {b0, (uint16_t)(b0 + 1), (uint16_t)(b0 + 2), b0, (uint16_t)(b0 + 2), (uint16_t)(b0 + 3)});
            }

            // walls owned by this cell: its west edge (wall_x) and north edge (wall_z)
            if (L.wall_x(cx, cz)) {
                B.wall_face({x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, 2);
                B.wall_face({x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, 2);
            }
            if (L.wall_z(cx, cz)) {
                B.wall_face({x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, 2);
                B.wall_face({x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, 2);
            }
            // grid point at this cell's north-west corner
            if (L.post(cx, cz)) B.box(x0, z0, hp, hp, 1);
            else if (L.pillar(cx, cz)) B.box(x0, z0, hl, hl, 1);
        }
    }

    out.verts.clear();
    out.idx.clear();
    for (int mat = 0; mat < MAT_COUNT; mat++) {
        uint16_t base = (uint16_t)out.verts.size();
        out.first[mat] = (uint32_t)out.idx.size();
        out.count[mat] = (uint32_t)B.i[mat].size();
        out.verts.insert(out.verts.end(), B.v[mat].begin(), B.v[mat].end());
        for (uint16_t ix : B.i[mat]) out.idx.push_back((uint16_t)(ix + base));
    }
}

} // namespace iris
