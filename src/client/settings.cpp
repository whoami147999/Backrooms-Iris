#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace iris {

void Settings::load(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        float fv = (float)std::atof(v.c_str());
        if (k == "mouse_sens") mouse_sens = fv;
        else if (k == "stick_sens") stick_sens = fv;
        else if (k == "invert_y") invert_y = v == "1";
        else if (k == "fov") fov = fv;
        else if (k == "head_bob") head_bob = v == "1";
        else if (k == "fullscreen") fullscreen = v == "1";
        else if (k == "vsync") vsync = v == "1";
        else if (k == "show_fps") show_fps = v == "1";
        else if (k == "name") name = v.substr(0, 14);
        else if (k == "last_ip") last_ip = v.substr(0, 40);
        else if (k == "start_level") start_level = std::atoi(v.c_str());
        else if (k == "difficulty") difficulty = std::atoi(v.c_str());
        else if (k == "film") film = v == "1";
        else if (k == "camcorder") camcorder = v == "1";
        else if (k == "atmosphere") atmosphere = v == "1";
        else if (k == "retro") retro = v == "1";
        else if (k == "volume") volume = fv;
    }
    if (mouse_sens < 0.1f || mouse_sens > 5) mouse_sens = 1;
    if (stick_sens < 0.1f || stick_sens > 5) stick_sens = 1;
    if (fov < 50 || fov > 110) fov = 70;
    if (name.empty()) name = "WANDERER";
    if (difficulty < 0 || difficulty > 3) difficulty = 1;
    if (volume < 0 || volume > 1) volume = 0.8f;
}

void Settings::save(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "mouse_sens=%.2f\nstick_sens=%.2f\ninvert_y=%d\nfov=%.0f\nhead_bob=%d\nfullscreen=%d\nvsync=%d\nshow_fps=%d\nname=%s\nlast_ip=%s\nstart_level=%d\ndifficulty=%d\nfilm=%d\ncamcorder=%d\natmosphere=%d\nretro=%d\nvolume=%.2f\n",
                 mouse_sens, stick_sens, invert_y, fov, head_bob, fullscreen, vsync, show_fps, name.c_str(), last_ip.c_str(), start_level, difficulty, film, camcorder, atmosphere, retro, volume);
    std::fclose(f);
}

} // namespace iris
