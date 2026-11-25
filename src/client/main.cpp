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
#include "app.h"

using namespace iris;

static void usage() {
    std::printf(
        "Backrooms Iris %s\n\n"
        "  --level N           jump straight into level N (0, 1, 5, 8, 9, 11)\n"
        "  --seed N            world seed for --level\n"
        "  --host              host a multiplayer game right away\n"
        "  --join IP           join a multiplayer game right away\n"
        "  --help              this text\n\n"
        "dev: --shot FILE.bmp [--after SECS] [--pos X,Z] [--yaw DEG] [--pitch DEG] [--walk SECS]\n"
        "     [--juice SECS] [--sanity N] [--health N] [--entity TYPE [--dist M]] [--screen NAME] [--autowalk] [--nohud]\n",
        IRIS_VERSION);
}

static int level_from_number(int n) {
    const int nums[] = {0, 1, 5, 8, 9, 11};
    for (int i = 0; i < 6; i++) if (nums[i] == n) return i;
    return -1;
}

int main(int argc, char** argv) {
    DevOptions dev;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--level") dev.level = level_from_number(std::atoi(next()));
        else if (a == "--seed") dev.seed = (uint32_t)std::strtoul(next(), nullptr, 10);
        else if (a == "--host") dev.host = true;
        else if (a == "--join") dev.join = next();
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
        else if (a == "--nohud") dev.hud = false;
        else { std::fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    App app;
    if (!app.init(dev)) return 1;
    app.run();
    app.shutdown();
    return 0;
}
