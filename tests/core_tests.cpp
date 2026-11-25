// Backrooms Iris - core tests (no SDL/GL). Run: ctest --test-dir build
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/collision.h"
#include "core/config.h"
#include "core/hash.h"
#include "core/level0.h"
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
    Level0 a(777), b(777), c(778);
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
        Level0 L(seed);
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
    Level0 L(1215);
    for (int i = 0; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            CHECK(!L.wall_x(i, j) && !L.wall_z(j, i), "spawn room must be open (%d,%d)", i, j);
        }
    CHECK(L.light(0, 0) == LightState::On, "spawn is always lit");
    CHECK(L.district(0, 0) == District::Rooms && L.district(4, -4) == District::Rooms, "spawn district is classic rooms");
}

// Walking speed matches the tuning constants.
static void test_walk_speed() {
    Level0 L(1215);
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
    Level0 L(31337);
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
    Level0 L(1215);
    PlayerState p;
    p.pos = {-500, 0, -500};
    InputCmd cmd;
    cmd.move_z = 1;
    cmd.buttons = BTN_SPRINT;
    for (int t = 0; t < 60 * 10; t++) player_tick(p, cmd, L, 1.0f / TICK_RATE);
    CHECK(p.exhausted || p.stamina < 0.4f, "10 s of sprinting should drain stamina (%.2f)", p.stamina);
}

int main() {
    test_determinism();
    test_connectivity();
    test_spawn_room();
    test_walk_speed();
    test_no_wall_penetration();
    test_stamina();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("all core tests passed\n");
    return 0;
}
