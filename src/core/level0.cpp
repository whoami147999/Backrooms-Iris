#include "level0.h"

#include <algorithm>

#include "config.h"
#include "hash.h"

namespace iris {
namespace {

constexpr int DISTRICT_CELLS = 10;

enum Salt : uint32_t {
    S_DISTRICT = 11, S_LINE = 21, S_DOOR = 22, S_JITTER = 23, S_ORIENT = 31,
    S_PILLAR = 41, S_LIGHT = 51, S_LIGHTSTATE = 52,
};

struct Style {
    float line_prob;   // chance a wall run exists on a grid-line segment
    float door_prob;   // chance a single cell of a run is left open (a doorway)
    int seg_len;       // length of wall runs, in cells
    float pillar_prob; // chance of a pillar on a free grid point
};

// Wall density stays well below the bond-percolation threshold (~50% walls) so the level
// remains one huge connected space: you can always keep walking.
const Style STYLES[(int)District::COUNT] = {
    /* Rooms     */ {0.50f, 0.34f, 3, 0.03f},
    /* Hall      */ {0.10f, 0.50f, 4, 0.32f},
    /* Maze      */ {0.62f, 0.30f, 2, 0.00f},
    /* Corridors */ {0.70f, 0.14f, 6, 0.00f}, // along the corridor; cross walls use Rooms
};

} // namespace

District Level0::district(int cx, int cz) const {
    // offset by half a district so spawn (cell 0,0) sits in the middle of one
    int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
    if (dx == 0 && dz == 0) return District::Rooms; // spawn district is always classic rooms
    float r = hash01(seed_, dx, dz, S_DISTRICT);
    if (r < 0.50f) return District::Rooms;
    if (r < 0.68f) return District::Hall;
    if (r < 0.84f) return District::Maze;
    return District::Corridors;
}

bool Level0::edge_wall(int line, int along, bool vertical, District d) const {
    const Style* st = &STYLES[(int)d];
    if (d == District::Corridors) {
        // corridors run in one direction per district; walls across them are sparse
        int dx = floor_div((vertical ? line : along) + DISTRICT_CELLS / 2, DISTRICT_CELLS);
        int dz = floor_div((vertical ? along : line) + DISTRICT_CELLS / 2, DISTRICT_CELLS);
        bool corridor_vertical = hash3(seed_, dx, dz, S_ORIENT) & 1;
        if (corridor_vertical != vertical) st = &STYLES[(int)District::Hall];
    }
    uint32_t dir = vertical ? 0u : 1000u;
    int jitter = (int)(hash3(seed_, line, 0, S_JITTER + dir) % (uint32_t)st->seg_len);
    int seg = floor_div(along + jitter, st->seg_len);
    if (hash01(seed_, line, seg, S_LINE + dir) >= st->line_prob) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= st->door_prob;
}

bool Level0::wall_x(int gx, int cz) const {
    if (in_spawn_zone(gx, cz) && gx != -1) return false; // keep the spawn room open
    return edge_wall(gx, cz, true, district(gx, cz));
}

bool Level0::wall_z(int cx, int gz) const {
    if (in_spawn_zone(cx, gz) && gz != -1) return false;
    return edge_wall(gz, cx, false, district(cx, gz));
}

bool Level0::post(int gx, int gz) const {
    return wall_x(gx, gz - 1) || wall_x(gx, gz) || wall_z(gx - 1, gz) || wall_z(gx, gz);
}

bool Level0::pillar(int gx, int gz) const {
    if (gx >= -1 && gx <= 2 && gz >= -1 && gz <= 2) return false;
    float p = STYLES[(int)district(gx, gz)].pillar_prob;
    if (p <= 0 || hash01(seed_, gx, gz, S_PILLAR) >= p) return false;
    return !post(gx, gz);
}

LightState Level0::light(int cx, int cz) const {
    // Panels sit on a regular 2x2-cell grid like a real office drop ceiling. Halls get
    // a denser grid; mazes are dimmer. Some panels are dead, some flicker.
    District d = district(cx, cz);
    bool slot;
    if (d == District::Hall) slot = ((cx + cz) & 1) == 0;
    else slot = (cx & 1) == 0 && (cz & 1) == 0;
    if (!slot) return LightState::None;
    if (cx == 0 && cz == 0) return LightState::On;
    float r = hash01(seed_, cx, cz, S_LIGHTSTATE);
    float dead = d == District::Maze ? 0.30f : 0.09f;
    if (r < dead) return LightState::Dead;
    if (r < dead + 0.05f) return LightState::Flicker;
    return LightState::On;
}

bool Level0::segment_blocked(Vec2 a, Vec2 b) const {
    // crossings of vertical grid lines x = gx*CELL
    if (std::fabs(b.x - a.x) > 1e-6f) {
        int g0 = floor_to_int(std::min(a.x, b.x) / CELL) + 1;
        int g1 = floor_to_int(std::max(a.x, b.x) / CELL);
        for (int gx = g0; gx <= g1; gx++) {
            float t = (gx * CELL - a.x) / (b.x - a.x);
            if (t <= 0 || t >= 1) continue;
            float z = a.z + (b.z - a.z) * t;
            if (wall_x(gx, floor_to_int(z / CELL))) return true;
        }
    }
    if (std::fabs(b.z - a.z) > 1e-6f) {
        int g0 = floor_to_int(std::min(a.z, b.z) / CELL) + 1;
        int g1 = floor_to_int(std::max(a.z, b.z) / CELL);
        for (int gz = g0; gz <= g1; gz++) {
            float t = (gz * CELL - a.z) / (b.z - a.z);
            if (t <= 0 || t >= 1) continue;
            float x = a.x + (b.x - a.x) * t;
            if (wall_z(floor_to_int(x / CELL), gz)) return true;
        }
    }
    return false;
}

} // namespace iris
