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
        case EntityType::Faceling: return 70;
        case EntityType::Animation: return 90;
        default: return 1e9f; // a smile can't be shot (and neither can the Landlord)
    }
}

const char* entity_name(EntityType t) {
    switch (t) {
        case EntityType::Bacteria: return "BACTERIA";
        case EntityType::Hound: return "HOUND";
        case EntityType::Smiler: return "SMILER";
        case EntityType::SkinStealer: return "SKIN-STEALER";
        case EntityType::Partygoer: return "PARTYGOER";
        case EntityType::Landlord: return "THE LANDLORD";
        case EntityType::Faceling: return "FACELING";
        case EntityType::Mangled: return "THE MANGLED";
        case EntityType::Animation: return "ANIMATION";
        case EntityType::FallenAngel: return "FALLEN ANGEL";
        case EntityType::MegMedic: return "M.E.G. MEDIC";
        case EntityType::MegScout: return "M.E.G. SCOUT";
        default: return "?";
    }
}

namespace {

struct Population { EntityType type; int count; };

// Who lives where (our take on the community lore).
std::vector<Population> population(LevelId id, bool night) {
    switch (id) {
        case LevelId::TSG0: if (night) return {{EntityType::Bacteria, 2}, {EntityType::Hound, 2}}; return {}; // nothing lives here by day
        case LevelId::TSG1: return {{EntityType::Landlord, 1}};
        case LevelId::TSG2: return {{EntityType::FallenAngel, 3}};
        case LevelId::TSG3: return {{EntityType::SkinStealer, 2}};                      // the islanders
        case LevelId::TSG4: return {{EntityType::Faceling, 4}, {EntityType::Mangled, 1}, {EntityType::Smiler, 1}};
        case LevelId::TSGEnd: return {};
        case LevelId::TheEnd: return {};
        case LevelId::L70: if (night) return {{EntityType::Hound, 3}, {EntityType::SkinStealer, 1}}; return {{EntityType::Hound, 1}, {EntityType::SkinStealer, 1}};
        case LevelId::L94: if (night) return {{EntityType::Animation, 4}, {EntityType::Smiler, 1}, {EntityType::Hound, 1}}; return {}; // the town is safe by day
        case LevelId::L188: return {{EntityType::Bacteria, 1}, {EntityType::SkinStealer, 1}, {EntityType::Smiler, 1}};
        case LevelId::L0: return {{EntityType::Bacteria, 1}, {EntityType::Smiler, 2}, {EntityType::Hound, 1}};
        case LevelId::L1: return {{EntityType::Hound, 4}, {EntityType::Bacteria, 1}};
        case LevelId::L2: return {{EntityType::Hound, 3}, {EntityType::Bacteria, 1}, {EntityType::Smiler, 2}};
        case LevelId::L4: return {{EntityType::Bacteria, 1}, {EntityType::Smiler, 1}};
        case LevelId::L5: return {{EntityType::Partygoer, 4}, {EntityType::Smiler, 2}};
        case LevelId::L6: return {{EntityType::Smiler, 5}, {EntityType::Bacteria, 1}};
        case LevelId::L8: return {{EntityType::Smiler, 5}, {EntityType::Hound, 2}};
        case LevelId::L9: return {{EntityType::SkinStealer, 3}, {EntityType::Hound, 2}};
        case LevelId::LFun: return {{EntityType::Partygoer, 7}};
        case LevelId::L7: return {{EntityType::Smiler, 2}};                          // the sea is nearly empty
        case LevelId::L19: return {{EntityType::Smiler, 3}, {EntityType::Hound, 1}};
        case LevelId::L21: return {{EntityType::Bacteria, 1}, {EntityType::SkinStealer, 1}};
        case LevelId::L22: return {{EntityType::Hound, 3}, {EntityType::SkinStealer, 1}};
        case LevelId::L24: return {{EntityType::Hound, 2}};
        case LevelId::L33: return {{EntityType::SkinStealer, 2}, {EntityType::Smiler, 1}, {EntityType::Partygoer, 2}};
        case LevelId::L36: return {{EntityType::Bacteria, 1}, {EntityType::Smiler, 2}, {EntityType::Hound, 1}};
        case LevelId::L51: return {{EntityType::Hound, 3}, {EntityType::Smiler, 2}};
        case LevelId::LRun: return {{EntityType::Partygoer, 10}, {EntityType::Hound, 3}, {EntityType::Bacteria, 1}, {EntityType::Smiler, 3}}; // the chase
        case LevelId::L3: return {{EntityType::Hound, 3}, {EntityType::Smiler, 2}};
        case LevelId::L10: return {{EntityType::Hound, 3}, {EntityType::SkinStealer, 1}};
        case LevelId::L13: return {{EntityType::SkinStealer, 2}, {EntityType::Partygoer, 1}, {EntityType::Smiler, 1}};
        case LevelId::TSGFar: return {{EntityType::Bacteria, 2}, {EntityType::Hound, 3}};
        case LevelId::TSGHope: return {{EntityType::Smiler, 4}, {EntityType::Bacteria, 1}};
        case LevelId::L11: return {{EntityType::SkinStealer, 1}};                   // (the city isn't as safe as it was)
        case LevelId::Hub: return {};
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
        case ObjType::Keys: std::snprintf(buf, sizeof buf, "FIND THE KEYCARDS %d/%d", o.have, o.need); break;
        case ObjType::Survive: {
            int left = std::max(0, (int)o.need - (int)o.have);
            std::snprintf(buf, sizeof buf, "SURVIVE %d:%02d", left / 60, left % 60);
            break;
        }
        default: return "";
    }
    std::string s = buf;
    if (o.required && !o.done) s += " TO UNLOCK THE EXITS";
    return s;
}

// =================================================================== lifecycle
void GameSim::start_level(LevelId id, uint32_t seed) {
    world = World(id, seed, difficulty);
    door_flip.clear();
    pried.clear();
    searched.clear();
    world.door_flip = &door_flip;
    epoch++;
    level_time = 0;
    ents.clear();
    props.clear();
    picked.clear();
    picked_order.clear();
    spawn_t_ = 0;
    pending_level_ = LevelId::COUNT;
    outposts_up.clear();
    if (id != LevelId::TheEnd) { escaped = false; won_t = 0; }
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
    if (id == LevelId::LRun || id == LevelId::TSGEnd || id == LevelId::Hub || id == LevelId::TheEnd) return; // the only objective here is the door
    if (sandbox) return;
    bool safe = id == LevelId::L11 || id == LevelId::L37;
    int pick = safe ? 0 : (int)(h % 3u);
    objective.reward = (Perk)((h / 3u + objectives_done) % (uint32_t)PERK_COUNT);
    bool hard = difficulty >= Difficulty::Hard;
    // 1.2: on these levels the exits won't open until the job's done
    switch (id) {
        case LevelId::L5: case LevelId::L13: case LevelId::TSG1: case LevelId::L188: case LevelId::L21:
            objective.required = true; pick = 3; break;   // the rooms' keycards
        case LevelId::L2: case LevelId::L3: case LevelId::L22: case LevelId::L36:
            objective.required = true; pick = 1; break;   // get the power back on
        case LevelId::L33: case LevelId::L51: case LevelId::L4:
            objective.required = true; pick = 0; break;   // the lost tapes
        default: break;
    }
    if (pick == 2) {
        static const uint8_t SECS[4] = {75, 100, 130, 160};
        objective.type = ObjType::Survive;
        objective.need = SECS[(int)difficulty];
        return;
    }
    // tapes lie on the floor; breaker boxes hang on walls
    bool few_walls = id == LevelId::TSG3 || id == LevelId::L94 || id == LevelId::TSG2 || id == LevelId::TSG0; // nothing to hang a breaker box on
    objective.type = pick == 3 ? ObjType::Keys : ((pick == 0 || few_walls) ? ObjType::Tapes : ObjType::Breakers);
    objective.need = (uint8_t)(hard ? 4 : 3);
    std::vector<std::pair<int, int>> cells;
    reachable_cells(cells, 4, 20); // 1.3: closer to where you start
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
        if (objective.type == ObjType::Tapes || objective.type == ObjType::Keys) {
            if (objective.type == ObjType::Keys && !world.guest_room(cx, cz) && tries < 450) continue; // keycards are left in the rooms
            pr.kind = objective.type == ObjType::Keys ? PropKind::Key : PropKind::Tape;
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
    if (placed.empty()) { objective.type = ObjType::Survive; objective.need = 90; objective.required = false; }
}

void GameSim::complete_objective() {
    if (objective.done || objective.type == ObjType::None) return;
    objective.done = true;
    objectives_done++;
    if (objective.required) event("THE EXITS ARE UNLOCKED");
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
    // a M.E.G. official within reach: talk to them
    for (Entity& e : ents) {
        if ((e.type != EntityType::MegMedic && e.type != EntityType::MegScout) || (e.pos - me).len() > 2.2f) continue;
        meg_talk(i, e);
        return;
    }
    // 1.3: a door or a drawer right in front of you (nothing to pick up closer than it)
    {
        float item_d = 9;
        for (int cz = pcz - 1; cz <= pcz + 1; cz++)
            for (int cx = pcx - 1; cx <= pcx + 1; cx++)
                if (world.item(cx, cz) != ItemType::None && !picked.count(World::item_id(cx, cz))) item_d = std::min(item_d, (world.item_pos(cx, cz) - me).len());
        for (const Prop& pr : props)
            if (pr.kind == PropKind::Item || pr.kind == PropKind::Tape || pr.kind == PropKind::Key || pr.kind == PropKind::Breaker) item_d = std::min(item_d, (pr.pos - me).len());
        if (item_d > 1.0f) {
            if (use_door(i, false)) return;
            if (search_furniture(i)) return;
        }
    }
    // objective things first: a tape at your feet or a breaker box within reach
    for (size_t k = 0; k < props.size(); k++) {
        Prop& pr = props[k];
        if (pr.kind != PropKind::Tape && pr.kind != PropKind::Breaker && pr.kind != PropKind::Key) continue;
        float d = (pr.pos - me).len();
        if (d > (pr.kind == PropKind::Breaker ? 1.7f : PICKUP_RANGE)) continue;
        if (pr.kind == PropKind::Breaker && pr.life > 0.5f) continue; // already on
        char buf[64];
        if (pr.kind == PropKind::Tape || pr.kind == PropKind::Key) {
            bool key = pr.kind == PropKind::Key;
            props.erase(props.begin() + (long)k);
            if (objective.type == (key ? ObjType::Keys : ObjType::Tapes)) objective.have++;
            std::snprintf(buf, sizeof buf, key ? "KEYCARD %d/%d" : "LOST TAPE %d/%d", objective.have, objective.need);
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
    const int cap = t == ItemType::Shells ? 12 : (info.tool ? 1 : MAX_ITEM_STACK);
    if (have >= cap) { toast(i, std::string("CAN'T CARRY MORE ") + info.name); return; }
    have = (uint8_t)std::min(cap, have + (t == ItemType::Shells ? 2 : 1));
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
    if (t == ItemType::Shells) { toast(i, "LOAD THEM: SELECT THE SHOTGUN"); return; } // ammo isn't used up by hand
    p.inv[(int)t]--;
    switch (t) {
        case ItemType::AlmondWater: // calms you down and puts the wind back in your lungs
            if (world.id() == LevelId::TSG0 && world.night(level_time)) { // ...except at night on 0Zero
                p.health = std::max(5.0f, p.health - 35.0f);
                p.hurt = 0;
                p.sanity = std::min(100.0f, p.sanity + 55.0f);
                toast(i, "IT TURNED INTO LIQUID PAIN");
                break;
            }
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
                if (unstoppable(e.type)) continue;
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
    if (unstoppable(e.type)) return;
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
    if (gun == ItemType::Shotgun && !sandbox) { // outside the sandbox it needs shells
        if (!p.has(ItemType::Shells)) { p.swing = cd - 0.4f; toast(i, "CLICK. NO SHELLS"); return; }
        p.inv[(int)ItemType::Shells]--;
    }
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
    if (!hit || unstoppable(hit->type)) return;
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
    sp.st.held = cmd.held < ITEM_COUNT && sp.st.inv[cmd.held] ? cmd.held : 0;
    sp.st.emote_t += 1.0f / TICK_RATE;
    sp.st.use_t += 1.0f / TICK_RATE;
    if (cmd.emote_ctr != sp.last_cmd.emote_ctr && sp.st.alive) { sp.st.emote = std::min<uint8_t>(cmd.emote, 4); sp.st.emote_t = 0; }
    if (sp.st.emote && (sp.st.speed > 0.6f || sp.st.emote_t > 8.0f)) sp.st.emote = 0; // moving (or a while) ends it
    if (sp.st.alive) {
        if (cmd.pickup_ctr != sp.last_cmd.pickup_ctr) try_pickup(i);
        if (cmd.use_ctr != sp.last_cmd.use_ctr) { use_item(i, (ItemType)cmd.use_item); sp.st.use_t = 0; }
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
            if ((e.pos - Vec2{p.pos.x, p.pos.z}).len() < 4.0f && e.type != EntityType::Smiler && !unstoppable(e.type)) { e.state = EntState::Stunned; e.state_t = 0; }
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
    if (unstoppable(e.type)) return;
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
        if (e.state == EntState::Stunned || friendly(e.type)) continue;
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

    // --- the weather and the water
    int cx = floor_to_int(p.pos.x / CELL), cz = floor_to_int(p.pos.z / CELL);
    if (!p.noclip && !god) {
        bool tick_msg = std::fmod(level_time, 5.0f) < dt;
        if (world.toxic(cx, cz)) { // 3Three: mercury and gasoline
            damage(i, 5.0f * dt, DeathCause::Toxic);
            if (!p.alive) return;
            if (tick_msg) toast(i, "THE WATER BURNS. GET TO AN ISLAND");
        }
        if (world.id() == LevelId::TSG2 && p.speed > WALK_SPEED * 1.25f) { // 2Two: 38 degrees in the snow
            damage(i, 1.4f * dt, DeathCause::Heat);
            if (!p.alive) return;
            if (tick_msg) toast(i, "TOO HOT TO RUN");
        }
        if (world.id() == LevelId::L70 && !world.house(cx, cz)) { // Froidetown: you can't stay out here
            damage(i, (world.night(level_time) ? 0.5f : 0.22f) * dt, DeathCause::Cold);
            if (!p.alive) return;
            if (tick_msg && p.health < 60) toast(i, "YOU'RE FREEZING. GET INSIDE A HOUSE");
        }
    }

    // --- exits
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            ExitDoor door;
            if (!world.exit_door(cx + dx, cz + dz, &door)) continue;
            if ((door.pos - Vec2{p.pos.x, p.pos.z}).len() < 0.72f) { // walk into the door
                if (objective.required && !objective.done && !sandbox) { // locked until the job's done
                    if (level_time - exit_msg_t_[i] > 3.0f) { toast(i, "IT'S LOCKED. " + objective_text(objective)); exit_msg_t_[i] = level_time; }
                    continue;
                }
                if (world.id() == LevelId::TheEnd) { // the last door. for real this time.
                    if (!escaped) { escaped = true; won_t = 0; event(sp.name + " OPENED THE LAST DOOR. YOU MADE IT OUT."); }
                    continue;
                }
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
    // BFS over cells, bounded so it stays cheap on old laptops.
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
    if (len > 0.05f && (e.pos - before).len() < step * 0.3f) entity_doors(e); // stuck against something: a door?
    keep_out(e);
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
    if (e.type == EntityType::MegMedic || e.type == EntityType::MegScout) { update_meg(e, dt); return; }
    if (e.state == EntState::Stunned) { // knocked down by a crowbar
        e.speed = 0;
        if (e.state_t > 4.0f) { e.state = EntState::Wander; e.state_t = 0; }
        return;
    }
    if (e.type != EntityType::Smiler && !unstoppable(e.type) && flee_flares(e, dt)) return;
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
            if (tp && pd < 45.0f && !friendly(e.type) && rng_.f01() < 0.4f) { // 1.2: they drift your way. they're hunting.
                cx = floor_to_int((e.pos.x + (ppos.x - e.pos.x) * 0.6f) / CELL);
                cz = floor_to_int((e.pos.z + (ppos.z - e.pos.z) * 0.6f) / CELL);
            }
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
            if (e.state == EntState::Retreat && e.state_t < 6.0f) { // gave up: wanders off for a while
                wander(1.4f);
            } else if (e.state == EntState::Wander || e.state == EntState::Retreat) {
                set_state(EntState::Wander);
                wander(1.4f);
                if (los && pd < 22.0f * sense) { set_state(EntState::Notice); e.target = pi; e.stare_t = 0; }
            } else if (e.state == EntState::Notice) { // it freezes when it spots you: that's your head start
                if (tp) e.yaw = turn_toward(e.yaw, angle_to(e.pos, ppos), 6 * dt);
                if (e.state_t > 1.6f) set_state(EntState::Chase);
            } else if (e.state == EntState::Chase) {
                // 1.1: a bit slower than a sprint (4.4 m/s), so running and breaking line of sight
                // works. It still outruns walking, and it gives up after a long chase.
                e.stare_t += dt;  // (reused: total chase time)
                if (los) { e.last_seen = ppos; e.state_t = 0; }
                move_entity(e, los ? ppos : e.last_seen, 4.15f, dt);
                if (!los && (e.last_seen - e.pos).len() < 1.0f) set_state(EntState::Search);
                if (e.state_t > 2.5f) set_state(EntState::Search);
                if (e.stare_t > 14.0f) { e.stare_t = 0; set_state(EntState::Retreat); }
                if (contact(0.75f)) lethal_touch(pi, e, DeathCause::Bacteria);
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
                move_entity(e, d < 14.0f && !world.segment_blocked(e.pos, tpos) ? tpos : e.last_seen, 4.1f, dt);
                if (d < 1.1f && e.attack_cd <= 0) {
                    damage(e.target, 40.0f, DeathCause::Hound);
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
                move_entity(e, e.last_seen, 6.8f, dt);
                if (contact(0.9f)) lethal_touch(pi, e, DeathCause::Smiler);
                if (e.state_t > 2.6f || (e.last_seen - e.pos).len() < 0.5f) { e.stare_t = 0; set_state(EntState::Wander); }
                break;
            }
            if (!tp || pd > 20.0f) { set_state(EntState::Wander); wander(0.5f); break; }
            bool watched = looking_at(*tp, e.pos, 20.0f, 0.9f);
            if (watched) e.stare_t += dt;
            else e.stare_t = std::max(0.0f, e.stare_t - dt * 0.5f);
            bool provoked = e.stare_t > 2.1f || (pd < 10.0f * sense && tp->speed > WALK_SPEED * 1.2f) || pd < 2.2f;
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
                if (e.state_t > 8.0f) set_state(EntState::Wander);
            } else {
                set_state(EntState::Wander);
                wander(1.6f);
                if (los && pd < 30.0f * sense) set_state(EntState::Chase);
            }
            break;
        }
        // ----------------------------------------------------------- Partygoers
        // =) They want you at the party. Slower than you, but they never, ever stop.
        case EntityType::Partygoer: {
            if (tp && pd < 20.0f * sense) { set_state(EntState::Chase); e.last_seen = ppos; }
            if (e.state == EntState::Chase) {
                if (!tp || pd > 50.0f) { set_state(EntState::Wander); break; }
                move_entity(e, ppos, world.id() == LevelId::LRun ? 3.3f : 2.35f, dt);
                if (contact(0.85f)) lethal_touch(pi, e, DeathCause::Partygoer);
            } else {
                set_state(EntState::Wander);
                wander(1.0f);
            }
            break;
        }
        // ----------------------------------------------------------- The Landlord (1One)
        // A walking shadow in a suit and fedora. Slow while he patrols his mansion; once he sees
        // you he comes for you, shouting. Nothing hurts him. Be quiet and keep opening doors.
        case EntityType::Landlord: {
            if (e.state == EntState::Chase) {
                if (los) { e.last_seen = ppos; e.state_t = 0; }
                move_entity(e, los ? ppos : e.last_seen, 3.7f, dt);
                if (e.state_t > 6.0f || !tp) { set_state(EntState::Search); e.stare_t = 0; }
                if (contact(0.8f)) lethal_touch(pi, e, DeathCause::Landlord);
            } else {
                set_state(e.state == EntState::Search && e.state_t < 8.0f ? EntState::Search : EntState::Wander);
                wander(e.state == EntState::Search ? 1.6f : 1.05f);
                if (los && pd < 15.0f * sense) {
                    if (e.stare_t <= 0) { event("THE LANDLORD: GET OUT OF MY HOUSE!"); e.stare_t = 1; }
                    set_state(EntState::Chase);
                    e.target = pi;
                }
            }
            break;
        }
        // ----------------------------------------------------------- Faceling (4Four)
        // Black jacket, beanie, no face, a knife. Corners you and takes something. They love almond
        // water: hand one over and they leave you alone for a while. Got nothing? That's worse.
        case EntityType::Faceling: {
            if (e.state == EntState::Retreat) { // got what it wanted
                if (tp && pd < 12.0f) move_entity(e, e.pos + (e.pos - ppos) * (3.0f / std::max(0.1f, pd)), 2.2f, dt);
                else wander(1.0f);
                if (e.state_t > e.stare_t) { set_state(EntState::Wander); }
                break;
            }
            if (e.state == EntState::Chase) {
                if (!tp || pd > 22.0f) { set_state(EntState::Wander); break; }
                move_entity(e, ppos, 3.1f, dt);
                if (contact(0.8f) && e.attack_cd <= 0) {
                    PlayerState& v = players[pi].st;
                    ItemType took = ItemType::None;
                    if (v.has(ItemType::AlmondWater)) took = ItemType::AlmondWater;
                    for (int tries = 0; tries < 24 && took == ItemType::None; tries++) {
                        int k = 1 + (int)rng_.range(0, (float)(ITEM_COUNT - 1));
                        if (k < ITEM_COUNT && v.inv[k] > 0 && (!item_info((ItemType)k).tool || tries > 12)) took = (ItemType)k;
                    }
                    e.attack_cd = 3.0f;
                    if (took != ItemType::None) {
                        v.inv[(int)took]--;
                        if (took == ItemType::Flashlight && !v.has(ItemType::Flashlight)) v.light_on = false;
                        toast(pi, std::string("A FACELING TOOK YOUR ") + item_info(took).name);
                        e.stare_t = took == ItemType::AlmondWater ? 60.0f : 25.0f; // almond water buys a whole minute
                    } else {
                        damage(pi, 32.0f, DeathCause::Faceling);
                        toast(pi, "YOU HAD NOTHING LEFT TO GIVE");
                        e.stare_t = 12.0f;
                    }
                    set_state(EntState::Retreat);
                }
            } else {
                set_state(EntState::Wander);
                wander(1.2f);
                if (los && pd < 11.0f * sense) { set_state(EntState::Chase); e.target = pi; }
            }
            break;
        }
        // ----------------------------------------------------------- The Mangled (4Four)
        // Taller than the buildings, all limbs. Slow, but it doesn't look where it puts its feet.
        case EntityType::Mangled: {
            if (los && pd < 40.0f) { set_state(EntState::Chase); move_entity(e, ppos, 1.9f, dt); }
            else { set_state(EntState::Wander); wander(1.5f); }
            if (contact(2.1f)) lethal_touch(pi, e, DeathCause::Mangled);
            break;
        }
        // ----------------------------------------------------------- Animation (Level 94, night)
        // A cartoon that moves like stop-motion: a few frames on, a few frames still.
        case EntityType::Animation: {
            bool frame_on = std::fmod(e.state_t, 0.36f) < 0.24f;
            if (e.state == EntState::Chase) {
                if (!tp || pd > 30.0f) { set_state(EntState::Wander); break; }
                if (los) e.last_seen = ppos;
                if (frame_on) move_entity(e, los ? ppos : e.last_seen, 5.2f, dt); else e.speed = 0;
                if (contact(0.85f)) lethal_touch(pi, e, DeathCause::Animation);
            } else {
                set_state(EntState::Wander);
                if (frame_on) wander(1.6f); else e.speed = 0;
                if (los && pd < 22.0f * sense) { set_state(EntState::Chase); e.target = pi; }
            }
            break;
        }
        // ----------------------------------------------------------- Fallen angel (2Two)
        // Black wings, blue robes, wounds that never close. Docile: it only turns to watch you.
        case EntityType::FallenAngel: {
            e.speed = 0;
            if (tp && pd < 10.0f) e.yaw = turn_toward(e.yaw, angle_to(e.pos, ppos), 1.2f * dt);
            if (tp && pd < 3.0f && e.stare_t <= 0) { toast(pi, "FAD3 INT0 TH3 SH4DY GR3Y"); e.stare_t = 1; }
            break;
        }
        default: break;
    }
}


// 1.2: hostile things never come within SPAWN_SAFE_R of where you arrive on a level, or into
// a M.E.G. outpost. They walk up to the edge and stop there.
bool GameSim::keep_out(Entity& e) {
    if (friendly(e.type) || world.id() == LevelId::LRun || sandbox) return false; // (sandbox: spawn things wherever you like)
    bool moved = false;
    auto push = [&](Vec2 c, float r) {
        Vec2 d = e.pos - c;
        float l = d.len();
        if (l >= r) return;
        if (l < 1e-3f) d = {1, 0}, l = 1;
        e.pos = c + d * (r / l);
        moved = true;
    };
    Vec3 s = world.spawn_point();
    push({s.x, s.z}, SPAWN_SAFE_R);
    if (world.has_outposts()) {
        int cx = floor_to_int(e.pos.x / CELL), cz = floor_to_int(e.pos.z / CELL);
        int rx = floor_div(cx + 12, 24), rz = floor_div(cz + 12, 24);
        for (int b = -1; b <= 1; b++)
            for (int a = -1; a <= 1; a++) {
                int ocx, ocz;
                if (world.outpost_region(rx + a, rz + b, ocx, ocz)) push(cell_center(ocx, ocz), OUTPOST_SAFE_R);
            }
    }
    if (moved && e.state == EntState::Chase && e.type != EntityType::Partygoer) { e.state = EntState::Search; e.state_t = 0; e.path.clear(); }
    return moved;
}


// ------------------------------------------------------------------ 1.3 doors and drawers
// the nearest piece of furniture of some sort in front of a player, within reach (the client uses it for prompts)
bool furniture_in_front(const World& w, const PlayerState& p, bool doors, Furn& out, float reach) {
    Vec2 me{p.pos.x, p.pos.z}, f = yaw_forward(p.yaw);
    int pcx = floor_to_int(me.x / CELL), pcz = floor_to_int(me.z / CELL);
    float best = 1e9f;
    for (int cz = pcz - 1; cz <= pcz + 1; cz++)
        for (int cx = pcx - 1; cx <= pcx + 1; cx++) {
            Furn fs[MAX_FURN];
            int n = w.furniture(cx, cz, fs);
            for (int k = 0; k < n; k++) {
                const Furn& d = fs[k];
                bool is_door = d.kind == FurnKind::DoorLeaf && d.width > 0;
                if (doors ? !is_door : !World::searchable(d.kind)) continue;
                Vec2 c = is_door ? d.hinge + (d.shut_dir + (w.door_open(World::door_id(d)) ? d.open_dir : d.shut_dir)) * (d.width * 0.25f)
                                 : Vec2{(d.x0 + d.x1) * 0.5f, (d.z0 + d.z1) * 0.5f};
                // distance to the box, so big furniture can be reached from its edge
                float qx = clampf(me.x, d.x0, d.x1), qz = clampf(me.z, d.z0, d.z1);
                float dist = is_door ? (c - me).len() : (Vec2{qx, qz} - me).len();
                Vec2 to = c - me;
                float tl = to.len();
                if (dist > reach || (tl > 0.3f && (to.x * f.x + to.z * f.z) / tl < 0.35f)) continue; // not in front of you
                if (dist < best) { best = dist; out = d; }
            }
        }
    return best < 1e8f;
}

bool GameSim::use_door(int i, bool quiet) {
    PlayerState& p = players[i].st;
    Furn d;
    if (!furniture_in_front(world, p, true, d, 1.7f)) return false;
    uint32_t id = World::door_id(d);
    bool open = world.door_open(id);
    if (!open && world.door_jammed(id) && !pried.count(id)) {
        if (p.has(ItemType::Crowbar)) {
            pried.insert(id);
            if (door_flip.count(id)) door_flip.erase(id); else door_flip.insert(id);
            if (!quiet) toast(i, "YOU PRY THE DOOR OPEN");
            p.swing = 0; // (it takes a swing)
        } else if (!quiet) {
            toast(i, "IT'S JAMMED. SOMETHING COULD PRY IT OPEN");
        }
        return true;
    }
    if (door_flip.count(id)) door_flip.erase(id); else door_flip.insert(id);
    // a door must not shut on anyone: if it would, it stays open
    if (open) {
        for (int j = 0; j < IRIS_MAX_PLAYERS; j++) {
            if (!players[j].active || !players[j].st.alive) continue;
            Vec2 q{players[j].st.pos.x, players[j].st.pos.z};
            Vec2 a = d.hinge, b = d.hinge + d.shut_dir * d.width;
            Vec2 ab = b - a;
            float t = clampf(((q - a).x * ab.x + (q - a).z * ab.z) / (ab.x * ab.x + ab.z * ab.z), 0, 1);
            if ((a + ab * t - q).len() < PLAYER_RADIUS + 0.06f) {
                if (door_flip.count(id)) door_flip.erase(id); else door_flip.insert(id);
                return true;
            }
        }
    }
    return true;
}

bool GameSim::search_furniture(int i) {
    PlayerState& p = players[i].st;
    Furn f;
    if (!furniture_in_front(world, p, false, f, 1.3f)) return false;
    uint32_t id = World::furn_id(f);
    if (searched.count(id)) { toast(i, "YOU ALREADY WENT THROUGH THIS"); return true; }
    searched.insert(id);
    ItemType t = world.container_item(f);
    if (t == ItemType::None) { toast(i, "NOTHING INSIDE"); return true; }
    const ItemInfo& info = item_info(t);
    uint8_t& have = p.inv[(int)t];
    const int cap = t == ItemType::Shells ? 12 : (info.tool ? 1 : MAX_ITEM_STACK);
    if (have >= cap) { spawn_item(t, {p.pos.x, p.pos.z}); toast(i, std::string("FOUND ") + info.name + " (NO ROOM: LEFT AT YOUR FEET)"); return true; }
    have = (uint8_t)std::min(cap, have + (t == ItemType::Shells ? 2 : 1));
    toast(i, std::string("FOUND ") + info.name);
    return true;
}

// An entity that walks into a shut door opens it (you'll hear it). Jammed doors stop it.
void GameSim::entity_doors(Entity& e) {
    if (e.type == EntityType::Smiler) return;
    int cx = floor_to_int(e.pos.x / CELL), cz = floor_to_int(e.pos.z / CELL);
    for (int b = -1; b <= 1; b++)
        for (int a = -1; a <= 1; a++) {
            Furn fs[MAX_FURN];
            int n = world.furniture(cx + a, cz + b, fs);
            for (int k = 0; k < n; k++) {
                const Furn& d = fs[k];
                if (d.kind != FurnKind::DoorLeaf || d.width <= 0) continue;
                uint32_t id = World::door_id(d);
                if (world.door_open(id) || (world.door_jammed(id) && !pried.count(id))) continue;
                Vec2 mid = d.hinge + d.shut_dir * (d.width * 0.5f);
                if ((mid - e.pos).len() > 0.95f) continue;
                if (door_flip.count(id)) door_flip.erase(id); else door_flip.insert(id);
                return;
            }
        }
}

// Talking to a M.E.G. official. The medic patches you up and hands out supplies; the scout
// tells you where the nearest exit is and walks with you for a while, rifle up.
void GameSim::meg_talk(int i, Entity& e) {
    SimPlayer& sp = players[i];
    PlayerState& p = sp.st;
    if (e.attack_cd > 0) { toast(i, e.type == EntityType::MegMedic ? "M.E.G. MEDIC: GIVE ME A MINUTE, WANDERER." : "M.E.G. SCOUT: ALREADY ON IT. STAY CLOSE."); return; }
    e.yaw = angle_to(e.pos, {p.pos.x, p.pos.z});
    if (e.type == EntityType::MegMedic) {
        static const char* LINES[] = {"M.E.G. MEDIC: HOLD STILL. THIS WILL STING.", "M.E.G. MEDIC: YOU LOOK TERRIBLE. HERE.",
                                      "M.E.G. MEDIC: DRINK THIS. SLOWLY.", "M.E.G. MEDIC: KEEP YOUR LIGHTS ON OUT THERE."};
        p.health = 100;
        p.sanity = std::min(100.0f, p.sanity + 45.0f);
        p.stamina = 1;
        p.exhausted = false;
        p.battery = 1;
        if (p.juice >= 0 && p.juice < JUICE_TIME - 6.0f) p.juice = -1; // they have cashew water on hand
        auto give = [&](ItemType t, int n) {
            int cap = t == ItemType::Shells ? 12 : MAX_ITEM_STACK;
            p.inv[(int)t] = (uint8_t)std::min(cap, p.inv[(int)t] + n);
        };
        give(ItemType::AlmondWater, 1);
        if (rng_.f01() < 0.5f) give(ItemType::Batteries, 1);
        if (p.has(ItemType::Shotgun) && rng_.f01() < 0.6f) give(ItemType::Shells, 2);
        toast(i, LINES[(int)(rng_.f01() * 4) % 4]);
        e.attack_cd = 75.0f;
    } else {
        Vec2 ex;
        ExitDoor door;
        bool found = nearest_exit({p.pos.x, p.pos.z}, ex, &door);
        if (found) {
            Vec2 d = ex - Vec2{p.pos.x, p.pos.z};
            const char* dir[8] = {"NORTH", "NORTH-EAST", "EAST", "SOUTH-EAST", "SOUTH", "SOUTH-WEST", "WEST", "NORTH-WEST"};
            float ang = std::atan2(d.x, -d.z); // 0 = north (-z)
            int k = ((int)std::lround(ang / (PI / 4)) + 8) % 8;
            char buf[96];
            std::snprintf(buf, sizeof buf, "M.E.G. SCOUT: EXIT ABOUT %d M %s. I'LL WALK YOU PART OF THE WAY.", (int)d.len(), dir[k]);
            toast(i, buf);
        } else {
            toast(i, "M.E.G. SCOUT: NO EXIT ON OUR MAPS AROUND HERE. I'LL COVER YOU FOR A BIT.");
        }
        e.target = i;
        e.state = EntState::Chase; // (escorting)
        e.state_t = 0;
        e.stare_t = 100.0f;        // seconds of escort left
        e.attack_cd = 140.0f;
    }
}

void GameSim::update_meg(Entity& e, float dt) {
    e.fire_cd = std::max(0.0f, e.fire_cd - dt);
    // shoot anything hostile that gets close (to it, or to whoever it's escorting)
    if (e.fire_cd <= 0) {
        float best = e.type == EntityType::MegScout ? 16.0f : 9.0f;
        Entity* tgt = nullptr;
        for (Entity& o : ents) {
            if (friendly(o.type) || unstoppable(o.type) || o.type == EntityType::Smiler || o.pos.x > 1e5f) continue;
            float d = (o.pos - e.pos).len();
            if (d < best && !world.segment_blocked(e.pos, o.pos)) { best = d; tgt = &o; }
        }
        if (tgt) {
            e.yaw = angle_to(e.pos, tgt->pos);
            e.fire_cd = e.type == EntityType::MegScout ? 0.9f : 1.6f;
            e.anim = 0; // (the renderer uses speed 0 + fire_cd for the aim pose)
            tgt->hp -= e.type == EntityType::MegScout ? 22.0f : 12.0f;
            if (tgt->hp <= 0) { event(std::string(entity_name(e.type)) + " TOOK DOWN A " + entity_name(tgt->type)); tgt->pos = {1e6f, 1e6f}; tgt->state = EntState::Retreat; }
            else { tgt->state = EntState::Stunned; tgt->state_t = 2.5f; tgt->path.clear(); }
            e.speed = 0;
            return;
        }
    }
    if (e.type == EntityType::MegScout && e.state == EntState::Chase && e.target >= 0) { // escorting
        e.stare_t -= dt;
        const SimPlayer& sp = players[e.target];
        if (!sp.active || !sp.st.alive || e.stare_t <= 0) {
            if (sp.active) toast(e.target, "M.E.G. SCOUT: THIS IS AS FAR AS I GO. GOOD LUCK, WANDERER.");
            e.state = EntState::Retreat; e.state_t = 0; e.target = -1;
            return;
        }
        Vec2 pp{sp.st.pos.x, sp.st.pos.z};
        if ((pp - e.pos).len() > 2.2f) move_entity(e, pp, std::max(2.4f, sp.st.speed * 1.05f), dt);
        else e.speed = 0;
        if ((pp - e.pos).len() > 40.0f) { e.pos = pp - yaw_forward(sp.st.yaw) * 1.5f; collide_circle(world, e.pos, 0.3f); } // (catches up)
        return;
    }
    // at the outpost: mill about near home
    if ((e.home - e.pos).len() > 3.5f) { move_entity(e, e.home, 1.6f, dt); return; }
    if (e.state == EntState::Retreat) e.state = EntState::Wander;
    if (e.state_t > 7.0f || (e.goal - e.pos).len() < 0.5f || e.goal.x == 0) {
        e.goal = e.home + Vec2{rng_.range(-2.0f, 2.0f), rng_.range(-2.0f, 2.0f)};
        e.state_t = 0;
    }
    if (std::fmod(e.state_t, 7.0f) < 3.5f) move_entity(e, e.goal, 0.8f, dt);
    else e.speed = 0;
    float pd;
    int pi = closest_player(e.pos, &pd);
    if (pi >= 0 && pd < 5.0f) { e.speed = 0; e.yaw = turn_toward(e.yaw, angle_to(e.pos, {players[pi].st.pos.x, players[pi].st.pos.z}), 3 * dt); }
}

// The officials walk out to meet you when you come near an outpost.
void GameSim::spawn_outposts() {
    if (!world.has_outposts() || !auto_spawn) return;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        const SimPlayer& sp = players[i];
        if (!sp.active || !sp.st.alive) continue;
        int cx = floor_to_int(sp.st.pos.x / CELL), cz = floor_to_int(sp.st.pos.z / CELL);
        int rx = floor_div(cx + 12, 24), rz = floor_div(cz + 12, 24);
        for (int b = -1; b <= 1; b++)
            for (int a = -1; a <= 1; a++) {
                int ocx, ocz;
                if (!world.outpost_region(rx + a, rz + b, ocx, ocz)) continue;
                Vec2 c = cell_center(ocx, ocz);
                if ((c - Vec2{sp.st.pos.x, sp.st.pos.z}).len() > 45.0f) continue;
                int key = pack(ocx, ocz);
                if (outposts_up.count(key)) continue;
                outposts_up.insert(key);
                Entity& m = spawn_entity(EntityType::MegMedic, c + Vec2{0.0f, CELL * 0.75f}, 0);
                m.home = m.pos;
                Entity& s = spawn_entity(EntityType::MegScout, c + Vec2{-CELL * 0.6f, -CELL * 0.6f}, 0);
                s.home = s.pos;
            }
    }
}

// Sandbox actions. Single player calls this directly; in multiplayer it arrives from a player.
void GameSim::sandbox_cmd(int i, uint8_t action, uint8_t arg, float yaw) {
    if (!sandbox || i < 0 || i >= IRIS_MAX_PLAYERS || !players[i].active) return;
    PlayerState& me = players[i].st;
    Vec2 here{me.pos.x, me.pos.z}, f = yaw_forward(yaw);
    auto ahead = [&](float dist) { // in front, never inside a wall
        Vec2 at = here;
        for (float d = 0.5f; d <= dist; d += 0.25f) {
            Vec2 c = here + f * d;
            if (world.segment_blocked(here, c) || world.solid(floor_to_int(c.x / CELL), floor_to_int(c.z / CELL))) break;
            at = c;
        }
        return at;
    };
    switch (action) {
        case SB_SPAWN_ENTITY:
            if (arg < (uint8_t)EntityType::COUNT && ents.size() < 48) {
                Entity& e = spawn_entity((EntityType)arg, ahead(4.5f), yaw + PI);
                e.home = e.pos;
                event(players[i].name + " SPAWNED " + entity_name((EntityType)arg));
            }
            break;
        case SB_SPAWN_ITEM:
            if (arg > 0 && arg < ITEM_COUNT && props.size() < 120) spawn_item((ItemType)arg, ahead(1.5f));
            break;
        case SB_GOD: god = !god; event(std::string("GOD MODE ") + (god ? "ON" : "OFF")); break;
        case SB_NOCLIP: me.noclip = !me.noclip; break;
        case SB_AUTOSPAWN: auto_spawn = !auto_spawn; event(std::string("ENTITIES SPAWN: ") + (auto_spawn ? "ON" : "OFF")); break;
        case SB_FREEZE: freeze_ents = !freeze_ents; break;
        case SB_CLEAR: ents.clear(); outposts_up.clear(); event("ENTITIES CLEARED"); break;
        case SB_HEAL: me.health = 100; me.sanity = 100; me.stamina = 1; me.juice = -1; break;
        case SB_TO_EXIT: { Vec2 ex; if (nearest_exit(here, ex)) { me.pos.x = ex.x; me.pos.z = ex.z; } break; }
        case SB_NEXT: next_level(); break;
        default: break;
    }
}

// The host picks a level in the lobby (or the game sends everyone back there).
void GameSim::start_run(LevelId level, Difficulty d, bool sandbox_mode, uint32_t seed) {
    if ((int)level >= (int)LevelId::COUNT) level = LevelId::L0;
    difficulty = d;
    sandbox = sandbox_mode;
    god = false; auto_spawn = true; freeze_ents = false;
    escaped = false; won_t = 0;
    objectives_done = 0;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        std::fill(players[i].perks, players[i].perks + PERK_COUNT, (uint8_t)0);
        respawn(i);
        players[i].st.inv[(int)ItemType::AlmondWater] = 1;
        if (sandbox_mode) for (int t = 1; t < ITEM_COUNT; t++) players[i].st.inv[t] = item_info((ItemType)t).tool ? 1 : 3;
    }
    start_level(level, seed ? seed : 1);
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
            float dist = (pp - c).len();
            // hidden = behind something, or far enough that the haze hides it
            if (dist < min_d || (dist < 34.0f && !world.segment_blocked(pp, c))) { ok = false; break; }
        }
        if (!ok) continue;
        { // never in the safe zones
            Vec3 s = world.spawn_point();
            if ((Vec2{s.x, s.z} - c).len() < SPAWN_SAFE_R + 6.0f) continue;
            Entity probe;
            probe.type = EntityType::Bacteria;
            probe.pos = c;
            if (keep_out(probe)) continue;
        }
        if (need_dark && light_at({c.x, 1.5f, c.z}) > 0.3f) continue;
        out = c;
        return true;
    }
    return false;
}

void GameSim::spawn_entities() {
    // despawn anything that drifted far from everyone (or faded away)
    const bool day = !world.night(level_time);
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        float d;
        closest_player(e.pos, &d);
        if (day && auto_spawn && (world.id() == LevelId::TSG0 || e.type == EntityType::Animation)) return true; // gone when the sun comes up
        if ((e.type == EntityType::MegMedic || e.type == EntityType::MegScout) && d > 75.0f) { // back at their post next time you pass
            int ocx, ocz;
            if (world.outpost(floor_to_int(e.home.x / CELL), floor_to_int(e.home.z / CELL), ocx, ocz)) outposts_up.erase(pack(ocx, ocz));
            return true;
        }
        return d > (e.type == EntityType::Mangled ? 110.0f : 75.0f);
    }), ents.end());
    spawn_outposts();
    if (auto_spawn && world.id() == LevelId::LRun) { // Level !: they come up the corridor behind you, already running
        if (level_time < 3.0f) return;
        for (const Population& pop : population(world.id(), world.night(level_time))) {
            int have = 0;
            for (const Entity& e : ents) have += e.type == pop.type;
            int want = std::max(1, (int)std::lround(pop.count * world.diff().ent_count));
            if (have >= want) continue;
            float back = -1e9f; // the rearmost player
            int who = -1;
            for (int i = 0; i < IRIS_MAX_PLAYERS; i++)
                if (players[i].active && players[i].st.alive && players[i].st.pos.z > back) { back = players[i].st.pos.z; who = i; }
            if (who < 0) return;
            if (pop.type == EntityType::Smiler) { // waiting up ahead, in the dark
                float front = 1e9f;
                for (int i = 0; i < IRIS_MAX_PLAYERS; i++)
                    if (players[i].active && players[i].st.alive) front = std::min(front, players[i].st.pos.z);
                int ahead_z = floor_to_int(front / CELL) - 12 - (int)rng_.range(0, 6);
                for (int cx = -1; cx <= 1; cx++) {
                    if (ahead_z < -66 || !world.open(cx, ahead_z)) continue;
                    spawn_entity(pop.type, cell_center(cx, ahead_z), 0);
                    return;
                }
                continue;
            }
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
    if (ents.size() >= 16) return;
    for (const Population& pop : population(world.id(), world.night(level_time))) {
        int have = 0;
        for (const Entity& e : ents) have += e.type == pop.type;
        int want = std::max(1, (int)std::lround(pop.count * world.diff().ent_count));
        if (have >= want) continue;
        Vec2 at;
        if (!find_spawn_cell(at, 22.0f, 42.0f, pop.type == EntityType::Smiler)) continue;
        spawn_entity(pop.type, at, rng_.range(-PI, PI));
        return; // one per spawn pass
    }
}

void GameSim::tick(float dt) {
    level_time += dt;
    if (escaped) won_t += dt;
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
