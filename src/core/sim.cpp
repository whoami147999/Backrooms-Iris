#include "sim.h"

#include <algorithm>
#include <cstdio>
#include <queue>
#include <unordered_map>

#include "collision.h"

namespace iris {

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
        case LevelId::L5: return {{EntityType::Partygoer, 3}, {EntityType::Smiler, 1}};
        case LevelId::L8: return {{EntityType::Smiler, 4}, {EntityType::Hound, 1}};
        case LevelId::L9: return {{EntityType::SkinStealer, 2}, {EntityType::Hound, 2}};
        default: return {}; // Level 11 is safe
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

// =================================================================== lifecycle
void GameSim::start_level(LevelId id, uint32_t seed) {
    world = World(id, seed);
    epoch++;
    level_time = 0;
    ents.clear();
    picked.clear();
    picked_order.clear();
    spawn_t_ = 0;
    pending_level_ = LevelId::COUNT;
    int n = 0;
    for (auto& p : players) {
        if (!p.active) continue;
        // keep health/sanity/inventory across levels, but move everyone to the new spawn
        Vec3 s = world.spawn_point();
        p.st.pos = {s.x + (n % 2) * 0.8f - 0.4f, 0, s.z + (n / 2) * 0.8f - 0.4f};
        p.st.vel = {0, 0};
        if (!p.st.alive) respawn((int)(&p - players));
        n++;
    }
    event(std::string("ENTERED ") + level_label(id));
}

int GameSim::add_player(const std::string& name) {
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        if (players[i].active) continue;
        players[i] = SimPlayer{};
        players[i].active = true;
        players[i].name = name.empty() ? "WANDERER" : name;
        respawn(i);
        players[i].st.inv[0] = 1; // everyone starts with one almond water
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
    uint8_t keep_inv[3] = {0, 0, 0};
    p.st = PlayerState{};
    std::copy(keep_inv, keep_inv + 3, p.st.inv);
    Vec3 s = world.spawn_point();
    p.st.pos = {s.x + (i % 2) * 0.8f - 0.4f, 0, s.z + (i / 2) * 0.8f - 0.4f};
    p.st.yaw = p.last_cmd.yaw;
    p.st.sanity = 80;
    p.respawn_t = 0;
}

void GameSim::event(const std::string& text) {
    events.push_back({++event_seq, text});
    while (events.size() > 8) events.pop_front();
}

// =================================================================== players
bool GameSim::looking_at(const PlayerState& p, Vec2 target, float max_dist, float cos_fov) const {
    Vec2 d = target - Vec2{p.pos.x, p.pos.z};
    float dist = d.len();
    if (dist > max_dist || dist < 1e-3f) return dist < 1e-3f;
    Vec2 f = yaw_forward(p.yaw);
    if ((d.x * f.x + d.z * f.z) / dist < cos_fov) return false;
    return !world.segment_blocked({p.pos.x, p.pos.z}, target);
}

int GameSim::nearest_item(const PlayerState& p, ItemType* type) const {
    int pcx = floor_to_int(p.pos.x / CELL), pcz = floor_to_int(p.pos.z / CELL);
    int best = -1;
    float best_d = PICKUP_RANGE;
    for (int cz = pcz - 1; cz <= pcz + 1; cz++)
        for (int cx = pcx - 1; cx <= pcx + 1; cx++) {
            ItemType t = world.item(cx, cz);
            if (t == ItemType::None || picked.count(World::item_id(cx, cz))) continue;
            float d = (world.item_pos(cx, cz) - Vec2{p.pos.x, p.pos.z}).len();
            if (d < best_d) {
                best_d = d;
                best = (int)World::item_id(cx, cz);
                if (type) *type = t;
            }
        }
    return best;
}

void GameSim::try_pickup(int i) {
    PlayerState& p = players[i].st;
    ItemType t = ItemType::None;
    int id = nearest_item(p, &t);
    if (id < 0) return;
    int slot = (int)t - 1;
    if (p.inv[slot] >= MAX_ITEM_STACK) return;
    p.inv[slot]++;
    picked.insert((uint32_t)id);
    picked_order.push_back((uint32_t)id);
}

void GameSim::use_item(int i, int slot) {
    PlayerState& p = players[i].st;
    if (slot < 0 || slot > 2 || p.inv[slot] == 0 || !p.alive) return;
    p.inv[slot]--;
    switch (slot) {
        case 0: // almond water: calms you down and puts the wind back in your lungs
            p.stamina = 1;
            p.exhausted = false;
            p.sanity = std::min(100.0f, p.sanity + 30.0f);
            break;
        case 1: // energy drink: fast for a little while, no stamina cost
            p.boost = ENERGY_TIME;
            p.stamina = 1;
            p.exhausted = false;
            break;
        case 2: // memory juice: you'll see things. then you won't see anything.
            if (p.juice < 0) p.juice = 0;
            event(players[i].name + " DRANK THE MEMORY JUICE");
            break;
    }
}

void GameSim::apply_input(int i, const InputCmd& cmd) {
    SimPlayer& sp = players[i];
    if (!sp.active) return;
    // action counters: act on every change since the last command we saw
    if (sp.st.alive) {
        if (cmd.pickup_ctr != sp.last_cmd.pickup_ctr) try_pickup(i);
        for (int s = 0; s < 3; s++)
            if (cmd.use_ctr[s] != sp.last_cmd.use_ctr[s]) use_item(i, s);
    }
    sp.last_cmd = cmd;
    player_tick(sp.st, cmd, world, 1.0f / TICK_RATE);
}

void GameSim::kill(int i, DeathCause c) {
    PlayerState& p = players[i].st;
    if (!p.alive) return;
    p.alive = false;
    p.health = 0;
    p.cause = c;
    p.inv[0] = p.inv[1] = p.inv[2] = 0;
    players[i].respawn_t = 10.0f;
    event(players[i].name + ": " + death_text(c));
}

void GameSim::damage(int i, float amount, DeathCause c) {
    PlayerState& p = players[i].st;
    if (!p.alive) return;
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
    if (p.boost > 0) {
        p.boost -= dt;
        if (p.boost <= 0) { p.boost = 0; p.stamina = 0.15f; p.exhausted = true; } // the crash
    }
    if (p.juice >= 0) {
        p.juice += dt;
        p.sanity = std::max(0.0f, p.sanity - dt * 1.2f);
        if (p.juice >= JUICE_TIME) { kill(i, DeathCause::MemoryJuice); return; }
    }

    // --- sanity: the level wears on you; darkness and things you see make it worse,
    // company makes it better.
    const LevelInfo& li = world.info();
    float drain = li.sanity_drain;
    float light = world.light_level(p.pos);
    if (light < 0.35f) drain += 0.35f * (1.0f - light / 0.35f);
    sp.near_ally = 0;
    for (int j = 0; j < IRIS_MAX_PLAYERS; j++) {
        if (j == i || !players[j].active || !players[j].st.alive) continue;
        if ((players[j].st.pos - p.pos).len() < 10.0f) sp.near_ally = 1;
    }
    if (sp.near_ally > 0) drain -= 0.25f;
    for (const Entity& e : ents) {
        float d = (e.pos - Vec2{p.pos.x, p.pos.z}).len();
        if (d > 18.0f) continue;
        if (!looking_at(p, e.pos, 18.0f, 0.55f)) continue;
        drain += e.type == EntityType::Smiler ? 2.5f : 1.2f;
    }
    p.sanity = clampf(p.sanity - drain * dt, 0, 100);
    if (p.sanity <= 0) {
        damage(i, 3.0f * dt, DeathCause::Sanity);
        if (!p.alive) return;
    }
    // slow health regen once you've been left alone for a bit
    if (p.hurt > 8.0f && p.health < 100) p.health = std::min(100.0f, p.health + 1.5f * dt);

    // --- exits
    int cx = floor_to_int(p.pos.x / CELL), cz = floor_to_int(p.pos.z / CELL);
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!world.exit_cell(cx + dx, cz + dz)) continue;
            Vec2 c = cell_center(cx + dx, cz + dz);
            if ((c - Vec2{p.pos.x, p.pos.z}).len() < 0.95f) {
                if (world.id() == LevelId::L11) { escaped = true; }
                pending_level_ = world.info().next;
            }
        }
}

// =================================================================== entities
int GameSim::closest_player(Vec2 pos, float* dist, bool need_los) const {
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++) {
        const SimPlayer& sp = players[i];
        if (!sp.active || !sp.st.alive) continue;
        Vec2 pp{sp.st.pos.x, sp.st.pos.z};
        float d = (pp - pos).len();
        if (d >= bd) continue;
        if (need_los && world.segment_blocked(pos, pp)) continue;
        bd = d;
        best = i;
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
    // straight line when visible, otherwise follow the BFS path
    Vec2 aim = goal;
    if (world.segment_blocked(e.pos, goal)) {
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
    } else {
        e.path.clear();
    }
    Vec2 d = aim - e.pos;
    float len = d.len();
    float turn = e.type == EntityType::Bacteria ? 4.0f : 7.0f;
    if (len > 0.05f) e.yaw = turn_toward(e.yaw, angle_to(e.pos, aim), turn * dt);
    float step = std::min(len, speed * dt);
    Vec2 before = e.pos;
    if (len > 1e-4f) e.pos += d * (step / len);
    if (e.type != EntityType::Smiler) collide_circle(world, e.pos, ent_radius(e.type));
    e.speed = (e.pos - before).len() / std::max(dt, 1e-4f);
    e.anim += e.speed * dt * 2.2f;
}

void GameSim::update_entity(Entity& e, float dt) {
    e.state_t += dt;
    e.attack_cd = std::max(0.0f, e.attack_cd - dt);
    float pd = 1e9f;
    int pi = closest_player(e.pos, &pd, false);
    const PlayerState* tp = pi >= 0 ? &players[pi].st : nullptr;
    Vec2 ppos = tp ? Vec2{tp->pos.x, tp->pos.z} : e.pos;
    bool los = tp && pd < 30.0f && !world.segment_blocked(e.pos, ppos);

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
            if (e.state == EntState::Wander) {
                wander(1.4f);
                if (los && pd < 24.0f) { set_state(EntState::Notice); e.target = pi; }
            } else if (e.state == EntState::Notice) {
                if (tp) e.yaw = turn_toward(e.yaw, angle_to(e.pos, ppos), 6 * dt);
                if (e.state_t > 1.1f) set_state(EntState::Chase);
            } else if (e.state == EntState::Chase) {
                if (los) e.last_seen = ppos;
                move_entity(e, los ? ppos : e.last_seen, 4.95f, dt);
                if (!los && e.state_t > 0.5f && (e.last_seen - e.pos).len() < 1.0f) set_state(EntState::Search);
                if (los) e.state_t = 0;
                if (e.state_t > 3.5f) set_state(EntState::Search);
                if (contact(0.9f)) kill(pi, DeathCause::Bacteria);
            } else { // Search
                wander(2.0f);
                if (los && pd < 18.0f) set_state(EntState::Chase);
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
                if (d < sp.st.noise() && d < hd) { hd = d; heard = i; }
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
                // back off slowly while you stare it down
                Vec2 away = e.pos - tpos;
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
        // it, and it comes for you. Light drives it off.
        case EntityType::Smiler: {
            float light = world.light_level({e.pos.x, 1.5f, e.pos.z});
            if (light > 0.45f) {
                set_state(EntState::Retreat);
                Vec2 away = e.pos - ppos;
                float al = away.len();
                if (al > 1e-3f) move_entity(e, e.pos + away * (1.0f / al), 2.0f, dt);
                if (e.state_t > 4.0f) e.pos = {1e6f, 1e6f}; // fades away; despawned next tick
                break;
            }
            if (e.state == EntState::Charge) {
                move_entity(e, e.last_seen, 6.4f, dt);
                if (contact(0.9f)) kill(pi, DeathCause::Smiler);
                if (e.state_t > 2.6f || (e.last_seen - e.pos).len() < 0.5f) { e.stare_t = 0; set_state(EntState::Wander); }
                break;
            }
            if (!tp || pd > 20.0f) { set_state(EntState::Wander); wander(0.5f); break; }
            bool watched = looking_at(*tp, e.pos, 20.0f, 0.9f);
            if (watched) e.stare_t += dt;
            else e.stare_t = std::max(0.0f, e.stare_t - dt * 0.5f);
            bool provoked = e.stare_t > 2.6f || (pd < 9.0f && tp->speed > WALK_SPEED * 1.2f) || pd < 2.0f;
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
                if (los && pd < 26.0f) set_state(EntState::Chase);
            }
            break;
        }
        // ----------------------------------------------------------- Partygoers
        // =) They want you at the party. Slower than you, but they never, ever stop.
        case EntityType::Partygoer: {
            if (tp && pd < 16.0f) {
                set_state(EntState::Chase);
                e.last_seen = ppos;
            }
            if (e.state == EntState::Chase) {
                if (!tp || pd > 45.0f) { set_state(EntState::Wander); break; }
                move_entity(e, ppos, 2.05f, dt);
                if (contact(0.85f)) kill(pi, DeathCause::Partygoer);
            } else {
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
            if ((pp - c).len() < min_d || !world.segment_blocked(pp, c)) { ok = false; break; }
        }
        if (!ok) continue;
        if (need_dark && world.light_level({c.x, 1.5f, c.z}) > 0.3f) continue;
        out = c;
        return true;
    }
    return false;
}

void GameSim::spawn_entities() {
    // despawn anything that drifted far from everyone
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        float d;
        closest_player(e.pos, &d, false);
        return d > 75.0f;
    }), ents.end());
    if (level_time < 35.0f) return; // a moment to get your bearings
    for (const Population& pop : population(world.id())) {
        int have = 0;
        for (const Entity& e : ents) have += e.type == pop.type;
        if (have >= pop.count) continue;
        Vec2 at;
        bool dark = pop.type == EntityType::Smiler;
        if (!find_spawn_cell(at, 26.0f, 44.0f, dark)) continue;
        Entity e;
        e.id = next_ent_id_++;
        if (next_ent_id_ == 0) next_ent_id_ = 1;
        e.type = pop.type;
        e.pos = at;
        e.goal = at;
        e.yaw = rng_.range(-PI, PI);
        ents.push_back(e);
        return; // one per spawn pass
    }
}

void GameSim::tick(float dt) {
    level_time += dt;
    for (int i = 0; i < IRIS_MAX_PLAYERS; i++)
        if (players[i].active) update_player_stats(i, dt);
    if (!freeze_ents) for (Entity& e : ents) update_entity(e, dt);
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
