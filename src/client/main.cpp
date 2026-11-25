// Backrooms Iris - client entry point, shared by every platform.
#if defined(__vita__)
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h> // on Windows this turns main() into SDL_main (SDL2main provides WinMain)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "../core/config.h"
#include "../core/level0.h"
#include "../core/player.h"
#include "../platform/platform.h"
#include "renderer.h"

using namespace iris;

namespace {

struct Options {
    uint32_t seed = 0;
    PlatformConfig plat;
    // dev / testing: render one frame from a fixed spot, save it, quit
    std::string shot_path;
    bool has_pos = false;
    float pos_x = 0, pos_z = 0, yaw_deg = 0, pitch_deg = 0;
    float walk_secs = 0; // simulate walking forward this long before the shot
};

void usage() {
    std::printf(
        "Backrooms Iris %s\n\n"
        "  --seed N            world seed (default: random)\n"
        "  --width W --height H window size (default 1280x720)\n"
        "  --fullscreen        start fullscreen (F11 toggles)\n"
        "  --no-vsync          uncapped framerate\n"
        "  --help              this text\n\n"
        "dev: --shot FILE.bmp [--pos X,Z] [--yaw DEG] [--pitch DEG] [--walk SECS]\n",
        IRIS_VERSION);
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--help" || a == "-h") { usage(); return false; }
        else if (a == "--seed") o.seed = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "--width") o.plat.width = std::atoi(next());
        else if (a == "--height") o.plat.height = std::atoi(next());
        else if (a == "--fullscreen") o.plat.fullscreen = true;
        else if (a == "--no-vsync") o.plat.vsync = false;
        else if (a == "--shot") { o.shot_path = next(); o.plat.hidden = true; }
        else if (a == "--pos") { o.has_pos = std::sscanf(next(), "%f,%f", &o.pos_x, &o.pos_z) == 2; }
        else if (a == "--yaw") o.yaw_deg = (float)std::atof(next());
        else if (a == "--pitch") o.pitch_deg = (float)std::atof(next());
        else if (a == "--walk") o.walk_secs = (float)std::atof(next());
        else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return false; }
    }
    return true;
}

std::string timestamp() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", std::localtime(&t));
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) return 0;
    if (opt.seed == 0) opt.seed = (uint32_t)std::time(nullptr) ^ 0x1215u;

    if (!platform_init(opt.plat)) return 1;
    std::printf("[iris] Backrooms Iris %s on %s, seed %u\n", IRIS_VERSION, platform_name(), opt.seed);

    RenderSettings rs;
#if defined(__vita__)
    rs.view_dist = 24.0f;       // weaker GPU: shorter draw distance, thicker fog (it's a vibe)
    rs.fog_density = 0.075f;
    rs.chunk_builds_per_frame = 1;
#endif
    Renderer renderer;
    renderer.init(rs);

    Level0 level(opt.seed);
    PlayerState player;
    player.pos = level.spawn_point();
    player.yaw = 0.4f;
    if (opt.has_pos) { player.pos.x = opt.pos_x; player.pos.z = opt.pos_z; }
    if (opt.yaw_deg || opt.pitch_deg || opt.has_pos) {
        player.yaw = opt.yaw_deg * PI / 180.0f;
        player.pitch = opt.pitch_deg * PI / 180.0f;
    }
    PlayerState prev = player;
    InputCmd cmd;
    cmd.yaw = player.yaw;
    cmd.pitch = player.pitch;

    // dev screenshot mode: optionally walk forward first, then render a single frame
    if (!opt.shot_path.empty()) {
        InputCmd walk = cmd;
        walk.move_z = 1;
        for (int t = 0; t < (int)(opt.walk_secs * TICK_RATE); t++) player_tick(player, walk, level, 1.0f / TICK_RATE);
        InputState in;
        platform_poll(in);
        Vec3 eye = player.pos + Vec3{0, player.eye_height(), 0};
        renderer.update_chunks(level, eye, -1);
        renderer.draw_world(eye, player.yaw, player.pitch, in.width, in.height);
        renderer.draw_hud(player.stamina, player.exhausted, in.width, in.height);
        bool ok = platform_save_screenshot(opt.shot_path);
        std::printf("[iris] %s %s (pos %.1f,%.1f)\n", ok ? "saved" : "FAILED", opt.shot_path.c_str(), player.pos.x, player.pos.z);
        platform_shutdown();
        return ok ? 0 : 1;
    }

    const float dt = 1.0f / TICK_RATE;
    const float mouse_sens = 0.0022f;   // radians per pixel
    double last = platform_time(), acc = 0, fps_t = last;
    int frames = 0;
    bool debug = false;
    InputState in;

    for (;;) {
        platform_poll(in);
        if (in.quit) break;
        if (in.toggle_debug) debug = !debug;

        double now = platform_time();
        double frame_dt = std::fmin(now - last, 0.25);
        last = now;

        // look: mouse is applied every frame (no tick latency), stick is rate-based
        cmd.yaw -= in.mouse_dx * mouse_sens;
        cmd.pitch -= in.mouse_dy * mouse_sens;
        auto curve = [](float v) { return v * std::fabs(v); }; // finer aim near centre
        cmd.yaw -= curve(in.look_x) * 2.8f * (float)frame_dt;
        cmd.pitch -= curve(in.look_y) * 1.9f * (float)frame_dt;
        cmd.pitch = clampf(cmd.pitch, -1.45f, 1.45f);
        cmd.yaw = std::fmod(cmd.yaw, 2 * PI);
        cmd.move_x = in.move_x;
        cmd.move_z = in.move_z;
        cmd.buttons = (uint8_t)((in.sprint ? BTN_SPRINT : 0) | (in.crouch ? BTN_CROUCH : 0));

        acc += frame_dt;
        while (acc >= dt) {
            prev = player;
            player_tick(player, cmd, level, dt);
            acc -= dt;
        }
        float alpha = (float)(acc / dt);

        // interpolate position between ticks; view angles come straight from input
        Vec3 pos = prev.pos + (player.pos - prev.pos) * alpha;
        float eye_h = lerpf(prev.eye_height(), player.eye_height(), alpha);
        float bob_amp = clampf(player.speed / WALK_SPEED, 0, 1.5f) * (1.0f - player.crouch * 0.5f);
        float bob = lerpf(prev.bob, player.bob, alpha);
        Vec2 right = yaw_right(cmd.yaw);
        float side = std::cos(bob * 0.5f) * 0.022f * bob_amp;
        Vec3 eye = pos + Vec3{right.x * side, eye_h + std::sin(bob) * 0.03f * bob_amp, right.z * side};

        renderer.update_chunks(level, eye, rs.chunk_builds_per_frame);
        renderer.draw_world(eye, cmd.yaw, cmd.pitch, in.width, in.height);
        renderer.draw_hud(player.stamina, player.exhausted, in.width, in.height);
        platform_swap();

        if (in.screenshot) {
            std::string path = platform_data_dir() + "iris_" + timestamp() + ".bmp";
            if (platform_save_screenshot(path)) std::printf("[iris] screenshot: %s\n", path.c_str());
        }

        frames++;
        if (now - fps_t >= 1.0) {
            if (debug)
                std::printf("[iris] %d fps | pos %.1f %.1f | chunks %d loaded, %d drawn | district %d\n", frames,
                            player.pos.x, player.pos.z, renderer.chunks_loaded(), renderer.chunks_drawn(),
                            (int)level.district(floor_to_int(player.pos.x / CELL), floor_to_int(player.pos.z / CELL)));
            frames = 0;
            fps_t = now;
        }
    }

    renderer.shutdown();
    platform_shutdown();
    return 0;
}
