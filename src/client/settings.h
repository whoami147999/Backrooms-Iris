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
    std::string host_password;  // multiplayer (PC): blank = no password
    std::string join_password;
    std::string host_port = "27315"; // UDP port your game listens on (forward it for internet play)
    int start_level = 0;        // index into LevelId
    int difficulty = 1;         // Difficulty: 0 easy .. 3 nightmare
    bool film = true;           // film look (bloom, grain, aberration, grading)
    bool camcorder = false;     // found-footage camcorder look
    bool atmosphere = true;     // light halos/shafts, dust, flicker
    bool retro = true;          // PS1 look
    float volume = 0.8f;        // master volume 0..1
    float music_volume = 0.6f;  // 1.2: the music, on top of the master volume
    int quality = -1;           // graphics preset 0 low .. 3 ultra (-1: platform default)
    bool safe_mode = false;     // Vita: no MSAA, sound, mipmaps or worker thread (turned on after a crashed boot)
    std::string safe_mode_ver;  // the version that turned it on (a new version starts without it)

    void load(const std::string& path);
    void save(const std::string& path) const;
};

} // namespace iris
