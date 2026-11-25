#include "world.h"

#include <algorithm>

#include "hash.h"

namespace iris {
namespace {

enum Salt : uint32_t {
    S_DISTRICT = 11, S_LINE = 21, S_DOOR = 22, S_JITTER = 23, S_ORIENT = 31, S_PILLAR = 41,
    S_LIGHT = 51, S_LIGHTSTATE = 52, S_SOLID = 61, S_HEIGHT = 62, S_TINT = 63, S_LOT = 64,
    S_EXIT = 71, S_ITEM = 81, S_ITEMTYPE = 82, S_ITEMPOS = 83, S_NOISE = 91,
};

const LevelInfo LEVELS[(int)LevelId::COUNT] = {
    // code       name                ceil  out    dark   fog                     dens   vita   ambient                light colour            pow   rad   y     pillar drain  items   next
    {"LEVEL 0",  "THE LOBBY",          2.7f, false, false, {0.24f, 0.21f, 0.12f}, 0.055f, 0.075f, {0.17f, 0.16f, 0.13f}, {1.00f, 0.95f, 0.82f}, 1.35f, 7.5f, 2.65f, 0.6f, 0.10f, 0.010f, LevelId::L1},
    {"LEVEL 1",  "HABITABLE ZONE",     4.2f, false, false, {0.20f, 0.22f, 0.24f}, 0.070f, 0.085f, {0.12f, 0.13f, 0.14f}, {0.92f, 0.96f, 1.00f}, 1.50f, 9.0f, 3.80f, 1.0f, 0.12f, 0.022f, LevelId::L5},
    {"LEVEL 5",  "TERROR HOTEL",       2.9f, false, false, {0.10f, 0.06f, 0.04f}, 0.085f, 0.100f, {0.10f, 0.07f, 0.05f}, {1.00f, 0.72f, 0.42f}, 1.30f, 6.5f, 2.30f, 0.6f, 0.20f, 0.014f, LevelId::L8},
    {"LEVEL 8",  "CAVE SYSTEM",        3.0f, false, true,  {0.00f, 0.00f, 0.00f}, 0.150f, 0.170f, {0.62f, 0.56f, 0.50f}, {0.45f, 1.00f, 0.70f}, 0.90f, 6.0f, 1.20f, 0.6f, 0.35f, 0.012f, LevelId::L9},
    {"LEVEL 9",  "THE SUBURBS",        6.0f, true,  false, {0.03f, 0.035f, 0.07f}, 0.060f, 0.075f, {0.15f, 0.16f, 0.24f}, {1.00f, 0.66f, 0.30f}, 1.60f, 11.f, 5.00f, 0.6f, 0.25f, 0.008f, LevelId::L11},
    {"LEVEL 11", "THE ENDLESS CITY",  60.0f, true,  false, {0.62f, 0.64f, 0.67f}, 0.030f, 0.045f, {0.88f, 0.88f, 0.90f}, {1.00f, 1.00f, 1.00f}, 0.00f, 1.0f, 3.00f, 0.6f, -0.60f, 0.006f, LevelId::L0},
};

float smooth(float t) { return t * t * (3 - 2 * t); }

// Smooth value noise over the integer lattice, ~[0,1).
float vnoise2(uint32_t seed, float x, float z, uint32_t salt) {
    int x0 = floor_to_int(x), z0 = floor_to_int(z);
    float fx = smooth(x - x0), fz = smooth(z - z0);
    float a = hash01(seed, x0, z0, salt), b = hash01(seed, x0 + 1, z0, salt);
    float c = hash01(seed, x0, z0 + 1, salt), d = hash01(seed, x0 + 1, z0 + 1, salt);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fz);
}

float fbm2(uint32_t seed, float x, float z, uint32_t salt) {
    return vnoise2(seed, x, z, salt) * 0.6f + vnoise2(seed, x * 2.1f, z * 2.1f, salt + 1) * 0.3f +
           vnoise2(seed, x * 4.3f, z * 4.3f, salt + 2) * 0.1f;
}

int pmod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }

// L0 district styles
struct Style { float line_prob, door_prob; int seg_len; float pillar_prob; };
const Style L0_STYLES[4] = {
    {0.50f, 0.34f, 3, 0.03f},  // Rooms
    {0.10f, 0.50f, 4, 0.32f},  // Hall
    {0.62f, 0.30f, 2, 0.00f},  // Maze
    {0.70f, 0.14f, 6, 0.00f},  // Corridors (along the corridor; cross walls use Hall)
};
constexpr int DISTRICT_CELLS = 10;
constexpr int EXIT_REGION = 12;

// L5: hotel corridors every 5 cells; L9 / L11: city blocks
constexpr int HOTEL_GRID = 5;
constexpr int SUBURB_GRID = 8;
constexpr int CITY_GRID = 10;

} // namespace

const LevelInfo& level_info(LevelId id) { return LEVELS[(int)id < (int)LevelId::COUNT ? (int)id : 0]; }

const char* level_label(LevelId id) {
    static char buf[(int)LevelId::COUNT][48];
    char* b = buf[(int)id];
    if (!b[0]) {
        const LevelInfo& li = level_info(id);
        int n = 0;
        for (const char* p = li.code; *p && n < 46; p++) b[n++] = *p;
        const char* sep = " - ";
        for (const char* p = sep; *p && n < 46; p++) b[n++] = *p;
        for (const char* p = li.name; *p && n < 46; p++) b[n++] = *p;
        b[n] = 0;
    }
    return b;
}

// ------------------------------------------------------------------ L0 helpers
World::District World::district(int cx, int cz) const {
    // offset by half a district so spawn (cell 0,0) sits in the middle of one
    int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
    if (dx == 0 && dz == 0) return District::Rooms;
    float r = hash01(seed_, dx, dz, S_DISTRICT);
    if (r < 0.50f) return District::Rooms;
    if (r < 0.68f) return District::Hall;
    if (r < 0.84f) return District::Maze;
    return District::Corridors;
}

bool World::edge_wall_l0(int line, int along, bool vertical) const {
    int cx = vertical ? line : along, cz = vertical ? along : line;
    District d = district(cx, cz);
    const Style* st = &L0_STYLES[(int)d];
    if (d == District::Corridors) {
        int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
        bool corridor_vertical = hash3(seed_, dx, dz, S_ORIENT) & 1;
        if (corridor_vertical != vertical) st = &L0_STYLES[(int)District::Hall];
    }
    uint32_t dir = vertical ? 0u : 1000u;
    int jitter = (int)(hash3(seed_, line, 0, S_JITTER + dir) % (uint32_t)st->seg_len);
    int seg = floor_div(along + jitter, st->seg_len);
    if (hash01(seed_, line, seg, S_LINE + dir) >= st->line_prob) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= st->door_prob;
}

bool World::edge_wall_l1(int line, int along, bool vertical) const {
    // long, sparse concrete partition walls in a huge warehouse
    uint32_t dir = vertical ? 0u : 1000u;
    int jitter = (int)(hash3(seed_, line, 0, S_JITTER + dir) % 6u);
    int seg = floor_div(along + jitter, 6);
    if (hash01(seed_, line, seg, S_LINE + dir) >= 0.22f) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= 0.22f;
}

// ------------------------------------------------------------------ geometry
bool World::solid(int cx, int cz) const {
    switch (id_) {
        case LevelId::L0: return false;
        case LevelId::L1: {
            // stacks of supply crates scattered through the warehouse
            if (near_spawn(cx, cz, 2)) return false;
            return hash01(seed_, cx, cz, S_SOLID) < 0.05f;
        }
        case LevelId::L5: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            if (mx == 0 || mz == 0) {
                // corridor; occasionally a dead end blocked by a room that juts out
                if (near_spawn(cx, cz, 2) || (mx == 0 && mz == 0)) return false;
                return hash01(seed_, cx, cz, S_SOLID) < 0.04f;
            }
            int bx = floor_div(cx, HOTEL_GRID), bz = floor_div(cz, HOTEL_GRID);
            if (hash01(seed_, bx, bz, S_LOT) < 0.12f) return false; // ballroom: whole block open
            return true; // guest rooms
        }
        case LevelId::L8: {
            if (near_spawn(cx, cz, 2)) return false;
            float n = fbm2(seed_, cx / 5.0f, cz / 5.0f, S_NOISE);
            float worm = fbm2(seed_, cx / 9.0f, cz / 9.0f, S_NOISE + 10);
            bool tunnel = std::fabs(worm - 0.5f) < 0.05f;   // winding tunnels link the caverns
            return n > 0.50f && !tunnel;
        }
        case LevelId::L9: {
            // per block: two houses, each 2x2 cells, with front lawns on the street sides
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx < 3 || mx > 6 || mz < 4 || mz > 5) return false;
            int lot = (mx - 3) / 2;
            int bx = floor_div(cx, SUBURB_GRID), bz = floor_div(cz, SUBURB_GRID);
            if (hash01(seed_, bx * 2 + lot, bz, S_LOT) < 0.15f) return false; // empty lot
            return true;
        }
        case LevelId::L11: {
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            if (mx < 3 || mx > 8 || mz < 3 || mz > 8) return false;
            int bx = floor_div(cx, CITY_GRID), bz = floor_div(cz, CITY_GRID);
            if (hash01(seed_, bx, bz, S_LOT) < 0.10f) return false; // plaza
            return true;
        }
        default: return false;
    }
}

float World::block_h(int cx, int cz) const {
    const LevelInfo& li = info();
    switch (id_) {
        case LevelId::L1: return 1.2f * (1 + (int)(hash3(seed_, cx, cz, S_HEIGHT) % 3u)); // 1-3 crates tall
        case LevelId::L9: return 4.6f;
        case LevelId::L11: {
            // each 3x3 quarter of a block is one building
            int qx = floor_div(cx - 3, 3), qz = floor_div(cz - 3, 3);
            float r = hash01(seed_, qx, qz, S_HEIGHT);
            return 14.0f + r * r * 70.0f;
        }
        default: return li.ceil_h;
    }
}

bool World::wall_x(int gx, int cz) const {
    switch (id_) {
        case LevelId::L0:
            if (near_spawn(gx, cz, 1) && gx != -1) return false; // keep the spawn room open
            return edge_wall_l0(gx, cz, true);
        case LevelId::L1:
            if (near_spawn(gx, cz, 2)) return false;
            return edge_wall_l1(gx, cz, true);
        default: return false;
    }
}

bool World::wall_z(int cx, int gz) const {
    switch (id_) {
        case LevelId::L0:
            if (near_spawn(cx, gz, 1) && gz != -1) return false;
            return edge_wall_l0(gz, cx, false);
        case LevelId::L1:
            if (near_spawn(cx, gz, 2)) return false;
            return edge_wall_l1(gz, cx, false);
        default: return false;
    }
}

bool World::post(int gx, int gz) const {
    if (id_ != LevelId::L0 && id_ != LevelId::L1) return false;
    return wall_x(gx, gz - 1) || wall_x(gx, gz) || wall_z(gx - 1, gz) || wall_z(gx, gz);
}

bool World::pillar(int gx, int gz) const {
    switch (id_) {
        case LevelId::L0: {
            if (gx >= -1 && gx <= 2 && gz >= -1 && gz <= 2) return false;
            float p = L0_STYLES[(int)district(gx, gz)].pillar_prob;
            if (p <= 0 || hash01(seed_, gx, gz, S_PILLAR) >= p) return false;
            return !post(gx, gz);
        }
        case LevelId::L1: {
            // the warehouse's structural columns: a regular grid, a few missing
            if (pmod(gx, 3) != 0 || pmod(gz, 3) != 0) return false;
            if (hash01(seed_, gx, gz, S_PILLAR) < 0.12f) return false;
            // don't put a column inside crates
            for (int a = -1; a <= 0; a++)
                for (int b = -1; b <= 0; b++)
                    if (solid(gx + a, gz + b)) return false;
            return !post(gx, gz);
        }
        case LevelId::L5: {
            // columns in the ballrooms
            int bx = floor_div(gx, HOTEL_GRID), bz = floor_div(gz, HOTEL_GRID);
            int mx = pmod(gx, HOTEL_GRID), mz = pmod(gz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return false;
            return hash01(seed_, bx, bz, S_LOT) < 0.12f && ((mx + mz) % 2 == 0);
        }
        default: return false;
    }
}

LightState World::light(int cx, int cz) const {
    if (solid(cx, cz)) return LightState::None;
    float r = hash01(seed_, cx, cz, S_LIGHTSTATE);
    switch (id_) {
        case LevelId::L0: {
            District d = district(cx, cz);
            bool slot = d == District::Hall ? ((cx + cz) & 1) == 0 : ((cx & 1) == 0 && (cz & 1) == 0);
            if (!slot) return LightState::None;
            if (cx == 0 && cz == 0) return LightState::On;
            float dead = d == District::Maze ? 0.30f : 0.09f;
            if (r < dead) return LightState::Dead;
            if (r < dead + 0.05f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L1: {
            if ((cx & 1) || (cz & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            // whole stretches of the warehouse lose power
            if (fbm2(seed_, cx / 14.0f, cz / 14.0f, S_LIGHT) < 0.33f) return LightState::Dead;
            if (r < 0.10f) return LightState::Dead;
            if (r < 0.18f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L5: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            bool corridor = mx == 0 || mz == 0;
            if (corridor) {
                // a sconce every other corridor cell
                if (((cx + cz) & 1) != 0) return LightState::None;
            } else if (mx != 2 || mz != 2) {
                return LightState::None; // one chandelier per ballroom
            }
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 10.0f, cz / 10.0f, S_LIGHT) < 0.30f) return LightState::Dead; // dark wings
            if (r < 0.15f) return LightState::Dead;
            if (r < 0.22f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L8:
            // clusters of glowing fungus are the only light down here
            if (near_spawn(cx, cz, 0)) return LightState::On;
            return r < 0.035f ? LightState::On : LightState::None;
        case LevelId::L9: {
            // streetlights on the sidewalk corners, about half of them working
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx != 2 || mz != 2) return LightState::None;
            if (near_spawn(cx, cz, 3)) return LightState::On;
            if (r < 0.45f) return LightState::Dead;
            if (r < 0.55f) return LightState::Flicker;
            return LightState::On;
        }
        default: return LightState::None;
    }
}

int World::floor_mat(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: // standing water on the concrete
            return fbm2(seed_, cx / 3.0f, cz / 3.0f, S_NOISE + 20) > 0.66f ? 1 : 0;
        case LevelId::L5: { // corridor runner vs ballroom parquet
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            return (mx == 0 || mz == 0) ? 0 : 1;
        }
        case LevelId::L9: {
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx < 2 || mz < 2) return 1;            // road
            if (mx == 2 || mx == 7 || mz == 2 || mz == 7) return 2; // sidewalk
            return 0;                                   // lawn
        }
        case LevelId::L11: {
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            if (mx < 2 || mz < 2) return 1;  // road
            return 2;                         // sidewalk / plaza
        }
        default: return 0;
    }
}

uint32_t World::tint(int cx, int cz) const {
    switch (id_) {
        case LevelId::L9: {
            // every house gets its own faded paint colour
            int mx = pmod(cx, SUBURB_GRID);
            int lot = (mx - 3) / 2;
            int bx = floor_div(cx, SUBURB_GRID), bz = floor_div(cz, SUBURB_GRID);
            static const uint32_t PAINT[] = {0xc9c2b0, 0x9fb3b8, 0xb8a98c, 0xa3aa8c, 0xc4b4a8, 0x8f9aa6, 0xd2cbb8};
            return PAINT[hash3(seed_, bx * 2 + lot, bz, S_TINT) % 7u];
        }
        case LevelId::L11: {
            int qx = floor_div(cx - 3, 3), qz = floor_div(cz - 3, 3);
            static const uint32_t STONE[] = {0xd8d4cc, 0xb9bcc0, 0xc8b8a4, 0x9ea4aa, 0xe2ddd2, 0xaaa196};
            return STONE[hash3(seed_, qx, qz, S_TINT) % 6u];
        }
        default: return 0xffffff;
    }
}

int World::region_style(int cx, int cz) const {
    if (id_ == LevelId::L0) return (int)district(cx, cz);
    return 0;
}

// ------------------------------------------------------------------ exits + items
bool World::exit_for_region(int rx, int rz, int& ex, int& ez) const {
    if (rx >= -1 && rx <= 1 && rz >= -1 && rz <= 1) return false; // not right next to spawn
    // probability per region keeps exits rare enough to be a hunt
    if (hash01(seed_, rx, rz, S_EXIT) > 0.38f) return false;
    int ox = 2 + (int)(hash3(seed_, rx, rz, S_EXIT + 1) % 8u);
    int oz = 2 + (int)(hash3(seed_, rx, rz, S_EXIT + 2) % 8u);
    int bx = rx * EXIT_REGION - EXIT_REGION / 2, bz = rz * EXIT_REGION - EXIT_REGION / 2;
    // spiral out from the chosen spot to the nearest open cell
    for (int r = 0; r <= 3; r++)
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                int cx = bx + ox + dx, cz = bz + oz + dz;
                if (!solid(cx, cz)) { ex = cx; ez = cz; return true; }
            }
    return false;
}

bool World::exit_cell(int cx, int cz) const {
    int rx = floor_div(cx + EXIT_REGION / 2, EXIT_REGION), rz = floor_div(cz + EXIT_REGION / 2, EXIT_REGION);
    int ex, ez;
    return exit_for_region(rx, rz, ex, ez) && ex == cx && ez == cz;
}

ItemType World::item(int cx, int cz) const {
    if (near_spawn(cx, cz, 0) || solid(cx, cz) || exit_cell(cx, cz)) return ItemType::None;
    if (cx < -32000 || cx > 32000 || cz < -32000 || cz > 32000) return ItemType::None; // id range
    float rate = info().item_rate;
    // a few items close to spawn so a run never starts empty-handed
    if (near_spawn(cx, cz, 6)) rate *= 2.5f;
    if (hash01(seed_, cx, cz, S_ITEM) >= rate) return ItemType::None;
    float t = hash01(seed_, cx, cz, S_ITEMTYPE);
    if (t < 0.55f) return ItemType::AlmondWater;
    if (t < 0.85f) return ItemType::EnergyDrink;
    return ItemType::MemoryJuice;
}

Vec2 World::item_pos(int cx, int cz) const {
    float jx = hash01(seed_, cx, cz, S_ITEMPOS) - 0.5f, jz = hash01(seed_, cx, cz, S_ITEMPOS + 1) - 0.5f;
    return {(cx + 0.5f + jx * 0.5f) * CELL, (cz + 0.5f + jz * 0.5f) * CELL};
}

// ------------------------------------------------------------------ navigation
bool World::can_step(int cx, int cz, int dx, int dz) const {
    int nx = cx + dx, nz = cz + dz;
    if (solid(nx, nz)) return false;
    if (dx == 1) return !wall_x(cx + 1, cz);
    if (dx == -1) return !wall_x(cx, cz);
    if (dz == 1) return !wall_z(cx, cz + 1);
    if (dz == -1) return !wall_z(cx, cz);
    return true;
}

bool World::segment_blocked(Vec2 a, Vec2 b) const {
    // Amanatides-Woo walk through the grid: check thin walls on every line crossed and
    // solid blocks in every cell entered.
    float x = a.x / CELL, z = a.z / CELL, tx = b.x / CELL, tz = b.z / CELL;
    int cx = floor_to_int(x), cz = floor_to_int(z);
    int ex = floor_to_int(tx), ez = floor_to_int(tz);
    float dx = tx - x, dz = tz - z;
    int sx = dx > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    float tdx = dx != 0 ? std::fabs(1.0f / dx) : 1e30f, tdz = dz != 0 ? std::fabs(1.0f / dz) : 1e30f;
    float tmx = dx != 0 ? ((sx > 0 ? (cx + 1 - x) : (x - cx)) * tdx) : 1e30f;
    float tmz = dz != 0 ? ((sz > 0 ? (cz + 1 - z) : (z - cz)) * tdz) : 1e30f;
    for (int guard = 0; guard < 512 && (cx != ex || cz != ez); guard++) {
        if (tmx < tmz) {
            if (tmx > 1.0f) break;
            int line = sx > 0 ? cx + 1 : cx;
            if (wall_x(line, cz)) return true;
            cx += sx;
            tmx += tdx;
        } else {
            if (tmz > 1.0f) break;
            int line = sz > 0 ? cz + 1 : cz;
            if (wall_z(cx, line)) return true;
            cz += sz;
            tmz += tdz;
        }
        if (solid(cx, cz)) return true;
    }
    return false;
}

float World::light_level(Vec3 p) const {
    const LevelInfo& li = info();
    float b = (li.ambient[0] + li.ambient[1] + li.ambient[2]) / 3.0f;
    if (li.dark) b *= 0.25f;
    int pcx = floor_to_int(p.x / CELL), pcz = floor_to_int(p.z / CELL);
    int r = (int)(li.light_radius / CELL) + 1;
    for (int cz = pcz - r; cz <= pcz + r; cz++)
        for (int cx = pcx - r; cx <= pcx + r; cx++) {
            LightState s = light(cx, cz);
            if (s != LightState::On && s != LightState::Flicker) continue;
            Vec2 lp{(cx + 0.5f) * CELL, (cz + 0.5f) * CELL};
            float d = (lp - Vec2{p.x, p.z}).len();
            if (d >= li.light_radius) continue;
            float att = 1.0f - d / li.light_radius;
            if (segment_blocked({p.x, p.z}, lp)) att *= 0.1f;
            b += att * att * li.light_power;
        }
    return b;
}

} // namespace iris
