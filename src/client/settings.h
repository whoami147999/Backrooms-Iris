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

    void load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace iris
