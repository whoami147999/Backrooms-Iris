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
};

// Things lying or burning in the world that aren't part of the level itself.
enum class PropKind : uint8_t { Item, Flare };
struct Prop {
    uint16_t id = 0;
    PropKind kind = PropKind::Item;
    ItemType item = ItemType::None;
    Vec2 pos;
    float life = 0;        // flares: seconds left
};
constexpr float FLARE_TIME = 40.0f;
constexpr float FLARE_RADIUS = 6.5f;

struct SimPlayer {
    bool active = false;
    std::string name;
    PlayerState st;
    InputCmd last_cmd;
    bool have_cmd = false;  // the first command only sets the counters' baseline
    float respawn_t = 0;    // multiplayer: seconds until respawn after dying
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
    SimPlayer players[IRIS_MAX_PLAYERS];
    std::vector<Entity> ents;
    std::vector<Prop> props;
    std::unordered_set<uint32_t> picked;  // level item ids taken this level
    std::vector<uint32_t> picked_order;   // same, in pick order (for snapshots)
    std::deque<SimEvent> events;          // recent messages for everyone's HUD
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
    bool nearest_exit(Vec2 from, Vec2& out) const;

    // helpers shared with the client (prompts, HUD)
    bool looking_at(const PlayerState& p, Vec2 target, float max_dist, float cos_fov) const;
    float light_at(Vec3 p) const;              // static lights + flares + flashlights pointed at it
    void event(const std::string& text);

private:
    Rng rng_;
    uint16_t next_ent_id_ = 1, next_prop_id_ = 1;
    float spawn_t_ = 0;
    LevelId pending_level_ = LevelId::COUNT;

    void use_item(int i, ItemType t);
    void try_pickup(int i);
    void swing(int i);
    void toast(int i, const std::string& s);
    void kill(int i, DeathCause c);
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
