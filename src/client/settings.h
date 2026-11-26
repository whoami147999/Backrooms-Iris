// Backrooms Iris - player settings, saved as key=value in the platform data dir.
#pragma once
#include <string>

namespace iris {

struct Settings {
    float mouse_sens = 1.0f;    // multiplier
    float stick_sens = 1.0f;
    bool invert_y = false;
    float fov = 70.0f;
    bool head_bob = true;
    bool fullscreen = false;
    bool vsync = true;
    bool show_fps = false;
    std::string name = "WANDERER";
    std::string last_ip = "192.168.1.";
    int start_level = 0;        // index into LevelId
    int difficulty = 1;         // Difficulty: 0 easy .. 3 nightmare
    bool film = true;           // film look (bloom, grain, aberration, grading)
    bool camcorder = false;     // found-footage camcorder look
    bool atmosphere = true;     // light halos/shafts, dust, flicker
    bool retro = true;          // PS1 look
    float volume = 0.8f;        // master volume 0..1
    bool safe_mode = false;     // Vita: no MSAA, sound, mipmaps or worker thread (turned on after a crashed boot)

    void load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace iris
