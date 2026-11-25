#include "collision.h"

namespace iris {
namespace {

struct Box { float x0, z0, x1, z1; };

void push_out(Vec2& p, float r, const Box& b) {
    float cx = clampf(p.x, b.x0, b.x1);
    float cz = clampf(p.z, b.z0, b.z1);
    float dx = p.x - cx, dz = p.z - cz;
    float d2 = dx * dx + dz * dz;
    if (d2 >= r * r) return;
    if (d2 > 1e-10f) {
        float d = std::sqrt(d2);
        float push = r - d;
        p.x += dx / d * push;
        p.z += dz / d * push;
    } else {
        // centre is inside the box: leave through the nearest side
        float l = p.x - b.x0, rr = b.x1 - p.x, t = p.z - b.z0, bb = b.z1 - p.z;
        float m = std::min(std::min(l, rr), std::min(t, bb));
        if (m == l) p.x = b.x0 - r;
        else if (m == rr) p.x = b.x1 + r;
        else if (m == t) p.z = b.z0 - r;
        else p.z = b.z1 + r;
    }
}

} // namespace

void collide_circle(const World& W, Vec2& pos, float r) {
    const float hw = WALL_T * 0.5f, hp = POST_T * 0.5f, hl = W.info().pillar_size * 0.5f;
    for (int pass = 0; pass < 3; pass++) {
        int cx0 = floor_to_int((pos.x - r) / CELL) - 1, cx1 = floor_to_int((pos.x + r) / CELL) + 1;
        int cz0 = floor_to_int((pos.z - r) / CELL) - 1, cz1 = floor_to_int((pos.z + r) / CELL) + 1;
        for (int gx = cx0; gx <= cx1; gx++) {
            for (int gz = cz0; gz <= cz1; gz++) {
                float x = gx * CELL, z = gz * CELL;
                if (W.solid(gx, gz)) push_out(pos, r, {x, z, x + CELL, z + CELL});
                if (W.wall_x(gx, gz)) push_out(pos, r, {x - hw, z, x + hw, z + CELL});
                if (W.wall_z(gx, gz)) push_out(pos, r, {x, z - hw, x + CELL, z + hw});
                if (W.post(gx, gz)) push_out(pos, r, {x - hp, z - hp, x + hp, z + hp});
                else if (W.pillar(gx, gz)) push_out(pos, r, {x - hl, z - hl, x + hl, z + hl});
                if (W.hotel()) { // guest-room furniture and doorway jambs
                    Furn f[MAX_FURN];
                    int n = W.furniture(gx, gz, f); // (every cell of a room repeats it: harmless)
                    for (int k = 0; k < n; k++)
                        if (f[k].solid) push_out(pos, r, {f[k].x0, f[k].z0, f[k].x1, f[k].z1});
                }
                if (W.pole(gx, gz)) { // streetlight pole in the middle of the cell
                    float mx = x + CELL * 0.5f, mz = z + CELL * 0.5f;
                    push_out(pos, r, {mx - 0.09f, mz - 0.09f, mx + 0.09f, mz + 0.09f});
                }
            }
        }
    }
}

} // namespace iris
