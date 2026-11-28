#include "chunk_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

    explicit Builder(const World& w) : W(w), li(w.info()) {
        buildings = w.houses() || w.city() || w.court();
    }
    bool buildings = false;   // an outdoor level with buildings you can walk into: indoors is darker
    bool terr = false;        // floors follow the terrain: work out their normals

    Vec3 ground_normal(float x, float z) const {
        const float e = 0.35f;
        float dx = W.floor_y(x + e, z) - W.floor_y(x - e, z), dz = W.floor_y(x, z + e) - W.floor_y(x, z - e);
        Vec3 n{-dx, 2 * e, -dz};
        return n * (1.0f / n.len());
    }

    // Baked lighting: ambient + light sources in range, attenuated, Lambert, dimmed hard
    // when something is in the way (so dead-light rooms are actually dark).
    void shade(Vec3 p, Vec3 n, float out[3]) const {
        out[0] = li.ambient[0]; out[1] = li.ambient[1]; out[2] = li.ambient[2];
        bool inside = false;
        if (buildings && li.outdoor) { // under a roof the daylight is a lot dimmer
            int scx = floor_to_int((p.x + n.x * 0.06f) / CELL), scz = floor_to_int((p.z + n.z * 0.06f) / CELL);
            if (W.indoors(scx, scz) && p.y < W.indoor_h(scx, scz) + 0.05f) {
                inside = true;
                for (int k = 0; k < 3; k++) out[k] *= 0.42f;
            }
        }
        if (li.outdoor && !li.dark && li.ambient[0] > 0.5f && !inside) {
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
                shade(p, (terr && n.y > 0.9f) ? ground_normal(p.x, p.z) : n, c);
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

    // ----- curves: cylinders, cones, spheres. Every vertex gets its own normal, so the baked
    // lighting rolls smoothly around them.
    static UV cell(int k) { float u0 = (k % 4) / 4.0f + 0.006f, v0 = (k / 4) / 4.0f + 0.006f; return {u0, v0, u0 + 0.238f, v0 + 0.238f}; }
    int seg_base = 8; // segments around a full circle (more on PC)
    void emit_quad(Material m, const Vertex& a, const Vertex& b2, const Vertex& c, const Vertex& d) {
        auto& vs = v[m];
        auto& is = i[m];
        if (vs.size() + 4 > 65000) return;
        uint16_t q = (uint16_t)vs.size();
        vs.insert(vs.end(), {a, b2, c, d});
        is.insert(is.end(), {q, (uint16_t)(q + 1), (uint16_t)(q + 2), q, (uint16_t)(q + 2), (uint16_t)(q + 3)});
    }
    Vertex lit(Vec3 p, Vec3 n, float u, float t, float ao = 1.0f) {
        float c[3];
        shade(p, n, c);
        Vertex vx = vert(p, u, t, c, ao);
        if (glow) { vx.r = 255; vx.g = (uint8_t)(215 * glow_g); vx.b = (uint8_t)(150 * glow_b); }
        return vx;
    }
    float glow_g = 1, glow_b = 1;
    // vertical cylinder / cone: radius r0 at y0, r1 at y1, centred on (x, z)
    void cyl(Material m, float x, float z, float r0, float r1, float y0, float y1, UV uv, bool top = true, bool bottom = false, float seg_mul = 1.0f) {
        int seg = std::max(6, (int)(seg_base * seg_mul));
        float slope = (r0 - r1) / std::max(0.01f, y1 - y0);
        for (int k = 0; k < seg; k++) {
            float a0 = 2 * PI * k / seg, a1 = 2 * PI * (k + 1) / seg;
            Vec3 n0{std::cos(a0), slope, std::sin(a0)}, n1{std::cos(a1), slope, std::sin(a1)};
            n0 = n0 * (1.0f / n0.len()); n1 = n1 * (1.0f / n1.len());
            float u0 = uv.u0 + (uv.u1 - uv.u0) * k / seg, u1 = uv.u0 + (uv.u1 - uv.u0) * (k + 1) / seg;
            Vec3 b0{x + std::cos(a0) * r0, y0, z + std::sin(a0) * r0}, b1{x + std::cos(a1) * r0, y0, z + std::sin(a1) * r0};
            Vec3 t0{x + std::cos(a0) * r1, y1, z + std::sin(a0) * r1}, t1{x + std::cos(a1) * r1, y1, z + std::sin(a1) * r1};
            float ao0 = y0 < 0.01f ? 0.7f : 1.0f;
            // CCW seen from outside: b1, b0, t0, t1
            emit_quad(m, lit(b1, n1, u1, uv.v1, ao0), lit(b0, n0, u0, uv.v1, ao0), lit(t0, n0, u0, uv.v0), lit(t1, n1, u1, uv.v0));
            if (top && r1 > 0.005f) {
                Vec3 c{x, y1, z};
                float cu = (uv.u0 + uv.u1) * 0.5f, cv = (uv.v0 + uv.v1) * 0.5f;
                Vertex vc = lit(c, {0, 1, 0}, cu, cv), va = lit(t0, {0, 1, 0}, u0, uv.v0), vb = lit(t1, {0, 1, 0}, u1, uv.v0);
                emit_quad(m, vc, vb, va, vc);
            }
            if (bottom && r0 > 0.005f) {
                Vec3 c{x, y0, z};
                Vertex vc = lit(c, {0, -1, 0}, uv.u0, uv.v0), va = lit(b0, {0, -1, 0}, u0, uv.v1), vb = lit(b1, {0, -1, 0}, u1, uv.v1);
                emit_quad(m, vc, va, vb, vc);
            }
        }
    }
    // a cylinder lying between two points (pipes, wheels, rails)
    void tube(Material m, Vec3 a, Vec3 bb, float r, UV uv, float seg_mul = 1.0f) {
        Vec3 ax = bb - a;
        float len = ax.len();
        if (len < 1e-4f) return;
        Vec3 d = ax * (1.0f / len);
        Vec3 up = std::fabs(d.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 e1{d.y * up.z - d.z * up.y, d.z * up.x - d.x * up.z, d.x * up.y - d.y * up.x};
        e1 = e1 * (1.0f / e1.len());
        Vec3 e2{d.y * e1.z - d.z * e1.y, d.z * e1.x - d.x * e1.z, d.x * e1.y - d.y * e1.x};
        int seg = std::max(6, (int)(seg_base * seg_mul));
        for (int k = 0; k < seg; k++) {
            float a0 = 2 * PI * k / seg, a1 = 2 * PI * (k + 1) / seg;
            Vec3 n0 = e1 * std::cos(a0) + e2 * std::sin(a0), n1 = e1 * std::cos(a1) + e2 * std::sin(a1);
            float u0 = uv.u0 + (uv.u1 - uv.u0) * k / seg, u1 = uv.u0 + (uv.u1 - uv.u0) * (k + 1) / seg;
            emit_quad(m, lit(a + n0 * r, n0, u0, uv.v1), lit(a + n1 * r, n1, u1, uv.v1), lit(bb + n1 * r, n1, u1, uv.v0), lit(bb + n0 * r, n0, u0, uv.v0));
        }
    }
    void sphere(Material m, Vec3 c, Vec3 r, UV uv, float seg_mul = 1.0f) {
        int seg = std::max(6, (int)(seg_base * seg_mul)), rings = std::max(4, seg / 2);
        for (int j = 0; j < rings; j++) {
            float p0 = -PI / 2 + PI * j / rings, p1 = -PI / 2 + PI * (j + 1) / rings;
            for (int k = 0; k < seg; k++) {
                float a0 = 2 * PI * k / seg, a1 = 2 * PI * (k + 1) / seg;
                auto P = [&](float ph, float a) { return Vec3{std::cos(ph) * std::cos(a), std::sin(ph), std::cos(ph) * std::sin(a)}; };
                Vec3 n00 = P(p0, a0), n01 = P(p0, a1), n10 = P(p1, a0), n11 = P(p1, a1);
                auto at = [&](Vec3 n) { return Vec3{c.x + n.x * r.x, c.y + n.y * r.y, c.z + n.z * r.z}; };
                float u0 = uv.u0 + (uv.u1 - uv.u0) * k / seg, u1 = uv.u0 + (uv.u1 - uv.u0) * (k + 1) / seg;
                float v0 = uv.v1 - (uv.v1 - uv.v0) * j / rings, v1 = uv.v1 - (uv.v1 - uv.v0) * (j + 1) / rings;
                emit_quad(m, lit(at(n01), n01, u1, v0), lit(at(n00), n00, u0, v0), lit(at(n10), n10, u0, v1), lit(at(n11), n11, u1, v1));
            }
        }
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

    // ----- furniture and props
    void furniture(const Furn& f, float H) {
        const UV WOOD = cell(FT_WOOD), SHEET = cell(FT_SHEET), VELVET = cell(FT_VELVET), DRAWER = cell(FT_DRAWER);
        const UV PAINT = cell(FT_PAINTING), METAL = cell(FT_METAL), LOCKER = cell(FT_LOCKER), CARD = cell(FT_CARDBOARD);
        const UV PLASTIC = cell(FT_PLASTIC), LEAVES = cell(FT_LEAVES), BARK = cell(FT_BARK), RUST = cell(FT_RUST);
        const UV FABRIC = cell(FT_FABRIC), STONE = cell(FT_STONE), GLOWC = cell(FT_GLOW), RUBBER = cell(FT_RUBBER);
        const Material M = MAT_FURN;
        float mx = (f.x0 + f.x1) * 0.5f, mz = (f.z0 + f.z1) * 0.5f, hx = (f.x1 - f.x0) * 0.5f, hz = (f.z1 - f.z0) * 0.5f;
        Vec2 fd = f.face;
        float rad = std::min(hx, hz);
        uint32_t hsh = (uint32_t)(std::fabs(mx * 131.0f + mz * 71.0f));
        auto legs = [&](float inset, float top, float r) {
            for (int a = -1; a <= 1; a += 2)
                for (int b = -1; b <= 1; b += 2) cyl(M, mx + a * (hx - inset), mz + b * (hz - inset), r, r, 0, top, METAL, false, false, 0.5f);
        };
        switch (f.kind) {
            case FurnKind::WallPiece: box(MAT_WALL, mx, mz, hx, hz, 0, H, 1.25f, H, 1); break;
            case FurnKind::Lintel: { // a smooth arch over the doorway: both faces and the curved underside
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                float w = along_x ? (f.x1 - f.x0) : (f.z1 - f.z0);
                float th = along_x ? hz : hx; // half thickness
                int segs = std::max(6, seg_base);
                float spring = 1.95f, rise = std::min(0.5f, H - 2.0f);
                const float TW = 1.25f;
                auto P = [&](float s_, float y, float side) { // along-wall coordinate, height, which face
                    return along_x ? Vec3{mx + s_, y, mz + side * th} : Vec3{mx + side * th, y, mz + s_};
                };
                Vec3 nf = along_x ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
                for (int k = 0; k < segs; k++) {
                    float t0 = (float)k / segs, t1 = (float)(k + 1) / segs;
                    float s0 = -w * 0.5f + w * t0, s1 = -w * 0.5f + w * t1;
                    float y0 = spring + std::sin(t0 * PI) * rise, y1 = spring + std::sin(t1 * PI) * rise;
                    auto uv = [&](float s_, float y, float& u, float& t) { u = (along_x ? mx + s_ : mz + s_) / TW; t = (H - y) / H; };
                    float ua, ta, ub, tb, uc, tc, ud, td;
                    uv(s0, y0, ua, ta); uv(s1, y1, ub, tb); uv(s1, H, uc, tc); uv(s0, H, ud, td);
                    // front face (+n): CCW seen from +n
                    if (along_x) {
                        emit_quad(MAT_WALL, lit(P(s0, y0, 1), nf, ua, ta), lit(P(s1, y1, 1), nf, ub, tb), lit(P(s1, H, 1), nf, uc, tc), lit(P(s0, H, 1), nf, ud, td));
                        emit_quad(MAT_WALL, lit(P(s1, y1, -1), nf * -1, ub, tb), lit(P(s0, y0, -1), nf * -1, ua, ta), lit(P(s0, H, -1), nf * -1, ud, td), lit(P(s1, H, -1), nf * -1, uc, tc));
                    } else {
                        emit_quad(MAT_WALL, lit(P(s1, y1, 1), nf, ub, tb), lit(P(s0, y0, 1), nf, ua, ta), lit(P(s0, H, 1), nf, ud, td), lit(P(s1, H, 1), nf, uc, tc));
                        emit_quad(MAT_WALL, lit(P(s0, y0, -1), nf * -1, ua, ta), lit(P(s1, y1, -1), nf * -1, ub, tb), lit(P(s1, H, -1), nf * -1, uc, tc), lit(P(s0, H, -1), nf * -1, ud, td));
                    }
                    // underside: faces down/inward along the curve
                    float tm = (t0 + t1) * 0.5f;
                    float dy = std::cos(tm * PI) * rise * PI / w; // slope of the curve
                    Vec3 n = along_x ? Vec3{dy, -1, 0} : Vec3{0, -1, dy};
                    n = n * (1.0f / n.len());
                    Vertex a = lit(P(s0, y0, -1), n, ua, 0.95f), b2 = lit(P(s1, y1, -1), n, ub, 0.95f), c = lit(P(s1, y1, 1), n, ub, 0.97f), d = lit(P(s0, y0, 1), n, ua, 0.97f);
                    if (along_x) emit_quad(MAT_WALL, b2, a, d, c);
                    else emit_quad(MAT_WALL, a, b2, c, d);
                }
                break;
            }
            case FurnKind::DoorLeaf: {
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, DRAWER, f.face);
                bool along_z = (f.z1 - f.z0) > (f.x1 - f.x0);
                float kx = along_z ? mx : mx - hx * 0.7f, kz = along_z ? f.z0 + 0.12f : mz;
                cyl(M, kx, kz, 0.035f, 0.035f, 1.0f, 1.08f, METAL, true, true, 0.5f);
                break;
            }
            case FurnKind::Bed: {
                const float in = 0.03f;
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, 0.28f, WOOD, WOOD, WOOD, f.face);
                fbox(M, f.x0 + in, f.z0 + in, f.x1 - in, f.z1 - in, 0.28f, 0.55f, SHEET, SHEET, SHEET, f.face);
                float bx0 = f.x0, bz0 = f.z0, bx1 = f.x1, bz1 = f.z1;
                if (fd.x > 0.5f) bx0 = mx - 0.1f;
                if (fd.x < -0.5f) bx1 = mx + 0.1f;
                if (fd.z > 0.5f) bz0 = mz - 0.1f;
                if (fd.z < -0.5f) bz1 = mz + 0.1f;
                fbox(M, bx0 + 0.01f, bz0 + 0.01f, bx1 - 0.01f, bz1 - 0.01f, 0.30f, 0.58f, VELVET, VELVET, VELVET, f.face);
                for (int side = -1; side <= 1; side += 2) { // pillows: soft and round
                    Vec2 across{std::fabs(fd.z), std::fabs(fd.x)};
                    Vec2 head{mx - fd.x * (std::fabs(fd.x) * hx - 0.3f), mz - fd.z * (std::fabs(fd.z) * hz - 0.3f)};
                    Vec2 c = head + across * (side * 0.36f);
                    sphere(M, {c.x, 0.6f, c.z}, {across.x * 0.3f + std::fabs(fd.x) * 0.17f, 0.07f, across.z * 0.3f + std::fabs(fd.z) * 0.17f}, SHEET, 0.75f);
                }
                break;
            }
            case FurnKind::Headboard: fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, VELVET, f.face); break;
            case FurnKind::Nightstand: case FurnKind::Dresser: case FurnKind::Wardrobe:
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0 + 0.06f, f.y1, WOOD, WOOD, DRAWER, f.face);
                legs(0.04f, 0.06f, 0.025f);
                break;
            case FurnKind::Lamp: { // table lamp, or a floor lamp when it's tall
                float pole = std::max(0.22f, (f.y1 - f.y0) - 0.38f);
                cyl(M, mx, mz, 0.08f, 0.05f, f.y0, f.y0 + 0.03f, METAL, true, false, 0.75f);
                cyl(M, mx, mz, 0.015f, 0.015f, f.y0, f.y0 + pole, METAL, false, false, 0.5f);
                glow = true;
                cyl(M, mx, mz, 0.13f, 0.08f, f.y0 + pole - 0.02f, f.y1, GLOWC, false, true, 1.0f);
                glow = false;
                break;
            }
            case FurnKind::Couch: { // seat, back on the wall side, two arms, cushions
                Vec2 back{-fd.x, -fd.z};
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.1f, 0.45f, FABRIC, FABRIC, FABRIC, fd);
                float bx0 = f.x0, bx1 = f.x1, bz0 = f.z0, bz1 = f.z1;
                if (back.z < -0.5f) bz1 = f.z0 + 0.22f; else if (back.z > 0.5f) bz0 = f.z1 - 0.22f;
                else if (back.x < -0.5f) bx1 = f.x0 + 0.22f; else bx0 = f.x1 - 0.22f;
                fbox(M, bx0, bz0, bx1, bz1, 0.45f, f.y1, FABRIC, FABRIC, FABRIC, fd);
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                for (int e = -1; e <= 1; e += 2) {
                    float ax = along_x ? (e < 0 ? f.x0 : f.x1 - 0.16f) : f.x0, az = along_x ? f.z0 : (e < 0 ? f.z0 : f.z1 - 0.16f);
                    fbox(M, ax, az, along_x ? ax + 0.16f : f.x1, along_x ? f.z1 : az + 0.16f, 0.1f, 0.62f, FABRIC, FABRIC, FABRIC, fd);
                }
                for (int k = 0; k < 2; k++) { // cushions: soft and round
                    float t = (k + 0.5f) / 2.0f;
                    float cxp = along_x ? f.x0 + 0.2f + (f.x1 - f.x0 - 0.4f) * t : mx, czp = along_x ? mz : f.z0 + 0.2f + (f.z1 - f.z0 - 0.4f) * t;
                    sphere(M, {cxp + fd.x * 0.05f, 0.5f, czp + fd.z * 0.05f}, {along_x ? 0.36f : 0.28f, 0.08f, along_x ? 0.28f : 0.36f}, FABRIC, 0.75f);
                }
                fbox(M, f.x0 + 0.05f, f.z0 + 0.05f, f.x1 - 0.05f, f.z1 - 0.05f, 0, 0.1f, WOOD, WOOD, WOOD, fd);
                break;
            }
            case FurnKind::Tv: { // a low cabinet with an old box TV on it, screen glowing faintly
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, 0.5f, WOOD, WOOD, DRAWER, fd);
                float tx = mx - fd.x * 0.02f, tz = mz - fd.z * 0.02f;
                float sx = std::fabs(fd.z) * 0.32f + std::fabs(fd.x) * 0.16f, sz = std::fabs(fd.x) * 0.32f + std::fabs(fd.z) * 0.16f;
                fbox(M, tx - sx, tz - sz, tx + sx, tz + sz, 0.5f, 1.05f, RUBBER, RUBBER, RUBBER, fd);
                glow = true; glow_g = 0.95f; glow_b = 1.1f;
                Vec3 sc{tx + fd.x * (std::fabs(fd.x) * sx + 0.005f), 0, tz + fd.z * (std::fabs(fd.z) * sz + 0.005f)};
                float qx = std::fabs(fd.z) * 0.26f + std::fabs(fd.x) * 0.002f, qz = std::fabs(fd.x) * 0.26f + std::fabs(fd.z) * 0.002f;
                fbox(M, sc.x - qx, sc.z - qz, sc.x + qx, sc.z + qz, 0.57f, 0.98f, GLOWC, GLOWC, GLOWC, fd);
                glow = false; glow_g = glow_b = 1;
                break;
            }
            case FurnKind::Fridge:
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, METAL, METAL, METAL, fd);
                {
                    Vec3 hc{mx + fd.x * (std::fabs(fd.x) * hx + 0.03f), 0, mz + fd.z * (std::fabs(fd.z) * hz + 0.03f)};
                    Vec2 side{std::fabs(fd.z), std::fabs(fd.x)};
                    hc.x += side.x * (hx - 0.08f); hc.z += side.z * (hz - 0.08f);
                    cyl(M, hc.x, hc.z, 0.012f, 0.012f, 0.9f, 1.5f, METAL, true, true, 0.5f);
                }
                break;
            case FurnKind::Scarecrow: // a cross of wood, a sack head, an old coat
                cyl(M, mx, mz, 0.05f, 0.04f, 0, 2.0f, WOOD, true, false, 0.75f);
                tube(M, {mx - 0.6f, 1.55f, mz}, {mx + 0.6f, 1.55f, mz}, 0.04f, WOOD, 0.75f);
                fbox(M, mx - 0.28f, mz - 0.12f, mx + 0.28f, mz + 0.12f, 0.9f, 1.6f, FABRIC, FABRIC, FABRIC, fd);
                sphere(M, {mx, 1.82f, mz}, {0.16f, 0.19f, 0.15f}, CARD, 0.75f);
                cyl(M, mx, mz, 0.28f, 0.1f, 1.95f, 2.12f, CARD, true, true, 0.75f); // hat
                break;
            case FurnKind::Painting: fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, WOOD, WOOD, PAINT, f.face); break;
            case FurnKind::Rug:
                grid(M, {f.x0, f.y1, f.z1}, {f.x1 - f.x0, 0, 0}, {0, 0, f.z0 - f.z1}, 1, 1, {0, 1, 0},
                     [&](Vec3 p, float& u, float& t) {
                         u = VELVET.u0 + (p.x - f.x0) / (f.x1 - f.x0) * (VELVET.u1 - VELVET.u0);
                         t = VELVET.v0 + (p.z - f.z0) / (f.z1 - f.z0) * (VELVET.v1 - VELVET.v0);
                     },
                     [](Vec3) { return 0.85f; });
                break;
            case FurnKind::Chair: {
                bool fabric = hsh & 1;
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.42f, 0.48f, fabric ? FABRIC : WOOD, fabric ? FABRIC : WOOD, fabric ? FABRIC : WOOD, fd);
                legs(0.04f, 0.42f, 0.018f);
                // the backrest sits on the side away from where it faces
                float bx0 = f.x0, bx1 = f.x1, bz0 = f.z0, bz1 = f.z1;
                if (fd.x > 0.5f) bx1 = f.x0 + 0.05f;
                else if (fd.x < -0.5f) bx0 = f.x1 - 0.05f;
                else if (fd.z > 0.5f) bz1 = f.z0 + 0.05f;
                else bz0 = f.z1 - 0.05f;
                fbox(M, bx0, bz0, bx1, bz1, 0.48f, f.y1, fabric ? FABRIC : WOOD, fabric ? FABRIC : WOOD, fabric ? FABRIC : WOOD, fd);
                break;
            }
            case FurnKind::Table:
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y1 - 0.05f, f.y1, WOOD, WOOD, WOOD, fd, true);
                legs(0.06f, f.y1 - 0.05f, 0.03f);
                break;
            case FurnKind::RoundTable: { // a white tablecloth draped over a round table
                float r = rad;
                cyl(M, mx, mz, r + 0.05f, r, 0.35f, f.y1, SHEET, true, false, 1.5f);
                cyl(M, mx, mz, 0.06f, 0.06f, 0, 0.35f, METAL, false, false, 0.5f);
                cyl(M, mx, mz, 0.25f, 0.2f, 0, 0.03f, METAL, true, false, 1.0f);
                break;
            }
            case FurnKind::Cabinet: { // a grey filing cabinet: drawers with handles
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, METAL, METAL, METAL, fd);
                for (int k = 0; k < 4; k++) {
                    float y = 0.18f + k * (f.y1 - 0.1f) / 4.0f;
                    Vec3 c{mx + fd.x * (std::fabs(fd.x) * hx + 0.02f), y, mz + fd.z * (std::fabs(fd.z) * hz + 0.02f)};
                    float ax = std::fabs(fd.z) * 0.08f + 0.01f, az = std::fabs(fd.x) * 0.08f + 0.01f;
                    fbox(M, c.x - ax, c.z - az, c.x + ax, c.z + az, y, y + 0.025f, RUBBER, RUBBER, RUBBER, fd);
                }
                break;
            }
            case FurnKind::Boxes: { // a few cardboard boxes stacked a bit crooked
                float y = 0;
                int nb = 1 + (int)(hsh % 3u);
                for (int k = 0; k < nb && y < f.y1; k++) {
                    float sh = 0.85f - k * 0.15f, ox = ((hsh >> (k * 3)) % 5u - 2.0f) * 0.03f, oz = ((hsh >> (k * 3 + 1)) % 5u - 2.0f) * 0.03f;
                    float bh = std::min(f.y1 - y, 0.3f + ((hsh >> (k + 4)) % 3u) * 0.08f);
                    fbox(M, mx - hx * sh + ox, mz - hz * sh + oz, mx + hx * sh + ox, mz + hz * sh + oz, y, y + bh, CARD, CARD, CARD, fd);
                    y += bh;
                }
                break;
            }
            case FurnKind::Barrel: case FurnKind::FireBarrel: {
                UV skin = f.kind == FurnKind::FireBarrel ? RUST : ((hsh & 1) ? METAL : RUST);
                cyl(M, mx, mz, rad, rad, 0, f.y1, skin, f.kind != FurnKind::FireBarrel, false, 1.25f);
                cyl(M, mx, mz, rad + 0.015f, rad + 0.015f, f.y1 * 0.3f, f.y1 * 0.3f + 0.04f, METAL, false, false, 1.25f);
                cyl(M, mx, mz, rad + 0.015f, rad + 0.015f, f.y1 * 0.68f, f.y1 * 0.68f + 0.04f, METAL, false, false, 1.25f);
                if (f.kind == FurnKind::FireBarrel) { // flames licking out of the top
                    glow = true; glow_g = 0.6f; glow_b = 0.25f;
                    cyl(M, mx, mz, rad * 0.85f, 0.02f, f.y1 - 0.05f, f.y1 + 0.35f, GLOWC, false, false, 0.75f);
                    glow = false; glow_g = glow_b = 1;
                }
                break;
            }
            case FurnKind::Cone:
                fbox(M, mx - 0.19f, mz - 0.19f, mx + 0.19f, mz + 0.19f, 0, 0.04f, RUBBER, RUBBER, RUBBER, fd);
                cyl(M, mx, mz, 0.15f, 0.025f, 0.04f, f.y1, PLASTIC, true, false, 1.0f);
                break;
            case FurnKind::Bench: {
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                for (int k = 0; k < 3; k++) { // slats
                    float o = -0.16f + k * 0.16f;
                    float sx0 = along_x ? f.x0 : mx + o - 0.06f, sx1 = along_x ? f.x1 : mx + o + 0.06f;
                    float sz0 = along_x ? mz + o - 0.06f : f.z0, sz1 = along_x ? mz + o + 0.06f : f.z1;
                    fbox(M, sx0, sz0, sx1, sz1, f.y1 - 0.04f, f.y1, WOOD, WOOD, WOOD, fd);
                }
                for (int e = -1; e <= 1; e += 2) {
                    float lx = along_x ? mx + e * (hx - 0.12f) : mx, lz = along_x ? mz : mz + e * (hz - 0.12f);
                    fbox(M, lx - (along_x ? 0.03f : hx), lz - (along_x ? hz : 0.03f), lx + (along_x ? 0.03f : hx), lz + (along_x ? hz : 0.03f), 0, f.y1 - 0.04f, METAL, METAL, METAL, fd);
                }
                break;
            }
            case FurnKind::Plant: // a pot, soil, a ball of leaves (sometimes long dead)
                cyl(M, mx, mz, rad * 0.55f, rad * 0.75f, 0, 0.42f, STONE, true, false, 1.0f);
                sphere(M, {mx, 0.85f, mz}, {rad * 0.95f, 0.45f, rad * 0.95f}, (hsh % 4u == 0) ? BARK : LEAVES, 1.0f);
                sphere(M, {mx + rad * 0.3f, 1.15f, mz - rad * 0.2f}, {rad * 0.6f, 0.3f, rad * 0.6f}, (hsh % 4u == 0) ? BARK : LEAVES, 0.75f);
                break;
            case FurnKind::Shelf: case FurnKind::Rack: {
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2)
                        fbox(M, mx + a * (hx - 0.03f) - 0.02f, mz + b * (hz - 0.03f) - 0.02f, mx + a * (hx - 0.03f) + 0.02f, mz + b * (hz - 0.03f) + 0.02f, 0, f.y1, METAL, METAL, METAL, fd);
                for (int k = 0; k < 4; k++) {
                    float y = 0.1f + k * (f.y1 - 0.15f) / 3.0f;
                    fbox(M, f.x0, f.z0, f.x1, f.z1, y, y + 0.03f, METAL, METAL, METAL, fd, true);
                    if (k < 3 && ((hsh >> k) & 1)) { // something left on it
                        float bw = 0.18f + ((hsh >> (k + 3)) % 3u) * 0.06f;
                        float ox = (((hsh >> (k + 5)) % 5u) - 2.0f) * 0.12f;
                        bool ax = (f.x1 - f.x0) > (f.z1 - f.z0);
                        float bx = ax ? mx + ox : mx, bz = ax ? mz : mz + ox;
                        fbox(M, bx - bw, bz - std::min(bw, hz * 0.8f), bx + bw, bz + std::min(bw, hz * 0.8f), y + 0.03f, y + 0.03f + 0.25f, CARD, CARD, CARD, fd);
                    }
                }
                break;
            }
            case FurnKind::Bin:
                cyl(M, mx, mz, rad * 0.8f, rad, 0, f.y1, RUBBER, false, false, 1.0f);
                cyl(M, mx, mz, rad + 0.02f, rad + 0.02f, f.y1, f.y1 + 0.04f, RUBBER, true, false, 1.0f);
                break;
            case FurnKind::Locker: {
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                for (int k = 0; k < 2; k++) {
                    float a0 = (along_x ? f.x0 : f.z0) + k * (along_x ? hx : hz), a1 = a0 + (along_x ? hx : hz) - 0.01f;
                    if (along_x) fbox(M, a0, f.z0, a1, f.z1, 0, f.y1, METAL, LOCKER, LOCKER, fd);
                    else fbox(M, f.x0, a0, f.x1, a1, 0, f.y1, METAL, LOCKER, LOCKER, fd);
                }
                break;
            }
            case FurnKind::Pallet:
                for (int k = 0; k < 5; k++) {
                    float o = -hz + 0.1f + k * (2 * hz - 0.2f) / 4.0f;
                    fbox(M, f.x0, mz + o - 0.05f, f.x1, mz + o + 0.05f, 0.1f, f.y1, WOOD, WOOD, WOOD, fd);
                }
                for (int k = -1; k <= 1; k++) fbox(M, mx + k * (hx - 0.06f) - 0.05f, f.z0, mx + k * (hx - 0.06f) + 0.05f, f.z1, 0, 0.1f, WOOD, WOOD, WOOD, fd);
                break;
            case FurnKind::Rubble:
                for (int k = 0; k < 5; k++) {
                    float ox = (((hsh >> (k * 2)) % 7u) - 3.0f) / 3.0f * hx * 0.7f, oz = (((hsh >> (k * 2 + 1)) % 7u) - 3.0f) / 3.0f * hz * 0.7f;
                    float r = 0.15f + ((hsh >> (k + 7)) % 4u) * 0.07f;
                    sphere(M, {mx + ox, r * 0.6f, mz + oz}, {r, r * 0.8f, r * 1.1f}, STONE, 0.6f);
                }
                break;
            case FurnKind::Tree: { // trunk, a few branches of leaves
                cyl(M, mx, mz, 0.24f, 0.14f, 0, 3.0f, BARK, false, false, 1.0f);
                sphere(M, {mx, 3.6f, mz}, {1.5f, 1.2f, 1.5f}, LEAVES, 1.25f);
                sphere(M, {mx + 0.7f, 3.0f, mz + 0.3f}, {0.9f, 0.8f, 0.9f}, LEAVES, 1.0f);
                sphere(M, {mx - 0.5f, 3.2f, mz - 0.6f}, {0.9f, 0.8f, 0.9f}, LEAVES, 1.0f);
                break;
            }
            case FurnKind::Mattress: {
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, SHEET, SHEET, SHEET, fd);
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                float px = along_x ? f.x0 + 0.25f : mx, pz = along_x ? mz : f.z0 + 0.25f;
                sphere(M, {px, f.y1 + 0.05f, pz}, {along_x ? 0.15f : 0.3f, 0.07f, along_x ? 0.3f : 0.15f}, FABRIC, 0.75f);
                break;
            }
            case FurnKind::Lantern:
                cyl(M, mx, mz, 0.1f, 0.1f, 0, 0.04f, METAL, true, false, 0.75f);
                glow = true; glow_g = 0.85f; glow_b = 0.5f;
                cyl(M, mx, mz, 0.075f, 0.075f, 0.04f, 0.24f, GLOWC, false, false, 0.75f);
                glow = false; glow_g = glow_b = 1;
                cyl(M, mx, mz, 0.1f, 0.02f, 0.24f, 0.3f, METAL, true, false, 0.75f);
                break;
            case FurnKind::Counter: {
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1 - 0.04f, WOOD, WOOD, DRAWER, fd);
                fbox(M, f.x0 - 0.03f, f.z0 - 0.03f, f.x1 + 0.03f, f.z1 + 0.03f, f.y1 - 0.04f, f.y1, STONE, STONE, STONE, fd);
                fbox(M, mx - 0.18f, mz - 0.15f, mx + 0.18f, mz + 0.15f, f.y1, f.y1 + 0.18f, RUBBER, RUBBER, RUBBER, fd); // till
                break;
            }
            case FurnKind::Car: { // body, a rounded cabin, four wheels
                static const uint32_t PAINT_C[] = {0x8a2b25, 0x2c4a7a, 0xb8b8b0, 0x3a3a3a, 0x5a6b3a, 0x9a8a5a, 0x6b6f78};
                set_tint(PAINT_C[hsh % 7u]);
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.28f, 0.82f, METAL, METAL, METAL, fd);
                float cx0 = along_x ? mx - hx * 0.55f : f.x0 + 0.08f, cx1 = along_x ? mx + hx * 0.4f : f.x1 - 0.08f;
                float cz0 = along_x ? f.z0 + 0.08f : mz - hz * 0.55f, cz1 = along_x ? f.z1 - 0.08f : mz + hz * 0.4f;
                fbox(M, cx0, cz0, cx1, cz1, 0.82f, 1.32f, METAL, METAL, METAL, fd);
                clear_tint();
                // dark windows on the cabin
                fbox(M, cx0 - 0.01f, cz0 - 0.01f, cx1 + 0.01f, cz1 + 0.01f, 0.9f, 1.25f, RUBBER, RUBBER, RUBBER, fd);
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2) {
                        float wx = along_x ? mx + a * hx * 0.65f : mx + a * (hx - 0.02f), wz = along_x ? mz + b * (hz - 0.02f) : mz + b * hz * 0.65f;
                        Vec3 axis = along_x ? Vec3{0, 0, 0.1f} : Vec3{0.1f, 0, 0};
                        tube(M, Vec3{wx, 0.3f, wz} - axis * (float)b * (along_x ? 1.0f : 0.0f) - axis * (float)a * (along_x ? 0.0f : 1.0f),
                             Vec3{wx, 0.3f, wz} + axis * (float)b * (along_x ? 1.0f : 0.0f) + axis * (float)a * (along_x ? 0.0f : 1.0f), 0.3f, RUBBER, 1.0f);
                    }
                break;
            }
            case FurnKind::Mailbox:
                fbox(M, mx - 0.04f, mz - 0.04f, mx + 0.04f, mz + 0.04f, 0, 0.9f, WOOD, WOOD, WOOD, fd);
                tube(M, {mx - 0.22f, 1.0f, mz}, {mx + 0.22f, 1.0f, mz}, 0.13f, METAL, 1.0f);
                break;
            // ----- 1.1
            case FurnKind::Clock: {
                if (f.y0 > 0.5f) { // on a wall: a round analog clock, its face glowing faintly so you can read it
                    Vec2 sd{std::fabs(fd.z), std::fabs(fd.x)};
                    float r = std::max(hx * sd.x + hz * sd.z, 0.12f);
                    float yc = (f.y0 + f.y1) * 0.5f;
                    Vec3 axis{fd.x * 0.03f, 0, fd.z * 0.03f};
                    tube(M, Vec3{mx, yc, mz} - axis, Vec3{mx, yc, mz} + axis, r, WOOD, 1.0f);
                    glow = true; glow_g = 0.95f; glow_b = 0.85f;
                    tube(M, Vec3{mx, yc, mz} + axis, Vec3{mx, yc, mz} + axis * 1.3f, r * 0.85f, SHEET, 1.0f);
                    glow = false; glow_g = glow_b = 1;
                    for (int k = 0; k < 2; k++) { // the hands
                        float a = k ? 1.2f : -0.6f, len = k ? r * 0.75f : r * 0.5f;
                        Vec3 c{mx + fd.x * 0.045f, yc, mz + fd.z * 0.045f};
                        Vec3 tip = c + Vec3{sd.x * std::sin(a) * len, std::cos(a) * len, sd.z * std::sin(a) * len};
                        tube(M, c, tip, 0.008f, RUBBER, 0.5f);
                    }
                } else if (f.solid) { // a grandfather clock: case, glass, pendulum, the face up top
                    fbox(M, f.x0 + 0.08f, f.z0 + 0.08f, f.x1 - 0.08f, f.z1 - 0.08f, 0, f.y1 - 0.45f, WOOD, WOOD, WOOD, fd);
                    fbox(M, f.x0, f.z0, f.x1, f.z1, f.y1 - 0.45f, f.y1, WOOD, WOOD, WOOD, fd);
                    glow = true; glow_g = 0.95f; glow_b = 0.85f;
                    sphere(M, {mx + fd.x * (std::fabs(fd.x) * hx + 0.01f), f.y1 - 0.22f, mz + fd.z * (std::fabs(fd.z) * hz + 0.01f)}, {0.17f * std::fabs(fd.z) + 0.02f, 0.17f, 0.17f * std::fabs(fd.x) + 0.02f}, SHEET, 0.75f);
                    glow = false; glow_g = glow_b = 1;
                    sphere(M, {mx + fd.x * (std::fabs(fd.x) * hx - 0.06f), 0.6f, mz + fd.z * (std::fabs(fd.z) * hz - 0.06f)}, {0.07f, 0.07f, 0.07f}, METAL, 0.6f);
                } else { // floating in the sea: a clock face lying on the water, tilted
                    cyl(M, mx, mz, rad, rad, 0.0f, 0.12f, WOOD, true, true, 1.25f);
                    glow = true; glow_g = 0.95f; glow_b = 0.85f;
                    cyl(M, mx, mz, rad * 0.86f, rad * 0.86f, 0.12f, 0.13f, SHEET, false, true, 1.25f);
                    glow = false; glow_g = glow_b = 1;
                    tube(M, {mx, 0.15f, mz}, {mx + rad * 0.6f, 0.15f, mz - rad * 0.2f}, 0.015f, RUBBER, 0.5f);
                    tube(M, {mx, 0.15f, mz}, {mx - rad * 0.1f, 0.15f, mz + rad * 0.45f}, 0.015f, RUBBER, 0.5f);
                }
                break;
            }
            case FurnKind::Fountain: { // a stone basin, a pillar, a bowl spilling almond water
                cyl(M, mx, mz, rad, rad, 0, 0.55f, STONE, true, false, 1.5f);
                cyl(M, mx, mz, rad * 0.88f, rad * 0.88f, 0.42f, 0.5f, PLASTIC, false, true, 1.5f); // the water
                cyl(M, mx, mz, 0.18f, 0.14f, 0.5f, 1.45f, STONE, false, false, 1.0f);
                cyl(M, mx, mz, 0.55f, 0.25f, 1.35f, 1.6f, STONE, true, true, 1.25f);
                sphere(M, {mx, 1.8f, mz}, {0.12f, 0.2f, 0.12f}, PLASTIC, 0.75f);
                break;
            }
            case FurnKind::Boat: { // a speedboat hauled up on the sand
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                fbox(M, f.x0 + 0.1f, f.z0 + 0.1f, f.x1 - 0.1f, f.z1 - 0.1f, 0.0f, 0.25f, PLASTIC, PLASTIC, PLASTIC, fd);
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.25f, 0.75f, PLASTIC, METAL, PLASTIC, fd);
                Vec3 bow = along_x ? Vec3{f.x1 + 0.4f, 0.5f, mz} : Vec3{mx, 0.5f, f.z1 + 0.4f};
                Vec3 bb = along_x ? Vec3{f.x1, 0.5f, mz} : Vec3{mx, 0.5f, f.z1};
                tube(M, bb, bow, along_x ? hz : hx, PLASTIC, 1.0f);
                fbox(M, mx - 0.25f, mz - 0.25f, mx + 0.25f, mz + 0.25f, 0.75f, 1.0f, RUBBER, RUBBER, RUBBER, fd); // windshield
                fbox(M, along_x ? f.x0 - 0.25f : mx - 0.12f, along_x ? mz - 0.12f : f.z0 - 0.25f, along_x ? f.x0 : mx + 0.12f, along_x ? mz + 0.12f : f.z0, 0.2f, 0.9f, METAL, METAL, METAL, fd); // outboard
                break;
            }
            case FurnKind::Siren: // a tall pole with a horn on top (it plays cartoon music by day)
                cyl(M, mx, mz, 0.06f, 0.05f, 0, f.y1 - 0.4f, METAL, false, false, 0.75f);
                for (int k = 0; k < 2; k++) {
                    Vec3 a{mx, f.y1 - 0.3f, mz}, b{mx + (k ? 0.45f : -0.45f), f.y1 - 0.15f, mz};
                    tube(M, a, b, 0.05f, METAL, 0.75f);
                    cyl(M, b.x, b.z, 0.17f, 0.17f, b.y - 0.17f, b.y + 0.17f, METAL, true, true, 0.75f);
                }
                break;
            case FurnKind::Stove: // a cast-iron range with four rings
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.05f, f.y1, METAL, RUBBER, DRAWER, fd);
                legs(0.04f, 0.05f, 0.025f);
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2)
                        cyl(M, mx + a * hx * 0.45f, mz + b * hz * 0.45f, 0.09f, 0.09f, f.y1, f.y1 + 0.02f, METAL, false, true, 0.75f);
                break;
            case FurnKind::Sink:
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1 - 0.04f, WOOD, WOOD, DRAWER, fd);
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y1 - 0.04f, f.y1, METAL, METAL, METAL, fd);
                cyl(M, mx - fd.x * 0.15f, mz - fd.z * 0.15f, 0.025f, 0.025f, f.y1, f.y1 + 0.28f, METAL, false, true, 0.5f);
                break;
            case FurnKind::Radio: // a 1930s cathedral radio on a small table
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, 0.55f, WOOD, WOOD, WOOD, fd);
                legs(0.04f, 0.5f, 0.02f);
                {
                    float rx = std::fabs(fd.z) * 0.24f + std::fabs(fd.x) * 0.13f, rz = std::fabs(fd.x) * 0.24f + std::fabs(fd.z) * 0.13f;
                    fbox(M, mx - rx, mz - rz, mx + rx, mz + rz, 0.55f, 0.88f, WOOD, WOOD, FABRIC, fd);
                    sphere(M, {mx, 0.9f, mz}, {rx, 0.14f, rz}, WOOD, 0.75f);
                }
                break;
            case FurnKind::Bookcase: {
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, WOOD, WOOD, WOOD, fd);
                for (int k = 0; k < 4; k++) { // rows of book spines, a little uneven
                    float y = 0.12f + k * (f.y1 - 0.2f) / 4.0f;
                    float inx = std::fabs(fd.x) * 0.06f, inz = std::fabs(fd.z) * 0.06f;
                    set_tint(((hsh >> k) & 1) ? 0xa04030 : 0x305070);
                    fbox(M, f.x0 + 0.04f + inx * (fd.x < 0), f.z0 + 0.04f + inz * (fd.z < 0), f.x1 - 0.04f - inx * (fd.x > 0), f.z1 - 0.04f - inz * (fd.z > 0),
                         y, y + 0.28f - ((hsh >> (k + 2)) % 3u) * 0.03f, FABRIC, FABRIC, FABRIC, fd);
                    clear_tint();
                }
                break;
            }
            case FurnKind::Snowman:
                sphere(M, {mx, 0.38f, mz}, {0.38f, 0.36f, 0.38f}, SHEET, 1.0f);
                sphere(M, {mx, 0.95f, mz}, {0.27f, 0.25f, 0.27f}, SHEET, 1.0f);
                sphere(M, {mx, 1.38f, mz}, {0.19f, 0.19f, 0.19f}, SHEET, 0.9f);
                cyl(M, mx, mz, 0.16f, 0.14f, 1.5f, 1.72f, RUBBER, true, true, 0.75f); // a hat
                tube(M, {mx + fd.x * 0.17f, 1.4f, mz + fd.z * 0.17f}, {mx + fd.x * 0.33f, 1.38f, mz + fd.z * 0.33f}, 0.025f, CARD, 0.5f);
                break;
            case FurnKind::Bush:
                sphere(M, {mx, 0.45f, mz}, {hx * 0.95f, 0.5f, hz * 0.95f}, LEAVES, 1.0f);
                sphere(M, {mx + hx * 0.35f, 0.6f, mz - hz * 0.2f}, {hx * 0.6f, 0.4f, hz * 0.6f}, LEAVES, 0.75f);
                break;
            case FurnKind::Table2: // a dining table: thick top, turned legs
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y1 - 0.06f, f.y1, WOOD, WOOD, WOOD, fd, true);
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2) cyl(M, mx + a * (hx - 0.08f), mz + b * (hz - 0.08f), 0.04f, 0.03f, 0, f.y1 - 0.06f, WOOD, false, false, 0.6f);
                break;
            case FurnKind::Igloo: break; // (igloos are solid cells, drawn as domes)
            // ----- 1.2
            case FurnKind::Lounger: { // a pool lounger: a tube frame, slats, the back raised away from the water
                Vec2 back{-fd.x, -fd.z};
                bool along_x = std::fabs(fd.x) > 0.5f;
                float L = along_x ? hx : hz;
                set_tint(0xf0f0ec);
                for (int k = 0; k < 6; k++) { // the flat part, slats across
                    float t0 = -L + 0.05f + k * (L * 1.25f / 6.0f), t1 = t0 + L * 1.25f / 6.0f - 0.03f;
                    float sx0 = along_x ? mx + fd.x * t0 : f.x0 + 0.03f, sx1 = along_x ? mx + fd.x * t1 : f.x1 - 0.03f;
                    float sz0 = along_x ? f.z0 + 0.03f : mz + fd.z * t0, sz1 = along_x ? f.z1 - 0.03f : mz + fd.z * t1;
                    fbox(M, std::min(sx0, sx1), std::min(sz0, sz1), std::max(sx0, sx1), std::max(sz0, sz1), 0.30f, 0.34f, SHEET, SHEET, SHEET, fd);
                }
                // the raised back: a tilted panel at the far end
                Vec2 c0{mx + back.x * L * 0.35f, mz + back.z * L * 0.35f}, c1{mx + back.x * (L - 0.02f), mz + back.z * (L - 0.02f)};
                Vec2 side{std::fabs(fd.z), std::fabs(fd.x)};
                float wdt = along_x ? hz - 0.03f : hx - 0.03f;
                Vec3 a{c0.x - side.x * wdt, 0.34f, c0.z - side.z * wdt}, b2{c0.x + side.x * wdt, 0.34f, c0.z + side.z * wdt};
                Vec3 c{c1.x + side.x * wdt, 0.95f, c1.z + side.z * wdt}, d{c1.x - side.x * wdt, 0.95f, c1.z - side.z * wdt};
                Vec3 nn{fd.x * 0.6f, 0.8f, fd.z * 0.6f};
                emit_quad(M, lit(a, nn, PLASTIC.u0, PLASTIC.v1), lit(b2, nn, PLASTIC.u1, PLASTIC.v1), lit(c, nn, PLASTIC.u1, PLASTIC.v0), lit(d, nn, PLASTIC.u0, PLASTIC.v0));
                emit_quad(M, lit(b2, nn * -1.0f, PLASTIC.u1, PLASTIC.v1), lit(a, nn * -1.0f, PLASTIC.u0, PLASTIC.v1), lit(d, nn * -1.0f, PLASTIC.u0, PLASTIC.v0), lit(c, nn * -1.0f, PLASTIC.u1, PLASTIC.v0));
                clear_tint();
                legs(0.06f, 0.30f, 0.02f);
                break;
            }
            case FurnKind::Float: { // an inflatable ring, red and white, bobbing on the water
                float r = rad * 0.75f;
                for (int k = 0; k < 10; k++) {
                    float a = k * 2 * PI / 10;
                    set_tint((k & 1) ? 0xf2f2f2 : 0xd83a2e);
                    sphere(M, {mx + std::cos(a) * r, (f.y0 + f.y1) * 0.5f, mz + std::sin(a) * r}, {0.2f, 0.09f, 0.2f}, SHEET, 0.6f);
                }
                clear_tint();
                break;
            }
            case FurnKind::Ladder: { // pool steps: two rails bending over the edge, three rungs
                Vec2 side{std::fabs(fd.z), std::fabs(fd.x)};
                for (int s2 = -1; s2 <= 1; s2 += 2) {
                    Vec3 b0{mx + side.x * 0.25f * s2, f.y0, mz + side.z * 0.25f * s2};
                    Vec3 t0{b0.x, 0.85f, b0.z}, t1{b0.x - fd.x * 0.35f, 0.95f, b0.z - fd.z * 0.35f}, t2{b0.x - fd.x * 0.45f, 0.0f, b0.z - fd.z * 0.45f};
                    tube(M, b0, t0, 0.025f, METAL, 0.5f);
                    tube(M, t0, t1, 0.025f, METAL, 0.5f);
                    tube(M, t1, t2, 0.025f, METAL, 0.5f);
                }
                for (int k = 0; k < 3; k++) {
                    float y = f.y0 + 0.3f + k * 0.35f;
                    if (y > -0.05f) break;
                    tube(M, {mx - side.x * 0.25f, y, mz - side.z * 0.25f}, {mx + side.x * 0.25f, y, mz + side.z * 0.25f}, 0.02f, METAL, 0.5f);
                }
                break;
            }
            case FurnKind::Slide: { // a water slide: a ladder tower, then a curving tube down into the deep end
                Vec2 side{std::fabs(fd.z), std::fabs(fd.x)};
                float top = 2.5f;
                Vec2 base{mx - fd.x * 0.5f, mz - fd.z * 0.5f};
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2)
                        cyl(M, base.x + side.x * a * 0.45f + fd.x * b * 0.45f, base.z + side.z * a * 0.45f + fd.z * b * 0.45f, 0.04f, 0.04f, 0, top, METAL, false, false, 0.5f);
                fbox(M, base.x - 0.55f, base.z - 0.55f, base.x + 0.55f, base.z + 0.55f, top - 0.06f, top, METAL, METAL, METAL, fd, true);
                for (int k = 0; k < 8; k++) { // rungs at the back
                    float y = 0.3f + k * 0.3f;
                    Vec2 r0 = base - Vec2{fd.x, fd.z} * 0.48f;
                    tube(M, {r0.x - side.x * 0.3f, y, r0.z - side.z * 0.3f}, {r0.x + side.x * 0.3f, y, r0.z + side.z * 0.3f}, 0.02f, METAL, 0.5f);
                }
                set_tint(0x2f8fe0);
                Vec3 prev{base.x + fd.x * 0.4f, top + 0.15f, base.z + fd.z * 0.4f};
                const int SEG = 9;
                for (int k = 1; k <= SEG; k++) { // out over the water and down, swinging to one side
                    float t = (float)k / SEG;
                    float along = 0.4f + t * 3.6f, swing = std::sin(t * PI) * 0.9f, y = (top + 0.15f) * (1.0f - t) * (1.0f - t * 0.25f) - 0.05f;
                    Vec3 p{base.x + fd.x * along + side.x * swing, y, base.z + fd.z * along + side.z * swing};
                    tube(M, prev, p, 0.36f, SHEET, 0.9f);
                    prev = p;
                }
                clear_tint();
                break;
            }
            case FurnKind::LifeguardChair: { // tall white chair, a ladder up the front
                set_tint(0xf4f4f0);
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2)
                        tube(M, {mx + a * hx, 0, mz + b * hz}, {mx + a * hx * 0.5f, 1.55f, mz + b * hz * 0.5f}, 0.035f, WOOD, 0.5f);
                fbox(M, mx - hx * 0.55f, mz - hz * 0.55f, mx + hx * 0.55f, mz + hz * 0.55f, 1.55f, 1.62f, WOOD, WOOD, WOOD, fd, true);
                float bx0 = mx - hx * 0.55f, bx1 = mx + hx * 0.55f, bz0 = mz - hz * 0.55f, bz1 = mz + hz * 0.55f;
                if (fd.x > 0.5f) bx1 = bx0 + 0.05f; else if (fd.x < -0.5f) bx0 = bx1 - 0.05f; else if (fd.z > 0.5f) bz1 = bz0 + 0.05f; else bz0 = bz1 - 0.05f;
                fbox(M, bx0, bz0, bx1, bz1, 1.62f, f.y1, WOOD, WOOD, WOOD, fd);
                clear_tint();
                for (int k = 0; k < 4; k++) {
                    float y = 0.35f + k * 0.35f;
                    Vec2 side{std::fabs(fd.z), std::fabs(fd.x)};
                    Vec2 c{mx + fd.x * hx * (1.0f - y / 1.55f * 0.5f), mz + fd.z * hz * (1.0f - y / 1.55f * 0.5f)};
                    tube(M, {c.x - side.x * hx * 0.6f, y, c.z - side.z * hz * 0.6f}, {c.x + side.x * hx * 0.6f, y, c.z + side.z * hz * 0.6f}, 0.02f, WOOD, 0.5f);
                }
                break;
            }
            case FurnKind::Palm: { // a potted palm: a leaning trunk, fronds drooping all round
                cyl(M, mx, mz, rad * 0.9f, rad, 0, 0.45f, STONE, true, false, 1.0f);
                Vec3 top{mx + 0.25f, f.y1 - 0.2f, mz + 0.1f};
                tube(M, {mx, 0.4f, mz}, top, 0.07f, BARK, 0.6f);
                for (int k = 0; k < 7; k++) {
                    float a = k * 2 * PI / 7 + 0.3f;
                    Vec3 tip{top.x + std::cos(a) * 1.0f, top.y - 0.45f, top.z + std::sin(a) * 1.0f};
                    Vec3 mid{(top.x + tip.x) * 0.5f, top.y + 0.12f, (top.z + tip.z) * 0.5f};
                    sphere(M, mid, {0.28f + std::fabs(std::cos(a)) * 0.22f, 0.05f, 0.28f + std::fabs(std::sin(a)) * 0.22f}, LEAVES, 0.6f);
                    sphere(M, {(mid.x + tip.x) * 0.5f, (mid.y + tip.y) * 0.5f, (mid.z + tip.z) * 0.5f}, {0.22f, 0.04f, 0.22f}, LEAVES, 0.5f);
                }
                break;
            }
            case FurnKind::Tent: { // a M.E.G. field tent: four poles, an olive canvas roof, the flaps tied back
                set_tint(0x6a7048);
                float ry = f.y1, ey = 1.55f;
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2) cyl(M, mx + a * (hx - 0.05f), mz + b * (hz - 0.05f), 0.03f, 0.03f, 0, ey, METAL, false, false, 0.5f);
                cyl(M, mx, mz, 0.035f, 0.035f, 0, ry, METAL, false, false, 0.5f);
                Vec3 apex{mx, ry, mz};
                Vec3 cs[4] = {{mx - hx, ey, mz - hz}, {mx + hx, ey, mz - hz}, {mx + hx, ey, mz + hz}, {mx - hx, ey, mz + hz}};
                for (int k = 0; k < 4; k++) {
                    Vec3 a = cs[k], b2 = cs[(k + 1) % 4];
                    Vec3 e1 = b2 - a, e2 = apex - a;
                    Vec3 nn{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                    nn = nn * (1.0f / nn.len());
                    if (nn.y < 0) nn = nn * -1.0f;
                    emit_quad(M, lit(a, nn, FABRIC.u0, FABRIC.v1), lit(b2, nn, FABRIC.u1, FABRIC.v1), lit(apex, nn, FABRIC.u1, FABRIC.v0), lit(apex, nn, FABRIC.u0, FABRIC.v0));
                    emit_quad(M, lit(b2, nn * -1.0f, FABRIC.u1, FABRIC.v1), lit(a, nn * -1.0f, FABRIC.u0, FABRIC.v1), lit(apex, nn * -1.0f, FABRIC.u0, FABRIC.v0), lit(apex, nn * -1.0f, FABRIC.u1, FABRIC.v0));
                }
                // the back wall: canvas down to the ground on the far side
                fbox(M, mx - hx, mz - hz, mx + hx, mz - hz + 0.02f, 0, ey, FABRIC, FABRIC, FABRIC, fd);
                clear_tint();
                break;
            }
            case FurnKind::Cot:
                legs(0.05f, 0.38f, 0.02f);
                set_tint(0x7a7a5a);
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.36f, f.y1, FABRIC, FABRIC, FABRIC, fd);
                clear_tint();
                break;
            case FurnKind::Sandbags: { // three courses of sacks, slumped
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                float L = along_x ? (f.x1 - f.x0) : (f.z1 - f.z0);
                int nb = std::max(2, (int)(L / 0.55f));
                set_tint(0xb8a678);
                for (int row = 0; row < 3; row++)
                    for (int k = 0; k < nb - (row & 1); k++) {
                        float t = (k + 0.5f + (row & 1) * 0.5f) / nb;
                        float px = along_x ? f.x0 + t * L : mx, pz = along_x ? mz : f.z0 + t * L;
                        sphere(M, {px, 0.13f + row * 0.24f, pz}, {along_x ? 0.3f : hx, 0.13f, along_x ? hz : 0.3f}, CARD, 0.5f);
                    }
                clear_tint();
                break;
            }
            case FurnKind::Crate: // a supply crate: planks, a darker frame, steel straps
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, WOOD, WOOD, WOOD, fd);
                set_tint(0x6a5a40);
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2)
                        fbox(M, mx + a * hx - (a > 0 ? 0.06f : 0.0f) - 0.005f, mz + b * hz - (b > 0 ? 0.06f : 0.0f) - 0.005f, mx + a * hx + (a < 0 ? 0.06f : 0.0f) + 0.005f, mz + b * hz + (b < 0 ? 0.06f : 0.0f) + 0.005f, 0, f.y1 + 0.005f, WOOD, WOOD, WOOD, fd);
                clear_tint();
                fbox(M, f.x0 - 0.01f, f.z0 - 0.01f, f.x1 + 0.01f, f.z1 + 0.01f, f.y1 * 0.45f, f.y1 * 0.45f + 0.05f, METAL, METAL, METAL, fd);
                break;
            case FurnKind::Banner: { // the M.E.G. flag: a pole, a dark blue banner with a white band
                cyl(M, mx, mz, 0.03f, 0.03f, 0, f.y1, METAL, true, false, 0.5f);
                Vec2 side{std::fabs(fd.z), std::fabs(fd.x)};
                float fx = mx + side.x * 0.45f, fz = mz + side.z * 0.45f;
                set_tint(0x1c2c58);
                fbox(M, fx - side.x * 0.42f - fd.x * 0.01f, fz - side.z * 0.42f - fd.z * 0.01f, fx + side.x * 0.42f + fd.x * 0.01f + std::fabs(fd.x) * 0.02f, fz + side.z * 0.42f + fd.z * 0.01f + std::fabs(fd.z) * 0.02f, f.y1 - 1.2f, f.y1 - 0.05f, FABRIC, FABRIC, FABRIC, fd);
                set_tint(0xe8e8e0);
                fbox(M, fx - side.x * 0.43f - std::fabs(fd.x) * 0.015f, fz - side.z * 0.43f - std::fabs(fd.z) * 0.015f, fx + side.x * 0.43f + std::fabs(fd.x) * 0.035f, fz + side.z * 0.43f + std::fabs(fd.z) * 0.035f, f.y1 - 0.72f, f.y1 - 0.56f, SHEET, SHEET, SHEET, fd);
                clear_tint();
                break;
            }
            case FurnKind::Radio2: // a field radio on a box, its little light on
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1 - 0.25f, WOOD, WOOD, WOOD, fd);
                set_tint(0x4a5038);
                fbox(M, mx - 0.18f, mz - 0.12f, mx + 0.18f, mz + 0.12f, f.y1 - 0.25f, f.y1, METAL, METAL, DRAWER, fd);
                clear_tint();
                cyl(M, mx + 0.12f, mz, 0.006f, 0.004f, f.y1, f.y1 + 0.6f, METAL, false, false, 0.5f);
                glow = true; glow_g = 0.3f; glow_b = 0.2f;
                sphere(M, {mx - 0.1f + fd.x * 0.13f, f.y1 - 0.07f, mz + fd.z * 0.13f}, {0.015f, 0.015f, 0.015f}, GLOWC, 0.5f);
                glow = false; glow_g = glow_b = 1;
                break;
            case FurnKind::Floodlight: { // a work light on a tripod, pointed at the tent
                for (int k = 0; k < 3; k++) {
                    float a = k * 2 * PI / 3;
                    tube(M, {mx + std::cos(a) * rad, 0, mz + std::sin(a) * rad}, {mx, f.y1 - 0.4f, mz}, 0.018f, METAL, 0.5f);
                }
                cyl(M, mx, mz, 0.02f, 0.02f, f.y1 - 0.45f, f.y1 - 0.1f, METAL, false, false, 0.5f);
                set_tint(0x303030);
                fbox(M, mx - 0.2f, mz - 0.2f, mx + 0.2f, mz + 0.2f, f.y1 - 0.15f, f.y1 + 0.15f, METAL, METAL, METAL, fd);
                clear_tint();
                glow = true;
                Vec3 lc{mx + fd.x * 0.205f, f.y1, mz + fd.z * 0.205f};
                float qx = std::fabs(fd.z) * 0.16f + 0.004f, qz = std::fabs(fd.x) * 0.16f + 0.004f;
                fbox(M, lc.x - qx, lc.z - qz, lc.x + qx, lc.z + qz, f.y1 - 0.12f, f.y1 + 0.12f, GLOWC, GLOWC, GLOWC, fd);
                glow = false;
                break;
            }
            case FurnKind::Desk: { // a desk: a top, a drawer pedestal, a dead monitor and a keyboard
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y1 - 0.04f, f.y1, WOOD, WOOD, WOOD, fd, true);
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                float px0 = along_x ? f.x1 - 0.45f : f.x0 + 0.03f, px1 = along_x ? f.x1 - 0.03f : f.x1 - 0.03f;
                float pz0 = along_x ? f.z0 + 0.03f : f.z1 - 0.45f, pz1 = along_x ? f.z1 - 0.03f : f.z1 - 0.03f;
                fbox(M, px0, pz0, px1, pz1, 0, f.y1 - 0.04f, WOOD, WOOD, DRAWER, fd);
                for (int a = -1; a <= 1; a += 2) { // legs on the other end
                    float lx = along_x ? f.x0 + 0.05f : mx + a * (hx - 0.05f), lz = along_x ? mz + a * (hz - 0.05f) : f.z0 + 0.05f;
                    cyl(M, lx, lz, 0.025f, 0.025f, 0, f.y1 - 0.04f, METAL, false, false, 0.5f);
                }
                Vec2 back{-fd.x, -fd.z};
                Vec2 mc{mx + back.x * (std::fabs(back.x) * hx - 0.18f), mz + back.z * (std::fabs(back.z) * hz - 0.18f)};
                float sx = std::fabs(fd.z) * 0.24f + std::fabs(fd.x) * 0.03f, sz = std::fabs(fd.x) * 0.24f + std::fabs(fd.z) * 0.03f;
                fbox(M, mc.x - sx, mc.z - sz, mc.x + sx, mc.z + sz, f.y1 + 0.12f, f.y1 + 0.46f, RUBBER, RUBBER, RUBBER, fd);
                cyl(M, mc.x, mc.z, 0.02f, 0.02f, f.y1, f.y1 + 0.12f, RUBBER, false, false, 0.5f);
                float kx = std::fabs(fd.z) * 0.22f + std::fabs(fd.x) * 0.08f, kz = std::fabs(fd.x) * 0.22f + std::fabs(fd.z) * 0.08f;
                fbox(M, mx + fd.x * 0.1f - kx, mz + fd.z * 0.1f - kz, mx + fd.x * 0.1f + kx, mz + fd.z * 0.1f + kz, f.y1, f.y1 + 0.02f, PLASTIC, PLASTIC, PLASTIC, fd);
                break;
            }
            case FurnKind::Escalator: { // a dead escalator up to the gallery: steps, glass sides, black handrails
                bool along_x = std::fabs(fd.x) > 0.5f;
                float L = along_x ? f.x1 - f.x0 : f.z1 - f.z0, Wd = along_x ? f.z1 - f.z0 : f.x1 - f.x0;
                Vec2 start{along_x ? (fd.x > 0 ? f.x0 : f.x1) : mx, along_x ? mz : (fd.z > 0 ? f.z0 : f.z1)};
                Vec2 dir{fd.x, fd.z}, side{std::fabs(fd.z), std::fabs(fd.x)};
                auto step_y = [&](float t) { return clampf((t - 0.15f) / 0.7f, 0, 1) * f.y1; };
                const int NS = 14;
                for (int k = 0; k < NS; k++) {
                    float t0 = (float)k / NS, t1 = (float)(k + 1) / NS;
                    float y = std::max(0.08f, step_y(t1));
                    Vec2 a = start + dir * (t0 * L), b2 = start + dir * (t1 * L);
                    float ix = side.x * (Wd * 0.5f - 0.12f), iz = side.z * (Wd * 0.5f - 0.12f);
                    fbox(M, std::min(a.x, b2.x) - ix, std::min(a.z, b2.z) - iz, std::max(a.x, b2.x) + ix, std::max(a.z, b2.z) + iz, 0, y, METAL, METAL, METAL, fd);
                }
                for (int s2 = -1; s2 <= 1; s2 += 2) { // balustrades
                    Vec2 off = side * (s2 * (Wd * 0.5f - 0.06f));
                    const int SEG = 6;
                    for (int k = 0; k < SEG; k++) {
                        float t0 = (float)k / SEG, t1 = (float)(k + 1) / SEG;
                        Vec2 a = start + dir * (t0 * L) + off, b2 = start + dir * (t1 * L) + off;
                        float y0a = step_y(t0), y0b = step_y(t1);
                        Vec3 nn{side.x * s2, 0, side.z * s2};
                        set_tint(0xa8d8d8);
                        Vertex q0 = lit({a.x, y0a, a.z}, nn, PLASTIC.u0, PLASTIC.v1), q1 = lit({b2.x, y0b, b2.z}, nn, PLASTIC.u1, PLASTIC.v1);
                        Vertex q2 = lit({b2.x, y0b + 0.95f, b2.z}, nn, PLASTIC.u1, PLASTIC.v0), q3 = lit({a.x, y0a + 0.95f, a.z}, nn, PLASTIC.u0, PLASTIC.v0);
                        emit_quad(M, q0, q1, q2, q3);
                        emit_quad(M, q1, q0, q3, q2);
                        clear_tint();
                        tube(M, {a.x, y0a + 0.98f, a.z}, {b2.x, y0b + 0.98f, b2.z}, 0.04f, RUBBER, 0.5f);
                    }
                }
                break;
            }
            case FurnKind::Shutter: // a mall kiosk with its roll-down shutter pulled to the counter
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, METAL, METAL, LOCKER, fd);
                set_tint(0x3aa0a0);
                fbox(M, f.x0 - 0.03f, f.z0 - 0.03f, f.x1 + 0.03f, f.z1 + 0.03f, f.y1, f.y1 + 0.06f, METAL, METAL, METAL, fd);
                clear_tint();
                break;
            case FurnKind::Planter: { // a big round planter, a shrub and some dead leaves
                cyl(M, mx, mz, rad, rad, 0, f.y1, STONE, false, false, 1.5f);
                cyl(M, mx, mz, rad + 0.04f, rad + 0.04f, f.y1 - 0.05f, f.y1, STONE, true, false, 1.5f);
                sphere(M, {mx, f.y1 + 0.35f, mz}, {rad * 0.75f, 0.45f, rad * 0.75f}, (hsh % 3u == 0) ? BARK : LEAVES, 1.0f);
                sphere(M, {mx + rad * 0.3f, f.y1 + 0.6f, mz - rad * 0.2f}, {rad * 0.45f, 0.4f, rad * 0.45f}, (hsh % 3u == 0) ? BARK : LEAVES, 0.75f);
                break;
            }
            case FurnKind::KioskSign: { // a sign board, faintly lit
                set_tint((hsh & 1) ? 0x40b0b0 : 0xe0d0a0);
                glow = true; glow_g = 0.95f; glow_b = 0.85f;
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, SHEET, SHEET, SHEET, fd);
                glow = false; glow_g = glow_b = 1;
                clear_tint();
                break;
            }
            case FurnKind::Window: break; // (windows are cut into the walls themselves)
            // ----- 1.3
            case FurnKind::DoorFrame: box(MAT_PROP, mx, mz, hx, hz, 0, f.y1, 0.3f, 2.3f, 1); break; // a shop's door surround
            case FurnKind::Toilet: {
                Vec2 back{-fd.x, -fd.z};
                set_tint(0xf2f2ee);
                cyl(M, mx + fd.x * 0.05f, mz + fd.z * 0.05f, 0.17f, 0.2f, 0, 0.4f, SHEET, true, false, 1.0f);
                fbox(M, mx + back.x * 0.22f - 0.18f, mz + back.z * 0.22f - 0.18f, mx + back.x * 0.22f + 0.18f, mz + back.z * 0.22f + 0.18f, 0.3f, 0.8f, SHEET, SHEET, SHEET, fd);
                clear_tint();
                break;
            }
            case FurnKind::Bathtub:
                set_tint(0xf0f0ea);
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, SHEET, SHEET, SHEET, fd);
                clear_tint();
                set_tint(0x7a8c90);
                fbox(M, f.x0 + 0.08f, f.z0 + 0.08f, f.x1 - 0.08f, f.z1 - 0.08f, f.y1 - 0.05f, f.y1 + 0.002f, SHEET, SHEET, SHEET, fd);
                clear_tint();
                break;
            case FurnKind::Mirror:
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, METAL, METAL, METAL, f.face);
                glow = true; glow_g = 0.95f; glow_b = 1.0f;
                set_tint(0x9aa6aa);
                fbox(M, f.x0 + std::fabs(fd.z) * 0.03f, f.z0 + std::fabs(fd.x) * 0.03f, f.x1 - std::fabs(fd.z) * 0.03f, f.z1 - std::fabs(fd.x) * 0.03f, f.y0 + 0.03f, f.y1 - 0.03f, SHEET, SHEET, SHEET, f.face);
                clear_tint();
                glow = false; glow_g = glow_b = 1;
                break;
            case FurnKind::Reception: // a long reception desk: a counter, a raised front
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1 - 0.05f, WOOD, WOOD, DRAWER, fd);
                fbox(M, f.x0 - 0.04f, f.z0 - 0.04f, f.x1 + 0.04f, f.z1 + 0.04f, f.y1 - 0.05f, f.y1, STONE, STONE, STONE, fd);
                fbox(M, mx - 0.2f, mz - 0.12f, mx + 0.2f, mz + 0.12f, f.y1, f.y1 + 0.3f, RUBBER, RUBBER, RUBBER, fd); // a monitor
                break;
            case FurnKind::Mailboxes: { // a bank of little brass doors on the wall
                fbox(M, f.x0, f.z0, f.x1, f.z1, f.y0, f.y1, METAL, METAL, LOCKER, fd);
                break;
            }
            case FurnKind::Elevator: { // lift doors set into the wall, a call button, an arrow lit above
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, METAL, METAL, METAL, fd);
                Vec2 sd{std::fabs(fd.z), std::fabs(fd.x)};
                float ex = mx + fd.x * 0.01f, ez = mz + fd.z * 0.01f;
                fbox(M, ex - sd.x * 0.005f - std::fabs(fd.x) * 0.02f, ez - sd.z * 0.005f - std::fabs(fd.z) * 0.02f, ex + sd.x * 0.005f + std::fabs(fd.x) * 0.02f, ez + sd.z * 0.005f + std::fabs(fd.z) * 0.02f, 0, f.y1 - 0.1f, RUBBER, RUBBER, RUBBER, fd);
                glow = true; glow_g = 0.6f; glow_b = 0.3f;
                Vec3 lc{mx + fd.x * 0.03f, f.y1 + 0.15f, mz + fd.z * 0.03f};
                sphere(M, lc, {0.05f, 0.05f, 0.05f}, GLOWC, 0.5f);
                glow = false; glow_g = glow_b = 1;
                break;
            }
            case FurnKind::Sofa: { // a lobby sofa, low and long
                Vec2 back{-fd.x, -fd.z};
                set_tint(0x6a4a3a);
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0.08f, 0.42f, FABRIC, FABRIC, FABRIC, fd);
                float bx0 = f.x0, bx1 = f.x1, bz0 = f.z0, bz1 = f.z1;
                if (back.z < -0.5f) bz1 = f.z0 + 0.2f; else if (back.z > 0.5f) bz0 = f.z1 - 0.2f; else if (back.x < -0.5f) bx1 = f.x0 + 0.2f; else bx0 = f.x1 - 0.2f;
                fbox(M, bx0, bz0, bx1, bz1, 0.42f, f.y1, FABRIC, FABRIC, FABRIC, fd);
                clear_tint();
                legs(0.05f, 0.08f, 0.02f);
                break;
            }
            case FurnKind::Boulder: { // a lumpy grey rock, half buried
                float r = rad;
                uint32_t hb = hsh;
                sphere(M, {mx, r * 0.35f, mz}, {r, r * 0.85f, r * 0.95f}, STONE, 1.0f);
                for (int k = 0; k < 3; k++) {
                    float a = (hb >> (k * 4) & 15) / 16.0f * 2 * PI;
                    sphere(M, {mx + std::cos(a) * r * 0.5f, r * 0.45f, mz + std::sin(a) * r * 0.5f}, {r * 0.55f, r * 0.5f, r * 0.55f}, STONE, 0.75f);
                }
                break;
            }
            case FurnKind::Lander: { // a lunar lander left behind: gold-foil body, four splayed legs, an empty hatch
                set_tint(0xd8b050);
                fbox(M, mx - 0.9f, mz - 0.9f, mx + 0.9f, mz + 0.9f, 0.9f, 1.8f, FABRIC, FABRIC, FABRIC, fd);
                clear_tint();
                fbox(M, mx - 0.6f, mz - 0.6f, mx + 0.6f, mz + 0.6f, 1.8f, 2.7f, METAL, METAL, METAL, fd);
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2) {
                        tube(M, {mx + a * 0.8f, 1.1f, mz + b * 0.8f}, {mx + a * 1.15f, 0.05f, mz + b * 1.15f}, 0.035f, METAL, 0.5f);
                        cyl(M, mx + a * 1.15f, mz + b * 1.15f, 0.16f, 0.12f, 0, 0.05f, METAL, true, false, 0.75f);
                    }
                fbox(M, mx + fd.x * 0.61f - 0.2f * std::fabs(fd.z) - 0.005f, mz + fd.z * 0.61f - 0.2f * std::fabs(fd.x) - 0.005f, mx + fd.x * 0.61f + 0.2f * std::fabs(fd.z) + 0.005f, mz + fd.z * 0.61f + 0.2f * std::fabs(fd.x) + 0.005f, 1.95f, 2.45f, RUBBER, RUBBER, RUBBER, fd);
                tube(M, {mx, 2.7f, mz}, {mx + 0.3f, 3.2f, mz}, 0.02f, METAL, 0.5f);
                break;
            }
            case FurnKind::Antenna: // a tripod mast with a dish, pointed at the Earth
                for (int k = 0; k < 3; k++) { float a = k * 2 * PI / 3; tube(M, {mx + std::cos(a) * rad, 0, mz + std::sin(a) * rad}, {mx, 1.4f, mz}, 0.02f, METAL, 0.5f); }
                cyl(M, mx, mz, 0.03f, 0.03f, 1.4f, f.y1 - 0.6f, METAL, false, false, 0.5f);
                cyl(M, mx, mz, 0.05f, 0.65f, f.y1 - 0.65f, f.y1 - 0.4f, SHEET, true, true, 1.25f);
                break;
            case FurnKind::Flag: { // a pole and a flag with nothing on it
                cyl(M, mx, mz, 0.02f, 0.02f, 0, f.y1, METAL, true, false, 0.5f);
                tube(M, {mx, f.y1 - 0.05f, mz}, {mx + 0.8f, f.y1 - 0.05f, mz}, 0.01f, METAL, 0.5f);
                fbox(M, mx + 0.02f, mz - 0.005f, mx + 0.8f, mz + 0.005f, f.y1 - 0.55f, f.y1 - 0.06f, SHEET, SHEET, SHEET, {0, 1});
                break;
            }
            case FurnKind::Pine: { // a snowed-on pine: trunk, three tiers, white on top
                cyl(M, mx, mz, 0.12f, 0.09f, 0, 1.2f, BARK, false, false, 0.75f);
                for (int k = 0; k < 3; k++) {
                    float y0 = 0.9f + k * 0.95f, r = 0.95f - k * 0.25f;
                    cyl(M, mx, mz, r, 0.05f, y0, y0 + 1.35f, LEAVES, true, false, 1.0f);
                    set_tint(0xf4f6fa);
                    cyl(M, mx, mz, r * 0.62f, 0.04f, y0 + 0.55f, y0 + 1.4f, SHEET, true, false, 0.9f);
                    clear_tint();
                }
                break;
            }
            case FurnKind::Fence: { // a run of white picket fence
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                float L = along_x ? f.x1 - f.x0 : f.z1 - f.z0;
                set_tint(0xf6f2e6);
                int np = std::max(3, (int)(L / 0.2f));
                for (int k = 0; k < np; k++) {
                    float t = (k + 0.5f) / np;
                    float px = along_x ? f.x0 + t * L : mx, pz = along_x ? mz : f.z0 + t * L;
                    fbox(M, px - 0.035f, pz - 0.015f, px + 0.035f, pz + 0.015f, 0, f.y1 - 0.08f, SHEET, SHEET, SHEET, fd);
                    cyl(M, px, pz, 0.035f, 0.0f, f.y1 - 0.08f, f.y1, SHEET, false, false, 0.5f);
                }
                for (int r2 = 0; r2 < 2; r2++) {
                    float y = 0.25f + r2 * 0.4f;
                    if (along_x) fbox(M, f.x0, mz - 0.03f, f.x1, mz - 0.01f, y, y + 0.06f, SHEET, SHEET, SHEET, fd);
                    else fbox(M, mx - 0.03f, f.z0, mx - 0.01f, f.z1, y, y + 0.06f, SHEET, SHEET, SHEET, fd);
                }
                clear_tint();
                break;
            }
            case FurnKind::PowerPole: // a wooden pole, a crossbar, insulators
                cyl(M, mx, mz, 0.12f, 0.09f, 0, f.y1, WOOD, true, false, 0.75f);
                fbox(M, mx - 1.0f, mz - 0.06f, mx + 1.0f, mz + 0.06f, f.y1 - 0.6f, f.y1 - 0.48f, WOOD, WOOD, WOOD, {0, 1});
                for (int k = -1; k <= 1; k++) cyl(M, mx + k * 0.8f, mz, 0.04f, 0.03f, f.y1 - 0.48f, f.y1 - 0.32f, SHEET, true, false, 0.5f);
                break;
            case FurnKind::WaterTower: { // a water tower on four legs, a round tank, a cone roof
                for (int a = -1; a <= 1; a += 2)
                    for (int b = -1; b <= 1; b += 2) tube(M, {mx + a * hx * 0.9f, 0, mz + b * hz * 0.9f}, {mx + a * hx * 0.6f, f.y1 - 3.0f, mz + b * hz * 0.6f}, 0.07f, METAL, 0.5f);
                set_tint(0xd8d8d0);
                cyl(M, mx, mz, rad * 0.95f, rad * 0.95f, f.y1 - 3.2f, f.y1 - 0.9f, SHEET, false, true, 1.5f);
                clear_tint();
                cyl(M, mx, mz, rad * 1.0f, 0.06f, f.y1 - 0.9f, f.y1, METAL, true, false, 1.5f);
                break;
            }
            case FurnKind::Cottage: { // a little clapboard house sitting alone on a hill, its windows dark
                static const uint32_t PAINT_H[] = {0xf2e2a8, 0xe8e2d0, 0xd8c890, 0xf0d8b0};
                set_tint(PAINT_H[hsh % 4u]);
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, 2.6f, SHEET, SHEET, SHEET, fd);
                clear_tint();
                for (int s2 = -1; s2 <= 1; s2 += 2) { // two windows on each long side
                    for (int k = -1; k <= 1; k += 2) {
                        float wx = mx + k * hx * 0.45f, wz = mz + s2 * (hz + 0.01f);
                        fbox(M, wx - 0.25f, wz - 0.01f, wx + 0.25f, wz + 0.01f, 1.0f, 1.8f, RUBBER, RUBBER, RUBBER, {0, (float)s2});
                    }
                }
                // the roof: two slopes meeting at a ridge along x
                set_tint(0x5a4038);
                for (int s2 = -1; s2 <= 1; s2 += 2) {
                    Vec3 a{f.x0 - 0.15f, 2.6f, mz + s2 * (hz + 0.2f)}, b{f.x1 + 0.15f, 2.6f, mz + s2 * (hz + 0.2f)}, c{f.x1 + 0.15f, f.y1, mz}, d{f.x0 - 0.15f, f.y1, mz};
                    Vec3 nn{0, 0.75f, s2 * 0.66f};
                    if (s2 > 0) emit_quad(M, lit(b, nn, WOOD.u1, WOOD.v1), lit(a, nn, WOOD.u0, WOOD.v1), lit(d, nn, WOOD.u0, WOOD.v0), lit(c, nn, WOOD.u1, WOOD.v0));
                    else emit_quad(M, lit(a, nn, WOOD.u0, WOOD.v1), lit(b, nn, WOOD.u1, WOOD.v1), lit(c, nn, WOOD.u1, WOOD.v0), lit(d, nn, WOOD.u0, WOOD.v0));
                }
                clear_tint();
                set_tint(PAINT_H[hsh % 4u]);
                for (int s2 = -1; s2 <= 1; s2 += 2) { // the gable ends
                    float x = s2 < 0 ? f.x0 : f.x1;
                    Vec3 a{x, 2.6f, mz - hz}, b{x, 2.6f, mz + hz}, c{x, f.y1 - 0.05f, mz};
                    Vec3 nn{(float)s2, 0, 0};
                    if (s2 > 0) emit_quad(M, lit(a, nn, 0, 1), lit(c, nn, 0.5f, 0), lit(c, nn, 0.5f, 0), lit(b, nn, 1, 1));
                    else emit_quad(M, lit(b, nn, 1, 1), lit(c, nn, 0.5f, 0), lit(c, nn, 0.5f, 0), lit(a, nn, 0, 1));
                }
                clear_tint();
                break;
            }
            case FurnKind::SpiralStair: { // tiled steps winding up round the column into the ceiling
                float gx = mx, gz = mz, R0 = 0.9f, R1 = 1.7f;
                const int NS = 22;
                set_tint(0xf4f6f4);
                for (int k = 0; k < NS; k++) {
                    float a0 = k * (2.2f * PI / NS), a1 = (k + 1) * (2.2f * PI / NS);
                    float y = 0.2f + k * (f.y1 - 0.2f) / NS;
                    Vec3 p0{gx + std::cos(a0) * R0, y, gz + std::sin(a0) * R0}, p1{gx + std::cos(a1) * R0, y, gz + std::sin(a1) * R0};
                    Vec3 p2{gx + std::cos(a1) * R1, y, gz + std::sin(a1) * R1}, p3{gx + std::cos(a0) * R1, y, gz + std::sin(a0) * R1};
                    emit_quad(M, lit(p0, {0, 1, 0}, SHEET.u0, SHEET.v0), lit(p3, {0, 1, 0}, SHEET.u1, SHEET.v0), lit(p2, {0, 1, 0}, SHEET.u1, SHEET.v1), lit(p1, {0, 1, 0}, SHEET.u0, SHEET.v1));
                    Vec3 q0 = p0 - Vec3{0, 0.18f, 0}, q3 = p3 - Vec3{0, 0.18f, 0};
                    Vec3 nn{-std::sin(a0), 0, std::cos(a0)};
                    emit_quad(M, lit(q0, nn * -1.0f, SHEET.u0, SHEET.v1), lit(p0, nn * -1.0f, SHEET.u0, SHEET.v0), lit(p3, nn * -1.0f, SHEET.u1, SHEET.v0), lit(q3, nn * -1.0f, SHEET.u1, SHEET.v1));
                    emit_quad(M, lit(q3, nn, SHEET.u1, SHEET.v1), lit(p3, nn, SHEET.u1, SHEET.v0), lit(p0, nn, SHEET.u0, SHEET.v0), lit(q0, nn, SHEET.u0, SHEET.v1));
                    if (k % 2 == 0) tube(M, {p3.x, y, p3.z}, {p3.x, y + 0.95f, p3.z}, 0.02f, METAL, 0.5f);
                }
                clear_tint();
                break;
            }
            case FurnKind::Bookshelf: { // a free-standing shelf unit, both sides, nearly empty
                fbox(M, f.x0, f.z0, f.x1, f.z1, 0, f.y1, WOOD, WOOD, WOOD, fd);
                bool along_x = (f.x1 - f.x0) > (f.z1 - f.z0);
                for (int k = 0; k < 4; k++) {
                    float y = 0.15f + k * (f.y1 - 0.2f) / 4.0f;
                    if ((hsh >> k) & 3) continue; // most shelves are bare
                    set_tint(((hsh >> (k + 4)) & 1) ? 0x8a3a2a : 0x2a4a6a);
                    float o = (((hsh >> (k + 6)) % 5u) - 2.0f) * 0.25f;
                    float bx = along_x ? mx + o : mx, bz = along_x ? mz : mz + o;
                    fbox(M, bx - (along_x ? 0.2f : hx + 0.01f), bz - (along_x ? hz + 0.01f : 0.2f), bx + (along_x ? 0.2f : hx + 0.01f), bz + (along_x ? hz + 0.01f : 0.2f), y, y + 0.24f, FABRIC, FABRIC, FABRIC, fd);
                    clear_tint();
                }
                break;
            }
            case FurnKind::GlassLift: { // the rotunda's glass lift: a teal-framed glass shaft up to the gallery
                float r = rad * 0.85f;
                set_tint(0x3a9c8c);
                for (int k = 0; k < 8; k++) { float a = k * PI / 4; cyl(M, mx + std::cos(a) * r, mz + std::sin(a) * r, 0.04f, 0.04f, 0, f.y1, METAL, false, false, 0.5f); }
                for (int k = 0; k < 4; k++) cyl(M, mx, mz, r + 0.03f, r + 0.03f, 0.05f + k * f.y1 / 3.5f, 0.12f + k * f.y1 / 3.5f, METAL, false, false, 1.5f);
                clear_tint();
                set_tint(0xb8d8d4);
                cyl(M, mx, mz, r, r, 0, f.y1, PLASTIC, true, false, 1.5f); // the glass (pale)
                clear_tint();
                set_tint(0xf0f0ec);
                cyl(M, mx, mz, r + 0.15f, r * 0.4f, f.y1, f.y1 + 1.2f, SHEET, true, false, 1.5f); // the domed top
                clear_tint();
                break;
            }
            case FurnKind::SignBoard: break;
            case FurnKind::Note: break;
            case FurnKind::Arch: break;


        }
    }

    // a face mapped onto one furniture atlas cell, repeating every `rep` metres
    void atlas_grid(int ft, Vec3 o, Vec3 a, Vec3 b, Vec3 n, int sub, float rep, float shade) {
        UV c = cell(ft);
        float la = a.len(), lb = b.len();
        Vec3 ad = a * (1.0f / la), bd = b * (1.0f / lb);
        int na = std::max(1, (int)std::lround(la / rep)), nb = std::max(1, (int)std::lround(lb / rep));
        for (int i = 0; i < na; i++)
            for (int j = 0; j < nb; j++) {
                Vec3 oo = o + ad * (la * i / na) + bd * (lb * j / nb);
                Vec3 aa = ad * (la / na), bb = bd * (lb / nb);
                grid(MAT_FURN, oo, aa, bb, sub, sub, n,
                     [&](Vec3 p, float& u, float& t) {
                         float s = (p - oo).dot(ad) / aa.len(), r = (p - oo).dot(bd) / bb.len();
                         u = c.u0 + s * (c.u1 - c.u0);
                         t = c.v0 + r * (c.v1 - c.v0);
                     },
                     [&](Vec3) { return shade; });
            }
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
        case LevelId::L0: case LevelId::L6: case LevelId::TSGFar: return LOBBY;
        case LevelId::L4: case LevelId::L21: case LevelId::L33: case LevelId::L36: case LevelId::LRun: case LevelId::TSGHope: return OFFICE;
        case LevelId::L1: case LevelId::L2: case LevelId::L22: case LevelId::L8: return INDUSTRIAL;
        case LevelId::L5: case LevelId::TSG1: case LevelId::L7: case LevelId::L19: case LevelId::L13: return HOTEL;
        case LevelId::L3: return INDUSTRIAL;
        case LevelId::L10: return OUTSIDE;
        case LevelId::L51: return STONE;
        case LevelId::L9: case LevelId::L11: case LevelId::L70: case LevelId::L94: return OUTSIDE;
        case LevelId::L188: case LevelId::TSG4: return STONE;
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
        case LevelId::L9: case LevelId::L94: tw = CELL; th = 2.9f; break; // window + door per cell face
        case LevelId::L70: tw = CELL; th = 3.5f; break;    // a storey of windows on the towers
        case LevelId::L188: tw = CELL; th = 3.2f; break;   // a window per cell per storey
        case LevelId::TSG4: tw = CELL; th = 5.0f; break;   // tall baroque storeys
        case LevelId::TheEnd: tw = CELL; th = 1.9f; break; // a run of shelves per cell
        case LevelId::L11: tw = CELL; th = 18.0f; break;   // ~3 m per storey
        case LevelId::L7: case LevelId::L24: tw = 3.0f; th = 3.0f; break;
        case LevelId::L19: tw = CELL; th = CELL; break;
        case LevelId::L21: tw = 4 * CELL; th = 2.6f; break; // four numbered doors, one per cell
        case LevelId::L22: tw = CELL; th = 1.35f; break;   // a car per cell
        case LevelId::L33: tw = CELL; th = 5.0f; break;    // one shop front per cell
        case LevelId::L36: tw = CELL; th = 4.5f; break;    // one window bay per cell
        case LevelId::L51: tw = 3.0f; th = 3.0f; break;
        case LevelId::LRun: tw = CELL; th = 2.7f; break;
        case LevelId::L3: tw = CELL; th = 3.0f; break;
        case LevelId::L10: tw = CELL; th = 4.0f; break;
        case LevelId::L13: tw = CELL; th = 2.7f; break;
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
    if (w.id() == LevelId::L2) { // barrel-vaulted maintenance tunnels
        int cx = floor_to_int(x / CELL), cz = floor_to_int(z / CELL);
        int mx = ((cx % 3) + 3) % 3, mz = ((cz % 3) + 3) % 3;
        float dx = (x / CELL - cx) - 0.5f, dz = (z / CELL - cz) - 0.5f; // -0.5..0.5 inside the cell
        float vx = std::cos(dx * PI), vz = std::cos(dz * PI);
        if (mx == 0 && mz == 0) return li.ceil_h + 0.4f * std::max(vx, vz);
        if (mz == 0) return li.ceil_h + 0.4f * vz; // tunnel along x: arched across z
        if (mx == 0) return li.ceil_h + 0.4f * vx;
        return li.ceil_h;
    }
    if (w.id() != LevelId::L8) return li.ceil_h;
    float n = vnoise_world(w.seed(), x / 2.3f, z / 2.3f, 401) * 0.7f + vnoise_world(w.seed(), x / 0.9f, z / 0.9f, 402) * 0.3f;
    return li.ceil_h - 0.2f - n * 1.0f;
}

void build_chunk_mesh(const World& W, int kx, int kz, ChunkMesh& out, int detail) {
    Builder B(W);
    const int D = std::max(1, std::min(detail, 3)); // PC: finer grids = smoother baked light
    B.seg_base = D >= 2 ? 12 : 8;                    // curves: rounder on PC, cheaper on the Vita
    const LevelInfo& li = W.info();
    const LevelId id = W.id();
    const bool cave = id == LevelId::L8 || id == LevelId::L19 || id == LevelId::L51 || id == LevelId::L2; // uneven ceilings
    const int c0x = kx * CHUNK_CELLS, c0z = kz * CHUNK_CELLS;
    out.exits.clear();
    out.items.clear();
    out.balloons.clear();
    out.lights.clear();
    out.water.clear();
    out.shadows.clear();
    out.doors.clear();
    out.containers.clear();
    out.notes.clear();
    B.terr = W.terrain();
    // how tall a building's outside walls are (the city, the hotel ring, houses)
    auto facade_h = [&](int fcx, int fcz) {
        if (W.city()) return W.block_h(fcx, fcz);
        if (W.court()) return li.ceil_h;
        if (id == LevelId::L33) return li.ceil_h;
        return W.house_h();
    };

    // light sources around the chunk
    int m = (int)(li.light_radius / CELL) + 1;
    for (int cz = c0z - m; cz < c0z + CHUNK_CELLS + m; cz++)
        for (int cx = c0x - m; cx < c0x + CHUNK_CELLS + m; cx++) {
            LightState s = W.light(cx, cz);
            if (s == LightState::On || s == LightState::Flicker)
                B.lights.push_back({{(cx + 0.5f) * CELL, W.indoors(cx, cz) ? W.indoor_h(cx, cz) - 0.2f : li.light_y, (cz + 0.5f) * CELL},
                                    li.light_power * (s == LightState::On ? 1.0f : 0.75f)});
            { // bedside lamps, camp lanterns and fire barrels light their surroundings a little
                Furn fs[MAX_FURN];
                bool owner = false;
                int nf = W.furniture(cx, cz, fs, &owner);
                for (int k = 0; owner && k < nf; k++) {
                    const Furn& f = fs[k];
                    if (f.kind == FurnKind::Lamp || f.kind == FurnKind::Lantern || f.kind == FurnKind::FireBarrel)
                        B.lights.push_back({{(f.x0 + f.x1) * 0.5f, f.kind == FurnKind::Lamp ? 0.95f : f.y1 + 0.2f, (f.z0 + f.z1) * 0.5f},
                                            f.kind == FurnKind::Lamp ? 0.55f : 0.75f});
                    if (f.kind == FurnKind::Floodlight) // the outpost's work light: bright
                        B.lights.push_back({{(f.x0 + f.x1) * 0.5f, f.y1, (f.z0 + f.z1) * 0.5f}, 1.4f});
                }
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
        Vec3 c = Vec3{o.x, 0, o.z} + ad * along + n * 0.012f;
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
                const bool indoors = W.indoors(cx, cz) && li.outdoor;  // a house, a shop, the hotel ring on an outdoor level
                const bool shop33 = id == LevelId::L33 && W.indoors(cx, cz); // inside a mall shop: a low ceiling
                const float IH = W.indoors(cx, cz) ? W.indoor_h(cx, cz) : H;
                const bool shack = (id == LevelId::TSG0 || id == LevelId::TSG2) && cx >= -1 && cx <= 1 && cz >= -1 && cz <= 1;
                if (W.terrain()) { // pools and hills: the floor follows the ground, finely
                    int ts = ((id == LevelId::L94 || id == LevelId::L24) ? 3 : (W.pool(cx, cz) ? 4 : 1)) * D;
                    B.grid(fmat, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, -CELL}, ts, ts, {0, 1, 0}, floor_uv,
                           [&](Vec3 p) { return (id == LevelId::L37 ? B.corner_ao(p) : 1.0f) * damp(p); },
                           [&](Vec3 p) { p.y = W.floor_y(p.x, p.z); return p; });
                    if (W.pool(cx, cz)) out.water.push_back({x0, z0, x1, z1, -0.14f});
                } else if (indoors && !W.court()) {
                    int ck = 0, qa, qb, dd;
                    if (W.city()) W.city_room(cx, cz, qa, qb, ck, dd);
                    static const int CITY_FLOOR[4] = {FT_STONE, FT_WOOD, FT_FABRIC, FT_STONE};
                    B.atlas_grid(W.city() ? CITY_FLOOR[ck & 3] : FT_WOOD, {x0, 0.002f, z1}, {CELL, 0, 0}, {0, 0, -CELL}, {0, 1, 0}, sub, 1.25f, 0.85f);
                    B.set_tint(0xf2ece0);
                    B.atlas_grid(FT_SHEET, {x0, IH - 0.02f, z0}, {CELL, 0, 0}, {0, 0, CELL}, {0, -1, 0}, sub, CELL, 0.75f);
                    B.clear_tint();
                } else {
                    B.grid(fmat, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, -CELL}, sub, sub, {0, 1, 0}, floor_uv,
                           [&](Vec3 p) { return B.corner_ao(p) * damp(p); });
                    if (W.court() && indoors) // the hotel ring's plaster ceiling
                        B.grid(MAT_CEIL, {x0, IH, z0}, {CELL, 0, 0}, {0, 0, CELL}, sub, sub, {0, -1, 0}, floor_uv, ao);
                }
                if (W.court() && W.court_cell(cx, cz) == 0) { // the atrium's glass roof, way up
                    float ry = li.ceil_h;
                    B.quad(MAT_LIGHT, {x0, ry, z0}, {x1, ry, z0}, {x1, ry, z1}, {x0, ry, z1}, 200, 0.92f, 0.94f, 1.0f);
                    Builder::UV uv{0, 0, 1, 1};
                    B.tube(MAT_PROP, {x0, ry - 0.06f, z0}, {x1, ry - 0.06f, z0}, 0.06f, uv, 0.5f);
                    B.tube(MAT_PROP, {x0, ry - 0.06f, z0}, {x0, ry - 0.06f, z1}, 0.06f, uv, 0.5f);
                }
                if (indoors && (W.city() || W.court())) // the building's flat roof, up where the facade ends
                    B.grid(MAT_ROOF, {x0, facade_h(cx, cz), z1}, {CELL, 0, 0}, {0, 0, -CELL}, 1, 1, {0, 1, 0}, floor_uv, [](Vec3) { return 1.0f; });
                if (shack) { // the shack's roof (rotten boards, open sky through the gaps)
                    B.atlas_grid(FT_WOOD, {x0, 2.58f, z0}, {CELL, 0, 0}, {0, 0, CELL}, {0, -1, 0}, 1, CELL, 0.6f);
                    B.atlas_grid(FT_WOOD, {x0, 2.62f, z1}, {CELL, 0, 0}, {0, 0, -CELL}, {0, 1, 0}, 1, CELL, 0.7f);
                }
                const bool gallery = id == LevelId::L33 && !shop33; // the mall's glass-roofed galleries
                if (!li.outdoor && !gallery) {
                    int csub = (cave ? 3 : 2) * D;
                    B.grid(MAT_CEIL, {x0, shop33 ? IH : H, z0}, {CELL, 0, 0}, {0, 0, CELL}, csub, csub, {0, -1, 0}, floor_uv, ao,
                           [&](Vec3 p) { if (cave) p.y = ceiling_y(W, p.x, p.z); return p; });
                }
                if (gallery) { // Hamilton-style: a glass barrel vault on teal ribs, an upper gallery round the edges
                    int mx = ((cx % 8) + 8) % 8, mz = ((cz % 8) + 8) % 8;
                    bool cross = mx < 3 && mz < 3;
                    const float VB = H, VR = 2.4f; // vault springs at the ceiling height, rises 2.4 m
                    Builder::UV glass{0, 0, 1, 1};
                    if (!cross) {
                        bool along_x = mz < 3; // the gallery runs along x (its width is across z)
                        int k = along_x ? mz : mx;
                        float a0 = along_x ? z0 : x0, hall0 = along_x ? (cz - mz) * CELL : (cx - mx) * CELL;
                        const int SEG = 3 * D;
                        for (int s2 = 0; s2 < SEG; s2++) {
                            float t0 = (a0 - hall0 + CELL * s2 / SEG) / (3 * CELL), t1 = (a0 - hall0 + CELL * (s2 + 1) / SEG) / (3 * CELL);
                            float y0 = VB + std::sin(t0 * PI) * VR, y1 = VB + std::sin(t1 * PI) * VR;
                            float c0 = hall0 + t0 * 3 * CELL, c1 = hall0 + t1 * 3 * CELL;
                            Vec3 n{0, -1, 0};
                            if (along_x) B.quad(MAT_LIGHT, {x0, y0, c0}, {x1, y0, c0}, {x1, y1, c1}, {x0, y1, c1}, 210, 0.86f, 0.95f, 1.0f);
                            else B.quad(MAT_LIGHT, {c0, y0, z1}, {c0, y0, z0}, {c1, y1, z0}, {c1, y1, z1}, 210, 0.86f, 0.95f, 1.0f);
                            (void)n;
                        }
                        (void)k;
                        // a white rib across the vault at every cell line
                        B.set_tint(0xf0f0ea);
                        for (int s2 = 0; s2 < 4; s2++) {
                            float t0 = s2 / 4.0f, t1 = (s2 + 1) / 4.0f;
                            if (t1 * 3 * CELL + hall0 < a0 - 0.01f || t0 * 3 * CELL + hall0 > a0 + CELL + 0.01f) continue;
                            float y0 = VB + std::sin(t0 * PI) * VR, y1 = VB + std::sin(t1 * PI) * VR;
                            float c0 = hall0 + t0 * 3 * CELL, c1 = hall0 + t1 * 3 * CELL;
                            Vec3 pa = along_x ? Vec3{x0, y0, c0} : Vec3{c0, y0, z0}, pb = along_x ? Vec3{x0, y1, c1} : Vec3{c1, y1, z0};
                            B.tube(MAT_PROP, pa, pb, 0.08f, glass, 0.5f);
                        }
                        B.clear_tint();
                    } else if (mx == 1 && mz == 1) { // the crossing: a glass dome over the rotunda
                        float dcx = x0 + CELL * 0.5f, dcz = z0 + CELL * 0.5f;
                        B.glow = true; B.glow_g = 0.95f; B.glow_b = 1.0f;
                        { // a hemisphere seen from inside
                            const int SEG = 12 * D / 2 + 6, RING = 4;
                            for (int j = 0; j < RING; j++) {
                                float p0 = PI / 2 * j / RING, p1 = PI / 2 * (j + 1) / RING;
                                for (int k = 0; k < SEG; k++) {
                                    float a0 = 2 * PI * k / SEG, a1 = 2 * PI * (k + 1) / SEG;
                                    auto P3 = [&](float ph, float a) { return Vec3{dcx + std::cos(ph) * std::cos(a) * 3.6f, VB + std::sin(ph) * 3.0f, dcz + std::cos(ph) * std::sin(a) * 3.6f}; };
                                    Vec3 dn{0, -1, 0};
                                    B.emit_quad(MAT_PROP, B.lit(P3(p0, a0), dn, 0, 0), B.lit(P3(p0, a1), dn, 1, 0), B.lit(P3(p1, a1), dn, 1, 1), B.lit(P3(p1, a0), dn, 0, 1));
                                }
                            }
                        }
                        B.glow = false; B.glow_g = B.glow_b = 1;
                        B.set_tint(0xf0f0ea);
                        B.cyl(MAT_PROP, dcx, dcz, 3.75f, 3.75f, VB - 0.3f, VB, glass, false, true, 2.0f);
                        B.clear_tint();
                    }
                    if (cross && !(mx == 1 && mz == 1)) // a flat skylight round the dome
                        B.quad(MAT_LIGHT, {x0, VB, z0}, {x1, VB, z0}, {x1, VB, z1}, {x0, VB, z1}, 190, 0.86f, 0.95f, 1.0f);
                    // the upper gallery: a slab along any side that meets a shop block, a glass railing with a teal handrail
                    static const int GX[4] = {-1, 1, 0, 0}, GZ[4] = {0, 0, -1, 1};
                    for (int sd = 0; sd < 4; sd++) {
                        int nx = cx + GX[sd], nz = cz + GZ[sd];
                        int nmx = ((nx % 8) + 8) % 8, nmz = ((nz % 8) + 8) % 8;
                        if (nmx < 3 || nmz < 3) continue; // the neighbour is gallery too
                        const float GY = 4.6f, GD = 1.2f;
                        float ax0 = sd == 0 ? x0 : (sd == 1 ? x1 - GD : x0), ax1 = sd == 0 ? x0 + GD : (sd == 1 ? x1 : x1);
                        float az0 = sd == 2 ? z0 : (sd == 3 ? z1 - GD : z0), az1 = sd == 2 ? z0 + GD : (sd == 3 ? z1 : z1);
                        B.set_tint(0xd8d4c8);
                        B.fbox(MAT_FURN, ax0, az0, ax1, az1, GY - 0.25f, GY, Builder::cell(FT_STONE), Builder::cell(FT_STONE), Builder::cell(FT_STONE), {0, 1}, true);
                        B.clear_tint();
                        // railing along the open edge
                        float ex0 = sd == 0 ? x0 + GD : (sd == 1 ? x1 - GD : x0), ex1 = sd == 0 ? x0 + GD : (sd == 1 ? x1 - GD : x1);
                        float ez0 = sd == 2 ? z0 + GD : (sd == 3 ? z1 - GD : z0), ez1 = sd == 2 ? z0 + GD : (sd == 3 ? z1 - GD : z1);
                        if (sd < 2) { ez0 = z0; ez1 = z1; } else { ex0 = x0; ex1 = x1; }
                        Builder::UV pl = Builder::cell(FT_PLASTIC);
                        B.set_tint(0x9cc8c8);
                        Vec3 nn = sd < 2 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
                        Vertex q0 = B.lit({ex0, GY, ez0}, nn, pl.u0, pl.v1), q1 = B.lit({ex1, GY, ez1}, nn, pl.u1, pl.v1);
                        Vertex q2 = B.lit({ex1, GY + 1.0f, ez1}, nn, pl.u1, pl.v0), q3 = B.lit({ex0, GY + 1.0f, ez0}, nn, pl.u0, pl.v0);
                        B.emit_quad(MAT_FURN, q0, q1, q2, q3);
                        B.emit_quad(MAT_FURN, q1, q0, q3, q2);
                        B.set_tint(0xc8a050);
                        B.tube(MAT_PROP, {ex0, GY + 1.02f, ez0}, {ex1, GY + 1.02f, ez1}, 0.045f, glass, 0.5f);
                        B.clear_tint();
                        if (hash01(W.seed(), cx * 4 + sd, cz, 7401) < 0.55f) { // ivy spilling over the gallery's edge
                            Builder::UV lv = Builder::cell(FT_LEAVES);
                            for (int k = 0; k < 3; k++) {
                                float t = (k + 0.5f) / 3.0f;
                                float ix = ex0 + (ex1 - ex0) * t, iz = ez0 + (ez1 - ez0) * t;
                                B.sphere(MAT_FURN, {ix, GY - 0.12f, iz}, {0.28f, 0.22f, 0.28f}, lv, 0.6f);
                                B.sphere(MAT_FURN, {ix, GY - 0.45f, iz}, {0.12f, 0.3f, 0.12f}, lv, 0.5f);
                            }
                        }
                    }
                }

                float lift;
                if (W.floating_block(cx, cz, lift)) { // 4Four: a building hanging in the air, its underside showing
                    float bh = W.block_h(cx, cz);
                    B.set_tint(W.tint(cx, cz));
                    B.box(MAT_BLOCK, x0 + CELL * 0.5f, z0 + CELL * 0.5f, CELL * 0.5f, CELL * 0.5f, lift, lift + bh, btw, bth, 1);
                    B.grid(MAT_ROOF, {x0, lift, z0}, {CELL, 0, 0}, {0, 0, CELL}, 1, 1, {0, -1, 0}, floor_uv, [](Vec3) { return 0.45f; });
                    B.clear_tint();
                }
                if (indoors && W.houses() && W.house(cx, cz)) { // the house's gable roof: the ridge runs along x through the middle
                    int lx, lz, hbx, hbz;
                    W.house_lot(cx, cz, lx, lz, hbx, hbz);
                    const float eave = W.house_h(), ridge = eave + 1.7f, ov = 0.25f;
                    bool front = lz == 0; // this row of the house rises toward +z
                    float zo = front ? z0 - ov : z1 + ov, zr = front ? z1 : z0;
                    float xa = x0 - (lx == 0 ? ov : 0.0f), xb = x1 + (lx == 1 ? ov : 0.0f);
                    Vec3 n = front ? Vec3{0, 0.8f, -0.6f} : Vec3{0, 0.8f, 0.6f};
                    B.set_tint(W.tint(cx, cz));
                    Vec3 o = front ? Vec3{xa, eave - 0.12f, zo} : Vec3{xb, eave - 0.12f, zo};
                    Vec3 av = front ? Vec3{xb - xa, 0, 0} : Vec3{xa - xb, 0, 0};
                    Vec3 bv{0, ridge - eave + 0.12f, zr - zo};
                    B.grid(MAT_ROOF, o, av, bv, 1, 1, n, [&](Vec3 p, float& u, float& t) { u = p.x / CELL; t = p.z / CELL; }, [](Vec3) { return 1.0f; });
                    B.grid(MAT_ROOF, o + av, av * -1.0f, bv, 1, 1, n * -1.0f, [&](Vec3 p, float& u, float& t) { u = p.x / CELL; t = p.z / CELL; }, [](Vec3) { return 0.5f; });
                    for (int sideX = 0; sideX <= 1; sideX++) { // the gable ends
                        if (lx != sideX) continue;
                        float x = sideX ? x1 : x0;
                        Vec3 A{x, eave, front ? z0 : z1}, Bv{x, eave, zr}, Cv{x, ridge, zr};
                        Vec3 e1 = Bv - A, e2 = Cv - A;
                        Vec3 cr{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                        float out_x = sideX ? 1.0f : -1.0f;
                        if (cr.x * out_x < 0) std::swap(Bv, Cv);
                        float c[3];
                        B.shade({x, eave + 0.8f, (z0 + z1) / 2}, {out_x, 0, 0}, c);
                        auto& vs = B.v[MAT_WALL];
                        auto& is = B.i[MAT_WALL];
                        uint16_t b0 = (uint16_t)vs.size();
                        for (Vec3 p : {A, Bv, Cv}) vs.push_back(B.vert(p, p.z / TEX_WALL, 0.02f + (ridge - p.y) / 2.7f * 0.4f, c, 1));
                        is.insert(is.end(), {b0, (uint16_t)(b0 + 1), (uint16_t)(b0 + 2)});
                    }
                    B.clear_tint();
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
                        float y = (id == LevelId::L1 ? li.light_y : (cave ? ceiling_y(W, cxm, czm) : ((indoors || shop33) ? IH : H))) - 0.03f;
                        if (indoors) { sx = 0.22f; sz = 0.22f; } // a ceiling lamp, not a street light
                        if (gallery) { // a pendant hanging from the vault on a long cable
                            y = li.light_y;
                            sx = sz = 0.35f;
                            B.box(MAT_PROP, cxm, czm, 0.012f, 0.012f, y, H + 1.5f, 0.1f, 1, 1);
                            B.cyl(MAT_PROP, cxm, czm, 0.42f, 0.12f, y - 0.02f, y + 0.3f, {0, 0, 1, 1}, true, false, 1.0f);
                        }
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
                if (W.exit_door(cx, cz, &door) && door.side < 0) { // exits that stand free in the cell
                    const float ex = door.pos.x, ez = door.pos.z;
                    switch (W.exit_style()) {
                        case ExitStyle::Hole: { // a pit dug ten metres down, a ladder going in, the shovel left beside it
                            Builder::UV uv{0, 0, 1, 1};
                            B.set_tint(0x050403);
                            B.cyl(MAT_FLOOR, ex, ez, 0.62f, 0.62f, -0.02f, 0.015f, uv, false, true, 1.25f);
                            B.set_tint(0x7a6a58);
                            for (int k = 0; k < 7; k++) {
                                float a = k * 2 * PI / 7 + 0.3f;
                                B.sphere(MAT_FLOOR, {ex + std::cos(a) * 0.95f, 0.02f, ez + std::sin(a) * 0.95f}, {0.42f, 0.22f, 0.42f}, uv, 0.75f);
                            }
                            B.clear_tint();
                            for (int s2 = -1; s2 <= 1; s2 += 2) B.tube(MAT_PROP, {ex - 0.2f * s2, -1.0f, ez - 0.35f}, {ex - 0.2f * s2, 0.7f, ez - 0.5f}, 0.025f, uv, 0.5f);
                            for (int k = 0; k < 3; k++) B.tube(MAT_PROP, {ex - 0.2f, k * 0.28f, ez - 0.38f - k * 0.03f}, {ex + 0.2f, k * 0.28f, ez - 0.38f - k * 0.03f}, 0.018f, uv, 0.5f);
                            B.tube(MAT_PROP, {ex + 1.05f, 0.0f, ez + 0.4f}, {ex + 1.15f, 1.15f, ez + 0.55f}, 0.022f, uv, 0.5f);
                            B.box(MAT_PROP, ex + 1.03f, ez + 0.38f, 0.12f, 0.03f, -0.05f, 0.25f, 0.3f, 0.3f, 1);
                            break;
                        }
                        case ExitStyle::Clock: { // an analog grandfather clock standing in the sea
                            Furn f{};
                            f.kind = FurnKind::Clock; f.x0 = ex - 0.4f; f.z0 = ez - 0.3f; f.x1 = ex + 0.4f; f.z1 = ez + 0.3f; f.y0 = 0.0f; f.y1 = 2.2f; f.solid = true; f.face = {0, 1};
                            B.furniture(f, H);
                            break;
                        }
                        case ExitStyle::LoneDoor: { // a door in its frame, standing in the void, both sides
                            const float DW = 1.1f, DH = 2.15f;
                            for (int s2 = -1; s2 <= 1; s2 += 2) {
                                Vec3 n{0, 0, (float)s2}, t{(float)s2, 0, 0};
                                Vec3 o = Vec3{ex, 0, ez} + n * 0.045f - t * (DW * 0.5f);
                                B.grid(MAT_DOOR, o, t * DW, Vec3{0, DH, 0}, 1, 2, n,
                                       [&](Vec3 p, float& u, float& tt) { u = (p - o).dot(t) / DW; tt = 1.0f - p.y / DH; },
                                       [](Vec3) { return 1.0f; });
                            }
                            B.box(MAT_PROP, ex - DW * 0.5f - 0.05f, ez, 0.05f, 0.08f, 0, DH + 0.1f, 0.3f, 2.3f, 1);
                            B.box(MAT_PROP, ex + DW * 0.5f + 0.05f, ez, 0.05f, 0.08f, 0, DH + 0.1f, 0.3f, 2.3f, 1);
                            B.box(MAT_PROP, ex, ez, DW * 0.5f + 0.1f, 0.08f, DH, DH + 0.12f, 0.3f, 0.3f, 1);
                            B.box(MAT_PROP, ex, ez + 0.04f, 0.32f, 0.035f, 2.24f, 2.48f, 0.6f, 0.3f, 1);
                            break;
                        }
                        default: break; // the fallen angel is drawn by the renderer (it's alive, more or less)
                    }
                    out.exits.push_back(door);
                } else if (W.exit_door(cx, cz, &door)) {
                    Vec3 n{door.normal.x, 0, door.normal.z};
                    Vec3 t{n.z, 0, -n.x}; // along the wall, left to right seen from the cell
                    Vec3 c{door.pos.x, 0, door.pos.z};
                    const float DW = 1.1f, DH = 2.15f;
                    // the slab: a box with real thickness, its face showing the door texture
                    const float face = 0.045f;
                    Vec3 dor = c + n * face - t * (DW * 0.5f);
                    B.grid(MAT_DOOR, dor, t * DW, Vec3{0, DH, 0}, 1, 2, n,
                           [&](Vec3 p, float& u, float& tt) { u = (p - dor).dot(t) / DW; tt = 1.0f - p.y / DH; },
                           [](Vec3 p) { return p.y < 0.01f ? 0.8f : 1.0f; });
                    B.vface(MAT_PROP, c + n * 0.012f - t * (DW * 0.5f), n * (face - 0.012f), t * -1.0f, 0, DH, 0.1f, DH, 1, 1, false);
                    B.vface(MAT_PROP, c + n * face + t * (DW * 0.5f), n * -(face - 0.006f), t, 0, DH, 0.1f, DH, 1, 1, false);
                    // a deep steel frame: two jambs and a header standing proud of the wall
                    auto frame = [&](Vec3 m, float half_t, float y0, float y1) {
                        float hx = std::fabs(t.x) * half_t + std::fabs(n.x) * 0.06f, hz = std::fabs(t.z) * half_t + std::fabs(n.z) * 0.06f;
                        B.box(MAT_PROP, m.x, m.z, hx, hz, y0, y1, 0.3f, 2.3f, 1);
                    };
                    frame(c + n * 0.05f - t * (DW * 0.5f + 0.05f), 0.05f, 0, DH + 0.1f);
                    frame(c + n * 0.05f + t * (DW * 0.5f + 0.05f), 0.05f, 0, DH + 0.1f);
                    frame(c + n * 0.05f, DW * 0.5f + 0.1f, DH, DH + 0.1f);
                    // push bar across the door on two brackets, hinges, a door closer at the top
                    Builder::UV metal{0, 0, 1, 1};
                    Vec3 bar0 = c + n * (face + 0.075f) - t * (DW * 0.42f), bar1 = c + n * (face + 0.075f) + t * (DW * 0.42f);
                    B.tube(MAT_PROP, bar0 + Vec3{0, 1.0f, 0}, bar1 + Vec3{0, 1.0f, 0}, 0.022f, metal, 0.75f);
                    for (int e = -1; e <= 1; e += 2) {
                        Vec3 bc = c + n * (face + 0.04f) + t * (DW * 0.42f * e);
                        float bxh = std::fabs(t.x) * 0.03f + std::fabs(n.x) * 0.04f, bzh = std::fabs(t.z) * 0.03f + std::fabs(n.z) * 0.04f;
                        B.box(MAT_PROP, bc.x, bc.z, bxh, bzh, 0.95f, 1.05f, 0.2f, 0.2f, 1);
                    }
                    for (int k = 0; k < 3; k++) {
                        Vec3 hc = c + n * (face + 0.01f) - t * (DW * 0.5f - 0.02f);
                        B.cyl(MAT_PROP, hc.x, hc.z, 0.018f, 0.018f, 0.25f + k * 0.8f, 0.37f + k * 0.8f, metal, true, true, 0.5f);
                    }
                    Vec3 cc = c + n * (face + 0.05f) + t * (DW * 0.25f);
                    float cxh = std::fabs(t.x) * 0.15f + std::fabs(n.x) * 0.04f, czh = std::fabs(t.z) * 0.15f + std::fabs(n.z) * 0.04f;
                    B.box(MAT_PROP, cc.x, cc.z, cxh, czh, DH - 0.12f, DH - 0.04f, 0.3f, 0.1f, 1);
                    // the EXIT sign's housing (its glowing face is drawn by the renderer)
                    Vec3 sc = c + n * 0.04f;
                    float sxh = std::fabs(t.x) * 0.32f + std::fabs(n.x) * 0.035f, szh = std::fabs(t.z) * 0.32f + std::fabs(n.z) * 0.035f;
                    B.box(MAT_PROP, sc.x, sc.z, sxh, szh, 2.24f, 2.48f, 0.6f, 0.3f, 1);
                    out.exits.push_back(door);
                }
                if (W.item(cx, cz) != ItemType::None) out.items.push_back(World::item_id(cx, cz));
                if (W.note(cx, cz)) out.notes.push_back(World::item_id(cx, cz));
                // ----- wheat: crossed alpha-tested cards, a little uneven
                if (W.crop(cx, cz)) {
                    int nq = 3 * D;
                    for (int k = 0; k < nq; k++) {
                        float px = x0 + CELL * (0.15f + 0.7f * hash01(W.seed(), cx, cz, 8001 + k)), pz = z0 + CELL * (0.15f + 0.7f * hash01(W.seed(), cx, cz, 8011 + k));
                        float ang = hash01(W.seed(), cx, cz, 8021 + k) * PI, hgt = 1.35f + hash01(W.seed(), cx, cz, 8031 + k) * 0.5f;
                        float wd = 1.3f;
                        Vec3 a2{std::cos(ang) * wd, 0, std::sin(ang) * wd};
                        Vec3 o = Vec3{px, 0, pz} - a2 * 0.5f;
                        float u0 = hash01(W.seed(), cx, cz, 8041 + k) * 0.5f;
                        B.grid(MAT_CROP, o, a2, Vec3{0, hgt, 0}, 1, 1, {-a2.z, 0, a2.x},
                               [&](Vec3 p, float& u, float& t) { u = u0 + (p - o).dot(a2) / (wd * wd) * 0.5f; t = 1.0f - p.y / hgt; },
                               [](Vec3 p) { return p.y < 0.1f ? 0.6f : 1.0f; });
                    }
                }
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
                if (id == LevelId::L4 && W.item(cx, cz) != ItemType::None) out.items.push_back(World::item_id(cx, cz)); // left on a desk
                float h = W.block_h(cx, cz);
                B.set_tint(W.tint(cx, cz));
                const float cxm0 = x0 + CELL * 0.5f, czm0 = z0 + CELL * 0.5f;
                bool special = false;
                if (id == LevelId::TSG0 || id == LevelId::TSG2) { // a tree trunk fills the cell
                    special = true;
                    uint32_t th = hash3(W.seed(), cx, cz, 6001);
                    float tx = cxm0 + ((th % 100u) / 100.0f - 0.5f) * 0.7f, tz = czm0 + (((th >> 8) % 100u) / 100.0f - 0.5f) * 0.7f;
                    Builder::UV bark{0, 0, 2.0f, 5.0f}, leaf{0, 0, 2.0f, 2.0f};
                    if (id == LevelId::TSG0) { // jungle: thick trunk, buttress roots, a wide canopy (and trees that clip into each other)
                        B.cyl(MAT_BLOCK, tx, tz, 0.6f, 0.36f, 0, 8.6f, bark, false, false, 1.25f);
                        for (int k = 0; k < 4; k++) {
                            float a = k * PI * 0.5f + (th & 7) * 0.2f;
                            B.tube(MAT_BLOCK, {tx + std::cos(a) * 1.05f, 0.0f, tz + std::sin(a) * 1.05f}, {tx + std::cos(a) * 0.25f, 1.3f, tz + std::sin(a) * 0.25f}, 0.13f, bark, 0.6f);
                        }
                        B.sphere(MAT_ROOF, {tx, 9.0f, tz}, {3.0f, 1.6f, 3.0f}, leaf, 1.25f);
                        B.sphere(MAT_ROOF, {tx + 1.6f, 8.0f, tz + 0.8f}, {1.9f, 1.2f, 1.9f}, leaf, 1.0f);
                        B.sphere(MAT_ROOF, {tx - 1.3f, 8.4f, tz - 1.2f}, {1.8f, 1.1f, 1.8f}, leaf, 1.0f);
                        if ((th >> 16) % 6u == 0) // two trees occupying the same space
                            B.tube(MAT_BLOCK, {tx + 0.7f, 0.0f, tz - 0.4f}, {tx - 0.6f, 8.0f, tz + 0.7f}, 0.3f, bark, 0.8f);
                    } else { // 2Two: snowed-on pines (the heat doesn't melt any of it)
                        B.cyl(MAT_BLOCK, tx, tz, 0.3f, 0.2f, 0, 2.4f, bark, false, false, 0.8f);
                        for (int k = 0; k < 3; k++) {
                            float y0 = 1.4f + k * 2.0f, r = 2.1f - k * 0.55f;
                            B.cyl(MAT_ROOF, tx, tz, r, 0.05f, y0, y0 + 3.0f, leaf, true, false, 1.25f);
                        }
                    }
                } else if (id == LevelId::L94 && W.hill(cx, cz)) { // the hills: soft, round, overlapping
                    special = true;
                    B.sphere(MAT_FLOOR, {cxm0, 0.0f, czm0}, {CELL * 0.98f, h, CELL * 0.98f}, {0, 0, 2.0f, 2.0f}, 1.5f);
                } else if (id == LevelId::L24) { // a great lumpy moon boulder
                    special = true;
                    float gy = W.floor_y(cxm0, czm0);
                    Builder::UV uv{0, 0, 2.0f, 2.0f};
                    size_t v0 = B.v[MAT_BLOCK].size();
                    B.sphere(MAT_BLOCK, {cxm0, h * 0.35f, czm0}, {1.25f, h * 0.7f, 1.15f}, uv, 1.25f);
                    uint32_t bh = hash3(W.seed(), cx, cz, 6101);
                    for (int k = 0; k < 3; k++) {
                        float a = ((bh >> (k * 5)) & 31) / 32.0f * 2 * PI;
                        B.sphere(MAT_BLOCK, {cxm0 + std::cos(a) * 0.6f, h * 0.45f, czm0 + std::sin(a) * 0.6f}, {0.8f, h * 0.5f, 0.75f}, uv, 0.9f);
                    }
                    for (size_t q = v0; q < B.v[MAT_BLOCK].size(); q++) B.v[MAT_BLOCK][q].y += gy - 0.2f;
                } else if (id == LevelId::L70 && h < 2.0f) { // an igloo
                    special = true;
                    Builder::UV uv{0, 0, 3.0f, 1.5f};
                    B.sphere(MAT_FLOOR, {cxm0, 0.0f, czm0}, {1.2f, h, 1.2f}, uv, 1.25f);
                    B.tube(MAT_FLOOR, {cxm0, 0.42f, czm0 + 0.6f}, {cxm0, 0.42f, czm0 + 1.45f}, 0.45f, uv, 0.75f);
                    B.set_tint(0x080808);
                    B.cyl(MAT_PROP, cxm0, czm0 + 1.47f, 0.33f, 0.33f, 0.0f, 0.6f, uv, false, false, 0.75f);
                }
                if (special) { // trees and hills don't fill the cell: it still needs ground under them
                    B.clear_tint();
                    int sfm = W.floor_mat(cx, cz);
                    Material sm = sfm == 1 ? MAT_FLOOR2 : (sfm == 2 ? MAT_FLOOR3 : MAT_FLOOR);
                    B.grid(sm, {x0, 0, z1}, {CELL, 0, 0}, {0, 0, -CELL}, 2 * D, 2 * D, {0, 1, 0}, floor_uv, [&](Vec3 p) { return 0.8f * damp(p); },
                           [&](Vec3 p) { if (W.terrain()) p.y = W.floor_y(p.x, p.z); return p; });
                }
                size_t shear0 = B.v[MAT_BLOCK].size(), shear1 = B.v[MAT_ROOF].size();
                if (!special) {
                // low furniture-like blocks use a different texture from the tall ones
                Material bmat = MAT_BLOCK;
                float tw = btw, th = bth;
                bool low = h < 2.2f;
                if (id == LevelId::L19 && low) { bmat = MAT_ROOF; tw = 1.2f; th = 1.2f; }   // boxes
                { int a1, a2, a3, a4; if (id == LevelId::L70 && W.house_lot(cx, cz, a1, a2, a3, a4) == 2) { bmat = MAT_WALL; tw = CELL; th = W.house_h(); } } // a locked house
                if (id == LevelId::L22 && !low) { bmat = MAT_WALL; tw = TEX_WALL; th = H; } // stairwell cores
                if (id == LevelId::L33 && low) { bmat = MAT_PROP; tw = 1.0f; th = 1.0f; }   // planters, shelves
                if (id == LevelId::L36 && low) { bmat = MAT_ROOF; tw = CELL; th = 1.0f; }   // seats, desks
                int sv = (li.outdoor ? (id == LevelId::L9 ? 2 : 1) : 2) * D;
                int su = (id == LevelId::L11 ? 1 : 2) * D;
                auto side = [&](int nx, int nz, Vec3 o, Vec3 a, Vec3 n) {
                    float nh = W.solid(nx, nz) ? W.block_h(nx, nz) : 0.0f;
                    if (id == LevelId::TSG4 && nh > 0) { // 4Four: leaning buildings pull away from their neighbours: never hide those faces
                        float l1, t1, l2, t2;
                        bool g1 = W.glitch(cx, cz, l1, t1), g2 = W.glitch(nx, nz, l2, t2);
                        bool other = World::city_quarter(cx) != World::city_quarter(nx) || World::city_quarter(cz) != World::city_quarter(nz);
                        if ((g1 || g2) && other) nh = 0;
                    }
                    if (nh >= h - 0.01f) return;
                    if (!low && W.indoors(nx, nz) && h > W.indoor_h(nx, nz)) { // the inside wall of a room / shop / corridor
                        float ih = W.indoor_h(nx, nz);
                        if (li.outdoor && !W.court()) { B.set_tint(0xe8dcc0); B.atlas_grid(FT_SHEET, Vec3{o.x, 0, o.z}, a, Vec3{0, ih, 0}, n, D, 1.25f, 0.8f); B.set_tint(W.tint(cx, cz)); }
                        else B.vface(MAT_WALL, o, a, n, 0, ih, TEX_WALL, ih, su, sv, false);
                        return;
                    }
                    B.vface(bmat, o, a, n, nh, h, tw, th, su, sv, cave && !low);
                    if (!low && nh <= 0.0f) wall_decal(o, a, n, std::min(h, H), cx * 7 + nx, cz * 7 + nz);
                    // the doors painted on these walls get a real frame and a knob, so they stand out of the wall
                    if (!low && nh <= 0.0f && (W.hotel() || id == LevelId::L21)) {
                        float len = a.len();
                        Vec3 ad = a * (1.0f / len);
                        float u0 = (id == LevelId::L21 ? 0.30f : 0.28f) * CELL, u1 = (id == LevelId::L21 ? 0.70f : 0.72f) * CELL;
                        float top = id == LevelId::L21 ? 2.6f * 0.82f : 2.9f * 0.78f;
                        if (id == LevelId::L21) { // only every door position (the texture repeats every cell)
                            float along0 = (Vec3{o.x, 0, o.z}).dot(ad) / CELL;
                            (void)along0;
                        }
                        Vec3 b0 = Vec3{o.x, 0, o.z} + n * 0.025f;
                        auto jamb = [&](float u) {
                            Vec3 c = b0 + ad * u;
                            float jx = std::fabs(ad.x) * 0.045f + std::fabs(n.x) * 0.025f, jz = std::fabs(ad.z) * 0.045f + std::fabs(n.z) * 0.025f;
                            B.box(MAT_PROP, c.x, c.z, jx, jz, 0, top + 0.06f, 0.3f, 2.3f, 1);
                        };
                        jamb(u0 - 0.03f);
                        jamb(u1 + 0.03f);
                        Vec3 hc = b0 + ad * ((u0 + u1) * 0.5f);
                        float hxh = std::fabs(ad.x) * ((u1 - u0) * 0.5f + 0.08f) + std::fabs(n.x) * 0.025f, hzh = std::fabs(ad.z) * ((u1 - u0) * 0.5f + 0.08f) + std::fabs(n.z) * 0.025f;
                        B.box(MAT_PROP, hc.x, hc.z, hxh, hzh, top, top + 0.07f, 1.0f, 0.2f, 1);
                        Vec3 kc = Vec3{o.x, 0, o.z} + ad * (u1 - (u1 - u0) * 0.14f) + n * 0.04f; // the knob
                        B.sphere(MAT_PROP, {kc.x, top * 0.48f, kc.z}, {0.035f, 0.035f, 0.035f}, {0.4f, 0.4f, 0.6f, 0.6f}, 0.5f);
                    }
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
                int hlx, hlz, hbx, hbz;
                if (!special && W.houses() && W.house_lot(cx, cz, hlx, hlz, hbx, hbz) == 2) {
                    // gable roof: houses are two cells deep, the ridge runs along x between them
                    float ridge = h + 1.7f;
                    bool front = hlz == 0; // this cell's roof rises toward +z
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
                } // (!special)
                float lift_, tilt;
                if (W.glitch(cx, cz, lift_, tilt) && tilt != 0) { // 4Four: the whole building leans (the walls bend with it)
                    for (size_t k = shear0; k < B.v[MAT_BLOCK].size(); k++) B.v[MAT_BLOCK][k].x += tilt * B.v[MAT_BLOCK][k].y;
                    for (size_t k = shear1; k < B.v[MAT_ROOF].size(); k++) B.v[MAT_ROOF][k].x += tilt * B.v[MAT_ROOF][k].y;
                }
                if (id == LevelId::L188) { // shadows standing behind some of the windows, watching
                    static const int NX[4] = {1, -1, 0, 0}, NZ[4] = {0, 0, 1, -1};
                    B.set_tint(0x060606);
                    for (int sd = 0; sd < 4; sd++) {
                        if (W.solid(cx + NX[sd], cz + NZ[sd])) continue;
                        for (int fl = 0; fl < 4; fl++) {
                            if (hash01(W.seed(), cx * 4 + sd, cz * 4 + fl, 7301) > 0.1f) continue;
                            float y = fl * 3.2f + 1.0f;
                            Vec3 c{cxm0 + NX[sd] * (CELL * 0.5f + 0.02f), 0, czm0 + NZ[sd] * (CELL * 0.5f + 0.02f)};
                            Builder::UV uv{0, 0, 1, 1};
                            B.sphere(MAT_PROP, {c.x, y + 1.05f, c.z}, {0.11f, 0.13f, 0.11f}, uv, 0.6f);
                            float wx = NX[sd] ? 0.03f : 0.2f, wz = NZ[sd] ? 0.03f : 0.2f;
                            B.box(MAT_PROP, c.x, c.z, wx, wz, y + 0.35f, y + 0.92f, 0.3f, 0.3f, 1);
                        }
                    }
                }
                B.clear_tint();
            }

            // ----- thin walls owned by this cell (west + north edges), corner posts, pillars
            // walls on outdoor levels belong to houses and shacks: house height, not the sky
            const float WH = li.wall_h > 10.0f ? (W.houses() ? W.house_h() : 2.6f) : li.wall_h;
            const bool low = WH < H - 0.01f;
            // a wall face inside a house gets wallpaper (atlas) instead of the outside siding
            auto inside_face = [&](int fcx, int fcz) { return W.houses() && W.house(fcx, fcz); };
            const float wtw = W.houses() ? CELL : (id == LevelId::TheEnd ? 4 * CELL : TEX_WALL); // siding: one window per cell; THE END: one sign every 10 m
            auto paper = [&](Vec3 o, Vec3 a, Vec3 n, float h) {
                B.set_tint(0xe8dcc0);
                B.atlas_grid(FT_SHEET, o, a, Vec3{0, h, 0}, n, D, 1.25f, 0.8f);
                B.clear_tint();
            };
            auto cap = [&](float xa, float za, float xb, float zb) { // top of a low wall
                B.grid(MAT_WALL, {xa, WH, zb}, {xb - xa, 0, 0}, {0, 0, za - zb}, 1, 1, {0, 1, 0},
                       [&](Vec3 p, float& u, float& t) { u = (p.x + p.z) / TEX_WALL; t = 0.97f; }, [](Vec3) { return 1.0f; });
            };
            // districts on the room-style levels: each part of the maze is a slightly different shade
            bool tinted = id == LevelId::L0 || id == LevelId::L6 || id == LevelId::LFun;
            if (tinted) {
                static const uint32_t DT[4] = {0xffffff, 0xeef5e2, 0xe2d6c0, 0xfff2d6};
                B.set_tint(DT[(int)W.district(cx, cz) & 3]);
            }
            const bool bldg = W.city() || W.court(); // 1.2: walls with a tall outside, an inside, windows and door headers
            auto bface = [&](Vec3 o, Vec3 a, Vec3 n, int fcx, int fcz, int ocx, int ocz, bool win) {
                // which surface faces into cell F (with cell O behind it), how tall, what it's made of
                float h;
                int kind; // 0 paper (city inside), 1 hotel wallpaper (court inside), 2 facade, 3 plain wall
                if (W.indoors(fcx, fcz)) { h = W.indoor_h(fcx, fcz); kind = W.court() ? 1 : 0; }
                else if (W.indoors(ocx, ocz)) { h = facade_h(ocx, ocz); kind = 2; }
                else { h = WH; kind = 3; }
                auto part = [&](Vec3 po, Vec3 pa, float y0, float y1) {
                    if (y1 - y0 < 0.01f || pa.len() < 0.01f) return;
                    if (kind == 0) { B.set_tint(0xe8dcc0); B.atlas_grid(FT_SHEET, Vec3{po.x, y0, po.z}, pa, Vec3{0, y1 - y0, 0}, n, D, 1.25f, 0.8f); B.clear_tint(); }
                    else if (kind == 2) { B.vface(MAT_BLOCK, po, pa, n, y0, y1, btw, bth, std::max(1, D), std::max(1, (int)((y1 - y0) / 4.0f)) * D, false); }
                    else B.vface(MAT_WALL, po, pa, n, y0, y1, TEX_WALL, kind == 1 ? h : y1 - y0, 2 * D, 2 * D, false);
                };
                if (!win) { part(o, a, 0, h); return; }
                float L = a.len();
                Vec3 ad = a * (1.0f / L);
                const float S = 0.95f, T = 2.15f, J = 0.6f; // sill, head, jamb width
                part(o, a, 0, S);
                part(o, a, T, h);
                part(o, ad * J, S, T);
                part(o + ad * (L - J), ad * J, S, T);
            };
            auto window_trim = [&](Vec3 c, Vec3 along, Vec3 n) { // the reveal, a sill and a cross of glazing bars in an empty frame
                const float S = 0.95f, T = 2.15f, Wd = CELL - 1.2f;
                Builder::UV uv{0, 0, 1, 1};
                Vec3 l = c - along * (Wd * 0.5f), r = c + along * (Wd * 0.5f);
                for (int s2 = -1; s2 <= 1; s2 += 2) {
                    Vec3 e = s2 < 0 ? l : r;
                    B.box(MAT_PROP, e.x, e.z, std::fabs(along.x) * 0.04f + std::fabs(n.x) * (hw + 0.03f), std::fabs(along.z) * 0.04f + std::fabs(n.z) * (hw + 0.03f), S, T, 0.2f, 1.2f, 1);
                }
                B.box(MAT_PROP, c.x, c.z, std::fabs(along.x) * (Wd * 0.5f + 0.06f) + std::fabs(n.x) * (hw + 0.06f), std::fabs(along.z) * (Wd * 0.5f + 0.06f) + std::fabs(n.z) * (hw + 0.06f), S - 0.06f, S, 1.0f, 0.1f, 1);
                B.box(MAT_PROP, c.x, c.z, std::fabs(along.x) * (Wd * 0.5f + 0.04f) + std::fabs(n.x) * (hw + 0.03f), std::fabs(along.z) * (Wd * 0.5f + 0.04f) + std::fabs(n.z) * (hw + 0.03f), T, T + 0.06f, 1.0f, 0.1f, 1);
                B.box(MAT_PROP, c.x, c.z, std::fabs(along.x) * 0.02f + std::fabs(n.x) * 0.02f, std::fabs(along.z) * 0.02f + std::fabs(n.z) * 0.02f, S, T, 0.1f, 1.0f, 1);
                B.tube(MAT_PROP, {l.x, (S + T) * 0.5f, l.z}, {r.x, (S + T) * 0.5f, r.z}, 0.02f, uv, 0.5f);
            };
            auto header = [&](Vec3 o, Vec3 a, Vec3 n, int fcx, int fcz, int ocx, int ocz) { // over a doorway: the facade continues above it
                const float DT = 2.45f;
                bool fin = W.indoors(fcx, fcz);
                float h = fin ? W.indoor_h(fcx, fcz) : facade_h(ocx, ocz);
                if (h <= DT) return;
                if (fin && !W.court()) { B.set_tint(0xe8dcc0); B.atlas_grid(FT_SHEET, Vec3{o.x, DT, o.z}, a, Vec3{0, h - DT, 0}, n, D, 1.25f, 0.8f); B.clear_tint(); }
                else B.vface(fin ? MAT_WALL : MAT_BLOCK, o, a, n, DT, h, fin ? TEX_WALL : btw, fin ? h : bth, D, D, false);
            };
            if (bldg) {
                if (W.wall_x(cx, cz)) {
                    bool win = W.window_edge(cx, cz, true);
                    bface({x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, cx, cz, cx - 1, cz, win);
                    bface({x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, cx - 1, cz, cx, cz, win);
                    if (win) window_trim({x0, 0, z0 + CELL * 0.5f}, {0, 0, 1}, {1, 0, 0});
                } else if (!W.solid(cx, cz) && !W.solid(cx - 1, cz) && W.indoors(cx, cz) != W.indoors(cx - 1, cz)) {
                    header({x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, cx, cz, cx - 1, cz);
                    header({x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, cx - 1, cz, cx, cz);
                    B.grid(MAT_WALL, {x0 - hw, 2.45f, z0}, {WALL_T, 0, 0}, {0, 0, CELL}, 1, 1, {0, -1, 0}, floor_uv, [](Vec3) { return 0.8f; });
                }
                if (W.wall_z(cx, cz)) {
                    bool win = W.window_edge(cz, cx, false);
                    bface({x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, cx, cz, cx, cz - 1, win);
                    bface({x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, cx, cz - 1, cx, cz, win);
                    if (win) window_trim({x0 + CELL * 0.5f, 0, z0}, {1, 0, 0}, {0, 0, 1});
                } else if (!W.solid(cx, cz) && !W.solid(cx, cz - 1) && W.indoors(cx, cz) != W.indoors(cx, cz - 1)) {
                    header({x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, cx, cz, cx, cz - 1);
                    header({x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, cx, cz - 1, cx, cz);
                    B.grid(MAT_WALL, {x0, 2.45f, z0 + hw}, {CELL, 0, 0}, {0, 0, -WALL_T}, 1, 1, {0, -1, 0}, floor_uv, [](Vec3) { return 0.8f; });
                }
                if (W.post(cx, cz)) {
                    float ph = WH;
                    for (int a2 = -1; a2 <= 0; a2++)
                        for (int b2 = -1; b2 <= 0; b2++)
                            if (W.indoors(cx + a2, cz + b2)) ph = std::max(ph, facade_h(cx + a2, cz + b2));
                    B.box(MAT_BLOCK, x0, z0, hp, hp, 0, ph, btw, bth, 1);
                }
            }
            if (!bldg && W.wall_x(cx, cz)) {
                bool tall = low && !li.outdoor && W.exit_on_edge(cx, cz, true); // office: the exit's wall section is full height
                float h = tall ? H : WH;
                if (inside_face(cx, cz)) paper({x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, h);
                else B.vface(MAT_WALL, {x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, 0, h, wtw, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (!low) { wall_decal({x0 + hw, 0, z1}, {0, 0, -CELL}, {1, 0, 0}, h, cx * 3, cz * 3 + 1); wall_decal({x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, h, cx * 3 + 1, cz * 3); }
                if (inside_face(cx - 1, cz)) paper({x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, h);
                else B.vface(MAT_WALL, {x0 - hw, 0, z0}, {0, 0, CELL}, {-1, 0, 0}, 0, h, wtw, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (tall) { // ends of the tall section
                    B.vface(MAT_WALL, {x0 - hw, 0, z1}, {WALL_T, 0, 0}, {0, 0, 1}, WH, H, TEX_WALL, H, 1, 1, false);
                    B.vface(MAT_WALL, {x0 + hw, 0, z0}, {-WALL_T, 0, 0}, {0, 0, -1}, WH, H, TEX_WALL, H, 1, 1, false);
                } else if (low) cap(x0 - hw, z0, x0 + hw, z1);
            }
            if (!bldg && W.wall_z(cx, cz)) {
                bool tall = low && !li.outdoor && W.exit_on_edge(cx, cz, false);
                float h = tall ? H : WH;
                if (inside_face(cx, cz)) paper({x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, h);
                else B.vface(MAT_WALL, {x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, 0, h, wtw, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (!low) { wall_decal({x0, 0, z0 + hw}, {CELL, 0, 0}, {0, 0, 1}, h, cx * 3 + 2, cz * 3); wall_decal({x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, h, cx * 3, cz * 3 + 2); }
                if (inside_face(cx, cz - 1)) paper({x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, h);
                else B.vface(MAT_WALL, {x1, 0, z0 - hw}, {-CELL, 0, 0}, {0, 0, -1}, 0, h, wtw, h, 2 * D, (low && !tall ? 1 : 3) * D, false);
                if (tall) {
                    B.vface(MAT_WALL, {x0, 0, z0 - hw}, {0, 0, WALL_T}, {-1, 0, 0}, WH, H, TEX_WALL, H, 1, 1, false);
                    B.vface(MAT_WALL, {x1, 0, z0 + hw}, {0, 0, -WALL_T}, {1, 0, 0}, WH, H, TEX_WALL, H, 1, 1, false);
                } else if (low) cap(x0, z0 - hw, x1, z0 + hw);
            }
            if (id == LevelId::L37) { // the poolrooms: every wall meets the floor and the ceiling in a soft tiled curve
                Builder::UV uv{0, 0, 1, 1};
                const float R = 0.17f;
                // tall arched windows glowing in some of the walls (light from somewhere else)
                auto arch = [&](Vec3 c, Vec3 along, Vec3 n) {
                    const float hwid = 0.36f, y0 = 0.9f, y1 = 2.5f;
                    Vec3 o = c + n * 0.012f;
                    Vec3 a = o - along * hwid, b = o + along * hwid;
                    B.quad(MAT_LIGHT, {b.x, y0, b.z}, {a.x, y0, a.z}, {a.x, y1, a.z}, {b.x, y1, b.z}, 235, 1.0f, 0.97f, 0.86f);
                    B.quad(MAT_LIGHT, {b.x, y1, b.z}, {a.x, y1, a.z}, {a.x, y0, a.z}, {b.x, y0, b.z}, 235, 1.0f, 0.97f, 0.86f);
                    const int SEG = 6;
                    for (int k = 0; k < SEG; k++) {
                        float t0 = PI * k / SEG, t1 = PI * (k + 1) / SEG;
                        Vec3 p0 = o + along * (std::cos(t0) * hwid), p1 = o + along * (std::cos(t1) * hwid);
                        p0.y = y1 + std::sin(t0) * hwid; p1.y = y1 + std::sin(t1) * hwid;
                        Vec3 cc{o.x, y1, o.z};
                        B.quad(MAT_LIGHT, p0, p1, cc, cc, 235, 1.0f, 0.97f, 0.86f);
                        B.quad(MAT_LIGHT, p1, p0, cc, cc, 235, 1.0f, 0.97f, 0.86f);
                    }
                };
                if (W.wall_x(cx, cz) && hash01(W.seed(), cx, cz, 7501) < 0.3f) {
                    arch({x0 + hw, 0, z0 + CELL * 0.5f}, {0, 0, -1}, {1, 0, 0});
                    arch({x0 - hw, 0, z0 + CELL * 0.5f}, {0, 0, 1}, {-1, 0, 0});
                }
                if (W.wall_z(cx, cz) && hash01(W.seed(), cx, cz, 7502) < 0.3f) {
                    arch({x0 + CELL * 0.5f, 0, z0 + hw}, {1, 0, 0}, {0, 0, 1});
                    arch({x0 + CELL * 0.5f, 0, z0 - hw}, {-1, 0, 0}, {0, 0, -1});
                }
                if (W.wall_x(cx, cz))
                    for (int s2 = -1; s2 <= 1; s2 += 2) {
                        float x = x0 + s2 * hw;
                        B.tube(MAT_WALL, {x, 0, z0}, {x, 0, z1}, R, uv, 1.0f);
                        B.tube(MAT_WALL, {x, H, z0}, {x, H, z1}, R, uv, 1.0f);
                    }
                if (W.wall_z(cx, cz))
                    for (int s2 = -1; s2 <= 1; s2 += 2) {
                        float z = z0 + s2 * hw;
                        B.tube(MAT_WALL, {x0, 0, z}, {x1, 0, z}, R, uv, 1.0f);
                        B.tube(MAT_WALL, {x0, H, z}, {x1, H, z}, R, uv, 1.0f);
                    }
            }
            if (bldg) {
                // (walls, windows, headers and posts were drawn above)
            } else if (W.post(cx, cz)) {
                if (id == LevelId::L37) { // round posts, so the wall ends are round too
                    Builder::UV uv{0, 0, 1, 1};
                    B.cyl(MAT_WALL, x0, z0, hp + 0.03f, hp + 0.03f, 0, WH, uv, false, false, 1.0f);
                } else {
                    B.box(MAT_WALL, x0, z0, hp, hp, 0, WH, TEX_WALL, WH, 1);
                }
                if (low) cap(x0 - hp, z0 - hp, x0 + hp, z0 + hp);
            } else if (W.pillar(cx, cz)) {
                bool square = id == LevelId::L0 || id == LevelId::L6 || id == LevelId::LFun || id == LevelId::TSGFar || id == LevelId::L1 || id == LevelId::TheEnd;
                if (square) B.box(MAT_PROP, x0, z0, hl, hl, 0, H, TEX_WALL, H, 1);
                else { // round columns, with a base and a capital where it suits
                    float r = hl * 1.1f;
                    float ptop = cave ? ceiling_y(W, x0, z0) : H;
                    Builder::UV uv{0, 0, 2 * PI * r / TEX_WALL, 1.0f};
                    B.cyl(MAT_PROP, x0, z0, r, r, 0, ptop, uv, false, false, 1.25f);
                    if (id == LevelId::L33) { // the mall's white columns: a green band at the foot, a moulded capital
                        Builder::UV ring{0, 0.9f, 2 * PI * r / TEX_WALL, 1.0f};
                        B.set_tint(0x2e8a70);
                        B.cyl(MAT_PROP, x0, z0, r * 1.12f, r * 1.12f, 0, 0.9f, ring, true, false, 1.25f);
                        B.clear_tint();
                        B.cyl(MAT_PROP, x0, z0, r * 1.2f, r * 1.12f, 0.9f, 1.0f, ring, true, false, 1.25f);
                        B.cyl(MAT_PROP, x0, z0, r * 1.05f, r * 1.4f, 4.25f, 4.6f, ring, true, false, 1.25f);
                    }
                    if (id == LevelId::L51 || id == LevelId::L5 || id == LevelId::TSG1 || id == LevelId::L37) {
                        Builder::UV ring{0, 0.9f, 2 * PI * r / TEX_WALL, 1.0f};
                        B.cyl(MAT_PROP, x0, z0, r * 1.35f, r * 1.3f, 0, 0.22f, ring, true, false, 1.25f);
                        B.cyl(MAT_PROP, x0, z0, r * 1.05f, r * 1.4f, ptop - 0.35f, ptop - 0.08f, ring, false, false, 1.25f);
                        B.cyl(MAT_PROP, x0, z0, r * 1.45f, r * 1.45f, ptop - 0.08f, ptop, ring, false, true, 1.25f);
                    }
                }
            }

            if (tinted) B.clear_tint();
            // ----- Level 2: pipes along the tunnel walls
            bool along_x;
            if (W.pipe_run(cx, cz, along_x)) {
                auto pipe = [&](float px, float pz, float hx, float hz, float y0, float y1) { // round pipes now
                    float r = (y1 - y0) * 0.5f, yc = (y0 + y1) * 0.5f;
                    Vec3 a = hx > hz ? Vec3{px - hx, yc, pz} : Vec3{px, yc, pz - hz}, b2 = hx > hz ? Vec3{px + hx, yc, pz} : Vec3{px, yc, pz + hz};
                    B.tube(MAT_PROP, a, b2, r, {0, 0, 1, CELL / 0.6f}, 1.0f);
                    // a bracket holding it to the wall
                    B.box(MAT_PROP, (a.x + b2.x) * 0.5f, (a.z + b2.z) * 0.5f, hx > hz ? 0.03f : r + 0.04f, hx > hz ? r + 0.04f : 0.03f, y0 - 0.02f, y1 + 0.02f, 0.3f, 0.3f, 1);
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
            // ----- furniture and props (hotel rooms draw from their corner cell; props from their own)
            {
                Furn fs[MAX_FURN];
                bool owner = false;
                int nf = W.furniture(cx, cz, fs, &owner);
                for (int k = 0; owner && k < nf; k++) {
                    const Furn& f = fs[k];
                    if (f.kind == FurnKind::DoorLeaf && f.width > 0) { out.doors.push_back(f); continue; } // drawn live
                    if (World::searchable(f.kind)) out.containers.push_back(f);
                    size_t vb[MAT_COUNT];
                    for (int mm = 0; mm < MAT_COUNT; mm++) vb[mm] = B.v[mm].size();
                    B.furniture(fs[k], W.indoors(cx, cz) ? W.indoor_h(cx, cz) : (li.outdoor ? 2.6f : H));
                    float gy = id == LevelId::L94 ? W.floor_y((f.x0 + f.x1) * 0.5f, (f.z0 + f.z1) * 0.5f) : 0.0f;
                    if (gy != 0.0f) // Motion's hills: stand it on the slope
                        for (int mm = 0; mm < MAT_COUNT; mm++)
                            for (size_t q = vb[mm]; q < B.v[mm].size(); q++) B.v[mm][q].y += gy;
                    if (f.solid && f.y1 > 0.3f && f.kind != FurnKind::WallPiece && f.kind != FurnKind::DoorLeaf && f.kind != FurnKind::Headboard)
                        out.shadows.push_back({(f.x0 + f.x1) * 0.5f, (f.z0 + f.z1) * 0.5f, (f.x1 - f.x0) * 0.5f + 0.12f, (f.z1 - f.z0) * 0.5f + 0.12f, gy + 0.012f});
                    if (f.kind == FurnKind::Lantern || f.kind == FurnKind::FireBarrel) // their glow gets a halo too
                        out.lights.push_back({{(f.x0 + f.x1) * 0.5f, f.y1 + 0.1f, (f.z0 + f.z1) * 0.5f}, -1.0f, 0.6f, LightState::Flicker});
                    if (f.kind == FurnKind::Floodlight)
                        out.lights.push_back({{(f.x0 + f.x1) * 0.5f + f.face.x * 0.25f, f.y1, (f.z0 + f.z1) * 0.5f + f.face.z * 0.25f}, -1.0f, 0.9f, LightState::On});
                }
            }
            if (W.balloon(cx, cz)) {
                float jx = (float)(hash3(W.seed(), cx, cz, 501) % 100u) / 100.0f - 0.5f, jz = (float)(hash3(W.seed(), cx, cz, 502) % 100u) / 100.0f - 0.5f;
                out.balloons.push_back({x0 + CELL * (0.5f + jx * 0.6f), 1.9f + jx * 0.4f, z0 + CELL * (0.5f + jz * 0.6f)});
            }
        }
    }

    if (id == LevelId::TSGHope) { // L0ST H0P3: the walls wobble (a pure function of position: no seams between chunks)
        for (auto& vs : B.v)
            for (Vertex& v : vs) {
                float ox = std::sin(v.y * 1.7f + v.z * 0.9f) * 0.11f, oz = std::cos(v.y * 1.3f + v.x * 0.8f) * 0.11f;
                v.x += ox * std::min(1.0f, v.y);
                v.z += oz * std::min(1.0f, v.y);
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
