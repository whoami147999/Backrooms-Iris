// Backrooms Iris - the client application: menus, HUD, and the frame loop.
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
    int level = -1;
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
};

class App {
public:
    bool init(const DevOptions& dev);
    void run();
    void shutdown();

private:
    enum class Screen { Title, Single, Multi, Host, Join, Settings, Controls, Connecting, Playing };

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
    double error_t_ = 0;

    // game state on the client side
    InputCmd cmd_;
    bool crouch_toggle_ = false;
    uint32_t epoch_ = 0xFFFFFFFF;
    bool needs_load_ = false;
    double level_title_t_ = -100;
    struct Feed { std::string text; double t; };
    std::vector<Feed> feed_;
    double escaped_t_ = -100;
    double now_ = 0, frame_dt_ = 0;
    float fps_ = 0;
    int frames_ = 0;
    double fps_t_ = 0;
    bool debug_ = false;
    int single_level_ = 0;
    std::string single_seed_;
    int host_level_ = 0;
    std::string join_ip_;
    World menu_world_{LevelId::L0, 7777};
    bool menu_world_loaded_ = false;

    Screen screen() const { return stack_.back(); }
    void go(Screen s);
    void back();
    void build_menu();
    void menu_input();
    void draw_menu();
    void draw_menu_background();
    void start_single();
    void start_host();
    void start_join(const std::string& addr);
    void leave_game();
    void game_input();
    void draw_game();
    void draw_hud(const GameView& v, const ScreenFx& fx);
    void draw_fx_overlays(const ScreenFx& fx);
    void draw_pause();
    void apply_display_settings();
    void dev_setup();
    float ui() const;  // UI scale (pixels per font pixel)
    std::string key_hint(const char* kb, const char* pad) const;
    void save_settings() { cfg_.save(data_dir_ + "settings.cfg"); }
};

} // namespace iris
