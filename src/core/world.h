// Backrooms Iris - the levels.
//
// A level is never stored. Every query is a pure function of (level, seed, grid position),
// so chunks generate in any order (on any thread), there are no seams, and the server answers
// "is there a wall here?" without building a mesh. Same seed + level = same world everywhere.
//
// Grid: cell (cx, cz) covers x in [cx*CELL, (cx+1)*CELL], z in [cz*CELL, (cz+1)*CELL].
// Thin walls live on cell edges, posts/pillars on grid points, solid blocks fill a cell.
#pragma once
#include <cstdint>

#include "config.h"
#include "math.h"

namespace iris {

enum class LevelId : uint8_t { L0, L1, L2, L4, L5, L6, L8, L9, L11, L37, LFun, COUNT };
enum class LightState : uint8_t { None, On, Dead, Flicker };

enum class ItemType : uint8_t {
    None,
    AlmondWater, EnergyDrink, MemoryJuice,            // drinks
    RoyalRation, LiquidPain, CashewWater, LuckyMilk,  // more from the wiki
    Firesalt,                                         // thrown: burns as a flare
    Flashlight, Crowbar,                              // tools / weapons (you carry one)
    COUNT
};
constexpr int ITEM_COUNT = (int)ItemType::COUNT;

struct ItemInfo {
    const char* name;
    bool tool;           // flashlight / crowbar: you own it or you don't
    const char* blurb;   // one line for the HUD and the sandbox menu
};
const ItemInfo& item_info(ItemType t);

struct LevelInfo {
    const char* code;      // "LEVEL 0"
    const char* name;      // "THE LOBBY"
    const char* number;    // "0", "37", "FUN"
    float ceil_h;          // floor to ceiling (indoor levels)
    float wall_h;          // height of thin walls (cubicles are lower than the ceiling)
    bool outdoor;          // no ceiling, sky instead
    bool dark;             // no ambient light: you see by your own little light radius
    float fog[3];
    float fog_density;     // exp2 fog on PC
    float fog_density_vita;
    float ambient[3];      // baked ambient light colour
    float light_color[3];
    float light_power;
    float light_radius;
    float light_y;         // height of the light sources
    float pillar_size;
    float sanity_drain;    // per second; negative = this level calms you down
    float item_rate;       // chance per open cell of an item
    LevelId next;          // where the exit door takes you
};

const LevelInfo& level_info(LevelId id);
const char* level_label(LevelId id);         // "LEVEL 9 - THE SUBURBS"
bool level_from_number(const char* s, LevelId& out); // "9", "37", "fun"

class World {
public:
    World(LevelId id = LevelId::L0, uint32_t seed = 1) : id_(id), seed_(seed) {}
    LevelId id() const { return id_; }
    uint32_t seed() const { return seed_; }
    const LevelInfo& info() const { return level_info(id_); }

    bool solid(int cx, int cz) const;        // cell completely filled (rock, building, room, crates, desk)
    float block_h(int cx, int cz) const;     // height of a solid cell's block
    bool wall_x(int gx, int cz) const;       // thin wall on line x = gx*CELL, z in [cz, cz+1] cells
    bool wall_z(int cx, int gz) const;       // thin wall on line z = gz*CELL, x in [cx, cx+1] cells
    bool post(int gx, int gz) const;         // corner post where thin walls meet
    bool pillar(int gx, int gz) const;       // free-standing pillar on a grid point
    bool pole(int cx, int cz) const;         // streetlight pole in the middle of this cell
    LightState light(int cx, int cz) const;  // light source in this cell
    int floor_mat(int cx, int cz) const;     // 0 main floor, 1 and 2 = alternates
    uint32_t tint(int cx, int cz) const;     // per-block colour variation (0xRRGGBB)
    bool balloon(int cx, int cz) const;      // Level Fun decorations
    bool pipe_run(int cx, int cz, bool& along_x) const; // Level 2: pipes along this corridor cell

    bool exit_cell(int cx, int cz) const;    // the exit door stands in this cell
    ItemType item(int cx, int cz) const;     // item lying in this cell (before anyone picked it up)
    Vec2 item_pos(int cx, int cz) const;
    static uint32_t item_id(int cx, int cz) { return ((uint32_t)(uint16_t)cx << 16) | (uint16_t)cz; }
    static void item_cell(uint32_t id, int& cx, int& cz) { cx = (int16_t)(id >> 16); cz = (int16_t)(id & 0xFFFF); }

    Vec3 spawn_point() const { return {CELL * 0.5f, 0.0f, CELL * 0.5f}; }

    // Navigation / line of sight
    bool open(int cx, int cz) const { return !solid(cx, cz); }
    bool can_step(int cx, int cz, int dx, int dz) const; // from cell to a 4-neighbour
    bool segment_blocked(Vec2 a, Vec2 b) const;          // low walls (cubicles) don't block sight
    float light_level(Vec3 p) const;         // ~0 (pitch black) .. 1+ (well lit), static lights only

    enum class District : uint8_t { Rooms, Hall, Maze, Corridors, COUNT };
    District district(int cx, int cz) const; // room-style levels (0, 6, Fun)

private:
    LevelId id_;
    uint32_t seed_;
    bool near_spawn(int cx, int cz, int r) const { return cx >= -r && cx <= r && cz >= -r && cz <= r; }
    bool rooms_level() const { return id_ == LevelId::L0 || id_ == LevelId::L6 || id_ == LevelId::LFun; }
    bool edge_wall_rooms(int line, int along, bool vertical) const;
    bool edge_wall_l1(int line, int along, bool vertical) const;
    bool edge_wall_l37(int line, int along, bool vertical) const;
    bool office_wall(int line, int along, bool vertical) const;
    bool exit_for_region(int rx, int rz, int& ex, int& ez) const;
};

} // namespace iris
