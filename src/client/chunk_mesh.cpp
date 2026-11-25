#include "chunk_mesh.h"

#include <algorithm>
#include <cmath>

#include "../core/config.h"
#include "../core/hash.h"

namespace iris {
namespace {

constexpr float TEX_WALL = 1.25f;   // wallpaper repeat width

struct Light { Vec3 pos; float power; };

float vnoise_world(uint32_t seed, float x, float z, uint32_t salt) {
    int ix = floor_to_int(x), iz = floor_to_int(z);
    float tx = x - ix, tz = z - iz;
    tx = tx * tx * (3 - 2 * tx);
    tz = tz * tz * (3 - 2 * tz);
    auto h = [&](int a, int b) { return hash01(seed, a, b, salt); };
    return lerpf(lerpf(h(ix, iz), h(ix + 1, iz), tx), lerpf(h(ix, iz + 1), h(ix + 1, iz + 1), tx), tz);
}

struct Builder {
    const World& W;
    const LevelInfo& li;
    std::vector<Light> lights;
    std::vector<Vertex> v[MAT_COUNT];
    std::vector<uint16_t> i[MAT_COUNT];
    float tint[3] = {1, 1, 1};

    explicit Builder(const World& w) : W(w), li(w.info()) {}

    // Baked lighting: ambient + light sources in range, attenuated, Lambert, dimmed hard
    // when something is in the way (so dead-light rooms are actually dark).
    void shade(Vec3 p, Vec3 n, float out[3]) const {
        out[0] = li.ambient[0]; out[1] = li.ambient[1]; out[2] = li.ambient[2];
        if (li.outdoor && !li.dark && li.ambient[0] > 0.5f) {
            // daylight: fake sun + sky so buildings read as 3D
            float sky = n.y > 0.5f ? 1.08f : 0.80f + 0.20f * (n.x * 0.6f + n.z * 0.8f);
            for (int k = 0; k < 3; k++) out[k] *= sky;
        }
        Vec2 sample{p.x + n.x * 0.06f, p.z + n.z * 0.06f};
        for (const Light& l : lights) {
            Vec3 d = l.pos - p;
            float dist = d.len();
            if (dist >= li.light_radius || dist < 1e-4f) continue;
            float att = 1.0f - dist / li.light_radius;
            att *= att;
            float lam = (n.y < -0.5f && l.pos.y > p.y - 0.5f) ? 0.30f : std::max(0.0f, n.dot(d) / dist);
            if (lam <= 0) continue;
            if (W.segment_blocked(sample, {l.pos.x, l.pos.z})) att *= 0.10f;
            float k = l.power * att * lam;
            for (int c = 0; c < 3; c++) out[c] += li.light_color[c] * k;
        }
    }

    static uint8_t to8(float f) { return (uint8_t)std::min(255.0f, std::max(0.0f, f * 255.0f)); }

    Vertex vert(Vec3 p, float u, float t, const float c[3], float ao) const {
        float o[3];
        for (int k = 0; k < 3; k++) {
            float b = c[k] * ao * tint[k];
            o[k] = b / (1.0f + b * 0.30f) * 1.12f; // soft roll-off instead of a hard clip
        }
        return {p.x, p.y, p.z, u, t, to8(o[0]), to8(o[1]), to8(o[2]), 255};
    }

    // Grid of (su+1)*(sv+1) vertices spanning o + a*s + b*t, CCW when seen from `n`.
    // `pos` may displace vertices (cave ceilings).
    template <typename UV, typename AO, typename POS>
    void grid(Material m, Vec3 o, Vec3 a, Vec3 b, int su, int sv, Vec3 n, UV uv, AO ao, POS pos) {
        auto& vs = v[m];
        auto& is = i[m];
        if (vs.size() + (size_t)((su + 1) * (sv + 1)) > 65000) return;
        uint16_t base = (uint16_t)vs.size();
        for (int y = 0; y <= sv; y++)
            for (int x = 0; x <= su; x++) {
                Vec3 p = pos(o + a * ((float)x / su) + b * ((float)y / sv));
                float uu, tt, c[3];
                uv(p, uu, tt);
                shade(p, n, c);
                vs.push_back(vert(p, uu, tt, c, ao(p)));
            }
        for (int y = 0; y < sv; y++)
            for (int x = 0; x < su; x++) {
                uint16_t q = (uint16_t)(base + y * (su + 1) + x);
                uint16_t r = (uint16_t)(q + 1), s = (uint16_t)(q + su + 1), t = (uint16_t)(s + 1);
                is.insert(is.end(), {q, r, t, q, t, s});
            }
    }
    template <typename UV, typename AO>
    void grid(Material m, Vec3 o, Vec3 a, Vec3 b, int su, int sv, Vec3 n, UV uv, AO ao) {
        grid(m, o, a, b, su, sv, n, uv, ao, [](Vec3 p) { return p; });
    }

    // Flat emissive quad (lights), corners CCW from the viewer side.
    void quad(Material m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint8_t lum, float r = 1, float g = 1, float bl = 1) {
        auto& vs = v[m];
        auto& is = i[m];
        uint16_t b0 = (uint16_t)vs.size();
        uint8_t cr = (uint8_t)(lum * r), cg = (uint8_t)(lum * g), cb = (uint8_t)(lum * bl);
        vs.push_back({a.x, a.y, a.z, 0, 0, cr, cg, cb, 255});
        vs.push_back({b.x, b.y, b.z, 1, 0, cr, cg, cb, 255});
        vs.push_back({c.x, c.y, c.z, 1, 1, cr, cg, cb, 255});
        vs.push_back({d.x, d.y, d.z, 0, 1, cr, cg, cb, 255});
        is.insert(is.end(), {b0, (uint16_t)(b0 + 1), (uint16_t)(b0 + 2), b0, (uint16_t)(b0 + 2), (uint16_t)(b0 + 3)});
    }

    // A vertical face from y0 to y1. `along` (unit) drives the u coordinate so patterns
    // continue across neighbouring faces.
    void vface(Material m, Vec3 o, Vec3 a, Vec3 n, float y0, float y1, float tex_w, float tex_h, int su, int sv, bool cave) {
        Vec3 ad = a * (1.0f / a.len());
        Vec3 base{o.x, y0, o.z};
        grid(m, base, a, Vec3{0, y1 - y0, 0}, su, sv, n,
             [&](Vec3 p, float& u, float& t) { u = p.dot(ad) / tex_w; t = (y1 - p.y) / tex_h; },
             [&](Vec3 p) { return p.y < 0.01f ? 0.80f : 1.0f; },
             [&](Vec3 p) {
                 if (cave && p.y > y1 - 0.01f) p.y = ceiling_y(W, p.x, p.z); // meet the lumpy ceiling
                 return p;
             });
    }

    // 4 sides of a box centred at (cx, cz)
    void box(Material m, float cx, float cz, float hx, float hz, float y0, float y1, float tex_w, float tex_h, int segs) {
        float x0 = cx - hx, x1 = cx + hx, z0 = cz - hz, z1 = cz + hz;
        vface(m, {x1, 0, z1}, {0, 0, z0 - z1}, {1, 0, 0}, y0, y1, tex_w, tex_h, segs, 2, false);
        vface(m, {x0, 0, z0}, {0, 0, z1 - z0}, {-1, 0, 0}, y0, y1, tex_w, tex_h, segs, 2, false);
        vface(m, {x0, 0, z1}, {x1 - x0, 0, 0}, {0, 0, 1}, y0, y1, tex_w, tex_h, segs, 2, false);
        vface(m, {x1, 0, z0}, {x0 - x1, 0, 0}, {0, 0, -1}, y0, y1, tex_w, tex_h, segs, 2, false);
    }

    // floor/ceiling darkening where they meet walls (cheap ambient occlusion)
    float corner_ao(Vec3 p) const {
        float gxf = p.x / CELL, gzf = p.z / CELL;
        int gx = (int)std::lround(gxf), gz = (int)std::lround(gzf);
        bool on_x = std::fabs(gxf - gx) < 1e-3f, on_z = std::fabs(gzf - gz) < 1e-3f;
        float ao = 1.0f;
        if (on_x && on_z) {
            if (W.post(gx, gz)) ao = 0.72f;
            int s = W.solid(gx - 1, gz - 1) + W.solid(gx, gz - 1) + W.solid(gx - 1, gz) + W.solid(gx, gz);
            if (s > 0 && s < 4) ao = std::min(ao, 0.80f);
        } else if (on_x) {
            int cz = floor_to_int(gzf);
            if (W.wall_x(gx, cz) || W.solid(gx - 1, cz) != W.solid(gx, cz)) ao = 0.82f;
        } else if (on_z) {
            int cx = floor_to_int(gxf);
            if (W.wall_z(cx, gz) || W.solid(cx, gz - 1) != W.solid(cx, gz)) ao = 0.82f;
        }
        return ao;
    }

    void set_tint(uint32_t rgb) {
        tint[0] = ((rgb >> 16) & 255) / 255.0f;
        tint[1] = ((rgb >> 8) & 255) / 255.0f;
        tint[2] = (rgb & 255) / 255.0f;
    }
    void clear_tint() { tint[0] = tint[1] = tint[2] = 1; }
};

// texture scale of a level's solid blocks: (metres per texture width, per height)
void block_tex(LevelId id, float& tw, float& th) {
    switch (id) {
        case LevelId::L1: tw = 1.2f; th = 1.2f; break;     // one crate per texture
        case LevelId::L5: tw = CELL; th = 2.9f; break;     // one door per cell
        case LevelId::L8: tw = 4.0f; th = 4.0f; break;
        case LevelId::L9: tw = CELL; th = 4.6f; break;     // window + door per cell face
        case LevelId::L11: tw = CELL; th = 18.0f; break;   // ~3 m per storey
        default: tw = TEX_WALL; th = 2.7f; break;
    }
}

} // namespace

float ceiling_y(const World& w, float x, float z) {
    const LevelInfo& li = w.info();
    if (w.id() != LevelId::L8) return li.ceil_h;
    float n = vnoise_world(w.seed(), x / 2.3f, z / 2.3f, 401) * 0.7f + vnoise_world(w.seed(), x / 0.9f, z / 0.9f, 402) * 0.3f;
    return li.ceil_h - 0.2f - n * 1.0f;
}

void build_chunk_mesh(const World& W, int kx, int kz, ChunkMesh& out) {
    Builder B(W);
    const LevelInfo& li = W.info();
    const LevelId id = W.id();
    const bool cave = id == LevelId::L8;
    const int c0x = kx * CHUNK_CELLS, c0z = kz * CHUNK_CELLS;
    out.exits.clear();
    out.items.clear();
    out.balloons.clear();

    // light sources around the chunk
    int m = (int)(li.light_radius / CELL) + 1;
    for (int cz = c0z - m; cz < c0z + CHUNK_CELLS + m; cz++)
        for (int cx = c0x - m; cx < c0x + CHUNK_CELLS + m; cx++) {
            LightState s = W.light(cx, cz);
            if (s == LightState::On || s == LightState::Flicker)
                B.lights.push_back({{(cx + 0.5f) * CELL, li.light_y, (cz + 0.5f) * CELL}, li.light_power * (s == LightState::On ? 1.0f : 0.75f)});
        }

    float btw, bth;
    block_tex(id, btw, bth);
    const float hw = WALL_T * 0.5f, hp = POST_T * 0.5f, hl = li.pillar_size * 0.5f;
    const float H = li.ceil_h;
    auto floor_uv = [](Vec3 p, float& u, float& t) { u = p.x / CELL; t = p.z / CELL; };
    auto ao = [&](Vec3 p) { return B.corner_ao(p); };
    auto damp = [&](Vec3 p) { return 0.80f + 0.25f * vnoise_world(W.seed(), p.x / 3.1f, p.z / 3.1f, 977); };

    for (int cz = c0z; cz < c0z + CHUNK_CELLS; cz++) {
        for (int cx = c0x; cx < c0x + CHUNK_CELLS; cx++) {
            float x0 = cx * CELL, z0 = cz * CELL, x1 = x0 + CELL, z1 = z0 + CELL;
            bool solid = W.solid(cx, cz);

            if (!solid) {
                // ----- floor + ceiling
                int fm = W.floor_mat(cx, cz);
                Material fmat = fm == 1 ? MAT_FLOOR2 : (fm == 2 ? MAT_FLOOR3 : MAT_FLOOR);
                int sub = li.outdoor && id == LevelId::L11 ? 1 : 2;
                B.grid(fmat, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, -CELL}, sub, sub, {0, 1, 0}, floor_uv,
                       [&](Vec3 p) { return B.corner_ao(p) * damp(p); });
                if (!li.outdoor) {
                    int csub = cave ? 3 : 2;
                    B.grid(MAT_CEIL, {x0, H, z0}, {CELL, 0, 0}, {0, 0, CELL}, csub, csub, {0, -1, 0}, floor_uv, ao,
                           [&](Vec3 p) { if (cave) p.y = ceiling_y(W, p.x, p.z); return p; });
                }

                // ----- light fixture
                LightState ls = W.light(cx, cz);
                if (ls != LightState::None) {
                    float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f;
                    uint8_t lum = ls == LightState::Dead ? 60 : 255;
                    if (id == LevelId::L8) { // glowing fungus on the floor
                        float s = 0.8f, y = 0.02f;
                        B.quad(MAT_LIGHT, {cxm - s, y, czm + s}, {cxm + s, y, czm + s}, {cxm + s, y, czm - s}, {cxm - s, y, czm - s}, lum);
                    } else if (id == LevelId::L9) { // streetlight: pole, head, lamp
                        B.box(MAT_PROP, cxm, czm, 0.07f, 0.07f, 0, li.light_y + 0.2f, 0.5f, 5.0f, 1);
                        B.box(MAT_PROP, cxm, czm + 0.35f, 0.15f, 0.45f, li.light_y + 0.1f, li.light_y + 0.3f, 0.5f, 0.5f, 1);
                        float y = li.light_y + 0.09f;
                        B.quad(MAT_LIGHT, {cxm - 0.12f, y, czm}, {cxm + 0.12f, y, czm}, {cxm + 0.12f, y, czm + 0.7f}, {cxm - 0.12f, y, czm + 0.7f}, lum);
                    } else {
                        float sx = 0.6f, sz = 0.3f;
                        if (id == LevelId::L5) { sx = sz = (W.floor_mat(cx, cz) == 1 ? 0.7f : 0.22f); } // chandelier vs sconce
                        if (id == LevelId::L1) { sx = 0.75f; sz = 0.12f; }                                // long tube
                        float y = (id == LevelId::L1 ? li.light_y : H) - 0.015f;
                        B.quad(MAT_LIGHT, {cxm - sx, y, czm - sz}, {cxm + sx, y, czm - sz}, {cxm + sx, y, czm + sz}, {cxm - sx, y, czm + sz}, lum);
                        if (id == LevelId::L1) { // hanging cables
                            B.box(MAT_PROP, cxm - 0.6f, czm, 0.01f, 0.01f, li.light_y, H, 0.1f, 1, 1);
                            B.box(MAT_PROP, cxm + 0.6f, czm, 0.01f, 0.01f, li.light_y, H, 0.1f, 1, 1);
                        }
                    }
                }

                // ----- the exit door: a free-standing metal door in a frame
                if (W.exit_cell(cx, cz)) {
                    float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f;
                    B.vface(MAT_DOOR, {cxm - 0.6f, 0, czm + 0.12f}, {1.2f, 0, 0}, {0, 0, 1}, 0, 2.25f, 1.2f, 2.25f, 1, 2, false);
                    B.vface(MAT_DOOR, {cxm + 0.6f, 0, czm - 0.12f}, {-1.2f, 0, 0}, {0, 0, -1}, 0, 2.25f, 1.2f, 2.25f, 1, 2, false);
                    B.box(MAT_PROP, cxm - 0.66f, czm, 0.06f, 0.14f, 0, 2.35f, 0.3f, 2.3f, 1);
                    B.box(MAT_PROP, cxm + 0.66f, czm, 0.06f, 0.14f, 0, 2.35f, 0.3f, 2.3f, 1);
                    B.box(MAT_PROP, cxm, czm, 0.72f, 0.14f, 2.25f, 2.37f, 1.4f, 0.2f, 1);
                    out.exits.push_back({cxm, 0, czm});
                }
                if (W.item(cx, cz) != ItemType::None) out.items.push_back(World::item_id(cx, cz));
            } else {
                // ----- solid block: sides facing open (or lower) neighbours, top if visible
                float h = W.block_h(cx, cz);
                B.set_tint(W.tint(cx, cz));
                int sv = li.outdoor ? (id == LevelId::L9 ? 2 : 1) : 2;
                int su = id == LevelId::L11 ? 1 : 2;
                auto side = [&](int nx, int nz, Vec3 o, Vec3 a, Vec3 n) {
                    float nh = W.solid(nx, nz) ? W.block_h(nx, nz) : 0.0f;
                    if (nh >= h - 0.01f) return;
                    B.vface(MAT_BLOCK, o, a, n, nh, h, btw, bth, su, sv, cave);
                };
                side(cx + 1, cz, {x1, 0, z1}, {0, 0, -CELL}, {1, 0, 0});
                side(cx - 1, cz, {x0, 0, z0}, {0, 0, CELL}, {-1, 0, 0});
                side(cx, cz + 1, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, 1});
                side(cx, cz - 1, {x1, 0, z0}, {-CELL, 0, 0}, {0, 0, -1});
                if (id == LevelId::L9) {
                    // gable roof: houses are two cells deep, the ridge runs along x between them
                    int mz = ((cz % 8) + 8) % 8;
                    float ridge = h + 1.8f;
                    bool front = mz == 4; // this cell's roof rises toward +z
                    float y_z1 = front ? ridge : h, y_z0 = front ? h : ridge;
                    Vec3 n = front ? Vec3{0, 0.8f, -0.6f} : Vec3{0, 0.8f, 0.6f};
                    B.grid(MAT_ROOF, {x0 - 0.02f, y_z1, z1}, {CELL + 0.04f, 0, 0}, {0, y_z0 - y_z1, -CELL}, 1, 1, n,
                           [&](Vec3 p, float& u, float& t) { u = p.x / CELL; t = p.z / CELL; },
                           [](Vec3) { return 1.0f; });
                    // half of a gable triangle on each open end
                    for (int sideX = -1; sideX <= 1; sideX += 2) {
                        if (W.solid(cx + sideX, cz)) continue;
                        float x = sideX > 0 ? x1 : x0;
                        Vec3 A{x, h, z0}, Bv{x, h, z1}, C{x, ridge, front ? z1 : z0};
                        Vec3 e1 = Bv - A, e2 = C - A;
                        Vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                        if (cr.x * sideX < 0) std::swap(Bv, C); // make it face outward
                        float c[3];
                        B.shade({x, h + 0.9f, (z0 + z1) / 2}, {(float)sideX, 0, 0}, c);
                        auto& vs = B.v[MAT_BLOCK];
                        auto& is = B.i[MAT_BLOCK];
                        uint16_t b0 = (uint16_t)vs.size();
                        auto uv = [&](Vec3 p) { return std::make_pair(p.z / CELL, 0.02f + (ridge - p.y) / 4.6f * 0.3f); };
                        for (Vec3 p : {A, Bv, C}) { auto t = uv(p); vs.push_back(B.vert(p, t.first, t.second, c, 1)); }
                        is.insert(is.end(), {b0, (uint16_t)(b0 + 1), (uint16_t)(b0 + 2)});
                    }
                } else if (li.outdoor || h < H - 0.01f) {
                    B.grid(MAT_ROOF, {x0, h, z1}, {CELL, 0, 0}, {0, 0, -CELL}, 1, 1, {0, 1, 0}, floor_uv, [](Vec3) { return 1.0f; });
                }
                B.clear_tint();
            }

            // ----- thin walls owned by this cell (west + north edges), corner posts, pillars
            const float WH = li.wall_h;
            const bool low = WH < H - 0.01f;
            auto cap = [&](float xa, float za, float xb, float zb) { // top of a low wall
                B.grid(MAT_WALL, {xa, WH, zb}, {xb - xa, 0, 0}, {0, 0, za - zb}, 1, 1, {0, 1, 0},
                       [&](Vec3 p, float& u, float& t) { u = (p.x + p.z) / TEX_WALL; t = 0.97f; }, [](Vec3) { return 1.0f; });
            };
            if (W.wall_x(cx, cz)) {
                B.vface(MAT_WALL, {x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, 0, WH, TEX_WALL, WH, 2, low ? 1 : 3, false);
                B.vface(MAT_WALL, {x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, 0, WH, TEX_WALL, WH, 2, low ? 1 : 3, false);
                if (low) cap(x0 - hw, z0, x0 + hw, z1);
            }
            if (W.wall_z(cx, cz)) {
                B.vface(MAT_WALL, {x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, 0, WH, TEX_WALL, WH, 2, low ? 1 : 3, false);
                B.vface(MAT_WALL, {x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, 0, WH, TEX_WALL, WH, 2, low ? 1 : 3, false);
                if (low) cap(x0, z0 - hw, x1, z0 + hw);
            }
            if (W.post(cx, cz)) {
                B.box(MAT_WALL, x0, z0, hp, hp, 0, WH, TEX_WALL, WH, 1);
                if (low) cap(x0 - hp, z0 - hp, x0 + hp, z0 + hp);
            } else if (W.pillar(cx, cz)) B.box(MAT_PROP, x0, z0, hl, hl, 0, H, TEX_WALL, H, 1);

            // ----- Level 2: pipes along the tunnel walls
            bool along_x;
            if (W.pipe_run(cx, cz, along_x)) {
                auto pipe = [&](float px, float pz, float hx, float hz, float y0, float y1) {
                    B.box(MAT_PROP, px, pz, hx, hz, y0, y1, 0.6f, 0.6f, 1);
                    B.grid(MAT_PROP, {px - hx, y0, pz - hz}, {hx * 2, 0, 0}, {0, 0, hz * 2}, 1, 1, {0, -1, 0},
                           [&](Vec3 p, float& u, float& t) { u = (p.x + p.z) / 0.6f; t = 0.5f; }, [](Vec3) { return 0.85f; });
                };
                float cxm = x0 + CELL * 0.5f, czm = z0 + CELL * 0.5f;
                for (int side = -1; side <= 1; side += 2) {
                    if (along_x) {
                        if (!W.solid(cx, cz + side)) continue;
                        float pz = czm + side * (CELL * 0.5f - 0.16f);
                        pipe(cxm, pz, CELL * 0.5f + 0.01f, 0.09f, H - 0.42f, H - 0.24f);
                        pipe(cxm, pz - side * 0.05f, CELL * 0.5f + 0.01f, 0.05f, H - 0.75f, H - 0.65f);
                    } else {
                        if (!W.solid(cx + side, cz)) continue;
                        float px = cxm + side * (CELL * 0.5f - 0.16f);
                        pipe(px, czm, 0.09f, CELL * 0.5f + 0.01f, H - 0.42f, H - 0.24f);
                        pipe(px - side * 0.05f, czm, 0.05f, CELL * 0.5f + 0.01f, H - 0.75f, H - 0.65f);
                    }
                }
            }
            if (W.balloon(cx, cz)) {
                float jx = (float)(hash3(W.seed(), cx, cz, 501) % 100u) / 100.0f - 0.5f, jz = (float)(hash3(W.seed(), cx, cz, 502) % 100u) / 100.0f - 0.5f;
                out.balloons.push_back({x0 + CELL * (0.5f + jx * 0.6f), 1.9f + jx * 0.4f, z0 + CELL * (0.5f + jz * 0.6f)});
            }
        }
    }

    out.verts.clear();
    out.idx.clear();
    for (int mat = 0; mat < MAT_COUNT; mat++) {
        if (out.verts.size() + B.v[mat].size() > 65535) { out.first[mat] = (uint32_t)out.idx.size(); out.count[mat] = 0; continue; }
        uint16_t base = (uint16_t)out.verts.size();
        out.first[mat] = (uint32_t)out.idx.size();
        out.count[mat] = (uint32_t)B.i[mat].size();
        out.verts.insert(out.verts.end(), B.v[mat].begin(), B.v[mat].end());
        for (uint16_t ix : B.i[mat]) out.idx.push_back((uint16_t)(ix + base));
    }
}

} // namespace iris
