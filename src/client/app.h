// Backrooms Iris - the client application: menus, HUD, sandbox tools, and the frame loop.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../platform/platform.h"
#include "effects.h"
#include "renderer.h"
#include "session.h"
#include "settings.h"
#if IRIS_MULTIPLAYER
#include "../net/client.h"
#include "tailscale.h"
#endif
#include "audio.h"
#include <unordered_map>

namespace iris {

struct DevOptions {          // command-line switches used for testing / screenshots
    std::string shot_path;   // save one screenshot and quit
    float shot_after = 0;    // seconds of normal play before the screenshot
    int level = -1;          // LevelId index
    uint32_t seed = 0;
    bool has_pos = false;
    float pos_x = 0, pos_z = 0, yaw_deg = 0, pitch_deg = 0;
    float walk = 0;
    float juice = -1, sanity = -1, health = -1;
    int entity = -1;         // put an entity in front of the camera
    float entity_dist = 5;
    std::string screen;      // open a menu screen directly
    bool host = false;
    std::string join;
    std::string password;    // dev: host / join password
    bool autowalk = false;
    std::string name;
    bool hud = true;
    bool sandbox = false;
    bool sandbox_menu = false;
    bool light = false;      // start with the flashlight on
    bool give_all = false;   // fill the inventory
    int select = -1;         // select this item type in the hotbar
    int flare = -1;          // drop a burning firesalt this far ahead
    std::string prop;        // drop a glow stick ("glow") or music box ("lure") ahead
    bool stun = false;       // the dev entity starts knocked down
    int difficulty = -1;
    int camcorder = -1;      // force the camcorder look on/off
    int film = -1;
    int retro = -1;
    float at_exit = -1;      // stand this far in front of the nearest exit door, facing it
    bool nv = false;         // night vision goggles on
    bool at_camp = false;    // stand next to the nearest wanderers' camp
    bool safe = false;       // dev: run with the Vita safe-mode settings
    bool vita_like = false;  // dev: PC with the Vita's render settings (profiling)
    int quality = -1;        // dev: graphics preset 0-3
    float time = -1;         // dev: seconds into the level (day / night)
};

class App {
public:
    bool init(const DevOptions& dev);
    void run();
    void shutdown();

private:
    enum class Screen { Title, Single, Sandbox, Multi, Host, Join, Settings, Controls, Connecting, Playing };

    struct Item {
        enum Kind { Button, Choice, Slider, Toggle, Text, Label, Spacer } kind = Button;
        std::string label;
        std::function<void()> on;
        std::vector<std::string> choices;
        int* choice = nullptr;
        float* value = nullptr;
        float vmin = 0, vmax = 1, vstep = 0.1f;
        bool* flag = nullptr;
        std::string* text = nullptr;
        std::string charset;
        int maxlen = 16;
        std::string hint;
        std::vector<std::string> hints; // Choice: one hint per value
    };
    struct Menu {
        std::string title, subtitle;
        std::vector<Item> items;
        int sel = 0;
    };

    DevOptions dev_;
    Settings cfg_;
    std::string data_dir_;
    Renderer ren_;
    RenderSettings rs_;
    InputState in_;
    std::unique_ptr<Session> session_;
#if IRIS_MULTIPLAYER
    NetClient browser_;          // LAN discovery for the Join screen
    double browse_t_ = 0;
    Tailscale tailscale_;        // tailnet discovery (PC): friends' games anywhere
    double tail_t_ = -100;
    TailStatus tail_;
#endif
    Audio audio_;
    // what the sound code saw last frame (to notice changes)
    struct AudioState {
        int step = 0;
        float swing = 99, health = 100, flash = 99;
        bool light = false, alive = true;
        uint32_t epoch = 0xFFFFFFFF, toast_seq = 0;
        double scan_t = -100;
        Vec2 fire_at;
        float fire_d = 1e9f;
        std::unordered_map<uint16_t, double> next;   // entity id -> next idle sound
        std::unordered_map<uint16_t, int> state;     // entity id -> last state
        std::unordered_map<int, int> other_step;     // other players' footsteps
    } as_;
    void update_audio(const struct GameView& v, Vec3 eye, float yaw);
    void play_at(Sfx s, Vec2 at, float vol, Vec3 eye, float yaw, const World& w, float pitch = 1);
    void item_sound(ItemType t);

    std::vector<Screen> stack_{Screen::Title};
    Menu menu_;
    bool paused_ = false;
    bool editing_ = false;       // a text field is being edited
    size_t cursor_ = 0;
    std::string error_;          // modal message

    // client-side game state
    InputCmd cmd_;
    bool crouch_toggle_ = false;
    ItemType selected_ = ItemType::None;
    uint32_t epoch_ = 0xFFFFFFFF;
    bool needs_load_ = false;
    double level_title_t_ = -100;
    struct Feed { std::string text; double t; };
    std::vector<Feed> feed_;
    std::string toast_;
    double toast_t_ = -100;
    uint32_t toast_seq_ = 0;
    double escaped_t_ = -100;
    double now_ = 0, frame_dt_ = 0;
    float fps_ = 0;
    bool film_on_ = false;   // the PC film shader ran this frame
    double compass_t_ = -100;
    float vm_last_yaw_ = 0, vm_sway_ = 0;
    Vec2 compass_to_;
    bool compass_ok_ = false;
    int frames_ = 0;
    int frames_total_ = 0;   // boot log: first frames only
    std::string crash_note_; // Vita: the stage the last run died in (shown on the title screen)
    bool night_shown_ = false;  // the level's day/night as last announced
    double use_anim_t_ = -100;  // last time you drank / ate / threw something (view model animation)
    ItemType use_anim_item_ = ItemType::None;
    double menu_t_ = 0;         // when the current menu screen opened (fade in)
    float menu_sel_y_ = -1;     // the selection bar glides between items
    double death_t_ = -100;     // when you died (the death screen fades in)
    bool stage_pending_ = false;
    double stage_t_ = 0;
    void stage(const std::string& s) { platform_boot_stage(s.c_str()); stage_pending_ = true; stage_t_ = platform_time(); }
    double fps_t_ = 0;
    bool debug_ = false;
    int single_level_ = 0;
    std::string single_seed_;
    int sandbox_level_ = 0;
    int host_level_ = 0;
    int lobby_level_ = 0;        // 1.2: what the host picks in the lobby
    bool lobby_sandbox_ = false;
    bool host_lobby_ = true;     // hosting starts in the lobby
    std::string join_ip_;
    World menu_world_{LevelId::L0, 7777};

    // sandbox spawn menu
    bool sb_open_ = false;
    int sb_tab_ = 0, sb_sel_ = 0;

    Screen screen() const { return stack_.back(); }
    bool in_game() const;
    void go(Screen s);
    void back();
    void build_menu();
    void menu_input();
    void menu_geom(float& y0, float& row, int& first, int& vis) const;
    int menu_scroll_ = -1;
    Sfx bgm_track_ = SFX_MUSIC_MENU;
    bool slider_drag_ = false;
    void draw_menu();
    void draw_menu_background();
    void start_session(std::unique_ptr<Session> s);
    void start_single(bool sandbox);
    void start_host();
    void start_join(const std::string& addr);
    uint16_t host_port() const;
    struct Quality { float view_dist; int texture_size; int mesh_detail; int msaa; float particles; float entity_detail; bool post_effects; bool shadows, reflections; };
    static Quality quality_preset(int level, bool vita);
    void apply_quality();
    void leave_game();
    void game_input();
    void sandbox_input();
    void draw_sandbox_menu();
    void draw_game(bool overlay_menu);
    void draw_hud(const struct GameView& v, const ScreenFx& fx);
    void draw_fx_overlays(const ScreenFx& fx);
    void apply_display_settings();
    void dev_setup();
    std::vector<ItemType> owned() const;
    float ui() const;  // UI scale (pixels per font pixel)
    std::string key_hint(const char* kb, const char* pad) const;
    void save_settings() { if (dev_.shot_path.empty()) cfg_.save(data_dir_ + "settings.cfg"); } // screenshots never overwrite your settings
};

} // namespace iris
