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
            case FurnKind::Shelf: {
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
        case LevelId::L0: case LevelId::L6: case LevelId::TSG: case LevelId::TSGBlack: return LOBBY;
        case LevelId::L4: case LevelId::L21: case LevelId::L33: case LevelId::L36: case LevelId::LRun: case LevelId::TSGHope: return OFFICE;
        case LevelId::L1: case LevelId::L2: case LevelId::L22: case LevelId::L8: return INDUSTRIAL;
        case LevelId::L5: case LevelId::TSGMansion: case LevelId::L7: case LevelId::L19: case LevelId::L13: return HOTEL;
        case LevelId::L3: return INDUSTRIAL;
        case LevelId::L10: return OUTSIDE;
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

    // light sources around the chunk
    int m = (int)(li.light_radius / CELL) + 1;
    for (int cz = c0z - m; cz < c0z + CHUNK_CELLS + m; cz++)
        for (int cx = c0x - m; cx < c0x + CHUNK_CELLS + m; cx++) {
            LightState s = W.light(cx, cz);
            if (s == LightState::On || s == LightState::Flicker)
                B.lights.push_back({{(cx + 0.5f) * CELL, li.light_y, (cz + 0.5f) * CELL}, li.light_power * (s == LightState::On ? 1.0f : 0.75f)});
            { // bedside lamps, camp lanterns and fire barrels light their surroundings a little
                Furn fs[MAX_FURN];
                bool owner = false;
                int nf = W.furniture(cx, cz, fs, &owner);
                for (int k = 0; owner && k < nf; k++) {
                    const Furn& f = fs[k];
                    if (f.kind == FurnKind::Lamp || f.kind == FurnKind::Lantern || f.kind == FurnKind::FireBarrel)
                        B.lights.push_back({{(f.x0 + f.x1) * 0.5f, f.kind == FurnKind::Lamp ? 0.95f : f.y1 + 0.2f, (f.z0 + f.z1) * 0.5f},
                                            f.kind == FurnKind::Lamp ? 0.55f : 0.75f});
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
                        float y = (id == LevelId::L1 ? li.light_y : (cave ? ceiling_y(W, cxm, czm) : H)) - 0.03f;
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
            // districts on the room-style levels: each part of the maze is a slightly different shade
            bool tinted = id == LevelId::L0 || id == LevelId::L6 || id == LevelId::LFun;
            if (tinted) {
                static const uint32_t DT[4] = {0xffffff, 0xeef5e2, 0xe2d6c0, 0xfff2d6};
                B.set_tint(DT[(int)W.district(cx, cz) & 3]);
            }
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
            } else if (W.pillar(cx, cz)) {
                bool square = id == LevelId::L0 || id == LevelId::L6 || id == LevelId::LFun || id == LevelId::TSG || id == LevelId::L1;
                if (square) B.box(MAT_PROP, x0, z0, hl, hl, 0, H, TEX_WALL, H, 1);
                else { // round columns, with a base and a capital where it suits
                    float r = hl * 1.1f;
                    float ptop = cave ? ceiling_y(W, x0, z0) : H;
                    Builder::UV uv{0, 0, 2 * PI * r / TEX_WALL, 1.0f};
                    B.cyl(MAT_PROP, x0, z0, r, r, 0, ptop, uv, false, false, 1.25f);
                    if (id == LevelId::L51 || id == LevelId::L5 || id == LevelId::TSGMansion || id == LevelId::L37) {
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
                    B.furniture(fs[k], H);
                    const Furn& f = fs[k];
                    if (f.kind == FurnKind::Lantern || f.kind == FurnKind::FireBarrel) // their glow gets a halo too
                        out.lights.push_back({{(f.x0 + f.x1) * 0.5f, f.y1 + 0.1f, (f.z0 + f.z1) * 0.5f}, -1.0f, 0.6f, LightState::Flicker});
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
