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

    // 1.3
    bool voice = true;          // proximity chat (PC)
    bool open_mic = false;      // false: push to talk (V)
    float voice_volume = 1.0f;
    bool reduce_flashing = false; // accessibility: no strobing lights, no screen flashes

    void load(const std::string& path);
    void save(const std::string& path) const;
};

// 1.3: what you've done, kept between games (stats.cfg next to the settings)
struct Stats {
    int runs = 0, deaths = 0, escapes = 0, levels_seen = 0;
    double play_seconds = 0;
    std::string notes;          // one char per note: '1' found
    std::string levels;         // one char per level: '1' visited
    void load(const std::string& path);
    void save(const std::string& path) const;
    bool note(int n) const { return n >= 1 && n <= (int)notes.size() && notes[(size_t)n - 1] == '1'; }
    int notes_found() const { int c = 0; for (char ch : notes) c += ch == '1'; return c; }
};

} // namespace iris
