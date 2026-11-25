// Backrooms Iris - the client application: menus, HUD, sandbox tools, and the frame loop.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../net/client.h"
#include "../platform/platform.h"
#include "effects.h"
#include "renderer.h"
#include "session.h"
#include "settings.h"

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
    bool autowalk = false;
    std::string name;
    bool hud = true;
    bool sandbox = false;
    bool sandbox_menu = false;
    bool light = false;      // start with the flashlight on
    bool give_all = false;   // fill the inventory
    int select = -1;         // select this item type in the hotbar
    int flare = -1;          // drop a burning firesalt this far ahead
    bool stun = false;       // the dev entity starts knocked down
    int difficulty = -1;
    int camcorder = -1;      // force the camcorder look on/off
    int film = -1;
    float at_exit = -1;      // stand this far in front of the nearest exit door, facing it
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
    NetClient browser_;          // LAN discovery for the Join screen
    double browse_t_ = 0;

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
    int frames_ = 0;
    int frames_total_ = 0;   // boot log: first frames only
    double fps_t_ = 0;
    bool debug_ = false;
    int single_level_ = 0;
    std::string single_seed_;
    int sandbox_level_ = 0;
    int host_level_ = 0;
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
    void draw_menu();
    void draw_menu_background();
    void start_session(std::unique_ptr<Session> s);
    void start_single(bool sandbox);
    void start_host();
    void start_join(const std::string& addr);
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
