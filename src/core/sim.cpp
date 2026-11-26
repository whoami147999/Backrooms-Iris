#include "sim.h"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <unordered_map>

#include "collision.h"

namespace iris {

float entity_max_hp(EntityType t) {
    switch (t) {
        case EntityType::Bacteria: return 160;
        case EntityType::Hound: return 80;
        case EntityType::SkinStealer: return 130;
        case EntityType::Partygoer: return 100;
        default: return 1e9f; // a smile can't be shot
    }
}

const char* entity_name(EntityType t) {
    switch (t) {
        case EntityType::Bacteria: return "BACTERIA";
        case EntityType::Hound: return "HOUND";
        case EntityType::Smiler: return "SMILER";
        case EntityType::SkinStealer: return "SKIN-STEALER";
        case EntityType::Partygoer: return "PARTYGOER";
        default: return "?";
    }
}

namespace {

struct Population { EntityType type; int count; };

// Who lives where (our take on the community lore).
std::vector<Population> population(LevelId id) {
    switch (id) {
        case LevelId::L0: return {{EntityType::Bacteria, 1}, {EntityType::Smiler, 1}};
        case LevelId::L1: return {{EntityType::Hound, 3}, {EntityType::Bacteria, 1}};
        case LevelId::L2: return {{EntityType::Hound, 2}, {EntityType::Bacteria, 1}, {EntityType::Smiler, 1}};
        case LevelId::L4: return {{EntityType::Bacteria, 1}};                       // mostly quiet
        case LevelId::L5: return {{EntityType::Partygoer, 3}, {EntityType::Smiler, 1}};
        case LevelId::L6: return {{EntityType::Smiler, 4}, {EntityType::Bacteria, 1}};
        case LevelId::L8: return {{EntityType::Smiler, 4}, {EntityType::Hound, 1}};
        case LevelId::L9: return {{EntityType::SkinStealer, 2}, {EntityType::Hound, 2}};
        case LevelId::LFun: return {{EntityType::Partygoer, 5}};
        case LevelId::L7: return {{EntityType::Smiler, 1}};                          // the sea is nearly empty
        case LevelId::L19: return {{EntityType::Smiler, 2}, {EntityType::Hound, 1}};
        case LevelId::L21: return {{EntityType::Bacteria, 1}};
        case LevelId::L22: return {{EntityType::Hound, 2}, {EntityType::SkinStealer, 1}};
        case LevelId::L24: return {{EntityType::Hound, 1}};
        case LevelId::L33: return {{EntityType::SkinStealer, 1}, {EntityType::Smiler, 1}, {EntityType::Partygoer, 1}};
        case LevelId::L36: return {{EntityType::Bacteria, 1}, {EntityType::Smiler, 1}};
        case LevelId::L51: return {{EntityType::Hound, 2}, {EntityType::Smiler, 2}};
        case LevelId::LRun: return {{EntityType::Partygoer, 5}, {EntityType::Hound, 1}}; // the chase (spawned behind you)
        case LevelId::TSG: return {{EntityType::Bacteria, 2}};
        case LevelId::L3: return {{EntityType::Hound, 2}, {EntityType::Smiler, 1}};
        case LevelId::L10: return {{EntityType::Hound, 2}, {EntityType::SkinStealer, 1}};
        case LevelId::L13: return {{EntityType::SkinStealer, 1}, {EntityType::Partygoer, 1}, {EntityType::Smiler, 1}};
        case LevelId::TSGBlack: return {{EntityType::Bacteria, 2}, {EntityType::Hound, 2}};
        case LevelId::TSGMansion: return {{EntityType::SkinStealer, 1}, {EntityType::Smiler, 2}};
        case LevelId::TSGHope: return {{EntityType::Smiler, 3}, {EntityType::Bacteria, 1}};
        default: return {}; // Level 11 and Level 37 are safe
    }
}

float ent_radius(EntityType t) { return t == EntityType::Hound ? 0.35f : 0.3f; }

inline int pack(int cx, int cz) { return (int)(((uint32_t)(uint16_t)cx << 16) | (uint16_t)cz); }
inline void unpack(int p, int& cx, int& cz) { cx = (int16_t)((uint32_t)p >> 16); cz = (int16_t)(p & 0xFFFF); }

Vec2 cell_center(int cx, int cz) { return {(cx + 0.5f) * CELL, (cz + 0.5f) * CELL}; }

float angle_to(Vec2 from, Vec2 to) { return std::atan2(-(to.x - from.x), -(to.z - from.z)); }

float turn_toward(float a, float target, float max_step) {
    float d = std::remainder(target - a, 2 * PI);
    if (d > max_step) d = max_step;
    if (d < -max_step) d = -max_step;
    return a + d;
}

} // namespace

std::string objective_text(const Objective& o) {
    char buf[64];
    switch (o.type) {
        case ObjType::Tapes: std::snprintf(buf, sizeof buf, "FIND THE LOST TAPES %d/%d", o.have, o.need); break;
        case ObjType::Breakers: std::snprintf(buf, sizeof buf, "TURN ON THE BREAKERS %d/%d", o.have, o.need); break;
        case ObjType::Survive: {
            int left = std::max(0, (int)o.need - (int)o.have);
            std::snprintf(buf, sizeof buf, "SURVIVE %d:%02d", left / 60, left % 60);
            break;
        }
        default: return "";
    }
    return buf;
}

// =================================================================== lifecycle
void GameSim::start_level(LevelId id, uint32_t seed) {
    world = World(id, seed, difficulty);
    epoch++;
    level_time = 0;
    ents.clear();
    props.clear();
    picked.clear();
    picked_order.clear();
    spawn_t_ = 0;
    pending_level_ = LevelId::COUNT;
    int n = 0;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        SimPlayer& p = players[i];
        if (!p.active) continue;
        // keep health/sanity/inventory across levels, but move everyone to the new spawn
        if (!p.st.alive) respawn(i);
        Vec3 s = world.spawn_point();
        p.st.pos = {s.x + (n % 2) * 0.8f - 0.4f, 0, s.z + (n / 2) * 0.8f - 0.4f};
        p.st.vel = {0, 0};
        n++;
    }
    event(std::string("ENTERED ") + level_label(id));
    setup_objective();
}

// =================================================================== objectives
// Open cells reachable from spawn, between min_depth and max_depth steps away.
bool GameSim::reachable_cells(std::vector<std::pair<int, int>>& out, int min_depth, int max_depth) {
    std::unordered_map<int, int> depth;
    std::queue<int> q;
    int start = pack(0, 0);
    depth[start] = 0;
    q.push(start);
    const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
    while (!q.empty() && depth.size() < 4000) {
        int cur = q.front();
        q.pop();
        int d = depth[cur];
        int cx, cz;
        unpack(cur, cx, cz);
        if (d >= min_depth) out.push_back({cx, cz});
        if (d >= max_depth) continue;
        for (int k = 0; k < 4; k++) {
            int nx = cx + DX[k], nz = cz + DZ[k];
            int n = pack(nx, nz);
            if (depth.count(n) || !world.can_step(cx, cz, DX[k], DZ[k])) continue;
            depth[n] = d + 1;
            q.push(n);
        }
    }
    return !out.empty();
}

void GameSim::setup_objective() {
    objective = Objective{};
    LevelId id = world.id();
    uint32_t h = hash3(world.seed(), (int)id, 0, 9001);
    if (id == LevelId::LRun) return; // the only objective here is the door at the end
    bool safe = id == LevelId::L11 || id == LevelId::L37;
    int pick = safe ? 0 : (int)(h % 3u);
    objective.reward = (Perk)((h / 3u + objectives_done) % (uint32_t)PERK_COUNT);
    bool hard = difficulty >= Difficulty::Hard;
    if (pick == 2) {
        static const uint8_t SECS[4] = {75, 100, 130, 160};
        objective.type = ObjType::Survive;
        objective.need = SECS[(int)difficulty];
        return;
    }
    // tapes lie on the floor; breaker boxes hang on walls
    objective.type = pick == 0 ? ObjType::Tapes : ObjType::Breakers;
    objective.need = (uint8_t)(hard ? 4 : 3);
    std::vector<std::pair<int, int>> cells;
    reachable_cells(cells, 7, 34);
    Rng r(h | 1u);
    std::vector<Vec2> placed;
    for (int tries = 0; tries < 600 && (int)placed.size() < objective.need && !cells.empty(); tries++) {
        auto c = cells[(size_t)(r.f01() * cells.size()) % cells.size()];
        int cx = c.first, cz = c.second;
        if (world.exit_cell(cx, cz) || world.item(cx, cz) != ItemType::None || world.pole(cx, cz)) continue;
        Vec2 at = cell_center(cx, cz);
        bool far_enough = true;
        for (Vec2 p : placed) if ((p - at).len() < 6 * CELL) far_enough = false;
        if (!far_enough && tries < 400) continue;
        Prop pr;
        pr.id = next_prop_id_++;
        if (next_prop_id_ == 0) next_prop_id_ = 1;
        if (objective.type == ObjType::Tapes) {
            pr.kind = PropKind::Tape;
            pr.pos = at + Vec2{r.range(-0.6f, 0.6f), r.range(-0.6f, 0.6f)};
            pr.item = (ItemType)(int)(r.f01() * 255); // just a random tilt for the renderer
        } else {
            int side = -1;
            float off = 0;
            for (int k = 0; k < 4 && side < 0; k++) {
                int s = ((int)(r.f01() * 4) + k) % 4;
                if (world.wall_face(cx, cz, s, &off)) side = s;
            }
            if (side < 0) continue;
            static const float NX[4] = {1, -1, 0, 0}, NZ[4] = {0, 0, 1, -1};
            pr.kind = PropKind::Breaker;
            pr.item = (ItemType)side;
            pr.pos = at - Vec2{NX[side], NZ[side]} * (CELL * 0.5f - off);
            pr.life = 0;
        }
        props.push_back(pr);
        placed.push_back(at);
    }
    objective.need = (uint8_t)std::max<size_t>(1, placed.size());
    if (placed.empty()) { objective.type = ObjType::Survive; objective.need = 90; }
}

void GameSim::complete_objective() {
    if (objective.done || objective.type == ObjType::None) return;
    objective.done = true;
    objectives_done++;
    const PerkInfo& pk = perk_info(objective.reward);
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        SimPlayer& sp = players[i];
        if (!sp.active) continue;
        uint8_t& lv = sp.perks[(int)objective.reward];
        if (lv < PERK_MAX) lv++;
        std::copy(sp.perks, sp.perks + PERK_COUNT, sp.st.perks);
        toast(i, std::string("OBJECTIVE DONE: ") + pk.name + " (" + pk.text + ")");
    }
    event(std::string("OBJECTIVE COMPLETE. EVERYONE GETS ") + pk.name);
}

void GameSim::next_level() { pending_level_ = world.info().next; }

int GameSim::add_player(const std::string& name) {
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        if (players[i].active) continue;
        players[i] = SimPlayer{};
        players[i].active = true;
        players[i].name = name.empty() ? "WANDERER" : name;
        respawn(i);
        players[i].st.inv[(int)ItemType::AlmondWater] = 1; // everyone starts with one almond water
        return i;
    }
    return -1;
}

void GameSim::remove_player(int i) {
    if (i < 0 || i >= IRIS_MAX_PLAYERS) return;
    players[i].active = false;
    for (auto& e : ents)
        if (e.target == i) e.target = -1;
}

void GameSim::respawn(int i) {
    SimPlayer& p = players[i];
    bool noclip = p.st.noclip;
    p.st = PlayerState{};           // dying drops everything you carried
    p.st.noclip = noclip;
    Vec3 s = world.spawn_point();
    p.st.pos = {s.x + (i % 2) * 0.8f - 0.4f, 0, s.z + (i / 2) * 0.8f - 0.4f};
    p.st.yaw = p.last_cmd.yaw;
    p.st.sanity = 80;
    std::copy(p.perks, p.perks + PERK_COUNT, p.st.perks); // perks belong to the run
    p.respawn_t = 0;
}

void GameSim::event(const std::string& text) {
    events.push_back({++event_seq, text});
    while (events.size() > 8) events.pop_front();
}

void GameSim::toast(int i, const std::string& s) {
    players[i].toast = s;
    players[i].toast_seq++;
}

// =================================================================== sandbox
Entity& GameSim::spawn_entity(EntityType t, Vec2 pos, float yaw) {
    Entity e;
    e.id = next_ent_id_++;
    if (next_ent_id_ == 0) next_ent_id_ = 1;
    e.type = t;
    e.hp = entity_max_hp(t);
    e.pos = pos;
    e.goal = pos;
    e.yaw = yaw;
    ents.push_back(e);
    return ents.back();
}

void GameSim::spawn_item(ItemType t, Vec2 pos) {
    if (t == ItemType::None || t == ItemType::COUNT) return;
    Prop p;
    p.id = next_prop_id_++;
    if (next_prop_id_ == 0) next_prop_id_ = 1;
    p.kind = PropKind::Item;
    p.item = t;
    p.pos = pos;
    props.push_back(p);
}

bool GameSim::nearest_exit(Vec2 from, Vec2& out, ExitDoor* door_out) const {
    int pcx = floor_to_int(from.x / CELL), pcz = floor_to_int(from.z / CELL);
    float best = 1e9f;
    for (int cz = pcz - 60; cz <= pcz + 60; cz++)
        for (int cx = pcx - 60; cx <= pcx + 60; cx++) {
            ExitDoor door;
            if (!world.exit_door(cx, cz, &door)) continue;
            Vec2 c = door.pos + door.normal * 0.9f; // standing in front of it
            float d = (c - from).len();
            if (d < best) { best = d; out = c; if (door_out) *door_out = door; }
        }
    return best < 1e8f;
}

// =================================================================== players
bool GameSim::looking_at(const PlayerState& p, Vec2 target, float max_dist, float cos_fov) const {
    Vec2 d = target - Vec2{p.pos.x, p.pos.z};
    float dist = d.len();
    if (dist > max_dist) return false;
    if (dist < 1e-3f) return true;
    Vec2 f = yaw_forward(p.yaw);
    if ((d.x * f.x + d.z * f.z) / dist < cos_fov) return false;
    return !world.segment_blocked({p.pos.x, p.pos.z}, target);
}

float GameSim::light_at(Vec3 p) const {
    float b = world.light_level(p);
    for (const Prop& f : props) {
        if (f.kind != PropKind::Flare) continue;
        float d = (f.pos - Vec2{p.x, p.z}).len();
        if (d < FLARE_RADIUS && !world.segment_blocked(f.pos, {p.x, p.z})) b += (1.0f - d / FLARE_RADIUS) * 1.2f;
    }
    for (const Prop& g : props) {
        if (g.kind != PropKind::Glow) continue;
        float d = (g.pos - Vec2{p.x, p.z}).len();
        if (d < GLOW_RADIUS && !world.segment_blocked(g.pos, {p.x, p.z})) b += (1.0f - d / GLOW_RADIUS) * 0.6f;
    }
    for (const SimPlayer& sp : players) {
        if (!sp.active || !sp.st.alive || !sp.st.light_on) continue;
        if (looking_at(sp.st, {p.x, p.z}, 14.0f, 0.93f)) b += 0.8f;
    }
    return b;
}

void GameSim::try_pickup(int i) {
    PlayerState& p = players[i].st;
    Vec2 me{p.pos.x, p.pos.z};
    // closest level item or dropped prop within reach
    int pcx = floor_to_int(p.pos.x / CELL), pcz = floor_to_int(p.pos.z / CELL);
    float best = PICKUP_RANGE;
    int best_cell_id = -1, best_prop = -1;
    ItemType t = ItemType::None;
    for (int cz = pcz - 1; cz <= pcz + 1; cz++)
        for (int cx = pcx - 1; cx <= pcx + 1; cx++) {
            ItemType it = world.item(cx, cz);
            if (it == ItemType::None || picked.count(World::item_id(cx, cz))) continue;
            float d = (world.item_pos(cx, cz) - me).len();
            if (d < best) { best = d; best_cell_id = (int)World::item_id(cx, cz); best_prop = -1; t = it; }
        }
    // objective things first: a tape at your feet or a breaker box within reach
    for (size_t k = 0; k < props.size(); k++) {
        Prop& pr = props[k];
        if (pr.kind != PropKind::Tape && pr.kind != PropKind::Breaker) continue;
        float d = (pr.pos - me).len();
        if (d > (pr.kind == PropKind::Breaker ? 1.7f : PICKUP_RANGE)) continue;
        if (pr.kind == PropKind::Breaker && pr.life > 0.5f) continue; // already on
        char buf[64];
        if (pr.kind == PropKind::Tape) {
            props.erase(props.begin() + (long)k);
            if (objective.type == ObjType::Tapes) objective.have++;
            std::snprintf(buf, sizeof buf, "LOST TAPE %d/%d", objective.have, objective.need);
        } else {
            pr.life = 1;
            if (objective.type == ObjType::Breakers) objective.have++;
            std::snprintf(buf, sizeof buf, "BREAKER ON %d/%d", objective.have, objective.need);
        }
        toast(i, buf);
        event(players[i].name + ": " + buf);
        if (objective.have >= objective.need) complete_objective();
        return;
    }
    for (size_t k = 0; k < props.size(); k++) {
        if (props[k].kind != PropKind::Item) continue;
        float d = (props[k].pos - me).len();
        if (d < best) { best = d; best_prop = (int)k; best_cell_id = -1; t = props[k].item; }
    }
    if (t == ItemType::None) return;
    const ItemInfo& info = item_info(t);
    uint8_t& have = p.inv[(int)t];
    if (have >= (info.tool ? 1 : MAX_ITEM_STACK)) { toast(i, std::string("CAN'T CARRY MORE ") + info.name); return; }
    have++;
    if (best_prop >= 0) props.erase(props.begin() + best_prop);
    else { picked.insert((uint32_t)best_cell_id); picked_order.push_back((uint32_t)best_cell_id); }
    toast(i, std::string("PICKED UP ") + info.name);
}

void GameSim::use_item(int i, ItemType t) {
    PlayerState& p = players[i].st;
    if (t == ItemType::NightVision && p.alive && p.has(t)) { // a tool you switch on and off
        p.nv_on = !p.nv_on && p.battery > 0.02f;
        return;
    }
    if ((int)t <= 0 || (int)t >= ITEM_COUNT || !p.alive || !p.has(t) || item_info(t).tool) return;
    p.inv[(int)t]--;
    switch (t) {
        case ItemType::AlmondWater: // calms you down and puts the wind back in your lungs
            p.stamina = 1;
            p.exhausted = false;
            p.sanity = std::min(100.0f, p.sanity + 30.0f);
            break;
        case ItemType::EnergyDrink:
            p.boost = ENERGY_TIME;
            p.stamina = 1;
            p.exhausted = false;
            break;
        case ItemType::MemoryJuice: // you'll see things. then you won't see anything.
            if (p.juice < 0) p.juice = 0;
            break;
        case ItemType::RoyalRation:
            p.health = std::min(100.0f, p.health + 45.0f);
            p.stamina = std::min(1.0f, p.stamina + 0.5f);
            break;
        case ItemType::LiquidPain: // it hurts. a lot. your head has never been clearer.
            p.health = std::max(5.0f, p.health - 35.0f);
            p.hurt = 0;
            p.sanity = std::min(100.0f, p.sanity + 55.0f);
            break;
        case ItemType::CashewWater:
            if (p.juice >= 0 && p.juice < JUICE_TIME - 8.0f) { p.juice = -1; toast(i, "THE VISIONS FADE"); }
            p.sanity = std::min(100.0f, p.sanity + 15.0f);
            p.slow = 12.0f;
            break;
        case ItemType::LuckyMilk:
            p.lucky = 35.0f;
            break;
        case ItemType::Firesalt: // thrown a few metres ahead; it burns as a flare
            add_prop(PropKind::Flare, throw_point(p, 5.0f), FLARE_TIME);
            break;
        case ItemType::SuperAlmond:
            p.stamina = 1;
            p.exhausted = false;
            p.sanity = std::min(100.0f, p.sanity + 70.0f);
            p.health = std::min(100.0f, p.health + 25.0f);
            break;
        case ItemType::MothJelly: p.regen = 15.0f; break;
        case ItemType::LiquidSilence: p.silent = 30.0f; toast(i, "YOU CAN'T HEAR YOUR OWN FOOTSTEPS"); break;
        case ItemType::MedKit: p.health = std::min(100.0f, p.health + 70.0f); break;
        case ItemType::GlowStick: add_prop(PropKind::Glow, throw_point(p, 2.0f), GLOW_TIME); break;
        case ItemType::MusicBox: add_prop(PropKind::Lure, throw_point(p, 6.0f), LURE_TIME); break;
        case ItemType::FlashBulb: { // a blinding flash: everything that can see it is stunned
            Vec2 at = throw_point(p, 4.0f);
            p.flash = 0;
            int n = 0;
            for (Entity& e : ents) {
                if ((e.pos - at).len() > 9.0f || world.segment_blocked(at, e.pos)) continue;
                n++;
                if (e.type == EntityType::Smiler) { e.state = EntState::Retreat; e.state_t = 0; continue; }
                e.state = EntState::Stunned;
                e.state_t = -1.5f; // 5.5 s
                e.path.clear();
            }
            for (int j = 0; j < IRIS_MAX_PLAYERS; j++) // anyone looking at it is dazzled too
                if (j != i && players[j].active && looking_at(players[j].st, at, 12.0f, 0.6f)) players[j].st.flash = 0;
            if (n) toast(i, n == 1 ? "IT'S STUNNED" : "THEY'RE STUNNED");
            break;
        }
        case ItemType::RabbitFoot: p.inv[(int)t]++; toast(i, "KEEP IT. IT WORKS ON ITS OWN"); break; // not used up by hand
        case ItemType::Batteries: p.battery = 1; toast(i, "FRESH BATTERIES"); break;
        case ItemType::Painkillers: p.painkill = 30.0f; break;
        case ItemType::Chalk: { // a mark on the floor at your feet, for the rest of the level
            int marks = 0;
            for (const Prop& pr : props) marks += pr.kind == PropKind::Mark;
            if (marks >= 40) for (size_t k = 0; k < props.size(); k++) if (props[k].kind == PropKind::Mark) { props.erase(props.begin() + (long)k); break; }
            add_prop(PropKind::Mark, {p.pos.x, p.pos.z}, 1e9f, (ItemType)(uint8_t)(std::fmod(p.yaw + 4 * PI, 2 * PI) / (2 * PI) * 255));
            break;
        }
        default: break;
    }
}

Vec2 GameSim::throw_point(const PlayerState& p, float max_d) const {
    // a few metres ahead, stopping at walls
    Vec2 f = yaw_forward(p.yaw);
    Vec2 from{p.pos.x, p.pos.z}, at = from;
    for (float d = 0.5f; d <= max_d; d += 0.25f) {
        Vec2 c = from + f * d;
        if (world.segment_blocked(from, c) || world.solid(floor_to_int(c.x / CELL), floor_to_int(c.z / CELL))) break;
        at = c;
    }
    return at;
}

void GameSim::add_prop(PropKind k, Vec2 pos, float life, ItemType item) {
    Prop pr;
    pr.id = next_prop_id_++;
    if (next_prop_id_ == 0) next_prop_id_ = 1;
    pr.kind = k;
    pr.pos = pos;
    pr.life = life;
    pr.item = item;
    props.push_back(pr);
}

void GameSim::attack(int i, ItemType weapon) {
    if (weapon == ItemType::Crowbar || weapon == ItemType::None) swing(i);
    else if (is_gun(weapon)) shoot(i, weapon);
}

// Damage from a gun. Smilers just leave; everything else bleeds, flinches, and dies at 0.
void GameSim::hurt_entity(size_t k, float dmg, int by) {
    Entity& e = ents[k];
    if (e.type == EntityType::Smiler) { e.state = EntState::Retreat; e.state_t = 0; return; }
    e.hp -= dmg;
    if (e.hp <= 0) {
        event(players[by].name + " KILLED A " + entity_name(e.type));
        e.pos = {1e6f, 1e6f}; // removed on the next despawn pass, far from everyone
        e.state = EntState::Retreat;
        return;
    }
    if (e.state != EntState::Stunned) { e.state = EntState::Stunned; e.state_t = 3.7f; } // a short flinch
    e.target = by;
    e.last_seen = {players[by].st.pos.x, players[by].st.pos.z};
}

void GameSim::shoot(int i, ItemType gun) {
    PlayerState& p = players[i].st;
    if (!p.alive || !p.has(gun)) return;
    float cd = gun == ItemType::Pistol ? 0.3f : gun == ItemType::Shotgun ? 0.85f : 0.1f;
    if (p.swing < cd) return;
    p.swing = 0;
    int pellets = gun == ItemType::Shotgun ? 8 : 1;
    float dmg = gun == ItemType::Pistol ? 34.0f : gun == ItemType::Shotgun ? 14.0f : 21.0f;
    float range = gun == ItemType::Shotgun ? 16.0f : 45.0f;
    float spread = gun == ItemType::Shotgun ? 0.11f : gun == ItemType::Rifle ? 0.025f : 0.012f;
    Vec2 from{p.pos.x, p.pos.z};
    for (int k = 0; k < pellets; k++) {
        float yaw = p.yaw + rng_.range(-spread, spread);
        Vec2 d = yaw_forward(yaw);
        // closest entity the ray passes through, if nothing solid is in the way
        int hit = -1;
        float best = range;
        for (size_t n = 0; n < ents.size(); n++) {
            Vec2 to = ents[n].pos - from;
            float t = to.x * d.x + to.z * d.z;
            if (t < 0.2f || t > best) continue;
            Vec2 c = from + d * t;
            if ((ents[n].pos - c).len() > (ents[n].type == EntityType::Smiler ? 0.6f : 0.42f)) continue;
            if (world.segment_blocked(from, ents[n].pos)) continue;
            best = t;
            hit = (int)n;
        }
        if (hit >= 0) hurt_entity((size_t)hit, dmg * (gun == ItemType::Shotgun ? std::max(0.35f, 1.0f - best / range) : 1.0f), i);
    }
    // gunfire is loud: every hound in earshot comes running
    for (Entity& e : ents)
        if (e.type == EntityType::Hound && (e.pos - from).len() < 35.0f && e.state != EntState::Stunned) { e.target = i; e.last_seen = from; }
}

void GameSim::swing(int i) {
    PlayerState& p = players[i].st;
    if (!p.alive || !p.has(ItemType::Crowbar) || p.swing < 0.75f) return;
    p.swing = 0;
    // hit the closest thing in front of you
    Entity* hit = nullptr;
    float best = 2.4f;
    for (Entity& e : ents) {
        float d = (e.pos - Vec2{p.pos.x, p.pos.z}).len();
        if (d < best && looking_at(p, e.pos, 2.4f, 0.6f)) { best = d; hit = &e; }
    }
    if (!hit) return;
    if (hit->type == EntityType::Smiler) { // you can't hit a smile. it just leaves.
        hit->state = EntState::Retreat;
        hit->state_t = 0;
        return;
    }
    hit->state = EntState::Stunned;
    hit->state_t = 0;
    hit->path.clear();
    Vec2 away = hit->pos - Vec2{p.pos.x, p.pos.z};
    float al = away.len();
    if (al > 1e-3f) {
        hit->pos += away * (1.3f / al);
        collide_circle(world, hit->pos, ent_radius(hit->type));
    }
}

void GameSim::apply_input(int i, const InputCmd& cmd) {
    SimPlayer& sp = players[i];
    if (!sp.active) return;
    if (!sp.have_cmd) { sp.last_cmd = cmd; sp.have_cmd = true; } // baseline for the counters
    if (sp.st.alive) {
        if (cmd.pickup_ctr != sp.last_cmd.pickup_ctr) try_pickup(i);
        if (cmd.use_ctr != sp.last_cmd.use_ctr) use_item(i, (ItemType)cmd.use_item);
        ItemType weapon = (ItemType)cmd.attack_item;
        if ((int)weapon >= ITEM_COUNT) weapon = ItemType::None;
        if (cmd.attack_ctr != sp.last_cmd.attack_ctr) attack(i, weapon);
        else if ((cmd.buttons & BTN_FIRE) && weapon == ItemType::Rifle) shoot(i, weapon); // automatic
        if (cmd.light_ctr != sp.last_cmd.light_ctr && sp.st.has(ItemType::Flashlight))
            sp.st.light_on = !sp.st.light_on && sp.st.battery > 0.02f;
    }
    sp.last_cmd = cmd;
    player_tick(sp.st, cmd, world, 1.0f / TICK_RATE);
}

void GameSim::kill(int i, DeathCause c) {
    PlayerState& p = players[i].st;
    if (!p.alive || (god && c != DeathCause::MemoryJuice)) return;
    if (p.juice >= 0 && c != DeathCause::MemoryJuice) return; // nothing else can kill you now. it's saving you for itself
    if (c != DeathCause::MemoryJuice && p.has(ItemType::RabbitFoot)) { // the rabbit's foot takes it instead
        p.inv[(int)ItemType::RabbitFoot]--;
        p.health = 40;
        p.hurt = 0;
        p.sanity = std::max(p.sanity, 30.0f);
        toast(i, "THE RABBIT'S FOOT SAVED YOU");
        for (Entity& e : ents) // and whatever did it is knocked back
            if ((e.pos - Vec2{p.pos.x, p.pos.z}).len() < 4.0f && e.type != EntityType::Smiler) { e.state = EntState::Stunned; e.state_t = 0; }
        return;
    }
    p.alive = false;
    p.health = 0;
    p.cause = c;
    p.light_on = false;
    // what you carried stays where you fell (a friend can pick it up)
    for (int t = 1; t < ITEM_COUNT; t++)
        for (int k = 0; k < p.inv[t] && props.size() < 40; k++)
            spawn_item((ItemType)t, Vec2{p.pos.x, p.pos.z} + Vec2{rng_.range(-0.5f, 0.5f), rng_.range(-0.5f, 0.5f)});
    std::fill(p.inv, p.inv + ITEM_COUNT, (uint8_t)0);
    players[i].respawn_t = 10.0f;
    event(players[i].name + ": " + death_text(c));
}

void GameSim::lethal_touch(int i, Entity& e, DeathCause c) {
    if (difficulty != Difficulty::Easy) { kill(i, c); return; }
    if (e.attack_cd > 0) return;
    damage(i, 60.0f / world.diff().damage, c); // easy: a bad hit you can walk away from
    e.attack_cd = 2.0f;
    e.state = EntState::Stunned; // and it loses you for a moment
    e.state_t = 2.5f;
}

void GameSim::damage(int i, float amount, DeathCause c) {
    PlayerState& p = players[i].st;
    if (!p.alive || god || p.juice >= 0) return;
    amount *= world.diff().damage * std::pow(0.85f, (float)p.perk(Perk::Tough)) * (p.painkill > 0 ? 0.5f : 1.0f);
    p.health -= amount;
    p.hurt = 0;
    p.sanity = std::max(0.0f, p.sanity - amount * 0.25f);
    if (p.health <= 0) kill(i, c);
}

void GameSim::update_player_stats(int i, float dt) {
    SimPlayer& sp = players[i];
    PlayerState& p = sp.st;
    if (!p.alive) {
        if (multiplayer) {
            sp.respawn_t -= dt;
            if (sp.respawn_t <= 0) respawn(i);
        }
        return;
    }
    p.hurt += dt;
    p.swing += dt;
    if (p.boost > 0) {
        p.boost -= dt;
        if (p.boost <= 0) { p.boost = 0; p.stamina = 0.15f; p.exhausted = true; } // the crash
    }
    p.slow = std::max(0.0f, p.slow - dt);
    p.silent = std::max(0.0f, p.silent - dt);
    p.painkill = std::max(0.0f, p.painkill - dt);
    p.flash += dt;
    if (p.regen > 0) { p.regen -= dt; p.health = std::min(100.0f, p.health + 4.0f * dt); }
    p.lucky = std::max(0.0f, p.lucky - dt);
    if (p.light_on || p.nv_on) {
        p.battery -= dt / 200.0f * world.diff().battery / (1.0f + 0.4f * p.perk(Perk::Battery)) * ((p.light_on && p.nv_on) ? 1.6f : 1.0f);
        if (p.battery <= 0) { p.battery = 0; p.light_on = false; p.nv_on = false; toast(i, "BATTERY IS DEAD"); }
    } else {
        p.battery = std::min(1.0f, p.battery + dt / 260.0f); // self-charging (wind-up) cell
    }
    if (p.juice >= 0) {
        p.juice += dt;
        p.sanity = std::max(0.0f, p.sanity - dt * 1.2f);
        if (p.juice >= JUICE_TIME) {
            bool g = god;
            god = false; // memory juice gets everyone, even in god mode
            kill(i, DeathCause::MemoryJuice);
            god = g;
            return;
        }
    }

    // --- sanity: the level wears on you; darkness and things you see make it worse,
    // company and light make it better.
    const LevelInfo& li = world.info();
    float drain = li.sanity_drain;
    float light = light_at(p.pos);
    if (p.light_on || p.nv_on) light += 0.4f;
    if (light < 0.35f) drain += 0.35f * (1.0f - light / 0.35f);
    for (int j = 0; j < IRIS_MAX_PLAYERS; j++) {
        if (j == i || !players[j].active || !players[j].st.alive) continue;
        if ((players[j].st.pos - p.pos).len() < 10.0f) { drain -= 0.25f; break; }
    }
    for (const Entity& e : ents) {
        if (e.state == EntState::Stunned) continue;
        if (!looking_at(p, e.pos, 18.0f, 0.55f)) continue;
        drain += e.type == EntityType::Smiler ? 2.5f : 1.2f;
    }
    if (drain > 0) drain *= world.diff().sanity * std::pow(0.8f, (float)p.perk(Perk::Nerves));
    if (god) drain = std::min(drain, 0.0f);
    p.sanity = clampf(p.sanity - drain * dt, 0, 100);
    if (p.sanity <= 0) {
        damage(i, 3.0f * dt, DeathCause::Sanity);
        if (!p.alive) return;
    }
    if (p.hurt > 8.0f && p.health < 100) p.health = std::min(100.0f, p.health + 1.5f * dt * world.diff().regen);

    // --- exits
    int cx = floor_to_int(p.pos.x / CELL), cz = floor_to_int(p.pos.z / CELL);
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            ExitDoor door;
            if (!world.exit_door(cx + dx, cz + dz, &door)) continue;
            if ((door.pos - Vec2{p.pos.x, p.pos.z}).len() < 0.72f) { // walk into the door
                if (world.id() == LevelId::TSGHope) { escaped = true; event(sp.name + " FOUND A WAY OUT..."); }
                pending_level_ = world.info().next;
            }
        }
}

// =================================================================== entities
int GameSim::closest_player(Vec2 pos, float* dist) const {
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        const SimPlayer& sp = players[i];
        if (!sp.active || !sp.st.alive) continue;
        float d = (Vec2{sp.st.pos.x, sp.st.pos.z} - pos).len();
        if (d < bd) { bd = d; best = i; }
    }
    if (dist) *dist = bd;
    return best;
}

bool GameSim::plan_path(Entity& e, Vec2 goal) {
    // BFS over cells, bounded so it stays cheap on an X230 (and a Vita host).
    int sx = floor_to_int(e.pos.x / CELL), sz = floor_to_int(e.pos.z / CELL);
    int gx = floor_to_int(goal.x / CELL), gz = floor_to_int(goal.z / CELL);
    e.path.clear();
    if (sx == gx && sz == gz) return true;
    const int R = 22;
    if (std::abs(gx - sx) > R || std::abs(gz - sz) > R) return false;
    std::unordered_map<int, int> came;
    std::queue<int> q;
    int start = pack(sx, sz), target = pack(gx, gz);
    came[start] = start;
    q.push(start);
    const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
    int expanded = 0;
    while (!q.empty() && expanded < 1600) {
        int cur = q.front();
        q.pop();
        expanded++;
        if (cur == target) break;
        int cx, cz;
        unpack(cur, cx, cz);
        for (int k = 0; k < 4; k++) {
            int nx = cx + DX[k], nz = cz + DZ[k];
            if (std::abs(nx - sx) > R || std::abs(nz - sz) > R) continue;
            int n = pack(nx, nz);
            if (came.count(n) || !world.can_step(cx, cz, DX[k], DZ[k])) continue;
            came[n] = cur;
            q.push(n);
        }
    }
    if (!came.count(target)) return false;
    for (int c = target; c != start; c = came[c]) e.path.push_back(c);
    std::reverse(e.path.begin(), e.path.end());
    return true;
}

void GameSim::move_entity(Entity& e, Vec2 goal, float speed, float dt) {
    Vec2 aim = goal;
    bool blocked = world.segment_blocked(e.pos, goal) || world.info().wall_h < 1.7f; // low walls: always path
    if (blocked) {
        e.repath_t -= dt;
        if (e.repath_t <= 0 || e.path.empty()) {
            plan_path(e, goal);
            e.repath_t = 0.6f;
        }
        while (!e.path.empty()) {
            int cx, cz;
            unpack(e.path.front(), cx, cz);
            Vec2 c = cell_center(cx, cz);
            if ((c - e.pos).len() < 0.6f) { e.path.erase(e.path.begin()); continue; }
            aim = c;
            break;
        }
        if (e.path.empty() && world.info().wall_h < 1.7f) aim = goal; // same cell: walk straight in
    } else {
        e.path.clear();
    }
    Vec2 d = aim - e.pos;
    float len = d.len();
    float turn = e.type == EntityType::Bacteria ? 4.0f : 7.0f;
    if (len > 0.05f) e.yaw = turn_toward(e.yaw, angle_to(e.pos, aim), turn * dt);
    float step = std::min(len, speed * dt);
    Vec2 before = e.pos;
    step *= world.diff().ent_speed;
    if (step > len) step = len;
    if (len > 1e-4f) e.pos += d * (step / len);
    if (e.type != EntityType::Smiler) collide_circle(world, e.pos, ent_radius(e.type));
    e.speed = (e.pos - before).len() / std::max(dt, 1e-4f);
    e.anim += e.speed * dt * 2.2f;
}

bool GameSim::lure_for(const Entity& e, Vec2& out) const {
    if (e.type == EntityType::Smiler) return false;
    float best = LURE_RANGE;
    bool found = false;
    for (const Prop& p : props) {
        if (p.kind != PropKind::Lure) continue;
        float d = (p.pos - e.pos).len();
        if (d < best) { best = d; out = p.pos; found = true; }
    }
    return found;
}

// Burning firesalt keeps everything (but the smilers, who just hate light) at a distance.
bool GameSim::flee_flares(Entity& e, float dt) {
    for (const Prop& f : props) {
        if (f.kind != PropKind::Flare) continue;
        Vec2 away = e.pos - f.pos;
        float d = away.len();
        if (d > FLARE_RADIUS - 1.0f) continue;
        if (d < 1e-3f) away = {1, 0}, d = 1;
        e.state = EntState::Retreat;
        move_entity(e, e.pos + away * (3.0f / d), 2.6f, dt);
        return true;
    }
    return false;
}

void GameSim::update_entity(Entity& e, float dt) {
    e.state_t += dt;
    e.attack_cd = std::max(0.0f, e.attack_cd - dt);
    if (e.state == EntState::Stunned) { // knocked down by a crowbar
        e.speed = 0;
        if (e.state_t > 4.0f) { e.state = EntState::Wander; e.state_t = 0; }
        return;
    }
    if (e.type != EntityType::Smiler && flee_flares(e, dt)) return;
    Vec2 lure;
    if (e.state != EntState::Charge && lure_for(e, lure)) { // the music box: they can't help it
        e.state = EntState::Search;
        if ((lure - e.pos).len() > 1.2f) move_entity(e, lure, e.type == EntityType::Partygoer ? 2.2f : 2.0f, dt);
        else e.speed = 0;
        e.yaw += dt * 1.5f; // swaying to it
        return;
    }

    float pd = 1e9f;
    int pi = closest_player(e.pos, &pd);
    const PlayerState* tp = pi >= 0 ? &players[pi].st : nullptr;
    Vec2 ppos = tp ? Vec2{tp->pos.x, tp->pos.z} : e.pos;
    bool los = tp && pd < 30.0f && !world.segment_blocked(e.pos, ppos);
    float sense = ((tp && tp->lucky > 0) ? 0.3f : 1.0f) * world.diff().hearing; // lucky o' milk

    auto wander = [&](float speed) {
        if (e.state_t > 6.0f || (e.goal - e.pos).len() < 0.8f || e.goal.x == 0) {
            int cx = floor_to_int(e.pos.x / CELL), cz = floor_to_int(e.pos.z / CELL);
            for (int tries = 0; tries < 8; tries++) {
                int nx = cx + (int)rng_.range(-6, 7), nz = cz + (int)rng_.range(-6, 7);
                if (world.open(nx, nz)) { e.goal = cell_center(nx, nz); break; }
            }
            e.state_t = 0;
        }
        move_entity(e, e.goal, speed, dt);
    };
    auto set_state = [&](EntState s) {
        if (e.state != s) { e.state = s; e.state_t = 0; e.path.clear(); }
    };
    auto contact = [&](float reach) { return tp && pd < reach; };

    switch (e.type) {
        // ----------------------------------------------------------- Bacteria
        // Tall, thin, fast. Freezes for a heartbeat when it spots you, then runs you down.
        // It turns slowly: corners and doors are how you lose it.
        case EntityType::Bacteria: {
            if (e.state == EntState::Wander || e.state == EntState::Retreat) {
                set_state(EntState::Wander);
                wander(1.4f);
                if (los && pd < 24.0f * sense) { set_state(EntState::Notice); e.target = pi; }
            } else if (e.state == EntState::Notice) {
                if (tp) e.yaw = turn_toward(e.yaw, angle_to(e.pos, ppos), 6 * dt);
                if (e.state_t > 1.1f) set_state(EntState::Chase);
            } else if (e.state == EntState::Chase) {
                if (los) { e.last_seen = ppos; e.state_t = 0; }
                move_entity(e, los ? ppos : e.last_seen, 4.95f, dt);
                if (!los && (e.last_seen - e.pos).len() < 1.0f) set_state(EntState::Search);
                if (e.state_t > 3.5f) set_state(EntState::Search);
                if (contact(0.9f)) lethal_touch(pi, e, DeathCause::Bacteria);
            } else { // Search
                wander(2.0f);
                if (los && pd < 18.0f * sense) set_state(EntState::Chase);
                if (e.state_t > 7.0f) set_state(EntState::Wander);
            }
            break;
        }
        // ----------------------------------------------------------- Hounds
        // Hunt by sound. Hold eye contact and they hesitate; turn your back or run and they lunge.
        case EntityType::Hound: {
            int heard = -1;
            float hd = 1e9f;
            for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
                const SimPlayer& sp = players[i];
                if (!sp.active || !sp.st.alive) continue;
                float d = (Vec2{sp.st.pos.x, sp.st.pos.z} - e.pos).len();
                if (d < sp.st.noise() * world.diff().hearing && d < hd) { hd = d; heard = i; }
            }
            if (heard >= 0) { e.target = heard; e.last_seen = {players[heard].st.pos.x, players[heard].st.pos.z}; }
            if (e.target >= 0 && (!players[e.target].active || !players[e.target].st.alive)) e.target = -1;
            if (e.target < 0) { set_state(EntState::Wander); wander(1.3f); break; }
            const PlayerState& t = players[e.target].st;
            Vec2 tpos{t.pos.x, t.pos.z};
            float d = (tpos - e.pos).len();
            bool watched = looking_at(t, e.pos, 12.0f, 0.82f);
            if (watched && d < 12.0f && t.speed < WALK_SPEED * 1.2f) {
                set_state(EntState::Stare);
                e.yaw = turn_toward(e.yaw, angle_to(e.pos, tpos), 5 * dt);
                Vec2 away = e.pos - tpos; // back off slowly while you stare it down
                float al = away.len();
                if (al > 1e-3f && d < 6.0f) move_entity(e, e.pos + away * (1.0f / al), 0.6f, dt);
                if (e.state_t > 9.0f) { e.target = -1; set_state(EntState::Wander); }
            } else {
                set_state(EntState::Chase);
                move_entity(e, d < 14.0f && !world.segment_blocked(e.pos, tpos) ? tpos : e.last_seen, 3.9f, dt);
                if (d < 1.1f && e.attack_cd <= 0) {
                    damage(e.target, 34.0f, DeathCause::Hound);
                    e.attack_cd = 1.4f;
                }
                if (heard < 0 && (e.last_seen - e.pos).len() < 1.0f) { e.target = -1; set_state(EntState::Wander); }
            }
            break;
        }
        // ----------------------------------------------------------- Smilers
        // A grin in the dark. It drifts closer while you're not looking. Stare at it, run near
        // it, and it comes for you. Light (lamps, flares, a flashlight in its face) drives it off.
        case EntityType::Smiler: {
            float light = light_at({e.pos.x, 1.5f, e.pos.z});
            if (light > 0.45f || e.state == EntState::Retreat) {
                set_state(EntState::Retreat);
                Vec2 away = e.pos - ppos;
                float al = away.len();
                if (al > 1e-3f) move_entity(e, e.pos + away * (1.0f / al), 2.4f, dt);
                if (e.state_t > 3.0f) e.pos = {1e6f, 1e6f}; // fades away; despawned next pass
                break;
            }
            if (e.state == EntState::Charge) {
                move_entity(e, e.last_seen, 6.4f, dt);
                if (contact(0.9f)) lethal_touch(pi, e, DeathCause::Smiler);
                if (e.state_t > 2.6f || (e.last_seen - e.pos).len() < 0.5f) { e.stare_t = 0; set_state(EntState::Wander); }
                break;
            }
            if (!tp || pd > 20.0f) { set_state(EntState::Wander); wander(0.5f); break; }
            bool watched = looking_at(*tp, e.pos, 20.0f, 0.9f);
            if (watched) e.stare_t += dt;
            else e.stare_t = std::max(0.0f, e.stare_t - dt * 0.5f);
            bool provoked = e.stare_t > 2.6f || (pd < 9.0f * sense && tp->speed > WALK_SPEED * 1.2f) || pd < 2.0f;
            if (provoked) {
                e.last_seen = ppos;
                set_state(EntState::Charge);
            } else {
                set_state(EntState::Stare);
                if (!watched) move_entity(e, ppos, 0.7f, dt);
                e.yaw = turn_toward(e.yaw, angle_to(e.pos, ppos), 3 * dt);
            }
            break;
        }
        // ----------------------------------------------------------- Skin-Stealers
        // Patrol the streets. See you, chase you, hit hard. A sprint can outpace them, barely.
        case EntityType::SkinStealer: {
            if (e.state == EntState::Chase) {
                if (los) { e.last_seen = ppos; e.state_t = 0; }
                move_entity(e, los ? ppos : e.last_seen, 4.25f, dt);
                if (contact(1.0f) && e.attack_cd <= 0) {
                    damage(pi, 50.0f, DeathCause::SkinStealer);
                    e.attack_cd = 1.2f;
                }
                if (e.state_t > 5.0f) set_state(EntState::Wander);
            } else {
                set_state(EntState::Wander);
                wander(1.5f);
                if (los && pd < 26.0f * sense) set_state(EntState::Chase);
            }
            break;
        }
        // ----------------------------------------------------------- Partygoers
        // =) They want you at the party. Slower than you, but they never, ever stop.
        case EntityType::Partygoer: {
            if (tp && pd < 16.0f * sense) { set_state(EntState::Chase); e.last_seen = ppos; }
            if (e.state == EntState::Chase) {
                if (!tp || pd > 45.0f) { set_state(EntState::Wander); break; }
                move_entity(e, ppos, 2.05f, dt);
                if (contact(0.85f)) lethal_touch(pi, e, DeathCause::Partygoer);
            } else {
                set_state(EntState::Wander);
                wander(1.0f);
            }
            break;
        }
        default: break;
    }
}

bool GameSim::find_spawn_cell(Vec2& out, float min_d, float max_d, bool need_dark) {
    int anchor = -1;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++)
        if (players[i].active && players[i].st.alive) { anchor = i; if (rng_.f01() < 0.5f) break; }
    if (anchor < 0) return false;
    Vec2 a{players[anchor].st.pos.x, players[anchor].st.pos.z};
    for (int tries = 0; tries < 24; tries++) {
        float ang = rng_.range(0, 2 * PI), d = rng_.range(min_d, max_d);
        Vec2 p = a + Vec2{std::cos(ang), std::sin(ang)} * d;
        int cx = floor_to_int(p.x / CELL), cz = floor_to_int(p.z / CELL);
        if (!world.open(cx, cz)) continue;
        Vec2 c = cell_center(cx, cz);
        bool ok = true;
        for (const SimPlayer& sp : players) { // never pop into view, never too close to anyone
            if (!sp.active) continue;
            Vec2 pp{sp.st.pos.x, sp.st.pos.z};
            float d = (pp - c).len();
            // hidden = behind something, or far enough that the haze hides it
            if (d < min_d || (d < 34.0f && !world.segment_blocked(pp, c))) { ok = false; break; }
        }
        if (!ok) continue;
        if (need_dark && light_at({c.x, 1.5f, c.z}) > 0.3f) continue;
        out = c;
        return true;
    }
    return false;
}

void GameSim::spawn_entities() {
    // despawn anything that drifted far from everyone (or faded away)
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        float d;
        closest_player(e.pos, &d);
        return d > 75.0f;
    }), ents.end());
    if (auto_spawn && world.id() == LevelId::LRun) { // Level !: they come up the corridor behind you, already running
        if (level_time < 3.0f) return;
        for (const Population& pop : population(world.id())) {
            int have = 0;
            for (const Entity& e : ents) have += e.type == pop.type;
            int want = std::max(1, (int)std::lround(pop.count * world.diff().ent_count));
            if (have >= want) continue;
            float back = -1e9f; // the rearmost player
            int who = -1;
            for (int i = 0; i < IRIS_MAX_PLAYERS; i++)
                if (players[i].active && players[i].st.alive && players[i].st.pos.z > back) { back = players[i].st.pos.z; who = i; }
            if (who < 0) return;
            int cz = std::min(2, floor_to_int(back / CELL) + 9 + (int)rng_.range(0, 4));
            for (int cx = -1; cx <= 1; cx++) {
                if (!world.open(cx, cz)) continue;
                Entity& e = spawn_entity(pop.type, cell_center(cx, cz), 0);
                e.state = EntState::Chase;
                e.target = who;
                e.last_seen = {players[who].st.pos.x, players[who].st.pos.z};
                return;
            }
        }
        return;
    }
    if (!auto_spawn || level_time < world.diff().grace) return; // a moment to get your bearings
    if (ents.size() >= 12) return;
    for (const Population& pop : population(world.id())) {
        int have = 0;
        for (const Entity& e : ents) have += e.type == pop.type;
        int want = std::max(1, (int)std::lround(pop.count * world.diff().ent_count));
        if (have >= want) continue;
        Vec2 at;
        if (!find_spawn_cell(at, 26.0f, 44.0f, pop.type == EntityType::Smiler)) continue;
        spawn_entity(pop.type, at, rng_.range(-PI, PI));
        return; // one per spawn pass
    }
}

void GameSim::tick(float dt) {
    level_time += dt;
    if (objective.type == ObjType::Survive && !objective.done) {
        bool anyone = false;
        for (const SimPlayer& sp : players) anyone |= sp.active && sp.st.alive;
        if (anyone) objective.have = (uint8_t)std::min<float>(objective.need, level_time);
        if (objective.have >= objective.need) complete_objective();
    }
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++)
        if (players[i].active) update_player_stats(i, dt);
    if (!freeze_ents)
        for (Entity& e : ents) update_entity(e, dt);
    // flares burn out
    for (Prop& p : props)
        if (p.kind == PropKind::Flare || p.kind == PropKind::Glow || p.kind == PropKind::Lure) p.life -= dt;
    props.erase(std::remove_if(props.begin(), props.end(), [](const Prop& p) {
        return (p.kind == PropKind::Flare || p.kind == PropKind::Glow || p.kind == PropKind::Lure) && p.life <= 0;
    }), props.end());
    spawn_t_ -= dt;
    if (spawn_t_ <= 0) {
        spawn_entities();
        spawn_t_ = 2.0f;
    }
    if (pending_level_ != LevelId::COUNT) {
        LevelId next = pending_level_;
        start_level(next, mix32(world.seed() + 0x51ed27u));
    }
}

} // namespace iris
