// Backrooms Iris - core tests (no SDL/GL). Run: ctest --test-dir build
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/collision.h"
#include "core/config.h"
#include "core/hash.h"
#include "core/protocol.h"
#include <cmath>
#include <cstring>
#include "core/world.h"
#include "core/sim.h"
#include "net/client.h"
#include "net/server.h"
#include "client/tailscale.h"
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
        bool run = (LevelId)li == LevelId::LRun; // one corridor, one door at the end
        bool hub = (LevelId)li == LevelId::Hub;  // the lobby: no exits, just the outpost's supplies
        CHECK(hub || exits >= (run ? 1 : 2), "%s: only %d reachable exits in a 350 m square", nm, exits);
        bool end = (LevelId)li == LevelId::TSGEnd; // TH3 3ND: nothing left to find
        CHECK(end ? items == 0 : (hub ? items <= 4 : items >= (run ? 2 : 20)), "%s: %d items", nm, items);
        std::printf("  %-9s reachable %5.1f%%  exits %3d  items %4d\n", nm, reach, exits, items);
    }
}

// Items do what they say. Memory juice kills on schedule.
static void use(GameSim& sim, int p, InputCmd& cmd, ItemType t) {
    cmd.use_item = (uint8_t)t;
    cmd.use_ctr++;
    sim.apply_input(p, cmd);
}

static void test_items() {
    GameSim sim(7);
    sim.start_level(LevelId::L11, 7); // safe level: nothing else interferes
    int p = sim.add_player("TEST");
    PlayerState& st = sim.players[p].st;
    InputCmd cmd;
    sim.apply_input(p, cmd); // baseline
    for (int i = 1; i < ITEM_COUNT; i++) st.inv[i] = 2;
    st.stamina = 0.1f; st.sanity = 40;
    use(sim, p, cmd, ItemType::AlmondWater);
    CHECK(st.stamina == 1.0f && st.sanity >= 70, "almond water: stamina %.2f sanity %.1f", st.stamina, st.sanity);
    st.health = 40;
    use(sim, p, cmd, ItemType::RoyalRation);
    CHECK(st.health >= 84, "royal ration should heal (%.0f)", st.health);
    st.sanity = 20; st.health = 100;
    use(sim, p, cmd, ItemType::LiquidPain);
    CHECK(st.health <= 66 && st.sanity >= 74, "liquid pain: hp %.0f sanity %.0f", st.health, st.sanity);
    use(sim, p, cmd, ItemType::LuckyMilk);
    CHECK(st.lucky > 30, "lucky o' milk timer");
    use(sim, p, cmd, ItemType::Firesalt);
    CHECK(!sim.props.empty() && sim.props.back().kind == PropKind::Flare, "firesalt should start a flare");
    // flashlight toggles only if you own it
    cmd.light_ctr++;
    sim.apply_input(p, cmd);
    CHECK(st.light_on, "flashlight should turn on");
    cmd.light_ctr++;
    sim.apply_input(p, cmd);
    CHECK(!st.light_on, "flashlight should turn off");
    // memory juice, then cashew water cures it
    use(sim, p, cmd, ItemType::MemoryJuice);
    for (int t = 0; t < 60 * 10; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    use(sim, p, cmd, ItemType::CashewWater);
    CHECK(st.juice < 0 && st.slow > 0, "cashew water should cure memory juice (juice %.1f)", st.juice);
    use(sim, p, cmd, ItemType::EnergyDrink);
    CHECK(st.boost > 0, "energy drink should boost");
    st.slow = 0;
    cmd.move_z = 1;
    for (int t = 0; t < 60; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    CHECK(st.speed > WALK_SPEED * 1.3f, "boosted walk speed %.2f", st.speed);
    cmd.move_z = 0;
    use(sim, p, cmd, ItemType::MemoryJuice);
    for (int t = 0; t < 60 * 47; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    CHECK(st.alive, "still alive at 47 s");
    for (int t = 0; t < 60 * 2; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    CHECK(!st.alive && st.cause == DeathCause::MemoryJuice, "memory juice should kill at %.0f s", JUICE_TIME);
    CHECK(!sim.props.empty(), "dying should drop your items");
}

// Picking things up: counters from an old game must not trigger anything; full stacks refuse.
static void test_pickup() {
    GameSim sim(3);
    sim.start_level(LevelId::L11, 3);
    int p = sim.add_player("P");
    PlayerState& st = sim.players[p].st;
    InputCmd stale;
    stale.pickup_ctr = 17; stale.use_ctr = 9; stale.use_item = (uint8_t)ItemType::AlmondWater;
    sim.apply_input(p, stale); // first command: only a baseline
    CHECK(st.inv[(int)ItemType::AlmondWater] == 1, "stale counters used an item at game start");
    sim.spawn_item(ItemType::Crowbar, {st.pos.x + 1.0f, st.pos.z});
    stale.pickup_ctr++;
    sim.apply_input(p, stale);
    CHECK(st.has(ItemType::Crowbar), "should pick up the crowbar 1 m away");
    sim.spawn_item(ItemType::Crowbar, {st.pos.x + 1.0f, st.pos.z});
    stale.pickup_ctr++;
    sim.apply_input(p, stale);
    CHECK(st.inv[(int)ItemType::Crowbar] == 1 && sim.players[p].toast.find("CAN'T") != std::string::npos, "tools don't stack");
    // crowbar stuns what's in front of you
    Entity& e = sim.spawn_entity(EntityType::Bacteria, {st.pos.x, st.pos.z - 1.5f}, 0);
    uint16_t id = e.id;
    stale.yaw = 0; stale.attack_ctr++;
    sim.apply_input(p, stale);
    bool stunned = false;
    for (auto& x : sim.ents) if (x.id == id) stunned = x.state == EntState::Stunned;
    CHECK(stunned, "crowbar should stun the bacteria");
}
// Run every level for a few simulated minutes with wandering players: entities spawn,
// chase, nobody crashes, nothing ends up inside walls.
static void test_sim_soak() {
    for (int li = 0; li < (int)LevelId::COUNT; li++) {
        GameSim sim(99);
        sim.multiplayer = true;
        sim.start_level((LevelId)li, 1000 + li);
        if ((LevelId)li == LevelId::L94) sim.level_time = 230; // Motion is safe by day: start near dusk so the night shift turns up
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
            if (sim.world.id() == (LevelId)li) { // (random walks find exits)
                int hostile = 0;
                for (const Entity& e : sim.ents) hostile += !friendly(e.type);
                max_ents = std::max(max_ents, hostile);
            }
            for (int i : {a, b}) deaths += !sim.players[i].st.alive && sim.players[i].respawn_t > 9.98f;
        }
        bool expect = (LevelId)li != LevelId::L37 && (LevelId)li != LevelId::TSGEnd && (LevelId)li != LevelId::Hub && (LevelId)li != LevelId::TSG2; // the safe levels (the angels are friendly)
        CHECK(!expect || max_ents > 0, "%s: no entities ever spawned", level_info((LevelId)li).code);
        CHECK(expect || max_ents == 0, "%s should be safe", level_info((LevelId)li).code);
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
    { // a chalk mark keeps its rotation in the item byte: it must still reach the clients
        Vec3 sp = srv.sim.world.spawn_point();
        Prop m;
        m.id = 60000;
        m.kind = PropKind::Mark;
        m.item = (ItemType)200;
        m.pos = Vec2{sp.x + 1, sp.z};
        m.life = 600;
        srv.sim.props.push_back(m);
    }
    NetClient cl;
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    cl.connect("127.0.0.1", 27399, "NETTEST", "", now());
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
    bool mark = false;
    for (const PropSnap& p : cl.snap.props) mark |= p.kind == PropKind::Mark && (int)p.item == 200;
    CHECK(mark, "chalk mark didn't reach the client");
    std::printf("  network: connected, %u snapshots, %u inputs acked, moved %.1f m\n", cl.snap_count, cl.snap.ack, moved);
    cl.disconnect();
    srv.stop();
}

// The host's defences: join handshake, passwords, padded status queries, junk input.
static void test_host_security() {
    Server srv;
    ServerConfig cfg;
    cfg.port = 27398;
    cfg.level = LevelId::L11;
    cfg.seed = 9;
    cfg.password = "Hunter2";
    std::string err;
    CHECK(srv.start(cfg, &err), "server start: %s", err.c_str());
    auto t0 = std::chrono::steady_clock::now();
    auto now = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    auto try_join = [&](const char* pw) {
        NetClient cl;
        double start = now();
        cl.connect("127.0.0.1", 27398, "tester<>!!", pw, start);
        while (now() - start < 2.0 && cl.state == NetClient::State::Connecting) {
            srv.poll();
            cl.poll(now());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::string why = cl.error;
        bool ok = cl.state == NetClient::State::Connected;
        cl.disconnect();
        for (int i = 0; i < 20; i++) { srv.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        return std::make_pair(ok, why);
    };
    auto r1 = try_join("");
    CHECK(!r1.first && r1.second == "THIS GAME HAS A PASSWORD", "joined without the password (%s)", r1.second.c_str());
    auto r2 = try_join("nope");
    CHECK(!r2.first && r2.second == "WRONG PASSWORD", "wrong password not rejected (%s)", r2.second.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(1600)); // wrong guesses cool down
    auto r3 = try_join("hunter2");   // case-insensitive
    CHECK(r3.first, "right password refused (%s)", r3.second.c_str());
    CHECK(srv.sim.players[0].name == "TESTER", "name not cleaned: '%s'", srv.sim.players[0].name.c_str());
    CHECK(srv.player_count() == 0, "player still in after leaving");

    // raw socket: a HELLO without the cookie gets only a small CHALLENGE, never a slot
    UdpSocket raw;
    CHECK(raw.open(0), "raw socket");
    NetAddr host{0x7F000001u, 27398};
    Writer w;
    w.header(PKT_HELLO);
    w.u16(IRIS_PROTOCOL_VERSION);
    w.str("SPOOF");
    raw.send(host, w.buf, w.n);
    // unpadded status query: ignored. padded: answered, and never bigger than the question
    raw.send(host, "IRIS?", 5);
    char q[QUERY_SIZE] = "IRIS?";
    raw.send(host, q, QUERY_SIZE);
    int challenges = 0, answers = 0, biggest = 0;
    double until = now() + 0.4;
    while (now() < until) {
        srv.poll();
        uint8_t buf[MAX_PACKET];
        NetAddr from;
        int n;
        while ((n = raw.recv(from, buf, sizeof buf)) >= 0) {
            biggest = std::max(biggest, n);
            if (n >= 5 && std::memcmp(buf, "IRIS!", 5) == 0) answers++;
            else { Reader r(buf, n); if (r.header() == PKT_CHALLENGE) challenges++; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(challenges == 1, "expected one challenge, got %d", challenges);
    CHECK(answers == 1, "expected exactly one status answer (padded query only), got %d", answers);
    CHECK(biggest <= QUERY_SIZE, "reply of %d bytes is bigger than the query", biggest);
    CHECK(srv.player_count() == 0, "a HELLO without a cookie got a slot");
    FoundServer f;
    char reply[QUERY_SIZE + 1] = "IRIS! name=\"SAM'S GAME\" protocol=7 players=1/4 level=\"LEVEL 0\" password=1";
    CHECK(parse_server_info(reply, f) && f.locked && f.max_players == 4, "status line not parsed");

    // junk input never reaches the sim as NaN / out-of-range values
    Writer cw;
    InputCmd bad;
    bad.yaw = std::nanf("");
    bad.pitch = 1e30f;
    bad.move_x = 5;
    write_cmd(cw, 1, bad);
    Reader cr(cw.buf, cw.n);
    uint32_t seq;
    InputCmd got = read_cmd(cr, seq);
    CHECK(std::isfinite(got.yaw) && got.pitch <= 1.56f && got.move_x <= 1.0f, "input not sanitized");
    CHECK(clean_name("  a  b!!c\x01  ") == "A BC", "clean_name: '%s'", clean_name("  a  b!!c\x01  ").c_str());
    CHECK(clean_name("") == "WANDERER", "empty name");
    raw.close();
    srv.stop();

    // 4 players max (host included): the 5th is turned away, everyone else plays
    Server open;
    ServerConfig oc;
    oc.port = 27397;
    oc.level = LevelId::L0;
    CHECK(open.start(oc, &err), "server start: %s", err.c_str());
    NetClient cls[5];
    double s0 = now();
    for (int k = 0; k < 4; k++) cls[k].connect("127.0.0.1", 27397, "P" + std::to_string(k), "", s0);
    while (now() - s0 < 1.5) {
        open.poll();
        for (int k = 0; k < 4; k++) cls[k].poll(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    cls[4].connect("127.0.0.1", 27397, "FIFTH", "", now());
    double s1 = now();
    while (now() - s1 < 1.5) {
        open.poll();
        for (int k = 0; k < 5; k++) cls[k].poll(now());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    int in = 0;
    for (int k = 0; k < 4; k++) in += cls[k].state == NetClient::State::Connected && cls[k].got_snapshot;
    CHECK(in == 4 && open.player_count() == 4, "only %d of 4 players got in", in);
    CHECK(cls[4].state == NetClient::State::Failed && cls[4].error == "GAME IS FULL", "5th player: %s", cls[4].error.c_str());
    for (auto& c : cls) c.disconnect();
    open.stop();
    std::printf("  host security: password, cookie handshake, padded queries, input sanitizing ok\n");
}

// Every difficulty still has reachable exits, harder ones have fewer; exits are always on a wall.
static void test_difficulty_exits() {
    for (int li = 0; li < (int)LevelId::COUNT; li++) {
        int prev = 1 << 30;
        for (int d = 0; d < (int)Difficulty::COUNT; d++) {
            World L((LevelId)li, 777, (Difficulty)d);
            int n = 0;
            for (int z = -80; z < 80; z++)
                for (int x = -80; x < 80; x++) {
                    ExitDoor door;
                    if (!L.exit_door(x, z, &door)) continue;
                    n++;
                    if (L.exit_style() == ExitStyle::Door)
                        CHECK(L.wall_face(x, z, door.side), "%s %s: exit door at %d,%d is not on a wall", level_info((LevelId)li).code,
                              diff_info((Difficulty)d).name, x, z);
                    else // holes, clocks, angels, lone doors stand free in the middle of an open cell
                        CHECK(door.side == -1 && std::fabs(door.pos.x - (x + 0.5f) * CELL) < 0.01f, "%s: free-standing exit misplaced", level_info((LevelId)li).code);
                    CHECK(!L.solid(x, z), "exit opens into a solid cell");
                }
            CHECK(n >= ((LevelId)li == LevelId::LRun ? 1 : 3) || (LevelId)li == LevelId::Hub, "%s %s: only %d exits in a 400 m square", level_info((LevelId)li).code, diff_info((Difficulty)d).name, n);
            CHECK(n <= prev + 2 || (LevelId)li == LevelId::TSGEnd, "%s: %s has more exits (%d) than an easier difficulty (%d)", level_info((LevelId)li).code,
                  diff_info((Difficulty)d).name, n, prev);
            prev = n;
        }
    }
    // the sim walks you into a door and on to the next level
    GameSim sim(31);
    sim.start_level(LevelId::L0, 31);
    sim.add_player("A");
    Vec2 at;
    ExitDoor door;
    CHECK(sim.nearest_exit({0, 0}, at, &door), "no exit near spawn");
    sim.players[0].st.pos = {at.x, 0, at.z};
    InputCmd cmd;
    cmd.yaw = std::atan2(door.normal.x, door.normal.z); // face the door
    cmd.move_z = 1;
    for (int t = 0; t < 120 && sim.world.id() == LevelId::L0; t++) { sim.apply_input(0, cmd); sim.tick(1.0f / TICK_RATE); }
    CHECK(sim.world.id() == LevelId::L1, "walking into the exit door should lead to level 1");
}

// Level 5: rooms can be entered, furniture collides, and the room block next to spawn exists.
static void test_hotel_rooms() {
    World L(LevelId::L5, 1215);
    CHECK(L.guest_room(1, 1) && L.guest_room(4, 4), "the block next to spawn should be guest rooms");
    int rooms = 0, doors = 0;
    for (int bz = -6; bz < 6; bz++)
        for (int bx = -6; bx < 6; bx++)
            for (int q = 0; q < 4; q++) {
                int cx = bx * 5 + 1 + (q & 1) * 2, cz = bz * 5 + 1 + (q >> 1) * 2;
                if (!L.guest_room(cx, cz)) continue;
                rooms++;
                // exactly one doorway out of the room's 8 outer edges, opening onto a corridor
                int open_edges = 0;
                for (int k = 0; k < 2; k++) {
                    open_edges += !L.wall_x(cx, cz + k) + !L.wall_x(cx + 2, cz + k);
                    open_edges += !L.wall_z(cx + k, cz) + !L.wall_z(cx + k, cz + 2);
                }
                CHECK(open_edges == 1, "room at %d,%d has %d doorways", cx, cz, open_edges);
                doors += open_edges;
                Furn f[MAX_FURN];
                int n = L.furniture(cx, cz, f);
                bool bed = false;
                for (int k = 0; k < n; k++) bed |= f[k].kind == FurnKind::Bed;
                CHECK(bed, "room at %d,%d has no bed", cx, cz);
            }
    CHECK(rooms > 100, "only %d guest rooms", rooms);
    // walk into the bed: you stop at it
    Furn f[MAX_FURN];
    int n = L.furniture(1, 1, f);
    for (int k = 0; k < n; k++) {
        if (f[k].kind != FurnKind::Bed) continue;
        Vec2 c{(f[k].x0 + f[k].x1) * 0.5f, (f[k].z0 + f[k].z1) * 0.5f};
        collide_circle(L, c, PLAYER_RADIUS);
        bool inside = c.x > f[k].x0 && c.x < f[k].x1 && c.z > f[k].z0 && c.z < f[k].z1;
        CHECK(!inside, "the bed should push you out");
    }
    std::printf("  hotel: %d guest rooms, %d doorways\n", rooms, doors);
}

// Objectives: tapes/breakers are placed and reachable, finishing gives everyone a perk that
// survives death and level changes, and perks actually change things.
static void test_objectives() {
    int types[(int)ObjType::COUNT] = {};
    for (uint32_t seed = 1; seed < 40; seed++) {
        GameSim sim(seed);
        sim.start_level((LevelId)(seed % (uint32_t)LevelId::COUNT), seed);
        sim.add_player("A");
        const Objective& o = sim.objective;
        types[(int)o.type]++;
        if (sim.world.id() == LevelId::LRun || sim.world.id() == LevelId::TSGEnd || sim.world.id() == LevelId::Hub) continue; // no objective on Level !, at the end or in the lobby
        CHECK(o.type != ObjType::None && o.need > 0, "seed %u: no objective", seed);
        int things = 0;
        for (const Prop& p : sim.props) things += p.kind == PropKind::Tape || p.kind == PropKind::Breaker || p.kind == PropKind::Key;
        if (o.type == ObjType::Tapes || o.type == ObjType::Breakers || o.type == ObjType::Keys) CHECK(things == o.need, "seed %u: %d props for %d needed", seed, things, o.need);
        if (o.type == ObjType::Tapes || o.type == ObjType::Breakers) {
            // collect them all by standing next to each one
            InputCmd cmd;
            sim.apply_input(0, cmd);
            for (size_t k = 0; k < sim.props.size();) {
                Prop pr = sim.props[k];
                if (pr.kind != PropKind::Tape && (pr.kind != PropKind::Breaker || pr.life > 0.5f)) { k++; continue; }
                sim.players[0].st.pos = {pr.pos.x, 0, pr.pos.z};
                cmd.pickup_ctr++;
                sim.apply_input(0, cmd);
                if (pr.kind == PropKind::Breaker) k++;
            }
            CHECK(o.done, "seed %u: objective not complete after collecting everything (%d/%d)", seed, o.have, o.need);
            int total = 0;
            for (int k = 0; k < PERK_COUNT; k++) total += sim.players[0].st.perks[k];
            CHECK(total == 1, "seed %u: should have one perk (%d)", seed, total);
            sim.respawn(0);
            total = 0;
            for (int k = 0; k < PERK_COUNT; k++) total += sim.players[0].st.perks[k];
            CHECK(total == 1, "perks should survive respawning");
        }
    }
    CHECK(types[1] && types[2] && types[3], "every objective type should show up (%d %d %d)", types[1], types[2], types[3]);
    // survive
    GameSim sim(5);
    sim.start_level(LevelId::L11, 5);
    sim.add_player("A");
    sim.objective = Objective{};
    sim.objective.type = ObjType::Survive;
    sim.objective.need = 3;
    for (int t = 0; t < 4 * 60; t++) sim.tick(1.0f / 60);
    CHECK(sim.objective.done, "survive objective should complete");
    // big lungs: sprint lasts longer
    World W(LevelId::L11, 1);
    auto sprint_time = [&](int lv) {
        PlayerState p;
        p.perks[(int)Perk::Stamina] = (uint8_t)lv;
        InputCmd c;
        c.move_z = 1;
        c.buttons = BTN_SPRINT;
        int t = 0;
        while (!p.exhausted && t < 6000) { player_tick(p, c, W, 1.0f / 60); t++; }
        return t;
    };
    CHECK(sprint_time(2) > sprint_time(0) * 1.4f, "big lungs should make stamina last longer");
    std::printf("  objectives: tapes %d breakers %d survive %d\n", types[1], types[2], types[3]);
}

// Sandbox guns kill things; the rabbit's foot cheats death once; music boxes pull entities in.
static void test_new_items() {
    GameSim sim(7);
    sim.sandbox = true;
    sim.auto_spawn = false;
    sim.start_level(LevelId::L11, 7);
    int p = sim.add_player("A");
    PlayerState& st = sim.players[p].st;
    st.inv[(int)ItemType::Pistol] = 1;
    InputCmd cmd;
    sim.apply_input(p, cmd);
    Entity& e = sim.spawn_entity(EntityType::Hound, {st.pos.x, st.pos.z - 5.0f}, 0);
    uint16_t id = e.id;
    cmd.yaw = 0; // facing -z
    cmd.attack_item = (uint8_t)ItemType::Pistol;
    for (int shot = 0; shot < 4; shot++) {
        cmd.attack_ctr++;
        sim.apply_input(p, cmd);
        for (int t = 0; t < 30; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
    }
    bool alive = false;
    for (auto& x : sim.ents) if (x.id == id && x.pos.x < 1e5f) alive = true;
    CHECK(!alive, "four pistol shots should kill a hound");
    // shotgun up close
    st.inv[(int)ItemType::Shotgun] = 1;
    Entity& e2 = sim.spawn_entity(EntityType::Partygoer, {st.pos.x, st.pos.z - 2.0f}, 0);
    float hp0 = e2.hp;
    uint16_t id2 = e2.id;
    cmd.attack_item = (uint8_t)ItemType::Shotgun;
    sim.freeze_ents = true;
    for (int t = 0; t < 60; t++) sim.tick(1.0f / 60); // the shotgun's pump
    sim.freeze_ents = false;
    cmd.attack_ctr++;
    sim.apply_input(p, cmd);
    for (auto& x : sim.ents) if (x.id == id2) CHECK(x.hp < hp0 - 40 || x.pos.x > 1e5f, "shotgun should hurt a lot up close (%.0f)", x.hp);
    // rabbit's foot
    sim.clear_entities();
    st.inv[(int)ItemType::RabbitFoot] = 1;
    Entity& b = sim.spawn_entity(EntityType::Bacteria, {st.pos.x, st.pos.z - 0.5f}, 0);
    b.state = EntState::Chase;
    for (int t = 0; t < 30; t++) sim.tick(1.0f / 60);
    CHECK(st.alive && st.inv[(int)ItemType::RabbitFoot] == 0, "the rabbit's foot should save you once");
    // music box
    sim.clear_entities();
    st.inv[(int)ItemType::MusicBox] = 1;
    cmd.use_item = (uint8_t)ItemType::MusicBox;
    cmd.use_ctr++;
    sim.apply_input(p, cmd);
    Vec2 box{0, 0};
    for (auto& pr : sim.props) if (pr.kind == PropKind::Lure) box = pr.pos;
    Entity& h = sim.spawn_entity(EntityType::Partygoer, box + Vec2{8, 0}, 0);
    uint16_t hid = h.id;
    float d0 = 8;
    for (int t = 0; t < 180; t++) sim.tick(1.0f / 60);
    for (auto& x : sim.ents) if (x.id == hid) CHECK((x.pos - box).len() < d0 - 3, "the music box should draw entities in");
}

// Tailscale: `tailscale status --json` is parsed into this machine + online peers.
static void test_tailscale() {
    const char* json = R"({
  "Version": "1.76.1", "BackendState": "Running", "TailscaleIPs": ["100.101.1.2", "fd7a:115c:a1e0::1"],
  "Self": {"ID": "n1", "HostName": "my-pc", "DNSName": "my-pc.tail1234.ts.net.", "OS": "linux",
           "TailscaleIPs": ["100.101.1.2", "fd7a:115c:a1e0::1"], "Online": true, "Capabilities": ["a", "b"]},
  "Peer": {
    "nodekey:abc": {"HostName": "friend-laptop", "DNSName": "friend-laptop.tail1234.ts.net.", "OS": "windows",
                    "TailscaleIPs": ["fd7a:115c:a1e0::2", "100.88.7.6"], "Online": true, "Relay": "nyc",
                    "PrimaryRoutes": null, "Tags": [], "LastSeen": "0001-01-01T00:00:00Z", "KeepAlive": false},
    "nodekey:def": {"HostName": "old-phone", "TailscaleIPs": ["100.70.0.9"], "Online": false, "Escaped": "a \"quoted\" name é"}
  },
  "CurrentTailnet": {"Name": "you@example.com", "MagicDNSSuffix": "tail1234.ts.net", "MagicDNSEnabled": true}
})";
    TailStatus st;
    CHECK(parse_tailscale_status(json, st), "tailscale json should parse");
    CHECK(st.running && st.self_ip == "100.101.1.2" && st.self_name == "my-pc", "self: %s %s", st.self_ip.c_str(), st.self_name.c_str());
    CHECK(st.peers.size() == 2, "two peers (%zu)", st.peers.size());
    bool friend_ok = false;
    for (auto& p : st.peers) if (p.name == "friend-laptop" && p.ip == "100.88.7.6" && p.online) friend_ok = true;
    CHECK(friend_ok, "the friend's IPv4 and online state");
    CHECK(is_tailscale_ip("100.64.0.1") && is_tailscale_ip("100.127.255.1") && !is_tailscale_ip("100.128.0.1") && !is_tailscale_ip("192.168.1.2"), "tailnet range");
    TailStatus bad;
    CHECK(!parse_tailscale_status("tailscale: command not found", bad), "garbage is not a status");
}

// Memory juice: fast and unkillable until it kills you. The new items do their thing.
static void test_juice_and_new_items() {
    GameSim sim(9);
    sim.start_level(LevelId::L11, 9);
    int p = sim.add_player("A");
    PlayerState& st = sim.players[p].st;
    InputCmd cmd;
    sim.apply_input(p, cmd);
    st.inv[(int)ItemType::MemoryJuice] = 1;
    use(sim, p, cmd, ItemType::MemoryJuice);
    Entity& b = sim.spawn_entity(EntityType::Bacteria, {st.pos.x, st.pos.z - 0.4f}, 0);
    b.state = EntState::Chase;
    for (int t = 0; t < 120; t++) sim.tick(1.0f / 60);
    CHECK(st.alive, "nothing should kill you while you're on memory juice");
    // and it makes you fast
    World W(LevelId::L11, 1);
    PlayerState a, j;
    j.juice = 0;
    InputCmd run;
    run.move_z = 1;
    run.buttons = BTN_SPRINT;
    for (int t = 0; t < 120; t++) { player_tick(a, run, W, 1.0f / 60); player_tick(j, run, W, 1.0f / 60); }
    CHECK(j.speed > a.speed * 1.3f, "memory juice should make you much faster (%.2f vs %.2f)", j.speed, a.speed);
    // batteries, painkillers, night vision, chalk
    GameSim s2(10);
    s2.start_level(LevelId::L11, 10);
    int q = s2.add_player("B");
    PlayerState& b2 = s2.players[q].st;
    InputCmd c2;
    s2.apply_input(q, c2);
    b2.battery = 0.1f;
    b2.inv[(int)ItemType::Batteries] = 1;
    use(s2, q, c2, ItemType::Batteries);
    CHECK(b2.battery > 0.99f, "batteries should refill the flashlight");
    b2.inv[(int)ItemType::NightVision] = 1;
    use(s2, q, c2, ItemType::NightVision);
    CHECK(b2.nv_on, "night vision should switch on");
    b2.inv[(int)ItemType::Chalk] = 2;
    use(s2, q, c2, ItemType::Chalk);
    bool mark = false;
    for (auto& pr : s2.props) mark |= pr.kind == PropKind::Mark;
    CHECK(mark && b2.inv[(int)ItemType::Chalk] == 1, "chalk should leave a mark");
}

// Every level now has props, and some have camps; nothing is placed inside a wall.
static void test_props() {
    int total = 0, camps = 0;
    for (int li = 0; li < (int)LevelId::COUNT; li++) {
        World L((LevelId)li, 2024);
        for (int z = -40; z < 40; z++)
            for (int x = -40; x < 40; x++) {
                Furn f[MAX_FURN];
                int n = L.furniture(x, z, f);
                total += n;
                int ccx, ccz;
                if (L.camp(x, z, ccx, ccz) && x == ccx && z == ccz) camps++;
                for (int k = 0; k < n; k++) CHECK(!L.solid(x, z), "%s: prop in a solid cell", level_info((LevelId)li).code);
            }
    }
    CHECK(total > 3000, "props everywhere (%d)", total);
    CHECK(camps > 5, "some camps (%d)", camps);
    std::printf("  props: %d pieces, %d camps\n", total, camps);
}

// 1.2: locked exits, shells, the safe zone, M.E.G. outposts, items on furniture, the lobby, sandbox commands.
static void test_12() {
    // exits stay shut on a level with a required objective, until it's done
    {
        GameSim sim(11);
        sim.start_level(LevelId::L3, 11);
        int p = sim.add_player("A");
        CHECK(sim.objective.required, "Level 3's objective should be required");
        Vec2 ex;
        ExitDoor door;
        CHECK(sim.nearest_exit({sim.players[p].st.pos.x, sim.players[p].st.pos.z}, ex, &door), "no exit near Level 3's spawn");
        sim.players[p].st.pos = {door.pos.x + door.normal.x * 0.3f, 0, door.pos.z + door.normal.z * 0.3f};
        uint32_t ep = sim.epoch;
        sim.tick(1.0f / 60);
        CHECK(sim.epoch == ep, "a locked exit let you through");
        sim.objective.have = sim.objective.need;
        sim.objective.done = true;
        sim.tick(1.0f / 60);
        CHECK(sim.epoch != ep, "the exit should open once the objective is done");
    }
    // the shotgun needs shells outside the sandbox, and uses one per shot
    {
        GameSim sim(12);
        sim.auto_spawn = false;
        sim.start_level(LevelId::L0, 12);
        int p = sim.add_player("A");
        PlayerState& st = sim.players[p].st;
        st.inv[(int)ItemType::Shotgun] = 1;
        InputCmd cmd;
        sim.apply_input(p, cmd);
        cmd.attack_item = (uint8_t)ItemType::Shotgun;
        for (int t = 0; t < 120; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
        cmd.attack_ctr++;
        sim.apply_input(p, cmd);
        CHECK(st.swing > 0.3f, "the shotgun fired with no shells");
        st.inv[(int)ItemType::Shells] = 2;
        for (int t = 0; t < 120; t++) { sim.apply_input(p, cmd); sim.tick(1.0f / 60); }
        cmd.attack_ctr++;
        sim.apply_input(p, cmd);
        CHECK(st.inv[(int)ItemType::Shells] == 1 && st.swing < 0.05f, "a shot should use one shell");
        CHECK(!item_info(ItemType::Shotgun).sandbox_only, "the shotgun turns up in the normal game now");
    }
    // nothing hostile walks into the spawn's safe zone
    {
        GameSim sim(13);
        sim.auto_spawn = false;
        sim.start_level(LevelId::L0, 13);
        int p = sim.add_player("A");
        Vec3 s = sim.world.spawn_point();
        Entity& e = sim.spawn_entity(EntityType::Bacteria, {s.x + SPAWN_SAFE_R + 4.0f, s.z}, 0);
        e.state = EntState::Chase;
        e.target = p;
        uint16_t id = e.id;
        for (int t = 0; t < 60 * 10; t++) sim.tick(1.0f / 60);
        for (auto& x : sim.ents)
            if (x.id == id) CHECK((x.pos - Vec2{s.x, s.z}).len() >= SPAWN_SAFE_R - 0.05f, "a bacteria got into the safe zone (%.1f m)", (x.pos - Vec2{s.x, s.z}).len());
    }
    // M.E.G. outposts: officials walk out when you come near, and the medic patches you up
    {
        GameSim sim(14);
        sim.start_level(LevelId::L1, 4242);
        int p = sim.add_player("A");
        int ox = 0, oz = 0;
        bool found = false;
        for (int r = 1; r < 6 && !found; r++)
            for (int b = -r; b <= r && !found; b++)
                for (int a = -r; a <= r && !found; a++) found = sim.world.outpost_region(a, b, ox, oz);
        CHECK(found, "no M.E.G. outpost on Level 1");
        if (found) {
            PlayerState& st = sim.players[p].st;
            st.pos = {(ox + 0.5f) * CELL, 0, (oz + 2.2f) * CELL};
            st.health = 20;
            for (int t = 0; t < 60 * 3; t++) sim.tick(1.0f / 60);
            int megs = 0;
            for (auto& x : sim.ents) megs += x.type == EntityType::MegMedic || x.type == EntityType::MegScout;
            CHECK(megs == 2, "expected two M.E.G. officials at the outpost, got %d", megs);
            for (auto& x : sim.ents) if (x.type == EntityType::MegMedic) { st.pos = {x.pos.x + 1.0f, 0, x.pos.z}; }
            InputCmd cmd;
            sim.apply_input(p, cmd);
            cmd.pickup_ctr++;
            sim.apply_input(p, cmd);
            CHECK(st.health >= 99.0f, "the medic should heal you (%.0f)", st.health);
        }
    }
    // items lie on furniture when there is some, and where they lie is a pure function
    {
        World w(LevelId::L5, 777);
        int on = 0, total = 0;
        for (int cz = -30; cz < 30; cz++)
            for (int cx = -30; cx < 30; cx++) {
                if (w.item(cx, cz) == ItemType::None) continue;
                total++;
                Vec2 a, b;
                float ya, yb;
                bool fa = w.item_spot(cx, cz, a, ya), fb = w.item_spot(cx, cz, b, yb);
                CHECK(fa == fb && a.x == b.x && a.z == b.z && ya == yb, "item spot isn't deterministic");
                on += ya > 0.1f;
            }
        CHECK(total > 0 && on * 2 > total, "most hotel items should be on furniture (%d of %d)", on, total);
    }
    // the lobby, and a run started from it; sandbox commands
    {
        GameSim sim(15);
        sim.start_level(LevelId::Hub, 15);
        int p = sim.add_player("A");
        CHECK(sim.objective.type == ObjType::None, "the lobby has no objective");
        sim.start_run(LevelId::L2, Difficulty::Hard, true, 99);
        CHECK(sim.world.id() == LevelId::L2 && sim.sandbox && sim.difficulty == Difficulty::Hard, "start_run didn't start the run");
        size_t n = sim.ents.size();
        sim.sandbox_cmd(p, GameSim::SB_SPAWN_ENTITY, (uint8_t)EntityType::Hound, 0);
        CHECK(sim.ents.size() == n + 1, "sandbox spawn didn't spawn");
        sim.sandbox = false;
        sim.sandbox_cmd(p, GameSim::SB_SPAWN_ENTITY, (uint8_t)EntityType::Hound, 0);
        CHECK(sim.ents.size() == n + 1, "sandbox commands must be ignored outside sandbox mode");
        sim.sandbox_cmd(p, GameSim::SB_SPAWN_ENTITY, 250, 0);
        CHECK(sim.ents.size() == n + 1, "bad entity type spawned something");
    }
    // walking out of TH3 3ND wins
    {
        GameSim sim(16);
        sim.start_level(LevelId::TSGEnd, 16);
        int p = sim.add_player("A");
        Vec2 ex;
        ExitDoor door;
        if (sim.nearest_exit({sim.players[p].st.pos.x, sim.players[p].st.pos.z}, ex, &door)) {
            sim.players[p].st.pos = {door.pos.x, 0, door.pos.z};
            sim.tick(1.0f / 60);
            CHECK(sim.escaped && sim.world.id() == LevelId::TSGEnd, "walking out of TH3 3ND should win (and stay there)");
        } else CHECK(false, "no door in TH3 3ND");
    }
    // terrain: pools have depth, Motion has hills, the player follows the ground
    {
        World w(LevelId::L37, 4242);
        CHECK(w.floor_y(-3 * CELL + 1.25f, 5 * CELL + 1.25f) < -1.5f, "Level 37's pools should be deep");
        CHECK(w.floor_y(1.25f, 1.25f) == 0.0f, "the poolrooms' spawn should be dry");
        World h(LevelId::L94, 4242);
        float top = 0;
        for (int cz = -40; cz < 40; cz += 2) for (int cx = -40; cx < 40; cx += 2) top = std::max(top, h.floor_y(cx * CELL, cz * CELL));
        CHECK(top > 8.0f, "Motion's hills should be big (%.1f m)", top);
        PlayerState ps;
        ps.pos = {-3 * CELL + 1.25f, 0, 5 * CELL + 1.25f};
        InputCmd c;
        for (int t = 0; t < 120; t++) player_tick(ps, c, w, 1.0f / 60);
        CHECK(ps.pos.y < -1.0f, "the player should sink into the pool (%.2f)", ps.pos.y);
    }
    std::printf("  1.2: locked exits, shells, safe zone, outposts, items on furniture, lobby, sandbox, the end, terrain ok\n");
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
    test_pickup();
    test_difficulty_exits();
    test_hotel_rooms();
    test_objectives();
    test_new_items();
    test_tailscale();
    test_juice_and_new_items();
    test_props();
    test_sim_soak();
    test_network();
    test_host_security();
    test_12();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("all core tests passed\n");
    return 0;
}
