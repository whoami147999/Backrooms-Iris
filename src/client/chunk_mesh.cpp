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

    // ----- furniture: boxes mapped onto quadrants of the furniture atlas
    struct UV { float u0, v0, u1, v1; };
    bool glow = false; // emissive: lamp shades
    void fface(Material m, Vec3 o, Vec3 a, Vec3 n, float y0, float y1, UV uv) {
        float L = a.len();
        if (L < 1e-4f || y1 - y0 < 1e-4f) return;
        Vec3 ad = a * (1.0f / L);
        Vec3 base{o.x, y0, o.z};
        size_t first = v[m].size();
        grid(m, base, a, Vec3{0, y1 - y0, 0}, 1, 1, n,
             [&](Vec3 p, float& u, float& t) {
                 float s = (Vec3{p.x, 0, p.z} - Vec3{o.x, 0, o.z}).dot(ad) / L, tt = (y1 - p.y) / (y1 - y0);
                 u = uv.u0 + s * (uv.u1 - uv.u0);
                 t = uv.v0 + tt * (uv.v1 - uv.v0);
             },
             [](Vec3 p) { return p.y < 0.01f ? 0.7f : 1.0f; });
        if (glow) for (size_t k = first; k < v[m].size(); k++) { v[m][k].r = 255; v[m][k].g = 215; v[m][k].b = 150; }
    }
    void fbox(Material m, float x0, float z0, float x1, float z1, float y0, float y1, UV side, UV top, UV front, Vec2 face,
              bool bottom = false) {
        fface(m, {x1, 0, z1}, {0, 0, z0 - z1}, {1, 0, 0}, y0, y1, face.x > 0.5f ? front : side);
        fface(m, {x0, 0, z0}, {0, 0, z1 - z0}, {-1, 0, 0}, y0, y1, face.x < -0.5f ? front : side);
        fface(m, {x0, 0, z1}, {x1 - x0, 0, 0}, {0, 0, 1}, y0, y1, face.z > 0.5f ? front : side);
        fface(m, {x1, 0, z0}, {x0 - x1, 0, 0}, {0, 0, -1}, y0, y1, face.z < -0.5f ? front : side);
        auto flat = [&](float y, Vec3 n, bool down) {
            size_t first = v[m].size();
            Vec3 o = down ? Vec3{x0, y, z0} : Vec3{x0, y, z1};
            Vec3 b = down ? Vec3{0, 0, z1 - z0} : Vec3{0, 0, z0 - z1};
            grid(m, o, {x1 - x0, 0, 0}, b, 1, 1, n,
                 [&](Vec3 p, float& u, float& t) {
                     u = top.u0 + (p.x - x0) / std::max(1e-4f, x1 - x0) * (top.u1 - top.u0);
                     t = top.v0 + (p.z - z0) / std::max(1e-4f, z1 - z0) * (top.v1 - top.v0);
                 },
                 [](Vec3) { return 1.0f; });
            if (glow) for (size_t k = first; k < v[m].size(); k++) { v[m][k].r = 255; v[m][k].g = 215; v[m][k].b = 150; }
        };
        flat(y1, {0, 1, 0}, false);
        if (bottom) flat(y0, {0, -1, 0}, true);
    }

    // ----- decals: a quad o, o+A, o+A+B, o+B (A x B points along n) showing atlas cell k
    void decal(Vec3 o, Vec3 A, Vec3 B, Vec3 n, int k) {
        float la = A.len(), lb = B.len();
        if (la < 1e-4f || lb < 1e-4f) return;
        Vec3 ad = A * (1.0f / la), bd = B * (1.0f / lb);
        float u0 = (k % 4) / 4.0f + 0.004f, v0 = (k / 4) / 4.0f + 0.004f, u1 = u0 + 0.242f, v1 = v0 + 0.242f;
        grid(MAT_DECAL, o, A, B, 1, 1, n,
             [&](Vec3 p, float& u, float& t) {
                 float s = (p - o).dot(ad) / la, r = (p - o).dot(bd) / lb;
                 u = u0 + s * (u1 - u0);
                 t = v1 - r * (v1 - v0); // B runs "up" the decal
             },
             [](Vec3) { return 1.0f; });
    }

    void set_tint(uint32_t rgb) {
        tint[0] = ((rgb >> 16) & 255) / 255.0f;
        tint[1] = ((rgb >> 8) & 255) / 255.0f;
        tint[2] = (rgb & 255) / 255.0f;
    }
    void clear_tint() { tint[0] = tint[1] = tint[2] = 1; }
};

// which details turn up on a level's floors and walls
struct DecalSet { int floor[8]; int nfloor; float floor_rate; int wall[6]; int nwall; float wall_rate; };
const DecalSet& decal_set(LevelId id) {
    static const DecalSet OFFICE{{DECAL_PAPER, DECAL_CRUMPLED, DECAL_STAIN, DECAL_STAIN2, DECAL_PRINTS, DECAL_TRASH, DECAL_CAN}, 7, 0.30f,
                                 {DECAL_OUTLET, DECAL_VENT, DECAL_STREAK, DECAL_MOLD, DECAL_SCUFF}, 5, 0.26f};
    static const DecalSet LOBBY{{DECAL_STAIN, DECAL_STAIN2, DECAL_PUDDLE, DECAL_PRINTS, DECAL_PAPER, DECAL_CRUMPLED}, 6, 0.30f,
                                {DECAL_OUTLET, DECAL_OUTLET, DECAL_VENT, DECAL_STREAK, DECAL_MOLD, DECAL_SCUFF}, 6, 0.28f};
    static const DecalSet INDUSTRIAL{{DECAL_CRACK, DECAL_PUDDLE, DECAL_STAIN2, DECAL_TRASH, DECAL_CAN, DECAL_GLASS}, 6, 0.30f,
                                     {DECAL_STREAK, DECAL_CRACK, DECAL_MOLD, DECAL_VENT}, 4, 0.24f};
    static const DecalSet HOTEL{{DECAL_STAIN, DECAL_PAPER, DECAL_GLASS, DECAL_STAIN2}, 4, 0.10f, {DECAL_STREAK, DECAL_SCUFF, DECAL_OUTLET}, 3, 0.12f};
    static const DecalSet STONE{{DECAL_CRACK, DECAL_LEAVES, DECAL_STAIN, DECAL_STAIN2}, 4, 0.16f, {DECAL_CRACK, DECAL_MOLD, DECAL_STREAK}, 3, 0.14f};
    static const DecalSet OUTSIDE{{DECAL_CRACK, DECAL_LEAVES, DECAL_STAIN2, DECAL_TRASH}, 4, 0.12f, {DECAL_STREAK, DECAL_CRACK}, 2, 0.06f};
    static const DecalSet PARTY{{DECAL_PAPER, DECAL_CAN, DECAL_TRASH, DECAL_STAIN, DECAL_CRUMPLED}, 5, 0.20f, {DECAL_SCUFF, DECAL_OUTLET, DECAL_STREAK}, 3, 0.12f};
    static const DecalSet MOON{{DECAL_CRACK}, 1, 0.05f, {DECAL_CRACK}, 1, 0.03f};
    static const DecalSet NONE{{0}, 0, 0, {0}, 0, 0};
    switch (id) {
        case LevelId::L0: case LevelId::L6: case LevelId::TSG: case LevelId::TSGBlack: return LOBBY;
        case LevelId::L4: case LevelId::L21: case LevelId::L33: case LevelId::L36: case LevelId::LRun: case LevelId::TSGHope: return OFFICE;
        case LevelId::L1: case LevelId::L2: case LevelId::L22: case LevelId::L8: return INDUSTRIAL;
        case LevelId::L5: case LevelId::TSGMansion: case LevelId::L7: case LevelId::L19: return HOTEL;
        case LevelId::L51: return STONE;
        case LevelId::L9: case LevelId::L11: return OUTSIDE;
        case LevelId::LFun: return PARTY;
        case LevelId::L24: return MOON;
        default: return NONE;
    }
}

// texture scale of a level's solid blocks: (metres per texture width, per height)
void block_tex(LevelId id, float& tw, float& th) {
    switch (id) {
        case LevelId::L1: tw = 1.2f; th = 1.2f; break;     // one crate per texture
        case LevelId::L5: tw = CELL; th = 2.9f; break;     // one door per cell
        case LevelId::L8: tw = 4.0f; th = 4.0f; break;
        case LevelId::L9: tw = CELL; th = 4.6f; break;     // window + door per cell face
        case LevelId::L11: tw = CELL; th = 18.0f; break;   // ~3 m per storey
        case LevelId::L7: case LevelId::L24: tw = 3.0f; th = 3.0f; break;
        case LevelId::L19: tw = CELL; th = CELL; break;
        case LevelId::L21: tw = 4 * CELL; th = 2.6f; break; // four numbered doors, one per cell
        case LevelId::L22: tw = CELL; th = 1.35f; break;   // a car per cell
        case LevelId::L33: tw = CELL; th = 5.0f; break;    // one shop front per cell
        case LevelId::L36: tw = CELL; th = 4.5f; break;    // one window bay per cell
        case LevelId::L51: tw = 3.0f; th = 3.0f; break;
        case LevelId::LRun: tw = CELL; th = 2.7f; break;
        default: tw = TEX_WALL; th = 2.7f; break;
    }
}

} // namespace

float ceiling_y(const World& w, float x, float z) {
    const LevelInfo& li = w.info();
    if (w.id() == LevelId::L19) { // the roof slopes down from a ridge over the middle of each lane
        float lz = z / CELL;
        float local = lz - floor_to_int(lz / 4.0f) * 4.0f; // 0..4: lane cells 0-2, wall 3
        float dist = std::fabs(local - 1.5f) * CELL;
        return std::max(1.65f, li.ceil_h - dist * 0.3f);
    }
    if (w.id() == LevelId::L51) { // a vast, lumpy cavern roof over the city
        float n = vnoise_world(w.seed(), x / 7.0f, z / 7.0f, 411) * 0.7f + vnoise_world(w.seed(), x / 2.5f, z / 2.5f, 412) * 0.3f;
        return li.ceil_h - 0.5f - n * 2.5f;
    }
    if (w.id() != LevelId::L8) return li.ceil_h;
    float n = vnoise_world(w.seed(), x / 2.3f, z / 2.3f, 401) * 0.7f + vnoise_world(w.seed(), x / 0.9f, z / 0.9f, 402) * 0.3f;
    return li.ceil_h - 0.2f - n * 1.0f;
}

void build_chunk_mesh(const World& W, int kx, int kz, ChunkMesh& out, int detail) {
    Builder B(W);
    const int D = std::max(1, std::min(detail, 3)); // PC: finer grids = smoother baked light
    const LevelInfo& li = W.info();
    const LevelId id = W.id();
    const bool cave = id == LevelId::L8 || id == LevelId::L19 || id == LevelId::L51; // uneven ceilings
    const int c0x = kx * CHUNK_CELLS, c0z = kz * CHUNK_CELLS;
    out.exits.clear();
    out.items.clear();
    out.balloons.clear();
    out.lights.clear();

    // light sources around the chunk
    int m = (int)(li.light_radius / CELL) + 1;
    for (int cz = c0z - m; cz < c0z + CHUNK_CELLS + m; cz++)
        for (int cx = c0x - m; cx < c0x + CHUNK_CELLS + m; cx++) {
            LightState s = W.light(cx, cz);
            if (s == LightState::On || s == LightState::Flicker)
                B.lights.push_back({{(cx + 0.5f) * CELL, li.light_y, (cz + 0.5f) * CELL}, li.light_power * (s == LightState::On ? 1.0f : 0.75f)});
            if (W.hotel()) { // bedside lamps light their rooms a little
                Furn fs[MAX_FURN];
                bool owner = false;
                int nf = W.furniture(cx, cz, fs, &owner);
                for (int k = 0; owner && k < nf; k++)
                    if (fs[k].kind == FurnKind::Lamp)
                        B.lights.push_back({{(fs[k].x0 + fs[k].x1) * 0.5f, 0.95f, (fs[k].z0 + fs[k].z1) * 0.5f}, 0.55f});
            }
        }

    float btw, bth;
    block_tex(id, btw, bth);
    const float hw = WALL_T * 0.5f, hp = POST_T * 0.5f, hl = li.pillar_size * 0.5f;
    const float H = li.ceil_h;
    auto floor_uv = [](Vec3 p, float& u, float& t) { u = p.x / CELL; t = p.z / CELL; };
    auto ao = [&](Vec3 p) { return B.corner_ao(p); };
    auto damp = [&](Vec3 p) { return 0.80f + 0.25f * vnoise_world(W.seed(), p.x / 3.1f, p.z / 3.1f, 977); };
    // a detail on a wall face: outlet, vent, water streak, mould, scuffs, cracks
    auto wall_decal = [&](Vec3 o, Vec3 a, Vec3 n, float top, int hx, int hz) {
        const DecalSet& ds = decal_set(id);
        if (!ds.nwall || hash01(W.seed(), hx, hz, 7101) >= ds.wall_rate) return;
        int d = ds.wall[hash3(W.seed(), hx, hz, 7111) % (uint32_t)ds.nwall];
        float len = a.len();
        Vec3 ad = a * (1.0f / len);
        float w = 0.6f, hh = 0.6f, y = 1.0f;
        switch (d) {
            case DECAL_OUTLET: w = 0.22f; hh = 0.22f; y = 0.3f; break;
            case DECAL_VENT: w = 0.7f; hh = 0.5f; y = top - 0.55f; break;
            case DECAL_STREAK: w = 0.7f; hh = 1.4f; y = top - 0.75f; break;
            case DECAL_SCUFF: w = 0.9f; hh = 0.45f; y = 0.25f; break;
            case DECAL_MOLD: w = 0.8f; hh = 0.8f; y = 0.6f + hash01(W.seed(), hx, hz, 7121) * 1.0f; break;
            default: w = 0.9f; hh = 0.9f; y = 0.7f + hash01(W.seed(), hx, hz, 7121) * 1.0f; break;
        }
        if (y + hh * 0.5f > top - 0.02f) y = top - 0.02f - hh * 0.5f;
        float along = (0.2f + 0.6f * hash01(W.seed(), hx, hz, 7131)) * len;
        Vec3 c = Vec3{o.x, 0, o.z} + ad * along + n * 0.008f;
        Vec3 A = ad * w, Bv{0, hh, 0};
        B.decal(c - A * 0.5f + Vec3{0, y - hh * 0.5f, 0}, A, Bv, n, d);
    };

    for (int cz = c0z; cz < c0z + CHUNK_CELLS; cz++) {
        for (int cx = c0x; cx < c0x + CHUNK_CELLS; cx++) {
            float x0 = cx * CELL, z0 = cz * CELL, x1 = x0 + CELL, z1 = z0 + CELL;
            bool solid = W.solid(cx, cz);

            if (!solid) {
                // ----- floor + ceiling
                int fm = W.floor_mat(cx, cz);
                Material fmat = fm == 1 ? MAT_FLOOR2 : (fm == 2 ? MAT_FLOOR3 : MAT_FLOOR);
                int sub = (li.outdoor && id == LevelId::L11 ? 1 : 2) * D;
                B.grid(fmat, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, -CELL}, sub, sub, {0, 1, 0}, floor_uv,
                       [&](Vec3 p) { return B.corner_ao(p) * damp(p); });
                if (!li.outdoor) {
                    int csub = (cave ? 3 : 2) * D;
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
                        out.lights.push_back({{cxm, 0.08f, czm}, -1.0f, 0.8f, ls}); // sx < 0: a glow on the floor, no shaft
                    } else if (id == LevelId::L51) { // a stone brazier with a fire in it
                        B.box(MAT_PROP, cxm, czm, 0.12f, 0.12f, 0, 1.15f, 0.5f, 1.2f, 1);
                        B.box(MAT_PROP, cxm, czm, 0.3f, 0.3f, 1.15f, 1.4f, 0.6f, 0.3f, 1);
                        float y = 1.41f, s = 0.24f;
                        B.quad(MAT_LIGHT, {cxm - s, y, czm + s}, {cxm + s, y, czm + s}, {cxm + s, y, czm - s}, {cxm - s, y, czm - s}, lum, 1, 0.6f, 0.3f);
                        out.lights.push_back({{cxm, 1.6f, czm}, -1.0f, 0.8f, ls});
                    } else if (W.pole(cx, cz)) { // streetlight: pole, head, lamp
                        B.box(MAT_PROP, cxm, czm, 0.07f, 0.07f, 0, li.light_y + 0.2f, 0.5f, 5.0f, 1);
                        B.box(MAT_PROP, cxm, czm + 0.35f, 0.15f, 0.45f, li.light_y + 0.1f, li.light_y + 0.3f, 0.5f, 0.5f, 1);
                        float y = li.light_y + 0.09f;
                        B.quad(MAT_LIGHT, {cxm - 0.12f, y, czm}, {cxm + 0.12f, y, czm}, {cxm + 0.12f, y, czm + 0.7f}, {cxm - 0.12f, y, czm + 0.7f}, lum);
                        out.lights.push_back({{cxm, y, czm + 0.35f}, 0.12f, 0.35f, ls});
                    } else {
                        float sx = 0.6f, sz = 0.3f;
                        if (W.hotel()) { sx = sz = (W.floor_mat(cx, cz) == 1 ? 0.7f : 0.22f); } // chandelier vs sconce
                        if (id == LevelId::L1) { sx = 0.75f; sz = 0.12f; }                                // long tube
                        float y = (id == LevelId::L1 ? li.light_y : H) - 0.015f;
                        out.lights.push_back({{cxm, y, czm}, sx, sz, ls});
                        B.quad(MAT_LIGHT, {cxm - sx, y, czm - sz}, {cxm + sx, y, czm - sz}, {cxm + sx, y, czm + sz}, {cxm - sx, y, czm + sz}, lum);
                        if (id == LevelId::L1) { // hanging cables
                            B.box(MAT_PROP, cxm - 0.6f, czm, 0.01f, 0.01f, li.light_y, H, 0.1f, 1, 1);
                            B.box(MAT_PROP, cxm + 0.6f, czm, 0.01f, 0.01f, li.light_y, H, 0.1f, 1, 1);
                        }
                    }
                }

                // ----- the exit door: set into the wall, in a steel frame
                ExitDoor door;
                if (W.exit_door(cx, cz, &door)) {
                    Vec3 n{door.normal.x, 0, door.normal.z};
                    Vec3 t{n.z, 0, -n.x}; // along the wall, left to right seen from the cell
                    Vec3 c{door.pos.x, 0, door.pos.z};
                    const float DW = 1.1f, DH = 2.15f;
                    B.vface(MAT_DOOR, c + n * 0.012f - t * (DW * 0.5f), t * DW, n, 0, DH, DW, DH, 1, 2, false);
                    // frame: two jambs and a header standing a little proud of the wall
                    auto frame = [&](Vec3 m, float half_t, float y0, float y1) {
                        float hx = std::fabs(t.x) * half_t + std::fabs(n.x) * 0.045f, hz = std::fabs(t.z) * half_t + std::fabs(n.z) * 0.045f;
                        B.box(MAT_PROP, m.x, m.z, hx, hz, y0, y1, 0.3f, 2.3f, 1);
                    };
                    frame(c + n * 0.03f - t * (DW * 0.5f + 0.04f), 0.045f, 0, DH + 0.08f);
                    frame(c + n * 0.03f + t * (DW * 0.5f + 0.04f), 0.045f, 0, DH + 0.08f);
                    frame(c + n * 0.03f, DW * 0.5f + 0.085f, DH, DH + 0.08f);
                    out.exits.push_back(door);
                }
                if (W.item(cx, cz) != ItemType::None) out.items.push_back(World::item_id(cx, cz));
                // ----- floor details: papers, stains, cracks, puddles
                if (!W.water(cx, cz) && !W.exit_cell(cx, cz)) {
                    const DecalSet& ds = decal_set(id);
                    for (int k = 0; k < 2 && ds.nfloor; k++) {
                        if (hash01(W.seed(), cx, cz, 7001 + k) >= ds.floor_rate) continue;
                        int d = ds.floor[hash3(W.seed(), cx, cz, 7011 + k) % (uint32_t)ds.nfloor];
                        float sz = d == DECAL_PUDDLE ? 1.3f : (d == DECAL_PAPER || d == DECAL_CAN || d == DECAL_CRUMPLED) ? 0.42f : 0.9f;
                        sz *= 0.8f + hash01(W.seed(), cx, cz, 7021 + k) * 0.5f;
                        float ang = hash01(W.seed(), cx, cz, 7031 + k) * 2 * PI;
                        float px = x0 + CELL * (0.2f + 0.6f * hash01(W.seed(), cx, cz, 7041 + k));
                        float pz = z0 + CELL * (0.2f + 0.6f * hash01(W.seed(), cx, cz, 7051 + k));
                        Vec3 A{std::cos(ang) * sz, 0, std::sin(ang) * sz};
                        Vec3 Bv{A.z, 0, -A.x}; // n x A for n = up
                        B.decal(Vec3{px, 0.006f + k * 0.002f, pz} - A * 0.5f - Bv * 0.5f, A, Bv, {0, 1, 0}, d);
                    }
                }
            } else {
                // ----- solid block: sides facing open (or lower) neighbours, top if visible
                float h = W.block_h(cx, cz);
                B.set_tint(W.tint(cx, cz));
                // low furniture-like blocks use a different texture from the tall ones
                Material bmat = MAT_BLOCK;
                float tw = btw, th = bth;
                bool low = h < 2.2f;
                if (id == LevelId::L19 && low) { bmat = MAT_ROOF; tw = 1.2f; th = 1.2f; }   // boxes
                if (id == LevelId::L22 && !low) { bmat = MAT_WALL; tw = TEX_WALL; th = H; } // stairwell cores
                if (id == LevelId::L33 && low) { bmat = MAT_PROP; tw = 1.0f; th = 1.0f; }   // planters, shelves
                if (id == LevelId::L36 && low) { bmat = MAT_ROOF; tw = CELL; th = 1.0f; }   // seats, desks
                int sv = (li.outdoor ? (id == LevelId::L9 ? 2 : 1) : 2) * D;
                int su = (id == LevelId::L11 ? 1 : 2) * D;
                auto side = [&](int nx, int nz, Vec3 o, Vec3 a, Vec3 n) {
                    float nh = W.solid(nx, nz) ? W.block_h(nx, nz) : 0.0f;
                    if (nh >= h - 0.01f) return;
                    B.vface(bmat, o, a, n, nh, h, tw, th, su, sv, cave && !low);
                    if (!low && nh <= 0.0f) wall_decal(o, a, n, std::min(h, H), cx * 7 + nx, cz * 7 + nz);
                };
                float mxc = x0 + CELL * 0.5f, mzc = z0 + CELL * 0.5f;
                if (id == LevelId::L22 && low) { // a parked car: body, cabin, wheels (the bay itself still blocks you)
                    B.box(MAT_BLOCK, mxc, mzc, 0.88f, 1.15f, 0.25f, 0.85f, CELL, 1.35f, 1);
                    B.grid(MAT_ROOF, {mxc - 0.88f, 0.85f, mzc + 1.15f}, {1.76f, 0, 0}, {0, 0, -2.3f}, 1, 1, {0, 1, 0},
                           [&](Vec3 p, float& u, float& t) { u = (p.z - mzc + 1.15f) / 2.3f; t = (p.x - mxc + 0.88f) / 1.76f; }, [](Vec3) { return 1.0f; });
                    B.box(MAT_ROOF, mxc, mzc + 0.1f, 0.78f, 0.62f, 0.85f, 1.32f, 2.3f, 1.0f, 1);
                    B.grid(MAT_ROOF, {mxc - 0.78f, 1.32f, mzc + 0.72f}, {1.56f, 0, 0}, {0, 0, -1.24f}, 1, 1, {0, 1, 0},
                           [&](Vec3 p, float& u, float& t) { u = 0.5f; t = (p.x - mxc) * 0.1f + 0.5f; }, [](Vec3) { return 1.0f; });
                    for (int wx = -1; wx <= 1; wx += 2)
                        for (int wz = -1; wz <= 1; wz += 2)
                            B.box(MAT_FLOOR3, mxc + wx * 0.8f, mzc + wz * 0.75f, 0.12f, 0.3f, 0, 0.55f, 0.6f, 0.6f, 1);
                } else if (id == LevelId::L36 && low && h < 0.8f) { // a row of airport seats with backs
                    for (int k = 0; k < 4; k++) {
                        float sx = x0 + CELL * (k + 0.5f) / 4.0f;
                        B.box(MAT_ROOF, sx, mzc, 0.27f, 0.25f, 0.38f, 0.48f, CELL / 4.0f, 0.5f, 1);
                        B.box(MAT_ROOF, sx, mzc - 0.24f, 0.27f, 0.04f, 0.48f, 0.95f, CELL / 4.0f, 0.5f, 1);
                        B.box(MAT_PROP, sx, mzc, 0.03f, 0.03f, 0, 0.38f, 0.2f, 0.4f, 1);
                    }
                } else {
                    side(cx + 1, cz, {x1, 0, z1}, {0, 0, -CELL}, {1, 0, 0});
                    side(cx - 1, cz, {x0, 0, z0}, {0, 0, CELL}, {-1, 0, 0});
                    side(cx, cz + 1, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, 1});
                    side(cx, cz - 1, {x1, 0, z0}, {-CELL, 0, 0}, {0, 0, -1});
                }
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
                } else if ((li.outdoor || h < H - 0.01f) && !(id == LevelId::L22 && low) && !(id == LevelId::L36 && low && h < 0.8f)) {
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
                bool tall = low && W.exit_on_edge(cx, cz, true); // office: the exit's wall section is full height
                float h = tall ? H : WH;
                B.vface(MAT_WALL, {x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, 0, h, TEX_WALL, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (!low) { wall_decal({x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, h, cx * 3, cz * 3 + 1); wall_decal({x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, h, cx * 3 + 1, cz * 3); }
                B.vface(MAT_WALL, {x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, 0, h, TEX_WALL, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (tall) { // ends of the tall section
                    B.vface(MAT_WALL, {x0 - hw, 0, z1}, {WALL_T, 0, 0}, {0, 0, 1}, WH, H, TEX_WALL, H, 1, 1, false);
                    B.vface(MAT_WALL, {x0 + hw, 0, z0}, {-WALL_T, 0, 0}, {0, 0, -1}, WH, H, TEX_WALL, H, 1, 1, false);
                } else if (low) cap(x0 - hw, z0, x0 + hw, z1);
            }
            if (W.wall_z(cx, cz)) {
                bool tall = low && W.exit_on_edge(cx, cz, false);
                float h = tall ? H : WH;
                B.vface(MAT_WALL, {x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, 0, h, TEX_WALL, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (!low) { wall_decal({x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, h, cx * 3 + 2, cz * 3); wall_decal({x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, h, cx * 3, cz * 3 + 2); }
                B.vface(MAT_WALL, {x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, 0, h, TEX_WALL, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (tall) {
                    B.vface(MAT_WALL, {x0, 0, z0 - hw}, {0, 0, WALL_T}, {-1, 0, 0}, WH, H, TEX_WALL, H, 1, 1, false);
                    B.vface(MAT_WALL, {x1, 0, z0 + hw}, {0, 0, -WALL_T}, {1, 0, 0}, WH, H, TEX_WALL, H, 1, 1, false);
                } else if (low) cap(x0, z0 - hw, x1, z0 + hw);
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
            // ----- Level 5: guest-room furniture and doorways (drawn once per room)
            if (W.hotel()) {
                Furn fs[MAX_FURN];
                bool owner = false;
                int nf = W.furniture(cx, cz, fs, &owner);
                if (owner) {
                    const Builder::UV WOOD{0.01f, 0.01f, 0.49f, 0.49f}, SHEET{0.51f, 0.01f, 0.99f, 0.49f};
                    const Builder::UV VELVET{0.01f, 0.51f, 0.49f, 0.99f}, DRAWER{0.51f, 0.51f, 0.99f, 0.74f};
                    const Builder::UV PAINT{0.51f, 0.76f, 0.99f, 0.99f};
                    for (int k = 0; k < nf; k++) {
                        const Furn& f = fs[k];
                        float mx = (f.x0 + f.x1) * 0.5f, mz = (f.z0 + f.z1) * 0.5f, hx = (f.x1 - f.x0) * 0.5f, hz = (f.z1 - f.z0) * 0.5f;
                        switch (f.kind) {
                            case FurnKind::WallPiece: B.box(MAT_WALL, mx, mz, hx, hz, 0, H, TEX_WALL, H, 1); break;
                            case FurnKind::Lintel:
                                B.box(MAT_WALL, mx, mz, hx, hz, f.y0, f.y1, TEX_WALL, H, 1);
                                B.grid(MAT_PROP, {f.x0, f.y0, f.z0}, {f.x1 - f.x0, 0, 0}, {0, 0, f.z1 - f.z0}, 1, 1, {0, -1, 0},
                                       [&](Vec3 p, float& u, float& t) { u = p.x + p.z; t = 0.5f; }, [](Vec3) { return 0.8f; });
                                break;
                            case FurnKind::DoorLeaf: B.fbox(MAT_FURN, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, WOOD, f.face); break;
                            case FurnKind::Bed: {
                                const float in = 0.03f;
                                B.fbox(MAT_FURN, f.x0, f.z0, f.x1, f.z1, 0, 0.28f, WOOD, WOOD, WOOD, f.face);
                                B.fbox(MAT_FURN, f.x0 + in, f.z0 + in, f.x1 - in, f.z1 - in, 0.28f, 0.55f, SHEET, SHEET, SHEET, f.face);
                                // a velvet blanket over the foot half, pillows at the head
                                Vec2 fd = f.face;
                                float bx0 = f.x0, bz0 = f.z0, bx1 = f.x1, bz1 = f.z1;
                                if (fd.x > 0.5f) bx0 = (f.x0 + f.x1) * 0.5f - 0.1f;
                                if (fd.x < -0.5f) bx1 = (f.x0 + f.x1) * 0.5f + 0.1f;
                                if (fd.z > 0.5f) bz0 = (f.z0 + f.z1) * 0.5f - 0.1f;
                                if (fd.z < -0.5f) bz1 = (f.z0 + f.z1) * 0.5f + 0.1f;
                                B.fbox(MAT_FURN, bx0 + 0.01f, bz0 + 0.01f, bx1 - 0.01f, bz1 - 0.01f, 0.30f, 0.58f, VELVET, VELVET, VELVET, f.face);
                                for (int side = -1; side <= 1; side += 2) {
                                    Vec2 across{std::fabs(fd.z), std::fabs(fd.x)}; // perpendicular to the bed's length
                                    Vec2 head{mx - fd.x * (std::fabs(fd.x) * hx - 0.3f), mz - fd.z * (std::fabs(fd.z) * hz - 0.3f)};
                                    Vec2 c = head + across * (side * 0.36f);
                                    float px = across.x * 0.3f + std::fabs(fd.x) * 0.18f, pz = across.z * 0.3f + std::fabs(fd.z) * 0.18f;
                                    B.fbox(MAT_FURN, c.x - px, c.z - pz, c.x + px, c.z + pz, 0.55f, 0.66f, SHEET, SHEET, SHEET, f.face);
                                }
                                break;
                            }
                            case FurnKind::Headboard: B.fbox(MAT_FURN, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, VELVET, f.face); break;
                            case FurnKind::Nightstand:
                            case FurnKind::Dresser:
                            case FurnKind::Wardrobe: B.fbox(MAT_FURN, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, DRAWER, f.face); break;
                            case FurnKind::Lamp: {
                                B.fbox(MAT_FURN, mx - 0.025f, mz - 0.025f, mx + 0.025f, mz + 0.025f, f.y0, f.y0 + 0.22f, WOOD, WOOD, WOOD, f.face);
                                B.glow = true;
                                B.fbox(MAT_FURN, f.x0, f.z0, f.x1, f.z1, f.y0 + 0.2f, f.y1, SHEET, SHEET, SHEET, f.face, true);
                                B.glow = false;
                                break;
                            }
                            case FurnKind::Painting: B.fbox(MAT_FURN, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, PAINT, f.face); break;
                            case FurnKind::Rug:
                                B.grid(MAT_FURN, {f.x0, f.y1, f.z1}, {f.x1 - f.x0, 0, 0}, {0, 0, f.z0 - f.z1}, 1, 1, {0, 1, 0},
                                       [&](Vec3 p, float& u, float& t) {
                                           u = VELVET.u0 + (p.x - f.x0) / (f.x1 - f.x0) * (VELVET.u1 - VELVET.u0);
                                           t = VELVET.v0 + (p.z - f.z0) / (f.z1 - f.z0) * (VELVET.v1 - VELVET.v0);
                                       },
                                       [](Vec3) { return 0.85f; });
                                break;
                        }
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
