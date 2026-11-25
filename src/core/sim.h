// Backrooms Iris - the game simulation: players, entities, items, sanity, exits.
//
// One GameSim runs the authoritative game. In single player / sandbox it lives in the client;
// in multiplayer it lives in the server (dedicated, or hosted inside a player's game) and
// clients only render snapshots of it. No rendering, no sockets, no SDL in here.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "hash.h"
#include "player.h"
#include "world.h"

namespace iris {

enum class EntityType : uint8_t { Bacteria, Hound, Smiler, SkinStealer, Partygoer, COUNT };
enum class EntState : uint8_t { Wander, Notice, Chase, Search, Stare, Charge, Retreat, Stunned };

const char* entity_name(EntityType t);

struct Entity {
    uint16_t id = 0;
    EntityType type = EntityType::Bacteria;
    EntState state = EntState::Wander;
    Vec2 pos;
    float yaw = 0;
    float state_t = 0;     // time in current state
    float anim = 0;        // walk cycle phase
    float speed = 0;       // current speed (for animation)
    int target = -1;       // player index
    Vec2 goal;             // where it's heading
    Vec2 last_seen;        // last known player position
    std::vector<int> path; // packed cells (cx, cz) to walk through
    float repath_t = 0;
    float attack_cd = 0;
    float stare_t = 0;     // smilers: how long they've been looked at
    float hp = 100;        // sandbox guns can kill things
};
float entity_max_hp(EntityType t);

// Things lying or burning in the world that aren't part of the level itself.
// Item: something to pick up. Flare: burning firesalt. Tape: objective pickup.
// Breaker: objective switch box on a wall (item = which wall side it hangs on, life 1 = switched on).
enum class PropKind : uint8_t { Item, Flare, Tape, Breaker, Glow, Lure };
struct Prop {
    uint16_t id = 0;
    PropKind kind = PropKind::Item;
    ItemType item = ItemType::None;
    Vec2 pos;
    float life = 0;        // flares: seconds left
};

// One objective per level. Finishing it gives everyone a perk for the rest of the run.
enum class ObjType : uint8_t { None, Tapes, Breakers, Survive, COUNT };
struct Objective {
    ObjType type = ObjType::None;
    uint8_t have = 0, need = 0;   // Survive: seconds
    bool done = false;
    Perk reward = Perk::Stamina;
};
std::string objective_text(const Objective& o); // "FIND THE LOST TAPES 1/3"
constexpr float FLARE_TIME = 40.0f;
constexpr float FLARE_RADIUS = 6.5f;
constexpr float GLOW_TIME = 180.0f, GLOW_RADIUS = 5.0f;  // glow stick
constexpr float LURE_TIME = 20.0f, LURE_RANGE = 26.0f;   // music box

struct SimPlayer {
    bool active = false;
    std::string name;
    PlayerState st;
    InputCmd last_cmd;
    bool have_cmd = false;  // the first command only sets the counters' baseline
    float respawn_t = 0;    // multiplayer: seconds until respawn after dying
    uint8_t perks[PERK_COUNT] = {};  // kept through deaths: they're the run's
    std::string toast;      // short personal message ("PICKED UP CROWBAR")
    uint32_t toast_seq = 0;
};

struct SimEvent {
    uint32_t seq;
    std::string text;
};

class GameSim {
public:
    World world;
    uint32_t epoch = 0;        // bumps on every level change
    float level_time = 0;      // seconds since the level started
    bool multiplayer = false;  // respawn timers instead of a death screen
    Difficulty difficulty = Difficulty::Medium;
    SimPlayer players[IRIS_MAX_PLAYERS];
    std::vector<Entity> ents;
    std::vector<Prop> props;
    std::unordered_set<uint32_t> picked;  // level item ids taken this level
    std::vector<uint32_t> picked_order;   // same, in pick order (for snapshots)
    std::deque<SimEvent> events;          // recent messages for everyone's HUD
    Objective objective;
    uint32_t objectives_done = 0;         // this run
    uint32_t event_seq = 0;
    bool escaped = false;                 // someone took the exit out of Level 11

    // sandbox / dev switches
    bool sandbox = false;
    bool god = false;           // nobody takes damage
    bool auto_spawn = true;     // entities show up on their own
    bool freeze_ents = false;   // entities hold still

    explicit GameSim(uint32_t seed = 1) : rng_(seed ? seed : 1) {}

    void start_level(LevelId id, uint32_t seed);
    void next_level();
    int add_player(const std::string& name);  // -1 when full
    void remove_player(int i);
    void respawn(int i);                      // fresh stats at the level spawn

    // One player input step (1/TICK_RATE seconds): movement + actions.
    void apply_input(int i, const InputCmd& cmd);
    // Everything else: entities, sanity, timers, deaths, exits, spawning.
    void tick(float dt);

    // sandbox tools
    Entity& spawn_entity(EntityType t, Vec2 pos, float yaw);
    void spawn_item(ItemType t, Vec2 pos);
    void clear_entities() { ents.clear(); }
    bool nearest_exit(Vec2 from, Vec2& out, ExitDoor* door = nullptr) const;

    // helpers shared with the client (prompts, HUD)
    bool looking_at(const PlayerState& p, Vec2 target, float max_dist, float cos_fov) const;
    float light_at(Vec3 p) const;              // static lights + flares + flashlights pointed at it
    void event(const std::string& text);

private:
    Rng rng_;
    uint16_t next_ent_id_ = 1, next_prop_id_ = 1;
    float spawn_t_ = 0;
    LevelId pending_level_ = LevelId::COUNT;

    void setup_objective();
    void complete_objective();
    bool reachable_cells(std::vector<std::pair<int, int>>& out, int min_depth, int max_depth);
    void use_item(int i, ItemType t);
    void try_pickup(int i);
    void swing(int i);
    void attack(int i, ItemType weapon);
    void shoot(int i, ItemType gun);
    void hurt_entity(size_t k, float dmg, int by);
    Vec2 throw_point(const PlayerState& p, float max_d) const;
    void add_prop(PropKind k, Vec2 pos, float life, ItemType item = ItemType::None);
    bool lure_for(const Entity& e, Vec2& out) const;
    void toast(int i, const std::string& s);
    void kill(int i, DeathCause c);
    void lethal_touch(int i, Entity& e, DeathCause c); // kills, except on easy where it badly hurts
    void damage(int i, float amount, DeathCause c);
    void update_player_stats(int i, float dt);
    void update_entity(Entity& e, float dt);
    void spawn_entities();
    bool find_spawn_cell(Vec2& out, float min_d, float max_d, bool need_dark);
    int closest_player(Vec2 p, float* dist) const;
    void move_entity(Entity& e, Vec2 goal, float speed, float dt);
    bool plan_path(Entity& e, Vec2 goal);
    bool flee_flares(Entity& e, float dt);
};

} // namespace iris
