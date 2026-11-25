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

enum class Difficulty : uint8_t { Easy, Medium, Hard, Nightmare, COUNT };
struct DiffInfo {
    const char* name;
    float ent_count;     // entity population multiplier
    float ent_speed;     // entity movement speed multiplier
    float damage;        // damage taken multiplier
    float sanity;        // sanity drain multiplier
    float items;         // item spawn rate multiplier
    float exits;         // chance that a 12x12 region has an exit door in it
    float grace;         // seconds on a new level before anything spawns
    float regen;         // health regeneration multiplier
    float battery;       // flashlight drain multiplier
    float hearing;       // how far entities hear and see you
    const char* blurb;
};
const DiffInfo& diff_info(Difficulty d);
bool difficulty_from_name(const char* s, Difficulty& out); // "easy", "nightmare", "2"

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

// An exit door, set into a wall (or the face of a solid block) on one side of a cell.
struct ExitDoor {
    int cx = 0, cz = 0;
    int side = 0;        // 0 west (-x), 1 east (+x), 2 north (-z), 3 south (+z) edge of the cell
    Vec2 pos;            // centre of the door, on the wall surface, floor level
    Vec2 normal;         // points out of the wall, into the cell
};

// Furniture and doorway pieces inside a room (Level 5 guest rooms). Boxes are axis aligned,
// in world metres. `solid` pieces collide.
enum class FurnKind : uint8_t { WallPiece, Lintel, DoorLeaf, Bed, Headboard, Nightstand, Lamp, Dresser, Wardrobe, Painting, Rug };
struct Furn {
    FurnKind kind;
    float x0, z0, x1, z1, y0, y1;
    bool solid;
    Vec2 face;           // which way the piece faces (into the room): drawers, paintings
};
constexpr int MAX_FURN = 16;

const LevelInfo& level_info(LevelId id);
const char* level_label(LevelId id);         // "LEVEL 9 - THE SUBURBS"
bool level_from_number(const char* s, LevelId& out); // "9", "37", "fun"

class World {
public:
    World(LevelId id = LevelId::L0, uint32_t seed = 1, Difficulty d = Difficulty::Medium) : id_(id), seed_(seed), diff_(d) {}
    LevelId id() const { return id_; }
    uint32_t seed() const { return seed_; }
    Difficulty difficulty() const { return diff_; }
    const DiffInfo& diff() const { return diff_info(diff_); }
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
    // Furniture of the room this cell belongs to (every cell of a room returns the whole room).
    // `owner` is true for the one cell that should draw it.
    int furniture(int cx, int cz, Furn* out, bool* owner = nullptr) const;
    bool guest_room(int cx, int cz) const;   // Level 5: this cell is inside an enterable room

    bool exit_door(int cx, int cz, ExitDoor* out = nullptr) const; // an exit door opens into this cell
    bool exit_cell(int cx, int cz) const { return exit_door(cx, cz); }
    bool exit_on_edge(int gx, int gz, bool vertical) const; // the door sits on this thin wall
    bool wall_face(int cx, int cz, int side, float* offset = nullptr) const; // full-height surface on that side
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
    Difficulty diff_;
    bool near_spawn(int cx, int cz, int r) const { return cx >= -r && cx <= r && cz >= -r && cz <= r; }
    bool rooms_level() const { return id_ == LevelId::L0 || id_ == LevelId::L6 || id_ == LevelId::LFun; }
    bool edge_wall_rooms(int line, int along, bool vertical) const;
    bool edge_wall_l1(int line, int along, bool vertical) const;
    bool edge_wall_l37(int line, int along, bool vertical) const;
    bool office_wall(int line, int along, bool vertical) const;
    bool exit_for_region(int rx, int rz, ExitDoor& out) const;
    struct Room { int bx, bz, qx, qz; float x0, z0, x1, z1; int door_side; int dcx, dcz; };
    bool hotel_room(int cx, int cz, Room& r) const;
    int hotel_block(int bx, int bz) const;   // 0 solid, 1 ballroom, 2 guest rooms
    bool hotel_wall(int line, int along, bool vertical) const;
};

} // namespace iris
