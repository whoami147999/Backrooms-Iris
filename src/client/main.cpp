// Backrooms Iris - client entry point, shared by every platform.
#if defined(__vita__)
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h> // on Windows this turns main() into SDL_main (SDL2main provides WinMain)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../core/config.h"
#include "../core/world.h"
#include "app.h"

using namespace iris;

static void usage() {
    std::printf(
        "Backrooms Iris %s\n\n"
        "  --level N           jump straight into level N (0 1 2 3 4 5 6 7 8 9 10 11 13 19 21 22 24 33 36 37 51 fun ! grey black mansion hope)\n"
        "  --sandbox           ...in sandbox mode\n"
        "  --seed N            world seed for --level\n"
        "  --host              host a multiplayer game right away\n"
        "  --join IP[:PORT]    join a multiplayer game right away\n"
        "  --password PW       password for --host / --join\n"
        "  --help              this text\n\n"
        "dev: --shot FILE.bmp [--after SECS] [--pos X,Z] [--yaw DEG] [--pitch DEG] [--walk SECS]\n"
        "     [--juice SECS] [--sanity N] [--health N] [--entity TYPE [--dist M]] [--screen NAME] [--autowalk] [--nohud]\n",
        IRIS_VERSION);
}

static int level_arg(const char* s) {
    LevelId id;
    return level_from_number(s, id) ? (int)id : -1;
}

int main(int argc, char** argv) {
    DevOptions dev;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--level") dev.level = level_arg(next());
        else if (a == "--seed") dev.seed = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "--host") dev.host = true;
        else if (a == "--difficulty") { Difficulty d; if (difficulty_from_name(next(), d)) dev.difficulty = (int)d; }
        else if (a == "--join") dev.join = next();
        else if (a == "--password") dev.password = next();
        else if (a == "--shot") dev.shot_path = next();
        else if (a == "--after") dev.shot_after = (float)std::atof(next());
        else if (a == "--pos") dev.has_pos = std::sscanf(next(), "%f,%f", &dev.pos_x, &dev.pos_z) == 2;
        else if (a == "--yaw") dev.yaw_deg = (float)std::atof(next());
        else if (a == "--pitch") dev.pitch_deg = (float)std::atof(next());
        else if (a == "--walk") dev.walk = (float)std::atof(next());
        else if (a == "--juice") dev.juice = (float)std::atof(next());
        else if (a == "--sanity") dev.sanity = (float)std::atof(next());
        else if (a == "--health") dev.health = (float)std::atof(next());
        else if (a == "--entity") dev.entity = std::atoi(next());
        else if (a == "--dist") dev.entity_dist = (float)std::atof(next());
        else if (a == "--screen") dev.screen = next();
        else if (a == "--autowalk") dev.autowalk = true;
        else if (a == "--name") dev.name = next();
        else if (a == "--sandbox") dev.sandbox = true;
        else if (a == "--sandbox-menu") { dev.sandbox = true; dev.sandbox_menu = true; }
        else if (a == "--light") dev.light = true;
        else if (a == "--give-all") dev.give_all = true;
        else if (a == "--select") dev.select = std::atoi(next());
        else if (a == "--flare") dev.flare = std::atoi(next());
        else if (a == "--prop") dev.prop = next();
        else if (a == "--stun") dev.stun = true;
        else if (a == "--camcorder") dev.camcorder = 1;
        else if (a == "--nofilm") dev.film = 0;
        else if (a == "--noretro") dev.retro = 0;
        else if (a == "--nv") dev.nv = true;
        else if (a == "--safe") dev.safe = true;
        else if (a == "--vita-like") dev.vita_like = true;
        else if (a == "--quality") dev.quality = std::atoi(next());
        else if (a == "--at-camp") dev.at_camp = true;
        else if (a == "--at-exit") dev.at_exit = (float)std::atof(next());
        else if (a == "--nohud") dev.hud = false;
        else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    App app;
    if (!app.init(dev)) return 1;
    app.run();
    app.shutdown();
    return 0;
}
