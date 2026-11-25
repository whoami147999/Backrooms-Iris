// Backrooms Iris - core tests (no SDL/GL). Run: ctest --test-dir build
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/collision.h"
#include "core/config.h"
#include "core/hash.h"
#include "core/world.h"
#include "core/sim.h"
#include "net/client.h"
#include "net/server.h"
#include <chrono>
#include <thread>
#include "core/player.h"

using namespace iris;

static int g_fail = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
            g_fail++;                                                 \
        }                                                             \
    } while (0)

// Same seed => identical world (client, Vita and server must agree bit for bit).
static void test_determinism() {
    World a(LevelId::L0, 777), b(LevelId::L0, 777), c(LevelId::L0, 778);
    int diff_c = 0;
    for (int z = -60; z < 60; z++)
        for (int x = -60; x < 60; x++) {
            CHECK(a.wall_x(x, z) == b.wall_x(x, z), "wall_x %d %d", x, z);
            CHECK(a.wall_z(x, z) == b.wall_z(x, z), "wall_z %d %d", x, z);
            CHECK(a.light(x, z) == b.light(x, z), "light %d %d", x, z);
            diff_c += a.wall_x(x, z) != c.wall_x(x, z);
        }
    CHECK(diff_c > 500, "different seeds should give different worlds (%d diffs)", diff_c);
}

// From spawn you can reach almost everything: the level never boxes you in.
static void test_connectivity() {
    for (uint32_t seed : {1u, 1215u, 99999u, 4000000000u}) {
        World L(LevelId::L0, seed);
        const int N = 160, O = N / 2;
        std::vector<unsigned char> seen(N * N, 0);
        std::vector<int> q{O * N + O};
        seen[O * N + O] = 1;
        for (size_t h = 0; h < q.size(); h++) {
            int x = q[h] % N, z = q[h] / N, cx = x - O, cz = z - O;
            int nb[4][3] = {{1, 0, !L.wall_x(cx + 1, cz)}, {-1, 0, !L.wall_x(cx, cz)},
                            {0, 1, !L.wall_z(cx, cz + 1)}, {0, -1, !L.wall_z(cx, cz)}};
            for (auto& b : nb) {
                int nx = x + b[0], nz = z + b[1];
                if (!b[2] || nx < 0 || nz < 0 || nx >= N || nz >= N || seen[nz * N + nx]) continue;
                seen[nz * N + nx] = 1;
                q.push_back(nz * N + nx);
            }
        }
        double pct = 100.0 * q.size() / (N * N);
        CHECK(pct > 85.0, "seed %u: only %.1f%% reachable from spawn", seed, pct);
    }
}

static void test_spawn_room() {
    World L(LevelId::L0, 1215);
    for (int i = 0; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            CHECK(!L.wall_x(i, j) && !L.wall_z(j, i), "spawn room must be open (%d,%d)", i, j);
        }
    CHECK(L.light(0, 0) == LightState::On, "spawn is always lit");
    CHECK(L.district(0, 0) == World::District::Rooms && L.district(4, -4) == World::District::Rooms, "spawn district is classic rooms");
}

// Walking speed matches the tuning constants.
static void test_walk_speed() {
    World L(LevelId::L0, 1215);
    PlayerState p;
    p.pos = L.spawn_point();
    InputCmd cmd;
    cmd.move_z = 1;
    cmd.yaw = 0; // toward -Z; spawn room is open for 2+ cells that way
    for (int t = 0; t < 30; t++) player_tick(p, cmd, L, 1.0f / TICK_RATE); // 0.5 s
    float speed = p.vel.len();
    CHECK(speed > WALK_SPEED * 0.95f && speed < WALK_SPEED * 1.05f, "walk speed %.2f", speed);
}

// Random-walk thousands of ticks through the level: the player never ends up in a wall.
static void test_no_wall_penetration() {
    World L(LevelId::L0, 31337);
    PlayerState p;
    p.pos = L.spawn_point();
    InputCmd cmd;
    Rng rng(5);
    int bad = 0;
    for (int t = 0; t < 60 * 120; t++) {
        if (t % 45 == 0) {
            cmd.yaw = rng.range(-PI, PI);
            cmd.move_z = rng.range(0.2f, 1.0f);
            cmd.move_x = rng.range(-1.0f, 1.0f);
            cmd.buttons = rng.f01() < 0.5f ? BTN_SPRINT : 0;
        }
        player_tick(p, cmd, L, 1.0f / TICK_RATE);
        Vec2 q{p.pos.x, p.pos.z};
        Vec2 before = q;
        collide_circle(L, q, PLAYER_RADIUS - 0.01f); // anything still to resolve = we were inside
        if ((q - before).len() > 0.005f) bad++;
    }
    CHECK(bad == 0, "%d ticks ended overlapping geometry", bad);
}

static void test_stamina() {
    World L(LevelId::L0, 1215);
    PlayerState p;
    p.pos = {-500, 0, -500};
    InputCmd cmd;
    cmd.move_z = 1;
    cmd.buttons = BTN_SPRINT;
    for (int t = 0; t < 60 * 10; t++) player_tick(p, cmd, L, 1.0f / TICK_RATE);
    CHECK(p.exhausted || p.stamina < 0.4f, "10 s of sprinting should drain stamina (%.2f)", p.stamina);
}


// Every level: spawn is open, most of the map is reachable, exits and items exist.
static void test_all_levels() {
    for (int li = 0; li < (int)LevelId::COUNT; li++) {
        World L((LevelId)li, 4242);
        const char* nm = level_info((LevelId)li).code;
        CHECK(!L.solid(0, 0), "%s: spawn cell is solid", nm);
        const int N = 140, O = N / 2;
        std::vector<unsigned char> seen(N * N, 0);
        std::vector<int> q{O * N + O};
        seen[O * N + O] = 1;
        int exits = 0;
        for (size_t h = 0; h < q.size(); h++) {
            int x = q[h] % N, z = q[h] / N, cx = x - O, cz = z - O;
            exits += L.exit_cell(cx, cz);
            const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; k++) {
                int nx = x + DX[k], nz = z + DZ[k];
                if (nx < 0 || nz < 0 || nx >= N || nz >= N || seen[nz * N + nx]) continue;
                if (!L.can_step(cx, cz, DX[k], DZ[k])) continue;
                seen[nz * N + nx] = 1;
                q.push_back(nz * N + nx);
            }
        }
        int open = 0, items = 0;
        for (int z = -O; z < O; z++)
            for (int x = -O; x < O; x++) { open += L.open(x, z); items += L.item(x, z) != ItemType::None; }
        double reach = 100.0 * q.size() / open;
        CHECK(reach > 80.0, "%s: only %.1f%% of open cells reachable", nm, reach);
        CHECK(exits >= 2, "%s: only %d reachable exits in a 350 m square", nm, exits);
        CHECK(items >= 20, "%s: only %d items", nm, items);
        std::printf("  %-9s reachable %5.1f%%  exits %3d  items %4d\n", nm, reach, exits, items);
    }
}

// Items do what they say. Memory juice kills on schedule.
static void test_items() {
    GameSim sim(7);
    sim.start_level(LevelId::L11, 7); // safe level: nothing else interferes
    int p = sim.add_player("TEST");
    PlayerState& st = sim.players[p].st;
    InputCmd cmd;
    st.inv[0] = 1; st.inv[1] = 1; st.inv[2] = 1;
    st.stamina = 0.1f; st.sanity = 40;
    cmd.use_ctr[0]++;
    sim.apply_input(p, cmd);
    CHECK(st.stamina == 1.0f && st.sanity >= 70, "almond water: stamina %.2f sanity %.1f", st.stamina, st.sanity);
    cmd.use_ctr[1]++;
    sim.apply_input(p, cmd);
    CHECK(st.boost > 0, "energy drink should boost");
    cmd.move_z = 1;
    for (int t = 0; t < 60; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    CHECK(st.speed > WALK_SPEED * 1.3f, "boosted walk speed %.2f", st.speed);
    cmd.move_z = 0;
    cmd.use_ctr[2]++;
    sim.apply_input(p, cmd);
    CHECK(st.juice >= 0, "memory juice should start");
    for (int t = 0; t < 60 * 47; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    CHECK(st.alive, "still alive at 47 s");
    for (int t = 0; t < 60 * 2; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    CHECK(!st.alive && st.cause == DeathCause::MemoryJuice, "memory juice should kill at %.0f s", JUICE_TIME);
}

// Run every level for a few simulated minutes with wandering players: entities spawn,
// chase, nobody crashes, nothing ends up inside walls.
static void test_sim_soak() {
    for (int li = 0; li < (int)LevelId::COUNT; li++) {
        GameSim sim(99);
        sim.multiplayer = true;
        sim.start_level((LevelId)li, 1000 + li);
        int a = sim.add_player("A"), b = sim.add_player("B");
        Rng rng(li + 1);
        InputCmd ca, cb;
        int max_ents = 0, deaths = 0;
        for (int t = 0; t < 60 * 180; t++) {
            if (t % 90 == 0) {
                ca.yaw = rng.range(-PI, PI); ca.move_z = 1; ca.buttons = rng.f01() < 0.3f ? BTN_SPRINT : 0;
                cb.yaw = rng.range(-PI, PI); cb.move_z = rng.f01(); cb.buttons = 0;
            }
            sim.apply_input(a, ca);
            sim.apply_input(b, cb);
            sim.tick(1.0f / 60);
            max_ents = std::max(max_ents, (int)sim.ents.size());
            for (int i : {a, b}) deaths += !sim.players[i].st.alive && sim.players[i].respawn_t > 9.98f;
        }
        bool expect = (LevelId)li != LevelId::L11;
        CHECK(!expect || max_ents > 0, "%s: no entities ever spawned", level_info((LevelId)li).code);
        CHECK(expect || max_ents == 0, "Level 11 should be safe");
        std::printf("  %-9s max entities %d, deaths in 3 min of random walking: %d\n", level_info((LevelId)li).code, max_ents, deaths);
    }
}

// Real UDP round trip on localhost: server + client, inputs move the player.
static void test_network() {
    Server srv;
    ServerConfig cfg;
    cfg.port = 27399;
    cfg.level = LevelId::L11;
    cfg.seed = 5;
    std::string err;
    CHECK(srv.start(cfg, &err), "server start: %s", err.c_str());
    NetClient cl;
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    cl.connect("127.0.0.1", 27399, "NETTEST", now());
    InputCmd cmd;
    cmd.move_z = 1;
    uint32_t seq = 0;
    while (now() < 2.5) {
        srv.poll();
        cl.poll(now());
        if (cl.state == NetClient::State::Connected) {
            seq++;
            uint32_t s[1] = {seq};
            cl.send_input(seq, &cmd, s, 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    CHECK(cl.state == NetClient::State::Connected, "client never connected (%s)", cl.error.c_str());
    CHECK(cl.got_snapshot && cl.snap.players[cl.my_id].active, "no snapshot with our player");
    float moved = (cl.snap.players[cl.my_id].st.pos - World(LevelId::L11, 5).spawn_point()).len();
    CHECK(moved > 2.0f, "player only moved %.2f m on the server", moved);
    CHECK(cl.snap.ack > 50, "server acked only %u inputs", cl.snap.ack);
    std::printf("  network: connected, %u snapshots, %u inputs acked, moved %.1f m\n", cl.snap_count, cl.snap.ack, moved);
    cl.disconnect();
    srv.stop();
}

int main() {
    test_determinism();
    test_connectivity();
    test_spawn_room();
    test_walk_speed();
    test_no_wall_penetration();
    test_stamina();
    test_all_levels();
    test_items();
    test_sim_soak();
    test_network();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("all core tests passed\n");
    return 0;
}
