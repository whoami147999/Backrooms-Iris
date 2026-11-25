#include "world.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "hash.h"

namespace iris {
namespace {

enum Salt : uint32_t {
    S_DISTRICT = 11, S_LINE = 21, S_DOOR = 22, S_JITTER = 23, S_ORIENT = 31, S_PILLAR = 41,
    S_LIGHT = 51, S_LIGHTSTATE = 52, S_SOLID = 61, S_HEIGHT = 62, S_TINT = 63, S_LOT = 64,
    S_EXIT = 71, S_ITEM = 81, S_ITEMTYPE = 82, S_ITEMPOS = 83, S_NOISE = 91, S_BALLOON = 95, S_ROOM = 97,
};

// clang-format off
const LevelInfo LEVELS[(int)LevelId::COUNT] = {
    // code        name               num    ceil   wall   out    dark   fog                      dens    vita    ambient                  light colour            pow   rad   y      pillar drain  items   next
    {"LEVEL 0",   "THE LOBBY",        "0",   2.7f,  2.7f,  false, false, {0.24f, 0.21f, 0.12f},   0.055f, 0.070f, {0.17f, 0.16f, 0.13f},   {1.00f, 0.95f, 0.82f},  1.35f, 7.5f, 2.65f, 0.6f, 0.10f, 0.011f, LevelId::L1},
    {"LEVEL 1",   "HABITABLE ZONE",   "1",   4.2f,  4.2f,  false, false, {0.20f, 0.22f, 0.24f},   0.065f, 0.080f, {0.13f, 0.14f, 0.15f},   {0.92f, 0.96f, 1.00f},  1.50f, 9.0f, 3.80f, 1.0f, 0.12f, 0.022f, LevelId::L2},
    {"LEVEL 2",   "PIPE DREAMS",      "2",   2.5f,  2.5f,  false, false, {0.07f, 0.045f, 0.03f},  0.090f, 0.100f, {0.10f, 0.08f, 0.06f},   {1.00f, 0.55f, 0.25f},  1.25f, 6.5f, 2.35f, 0.6f, 0.20f, 0.016f, LevelId::L4},
    {"LEVEL 4",   "ABANDONED OFFICE", "4",   2.8f,  1.55f, false, false, {0.20f, 0.21f, 0.23f},   0.050f, 0.065f, {0.16f, 0.17f, 0.19f},   {0.95f, 0.98f, 1.00f},  1.30f, 7.5f, 2.75f, 0.6f, 0.06f, 0.018f, LevelId::L5},
    {"LEVEL 5",   "TERROR HOTEL",     "5",   2.9f,  2.9f,  false, false, {0.10f, 0.06f, 0.04f},   0.085f, 0.100f, {0.10f, 0.07f, 0.05f},   {1.00f, 0.72f, 0.42f},  1.30f, 6.5f, 2.30f, 0.6f, 0.20f, 0.014f, LevelId::L6},
    {"LEVEL 6",   "LIGHTS OUT",       "6",   2.7f,  2.7f,  false, true,  {0.00f, 0.00f, 0.00f},   0.240f, 0.260f, {0.30f, 0.30f, 0.32f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 2.65f, 0.6f, 0.40f, 0.012f, LevelId::L8},
    {"LEVEL 8",   "CAVE SYSTEM",      "8",   3.0f,  3.0f,  false, true,  {0.00f, 0.00f, 0.00f},   0.150f, 0.170f, {0.62f, 0.56f, 0.50f},   {0.45f, 1.00f, 0.70f},  0.90f, 6.0f, 1.20f, 0.6f, 0.35f, 0.012f, LevelId::L9},
    {"LEVEL 9",   "THE SUBURBS",      "9",   6.0f,  6.0f,  true,  false, {0.03f, 0.035f, 0.07f},  0.060f, 0.075f, {0.15f, 0.16f, 0.24f},   {1.00f, 0.66f, 0.30f},  1.60f, 11.f, 5.00f, 0.6f, 0.25f, 0.008f, LevelId::L37},
    {"LEVEL 11",  "THE ENDLESS CITY", "11",  60.0f, 60.0f, true,  false, {0.62f, 0.64f, 0.67f},   0.030f, 0.045f, {0.88f, 0.88f, 0.90f},   {1.00f, 1.00f, 1.00f},  0.00f, 1.0f, 3.00f, 0.6f, -0.60f, 0.006f, LevelId::L0},
    {"LEVEL 37",  "SUBLIMITY",        "37",  4.0f,  4.0f,  false, false, {0.70f, 0.82f, 0.86f},   0.035f, 0.050f, {0.55f, 0.62f, 0.66f},   {0.90f, 1.00f, 1.00f},  0.80f, 8.0f, 3.95f, 0.7f, -0.05f, 0.009f, LevelId::LFun},
    {"LEVEL FUN", "=)",               "FUN", 2.7f,  2.7f,  false, false, {0.30f, 0.20f, 0.24f},   0.060f, 0.075f, {0.22f, 0.18f, 0.19f},   {1.00f, 0.90f, 0.85f},  1.40f, 7.5f, 2.65f, 0.6f, 0.30f, 0.012f, LevelId::L11},
};

const ItemInfo ITEMS[ITEM_COUNT] = {
    {"NOTHING", false, ""},
    {"ALMOND WATER", false, "REFILLS STAMINA, CALMS YOU DOWN"},
    {"ENERGY DRINK", false, "FAST FOR A LITTLE WHILE"},
    {"MEMORY JUICE", false, "DON'T"},
    {"ROYAL RATION", false, "A REAL MEAL. HEALS YOU"},
    {"LIQUID PAIN", false, "HURTS. CLEARS YOUR HEAD"},
    {"CASHEW WATER", false, "CURES MEMORY JUICE. MAKES YOU SLOW"},
    {"LUCKY O' MILK", false, "THEY STOP NOTICING YOU FOR A BIT"},
    {"FIRESALT", false, "THROW IT: BURNS BRIGHT, KEEPS THEM AWAY"},
    {"FLASHLIGHT", true, "F / TRIANGLE: LIGHT. SMILERS HATE IT"},
    {"CROWBAR", true, "SWING TO STUN WHATEVER IS IN FRONT OF YOU"},
};
const DiffInfo DIFFS[(int)Difficulty::COUNT] = {
    // name        count  speed  dmg    sanity items  exits  grace  regen  batt   hearing
    {"EASY",      0.60f, 0.85f, 0.60f, 0.60f, 1.50f, 0.55f, 60.0f, 2.00f, 0.70f, 0.80f, "SLOWER ENTITIES, MORE ITEMS, MORE EXITS"},
    {"MEDIUM",    1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 0.36f, 35.0f, 1.00f, 1.00f, 1.00f, "THE BACKROOMS AS INTENDED"},
    {"HARD",      1.50f, 1.08f, 1.35f, 1.30f, 0.75f, 0.27f, 20.0f, 0.50f, 1.30f, 1.20f, "MORE OF THEM, FEWER EXITS, LESS TO DRINK"},
    {"NIGHTMARE", 2.00f, 1.18f, 1.80f, 1.70f, 0.50f, 0.20f, 8.0f,  0.00f, 1.60f, 1.40f, "NO HEALING. THEY HEAR EVERYTHING. GOOD LUCK"},
};
// clang-format on

float smooth(float t) { return t * t * (3 - 2 * t); }

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

struct Style { float line_prob, door_prob; int seg_len; float pillar_prob; };
const Style ROOM_STYLES[4] = {
    {0.50f, 0.34f, 3, 0.03f},  // Rooms
    {0.10f, 0.50f, 4, 0.32f},  // Hall
    {0.62f, 0.30f, 2, 0.00f},  // Maze
    {0.70f, 0.14f, 6, 0.00f},  // Corridors (along the corridor; cross walls use Hall)
};
constexpr int DISTRICT_CELLS = 10;
constexpr int EXIT_REGION = 12;
constexpr int PIPE_GRID = 3;     // Level 2 tunnels
constexpr int OFFICE_GRID = 6;   // Level 4 cubicle blocks
constexpr int HOTEL_GRID = 5;    // Level 5 corridors
constexpr int SUBURB_GRID = 8;   // Level 9 blocks
constexpr int CITY_GRID = 10;    // Level 11 blocks

// Level 4: each 6x6 block holds four 2x2 cubicles separated by aisles. Every cubicle
// has one opening (side + which of its two cells) and a desk in the opposite corner.
struct Cubicle { int side; int idx; }; // side: 0 west, 1 east, 2 north (-z), 3 south (+z)
Cubicle cubicle(uint32_t seed, int bx, int bz, int qx, int qz) {
    uint32_t h = hash3(seed, bx * 2 + qx, bz * 2 + qz, S_LOT);
    return {(int)(h % 4u), (int)((h >> 2) & 1u)};
}
// cell -> cubicle coordinates; false if the cell is an aisle
bool cell_cubicle(int c, int& b, int& q, int& l) {
    int m = pmod(c, OFFICE_GRID);
    if (m == 0 || m == 3) return false;
    b = floor_div(c, OFFICE_GRID);
    q = m >= 3 ? 1 : 0;
    l = m - (q ? 4 : 1);
    return true;
}


// Room-local frame: u runs along the door wall, v from the far wall (0) to the door wall (2 cells).
struct RoomFrame {
    float x0, z0, x1, z1;
    int side;
    float u_door;
    template <typename R> explicit RoomFrame(const R& r) : x0(r.x0), z0(r.z0), x1(r.x1), z1(r.z1), side(r.door_side) {
        float dx = (r.dcx + 0.5f) * CELL, dz = (r.dcz + 0.5f) * CELL;
        switch (side) {
            case 3: u_door = dx - x0; break;   // door on the south (+z) wall
            case 2: u_door = x1 - dx; break;   // north
            case 1: u_door = z1 - dz; break;   // east
            default: u_door = dz - z0; break;  // west
        }
    }
    Vec2 to_world(float u, float v) const {
        switch (side) {
            case 3: return {x0 + u, z0 + v};
            case 2: return {x1 - u, z1 - v};
            case 1: return {x0 + v, z1 - u};
            default: return {x1 - v, z0 + u};
        }
    }
    Vec2 dir(float du, float dv) const { // a direction in the local frame, in world axes
        Vec2 o = to_world(0, 0), p = to_world(du, dv);
        return p - o;
    }
};
} // namespace

const LevelInfo& level_info(LevelId id) { return LEVELS[(int)id < (int)LevelId::COUNT ? (int)id : 0]; }
const ItemInfo& item_info(ItemType t) { return ITEMS[(int)t < ITEM_COUNT ? (int)t : 0]; }
const DiffInfo& diff_info(Difficulty d) { return DIFFS[(int)d < (int)Difficulty::COUNT ? (int)d : 1]; }

bool difficulty_from_name(const char* s, Difficulty& out) {
    std::string v;
    for (const char* p = s; *p; p++) if (std::isalnum((unsigned char)*p)) v += (char)std::toupper((unsigned char)*p);
    for (int i = 0; i < (int)Difficulty::COUNT; i++)
        if (v == DIFFS[i].name || v == std::to_string(i)) { out = (Difficulty)i; return true; }
    if (v == "NORMAL") { out = Difficulty::Medium; return true; }
    return false;
}

const char* level_label(LevelId id) {
    static char buf[(int)LevelId::COUNT][48];
    char* b = buf[(int)id];
    if (!b[0]) {
        const LevelInfo& li = level_info(id);
        std::strncat(b, li.code, 47);
        std::strncat(b, " - ", 47 - std::strlen(b));
        std::strncat(b, li.name, 47 - std::strlen(b));
    }
    return b;
}

bool level_from_number(const char* s, LevelId& out) {
    std::string v;
    for (const char* p = s; *p; p++) if (std::isalnum((unsigned char)*p)) v += (char)std::toupper((unsigned char)*p);
    if (v.rfind("LEVEL", 0) == 0) v = v.substr(5);
    for (int i = 0; i < (int)LevelId::COUNT; i++)
        if (v == LEVELS[i].number) { out = (LevelId)i; return true; }
    return false;
}

// ------------------------------------------------------------------ room-style levels
World::District World::district(int cx, int cz) const {
    // offset by half a district so spawn (cell 0,0) sits in the middle of one
    int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
    if (dx == 0 && dz == 0) return District::Rooms;
    float r = hash01(seed_, dx, dz, S_DISTRICT);
    if (id_ == LevelId::LFun) return r < 0.75f ? District::Rooms : District::Hall; // party rooms
    if (r < 0.50f) return District::Rooms;
    if (r < 0.68f) return District::Hall;
    if (r < 0.84f) return District::Maze;
    return District::Corridors;
}

bool World::edge_wall_rooms(int line, int along, bool vertical) const {
    int cx = vertical ? line : along, cz = vertical ? along : line;
    District d = district(cx, cz);
    const Style* st = &ROOM_STYLES[(int)d];
    if (d == District::Corridors) {
        int dx = floor_div(cx + DISTRICT_CELLS / 2, DISTRICT_CELLS), dz = floor_div(cz + DISTRICT_CELLS / 2, DISTRICT_CELLS);
        bool corridor_vertical = hash3(seed_, dx, dz, S_ORIENT) & 1;
        if (corridor_vertical != vertical) st = &ROOM_STYLES[(int)District::Hall];
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

bool World::edge_wall_l37(int line, int along, bool vertical) const {
    // tiled partitions every 4 cells with wide archways: big airy pool halls
    if (pmod(line, 4) != 0) return false;
    uint32_t dir = vertical ? 0u : 1000u;
    int seg = floor_div(along, 3);
    if (hash01(seed_, line, seg, S_LINE + dir) >= 0.65f) return false;
    return hash01(seed_, line, along, S_DOOR + dir) >= 0.40f;
}

bool World::office_wall(int line, int along, bool vertical) const {
    int b, q, l;
    if (!cell_cubicle(along, b, q, l)) return false; // aisles have no walls across them
    int m = pmod(line, OFFICE_GRID);
    int ob, oq, side;
    if (m == 1) { ob = floor_div(line, OFFICE_GRID); oq = 0; side = 0; }
    else if (m == 3) { ob = floor_div(line, OFFICE_GRID); oq = 0; side = 1; }
    else if (m == 4) { ob = floor_div(line, OFFICE_GRID); oq = 1; side = 0; }
    else if (m == 0) { ob = floor_div(line, OFFICE_GRID) - 1; oq = 1; side = 1; }
    else return false;
    Cubicle c = vertical ? cubicle(seed_, ob, b, oq, q) : cubicle(seed_, b, ob, q, oq);
    int want = vertical ? side : side + 2;
    return !(c.side == want && c.idx == l);
}

// ------------------------------------------------------------------ geometry
bool World::solid(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: // stacks of supply crates scattered through the warehouse
            if (near_spawn(cx, cz, 2)) return false;
            return hash01(seed_, cx, cz, S_SOLID) < 0.05f;
        case LevelId::L2: { // maintenance tunnels in a 3-cell grid, the occasional boiler room
            int mx = pmod(cx, PIPE_GRID), mz = pmod(cz, PIPE_GRID);
            if (near_spawn(cx, cz, 1)) return false;
            if (mx == 0 || mz == 0) {
                if (mx == 0 && mz == 0) return false;
                return hash01(seed_, cx, cz, S_SOLID) < 0.05f; // dead ends
            }
            return hash01(seed_, floor_div(cx, PIPE_GRID), floor_div(cz, PIPE_GRID), S_LOT) >= 0.12f;
        }
        case LevelId::L4: { // a desk in the corner of every cubicle
            int bx, qx, lx, bz, qz, lz;
            if (!cell_cubicle(cx, bx, qx, lx) || !cell_cubicle(cz, bz, qz, lz)) return false;
            Cubicle c = cubicle(seed_, bx, bz, qx, qz);
            int ox = c.side == 0 ? 0 : c.side == 1 ? 1 : c.idx;
            int oz = c.side == 2 ? 0 : c.side == 3 ? 1 : c.idx;
            return lx == 1 - ox && lz == 1 - oz;
        }
        case LevelId::L5: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return false; // corridors stay clear: every room door opens onto one
            return hotel_block(floor_div(cx, HOTEL_GRID), floor_div(cz, HOTEL_GRID)) == 0;
        }
        case LevelId::L8: {
            if (near_spawn(cx, cz, 2)) return false;
            float n = fbm2(seed_, cx / 5.0f, cz / 5.0f, S_NOISE);
            float worm = fbm2(seed_, cx / 9.0f, cz / 9.0f, S_NOISE + 10);
            return n > 0.50f && std::fabs(worm - 0.5f) >= 0.05f; // winding tunnels link the caverns
        }
        case LevelId::L9: {
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx < 3 || mx > 6 || mz < 4 || mz > 5) return false;
            int lot = (mx - 3) / 2;
            return hash01(seed_, floor_div(cx, SUBURB_GRID) * 2 + lot, floor_div(cz, SUBURB_GRID), S_LOT) >= 0.15f;
        }
        case LevelId::L11: {
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            if (mx < 3 || mx > 8 || mz < 3 || mz > 8) return false;
            return hash01(seed_, floor_div(cx, CITY_GRID), floor_div(cz, CITY_GRID), S_LOT) >= 0.10f;
        }
        default: return false;
    }
}

float World::block_h(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: return 1.2f * (1 + (int)(hash3(seed_, cx, cz, S_HEIGHT) % 3u)); // 1-3 crates tall
        case LevelId::L4: return 0.76f;  // desk
        case LevelId::L9: return 4.6f;
        case LevelId::L11: {
            int qx = floor_div(cx - 3, 3), qz = floor_div(cz - 3, 3); // each 3x3 quarter is one building
            float r = hash01(seed_, qx, qz, S_HEIGHT);
            return 14.0f + r * r * 70.0f;
        }
        default: return info().ceil_h;
    }
}

bool World::wall_x(int gx, int cz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L6: case LevelId::LFun:
            if (near_spawn(gx, cz, 1) && gx != -1) return false; // keep the spawn room open
            return edge_wall_rooms(gx, cz, true);
        case LevelId::L1:
            if (near_spawn(gx, cz, 2)) return false;
            return edge_wall_l1(gx, cz, true);
        case LevelId::L4: return office_wall(gx, cz, true);
        case LevelId::L5: return hotel_wall(gx, cz, true);
        case LevelId::L37:
            if (near_spawn(gx, cz, 2)) return false;
            return edge_wall_l37(gx, cz, true);
        default: return false;
    }
}

bool World::wall_z(int cx, int gz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L6: case LevelId::LFun:
            if (near_spawn(cx, gz, 1) && gz != -1) return false;
            return edge_wall_rooms(gz, cx, false);
        case LevelId::L1:
            if (near_spawn(cx, gz, 2)) return false;
            return edge_wall_l1(gz, cx, false);
        case LevelId::L4: return office_wall(gz, cx, false);
        case LevelId::L5: return hotel_wall(gz, cx, false);
        case LevelId::L37:
            if (near_spawn(cx, gz, 2)) return false;
            return edge_wall_l37(gz, cx, false);
        default: return false;
    }
}

bool World::post(int gx, int gz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L1: case LevelId::L4: case LevelId::L5: case LevelId::L6: case LevelId::L37: case LevelId::LFun:
            return wall_x(gx, gz - 1) || wall_x(gx, gz) || wall_z(gx - 1, gz) || wall_z(gx, gz);
        default: return false;
    }
}

bool World::pillar(int gx, int gz) const {
    switch (id_) {
        case LevelId::L0: case LevelId::L6: case LevelId::LFun: {
            if (gx >= -1 && gx <= 2 && gz >= -1 && gz <= 2) return false;
            float p = ROOM_STYLES[(int)district(gx, gz)].pillar_prob;
            if (p <= 0 || hash01(seed_, gx, gz, S_PILLAR) >= p) return false;
            return !post(gx, gz);
        }
        case LevelId::L1: {
            if (pmod(gx, 3) != 0 || pmod(gz, 3) != 0) return false;
            if (hash01(seed_, gx, gz, S_PILLAR) < 0.12f) return false;
            for (int a = -1; a <= 0; a++)
                for (int b = -1; b <= 0; b++)
                    if (solid(gx + a, gz + b)) return false;
            return !post(gx, gz);
        }
        case LevelId::L5: {
            int mx = pmod(gx, HOTEL_GRID), mz = pmod(gz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return false;
            return hotel_block(floor_div(gx, HOTEL_GRID), floor_div(gz, HOTEL_GRID)) == 1 && ((mx + mz) % 2 == 0);
        }
        case LevelId::L37: { // rows of tiled columns
            if (pmod(gx, 2) != 0 || pmod(gz, 2) != 0 || near_spawn(gx, gz, 2)) return false;
            if (pmod(gx, 4) == 0 || pmod(gz, 4) == 0) return false;
            return !post(gx, gz) && hash01(seed_, gx, gz, S_PILLAR) < 0.7f;
        }
        default: return false;
    }
}

bool World::pole(int cx, int cz) const {
    return id_ == LevelId::L9 && light(cx, cz) != LightState::None;
}

LightState World::light(int cx, int cz) const {
    if (solid(cx, cz)) return LightState::None;
    float r = hash01(seed_, cx, cz, S_LIGHTSTATE);
    switch (id_) {
        case LevelId::L0: case LevelId::LFun: {
            District d = district(cx, cz);
            bool slot = d == District::Hall ? ((cx + cz) & 1) == 0 : ((cx & 1) == 0 && (cz & 1) == 0);
            if (!slot) return LightState::None;
            if (cx == 0 && cz == 0) return LightState::On;
            float dead = id_ == LevelId::LFun ? 0.03f : (d == District::Maze ? 0.30f : 0.09f);
            if (r < dead) return LightState::Dead;
            if (r < dead + 0.05f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L6: // every panel is dead. bring your own light.
            return ((cx & 1) == 0 && (cz & 1) == 0) ? LightState::Dead : LightState::None;
        case LevelId::L1: {
            if ((cx & 1) || (cz & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 14.0f, cz / 14.0f, S_LIGHT) < 0.33f) return LightState::Dead; // power out
            if (r < 0.10f) return LightState::Dead;
            if (r < 0.18f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L2: {
            if (((cx + cz) & 1) != 0) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (r < 0.30f) return LightState::Dead;
            if (r < 0.42f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L4: {
            if ((cx & 1) || (cz & 1)) return LightState::None;
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 12.0f, cz / 12.0f, S_LIGHT) < 0.25f) return LightState::Dead;
            if (r < 0.12f) return LightState::Dead;
            if (r < 0.18f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L5: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            bool corridor = mx == 0 || mz == 0;
            Room room;
            if (hotel_room(cx, cz, room)) { // one ceiling lamp per guest room, often dead
                RoomFrame f(room);
                Vec2 lp = f.to_world(f.u_door, CELL * 0.5f); // in the door's lane, by the far wall
                if (floor_to_int(lp.x / CELL) != cx || floor_to_int(lp.z / CELL) != cz) return LightState::None;
                if (r < 0.25f) return LightState::Dead;
                if (r < 0.37f) return LightState::Flicker;
                return LightState::On;
            }
            if (corridor) { if (((cx + cz) & 1) != 0) return LightState::None; }
            else if (mx != 2 || mz != 2) return LightState::None; // one chandelier per ballroom
            if (near_spawn(cx, cz, 1)) return LightState::On;
            if (fbm2(seed_, cx / 10.0f, cz / 10.0f, S_LIGHT) < 0.30f) return LightState::Dead;
            if (r < 0.15f) return LightState::Dead;
            if (r < 0.22f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L8:
            if (near_spawn(cx, cz, 0)) return LightState::On;
            return r < 0.035f ? LightState::On : LightState::None; // glowing fungus
        case LevelId::L9: {
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx != 2 || mz != 2) return LightState::None;
            if (near_spawn(cx, cz, 3)) return LightState::On;
            if (r < 0.45f) return LightState::Dead;
            if (r < 0.55f) return LightState::Flicker;
            return LightState::On;
        }
        case LevelId::L37:
            if ((cx & 1) || (cz & 1)) return LightState::None;
            return r < 0.03f ? LightState::Dead : LightState::On; // skylights
        default: return LightState::None;
    }
}

int World::floor_mat(int cx, int cz) const {
    switch (id_) {
        case LevelId::L1: return fbm2(seed_, cx / 3.0f, cz / 3.0f, S_NOISE + 20) > 0.66f ? 1 : 0; // standing water
        case LevelId::L2: { // metal grates at the junctions and in boiler rooms
            int mx = pmod(cx, PIPE_GRID), mz = pmod(cz, PIPE_GRID);
            return (mx == 0) == (mz == 0) ? 1 : 0;
        }
        case LevelId::L5: {
            int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
            if (mx == 0 || mz == 0) return 0;
            return guest_room(cx, cz) ? 2 : 1; // worn guest-room carpet / ballroom parquet
        }
        case LevelId::L9: {
            int mx = pmod(cx, SUBURB_GRID), mz = pmod(cz, SUBURB_GRID);
            if (mx < 2 || mz < 2) return 1;
            if (mx == 2 || mx == 7 || mz == 2 || mz == 7) return 2;
            return 0;
        }
        case LevelId::L11: {
            int mx = pmod(cx, CITY_GRID), mz = pmod(cz, CITY_GRID);
            return (mx < 2 || mz < 2) ? 1 : 2;
        }
        case LevelId::L37: return fbm2(seed_, cx / 4.0f, cz / 4.0f, S_NOISE + 30) > 0.62f ? 1 : 0; // deep pools
        default: return 0;
    }
}

uint32_t World::tint(int cx, int cz) const {
    switch (id_) {
        case LevelId::L9: {
            int lot = (pmod(cx, SUBURB_GRID) - 3) / 2;
            static const uint32_t PAINT[] = {0xc9c2b0, 0x9fb3b8, 0xb8a98c, 0xa3aa8c, 0xc4b4a8, 0x8f9aa6, 0xd2cbb8};
            return PAINT[hash3(seed_, floor_div(cx, SUBURB_GRID) * 2 + lot, floor_div(cz, SUBURB_GRID), S_TINT) % 7u];
        }
        case LevelId::L11: {
            int qx = floor_div(cx - 3, 3), qz = floor_div(cz - 3, 3);
            static const uint32_t STONE[] = {0xd8d4cc, 0xb9bcc0, 0xc8b8a4, 0x9ea4aa, 0xe2ddd2, 0xaaa196};
            return STONE[hash3(seed_, qx, qz, S_TINT) % 6u];
        }
        default: return 0xffffff;
    }
}

bool World::balloon(int cx, int cz) const {
    return id_ == LevelId::LFun && !solid(cx, cz) && !near_spawn(cx, cz, 0) && hash01(seed_, cx, cz, S_BALLOON) < 0.14f;
}

bool World::pipe_run(int cx, int cz, bool& along_x) const {
    if (id_ != LevelId::L2 || solid(cx, cz)) return false;
    int mx = pmod(cx, PIPE_GRID), mz = pmod(cz, PIPE_GRID);
    if (mz == 0 && mx != 0) { along_x = true; return true; }
    if (mx == 0 && mz != 0) { along_x = false; return true; }
    return false;
}

// ------------------------------------------------------------------ Level 5 guest rooms
// Each hotel block (4x4 cells between corridors) is solid (locked rooms: painted doors), a
// ballroom, or four enterable 2x2-cell guest rooms, each with one doorway onto a corridor.
int World::hotel_block(int bx, int bz) const {
    if (bx == 0 && bz == 0) return 2; // a room block right next to spawn: you'll find one
    float r = hash01(seed_, bx, bz, S_LOT);
    if (r < 0.12f) return 1;
    return r < 0.60f ? 2 : 0;
}

bool World::hotel_room(int cx, int cz, Room& r) const {
    if (id_ != LevelId::L5) return false;
    int mx = pmod(cx, HOTEL_GRID), mz = pmod(cz, HOTEL_GRID);
    if (mx == 0 || mz == 0) return false;
    r.bx = floor_div(cx, HOTEL_GRID);
    r.bz = floor_div(cz, HOTEL_GRID);
    if (hotel_block(r.bx, r.bz) != 2) return false;
    r.qx = mx <= 2 ? 0 : 1;
    r.qz = mz <= 2 ? 0 : 1;
    int c0x = r.bx * HOTEL_GRID + 1 + r.qx * 2, c0z = r.bz * HOTEL_GRID + 1 + r.qz * 2;
    r.x0 = c0x * CELL; r.x1 = r.x0 + 2 * CELL;
    r.z0 = c0z * CELL; r.z1 = r.z0 + 2 * CELL;
    uint32_t h = hash3(seed_, r.bx * 2 + r.qx, r.bz * 2 + r.qz, S_ROOM);
    bool vertical = h & 1;
    int idx = (int)((h >> 1) & 1u);
    if (vertical) {
        r.door_side = r.qx == 0 ? 0 : 1;
        r.dcx = r.qx == 0 ? c0x : c0x + 1;
        r.dcz = c0z + idx;
    } else {
        r.door_side = r.qz == 0 ? 2 : 3;
        r.dcz = r.qz == 0 ? c0z : c0z + 1;
        r.dcx = c0x + idx;
    }
    return true;
}

bool World::guest_room(int cx, int cz) const {
    Room r;
    return hotel_room(cx, cz, r);
}

bool World::hotel_wall(int line, int along, bool vertical) const {
    // cells on both sides of this edge
    int ax = vertical ? line - 1 : along, az = vertical ? along : line - 1;
    int bx = vertical ? line : along, bz = vertical ? along : line;
    Room ra, rb;
    bool ina = hotel_room(ax, az, ra), inb = hotel_room(bx, bz, rb);
    if (!ina && !inb) return false;
    if (ina && inb && ra.x0 == rb.x0 && ra.z0 == rb.z0) return false; // inside one room
    // a room's own doorway
    for (int k = 0; k < 2; k++) {
        const Room& r = k == 0 ? ra : rb;
        if (!(k == 0 ? ina : inb)) continue;
        if (vertical && r.door_side == 0 && line == r.dcx && along == r.dcz) return false;
        if (vertical && r.door_side == 1 && line == r.dcx + 1 && along == r.dcz) return false;
        if (!vertical && r.door_side == 2 && line == r.dcz && along == r.dcx) return false;
        if (!vertical && r.door_side == 3 && line == r.dcz + 1 && along == r.dcx) return false;
    }
    return true;
}

int World::furniture(int cx, int cz, Furn* out, bool* owner) const {
    Room room;
    if (!hotel_room(cx, cz, room)) return 0;
    RoomFrame f(room);
    if (owner) *owner = floor_to_int((room.x0 + 0.1f) / CELL) == cx && floor_to_int((room.z0 + 0.1f) / CELL) == cz;
    int n = 0;
    const float H = info().ceil_h, hw = WALL_T * 0.5f, R = 2 * CELL;
    auto add = [&](FurnKind k, float u0, float v0, float u1, float v1, float y0, float y1, bool solid) {
        if (n >= MAX_FURN) return;
        Vec2 a = f.to_world(u0, v0), b = f.to_world(u1, v1);
        Furn& p = out[n++];
        p.kind = k;
        p.x0 = std::min(a.x, b.x); p.x1 = std::max(a.x, b.x);
        p.z0 = std::min(a.z, b.z); p.z1 = std::max(a.z, b.z);
        p.y0 = y0; p.y1 = y1;
        p.solid = solid;
        p.face = f.dir(0, 1); // into the room from the far wall; overridden below where needed
    };
    uint32_t h = hash3(seed_, room.bx * 2 + room.qx, room.bz * 2 + room.qz, S_ROOM + 1);
    const float ud = f.u_door;
    // doorway: wall either side of a 1.1 m opening, a lintel over it, the door swung open
    add(FurnKind::WallPiece, ud - CELL * 0.5f, R - hw, ud - 0.55f, R + hw, 0, H, true);
    add(FurnKind::WallPiece, ud + 0.55f, R - hw, ud + CELL * 0.5f, R + hw, 0, H, true);
    add(FurnKind::Lintel, ud - 0.55f, R - hw, ud + 0.55f, R + hw, 2.2f, H, false);
    add(FurnKind::DoorLeaf, ud - 0.55f, R - 1.0f, ud - 0.50f, R - hw, 0, 2.15f, true);
    // the bed against the far wall, on the other side from the door
    float bc = ud < CELL ? 3.6f : 1.4f;
    add(FurnKind::Bed, bc - 0.8f, 0.14f, bc + 0.8f, 2.2f, 0, 0.55f, true);
    add(FurnKind::Headboard, bc - 0.85f, hw + 0.01f, bc + 0.85f, 0.14f, 0, 1.15f, true);
    add(FurnKind::Nightstand, bc - 1.3f, hw + 0.02f, bc - 0.86f, 0.5f, 0, 0.6f, true);
    add(FurnKind::Nightstand, bc + 0.86f, hw + 0.02f, bc + 1.3f, 0.5f, 0, 0.6f, true);
    if (h & 1) add(FurnKind::Lamp, bc - 1.2f, 0.15f, bc - 0.96f, 0.39f, 0.6f, 0.98f, false);
    add(FurnKind::Painting, bc - 0.45f, hw + 0.005f, bc + 0.45f, hw + 0.03f, 1.35f, 1.95f, false);
    out[n - 1].face = f.dir(0, 1);
    // a dresser on the side wall away from the bed, a wardrobe in the corner by the door
    float du0 = ud < CELL ? hw + 0.02f : R - hw - 0.55f;
    add(FurnKind::Dresser, du0, 1.0f, du0 + 0.53f, 2.3f, 0, 0.9f, true);
    out[n - 1].face = f.dir(ud < CELL ? 1.0f : -1.0f, 0);
    if (h & 2) {
        float wu = ud < CELL ? R - hw - 0.62f : hw + 0.02f;
        add(FurnKind::Wardrobe, wu, R - hw - 1.1f, wu + 0.6f, R - hw - 0.05f, 0, 2.0f, true);
        out[n - 1].face = f.dir(ud < CELL ? -1.0f : 1.0f, 0);
    }
    add(FurnKind::Rug, 0.9f, 2.45f, R - 0.9f, 3.9f, 0, 0.012f, false);
    return n;
}

// ------------------------------------------------------------------ exits + items
bool World::wall_face(int cx, int cz, int side, float* offset) const {
    static const int DX[4] = {-1, 1, 0, 0}, DZ[4] = {0, 0, -1, 1};
    bool thin = side == 0 ? wall_x(cx, cz) : side == 1 ? wall_x(cx + 1, cz) : side == 2 ? wall_z(cx, cz) : wall_z(cx, cz + 1);
    if (thin) {
        if (offset) *offset = WALL_T * 0.5f;
        return info().wall_h >= 2.3f || id_ == LevelId::L4; // office: the door's wall section goes up to the ceiling
    }
    int nx = cx + DX[side], nz = cz + DZ[side];
    if (id_ != LevelId::L1 && solid(nx, nz) && block_h(nx, nz) >= 2.3f) { // not on a stack of crates
        if (offset) *offset = 0;
        return true;
    }
    return false;
}

// One chance per 12x12-cell region (fewer on harder difficulties). The door is set into a
// real wall near a random spot in the region, so it has to be found, not spotted.
bool World::exit_for_region(int rx, int rz, ExitDoor& out) const {
    int keep_away = diff_ >= Difficulty::Hard ? 2 : 1;
    if (std::abs(rx) <= keep_away && std::abs(rz) <= keep_away) return false; // not right next to spawn
    if (hash01(seed_, rx, rz, S_EXIT) > diff().exits) return false;
    int ox = 2 + (int)(hash3(seed_, rx, rz, S_EXIT + 1) % 8u);
    int oz = 2 + (int)(hash3(seed_, rx, rz, S_EXIT + 2) % 8u);
    int first_side = (int)(hash3(seed_, rx, rz, S_EXIT + 3) % 4u);
    int bx = rx * EXIT_REGION - EXIT_REGION / 2, bz = rz * EXIT_REGION - EXIT_REGION / 2;
    for (int r = 0; r <= 4; r++)
        for (int dz = -r; dz <= r; dz++)
            for (int dx = -r; dx <= r; dx++) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                int cx = bx + ox + dx, cz = bz + oz + dz;
                if (solid(cx, cz) || pole(cx, cz)) continue;
                for (int k = 0; k < 4; k++) {
                    int side = (first_side + k) % 4;
                    float off;
                    if (!wall_face(cx, cz, side, &off)) continue;
                    out.cx = cx;
                    out.cz = cz;
                    out.side = side;
                    static const float NX[4] = {1, -1, 0, 0}, NZ[4] = {0, 0, 1, -1};
                    out.normal = {NX[side], NZ[side]};
                    float x = (cx + 0.5f) * CELL, z = (cz + 0.5f) * CELL;
                    if (side == 0) x = cx * CELL + off;
                    if (side == 1) x = (cx + 1) * CELL - off;
                    if (side == 2) z = cz * CELL + off;
                    if (side == 3) z = (cz + 1) * CELL - off;
                    out.pos = {x, z};
                    return true;
                }
            }
    return false;
}

bool World::exit_door(int cx, int cz, ExitDoor* out) const {
    int rx = floor_div(cx + EXIT_REGION / 2, EXIT_REGION), rz = floor_div(cz + EXIT_REGION / 2, EXIT_REGION);
    if (std::abs(rx) <= 1 && std::abs(rz) <= 1) return false;
    if (hash01(seed_, rx, rz, S_EXIT) > diff().exits) return false; // cheap early out
    ExitDoor d;
    if (!exit_for_region(rx, rz, d) || d.cx != cx || d.cz != cz) return false;
    if (out) *out = d;
    return true;
}

bool World::exit_on_edge(int gx, int gz, bool vertical) const {
    ExitDoor d;
    if (vertical) // line x = gx, cell row gz: the door is in cell (gx, gz) facing west or (gx-1, gz) facing east
        return (exit_door(gx, gz, &d) && d.side == 0) || (exit_door(gx - 1, gz, &d) && d.side == 1);
    return (exit_door(gx, gz, &d) && d.side == 2) || (exit_door(gx, gz - 1, &d) && d.side == 3);
}

ItemType World::item(int cx, int cz) const {
    if (near_spawn(cx, cz, 0) || solid(cx, cz) || exit_cell(cx, cz) || pole(cx, cz)) return ItemType::None;
    if (cx < -32000 || cx > 32000 || cz < -32000 || cz > 32000) return ItemType::None; // id range
    // a flashlight waits right next to spawn where you'll need one
    if (cx == 1 && cz == 1 && (id_ == LevelId::L0 || id_ == LevelId::L6 || id_ == LevelId::L8)) return ItemType::Flashlight;
    float rate = info().item_rate * diff().items;
    if (near_spawn(cx, cz, 6)) rate *= 2.5f;
    if (id_ == LevelId::L5 && guest_room(cx, cz)) rate *= 3.0f; // rooms are worth searching
    if (hash01(seed_, cx, cz, S_ITEM) >= rate) return ItemType::None;
    // weights per item, with a few level flavours
    float w[ITEM_COUNT] = {0, 30, 15, 6, 10, 6, 6, 6, 8, 5, 5};
    bool dark = info().dark || id_ == LevelId::L2;
    if (dark) w[(int)ItemType::Flashlight] *= 3, w[(int)ItemType::Firesalt] *= 2;
    if (id_ == LevelId::L1) w[(int)ItemType::RoyalRation] *= 2.5f, w[(int)ItemType::AlmondWater] *= 1.5f; // supply crates
    if (id_ == LevelId::L2) w[(int)ItemType::Crowbar] *= 2.5f;
    if (id_ == LevelId::LFun) w[(int)ItemType::MemoryJuice] *= 2.0f;
    float sum = 0;
    for (float x : w) sum += x;
    float t = hash01(seed_, cx, cz, S_ITEMTYPE) * sum;
    for (int i = 1; i < ITEM_COUNT; i++) {
        t -= w[i];
        if (t < 0) return (ItemType)i;
    }
    return ItemType::AlmondWater;
}

Vec2 World::item_pos(int cx, int cz) const {
    float jx = hash01(seed_, cx, cz, S_ITEMPOS) - 0.5f, jz = hash01(seed_, cx, cz, S_ITEMPOS + 1) - 0.5f;
    return {(cx + 0.5f + jx * 0.5f) * CELL, (cz + 0.5f + jz * 0.5f) * CELL};
}

// ------------------------------------------------------------------ navigation
bool World::can_step(int cx, int cz, int dx, int dz) const {
    if (solid(cx + dx, cz + dz)) return false;
    if (dx == 1) return !wall_x(cx + 1, cz);
    if (dx == -1) return !wall_x(cx, cz);
    if (dz == 1) return !wall_z(cx, cz + 1);
    if (dz == -1) return !wall_z(cx, cz);
    return true;
}

bool World::segment_blocked(Vec2 a, Vec2 b) const {
    // Amanatides-Woo walk through the grid: thin walls on every line crossed (unless they're
    // low enough to see over) and solid blocks taller than eye level in every cell entered.
    const bool walls_block = info().wall_h >= 1.7f;
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
            if (walls_block && wall_x(sx > 0 ? cx + 1 : cx, cz)) return true;
            cx += sx;
            tmx += tdx;
        } else {
            if (tmz > 1.0f) break;
            if (walls_block && wall_z(cx, sz > 0 ? cz + 1 : cz)) return true;
            cz += sz;
            tmz += tdz;
        }
        if (solid(cx, cz) && block_h(cx, cz) > 1.7f) return true;
    }
    return false;
}

float World::light_level(Vec3 p) const {
    const LevelInfo& li = info();
    float b = (li.ambient[0] + li.ambient[1] + li.ambient[2]) / 3.0f;
    if (li.dark) b *= 0.25f;
    if (li.light_power <= 0) return b;
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
