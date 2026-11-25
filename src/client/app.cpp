#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "../core/config.h"
#include "../core/hash.h"
#include "font.h"

namespace iris {
namespace {

const char* LEVEL_NAMES[(int)LevelId::COUNT] = {
    "LEVEL 0 - THE LOBBY", "LEVEL 1 - HABITABLE ZONE", "LEVEL 5 - TERROR HOTEL",
    "LEVEL 8 - CAVE SYSTEM", "LEVEL 9 - THE SUBURBS", "LEVEL 11 - THE ENDLESS CITY",
};

std::string timestamp() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", std::localtime(&t));
    return buf;
}

const char* item_name(int slot) {
    return slot == 0 ? "ALMOND WATER" : (slot == 1 ? "ENERGY DRINK" : "MEMORY JUICE");
}

} // namespace

// =================================================================== setup
float App::ui() const { return std::max(1.0f, std::floor(ren_.height / 300.0f)); }

std::string App::key_hint(const char* kb, const char* pad) const {
    return in_.using_gamepad || platform_is_vita() ? pad : kb;
}

bool App::init(const DevOptions& dev) {
    dev_ = dev;
    data_dir_ = platform_data_dir();
    cfg_.load(data_dir_ + "settings.cfg");
    single_level_ = std::clamp(cfg_.start_level, 0, (int)LevelId::COUNT - 1);
    join_ip_ = cfg_.last_ip;

    PlatformConfig pc;
    pc.fullscreen = cfg_.fullscreen && dev.shot_path.empty();
    pc.vsync = cfg_.vsync;
    pc.hidden = !dev.shot_path.empty();
    if (!platform_init(pc)) return false;
    std::printf("[iris] Backrooms Iris %s on %s\n", IRIS_VERSION, platform_name());

    if (platform_is_vita()) {
        rs_.view_dist = 24.0f;
        rs_.texture_size = 256;
        rs_.chunk_builds_per_frame = 1;
        rs_.post_effects = false;
    }
    ren_.init(rs_);
    net_init();
    dev_setup();
    build_menu();
    return true;
}

void App::shutdown() {
    session_.reset();
    browser_.scan_stop();
    ren_.shutdown();
    save_settings();
    platform_shutdown();
}

void App::apply_display_settings() {
    platform_set_fullscreen(cfg_.fullscreen);
    platform_set_vsync(cfg_.vsync);
}

// Command-line shortcuts for testing: jump straight into a level / screen / state.
void App::dev_setup() {
    cmd_.yaw = dev_.yaw_deg * PI / 180.0f;
    cmd_.pitch = dev_.pitch_deg * PI / 180.0f;
    if (!dev_.name.empty()) cfg_.name = dev_.name;
    if (dev_.host) { host_level_ = std::max(0, dev_.level); start_host(); return; }
    if (!dev_.join.empty()) { start_join(dev_.join); return; }
    if (!dev_.screen.empty()) {
        const std::string& s = dev_.screen;
        if (s == "single") go(Screen::Single);
        else if (s == "multi") go(Screen::Multi);
        else if (s == "join") { go(Screen::Multi); go(Screen::Join); }
        else if (s == "host") { go(Screen::Multi); go(Screen::Host); }
        else if (s == "settings") go(Screen::Settings);
        else if (s == "controls") go(Screen::Controls);
        else if (s == "pause") { dev_.level = std::max(dev_.level, 0); }
        if (s != "pause") return;
    }
    if (dev_.level < 0) return;
    session_ = make_single_player((LevelId)dev_.level, dev_.seed ? dev_.seed : 1215, cfg_.name);
    stack_ = {Screen::Title, Screen::Playing};
    cmd_.yaw = dev_.yaw_deg * PI / 180.0f;
    cmd_.pitch = dev_.pitch_deg * PI / 180.0f;
    // the dev hooks poke the sim directly (screenshots / testing)
    if (GameSim* sim = session_->debug_sim()) {
        PlayerState& p = sim->players[0].st;
        if (dev_.has_pos) { p.pos.x = dev_.pos_x; p.pos.z = dev_.pos_z; }
        InputCmd walk = cmd_;
        walk.move_z = dev_.walk > 0 ? 1.0f : 0.0f;
        for (int t = 0; t < (int)(std::max(dev_.walk, 0.05f) * TICK_RATE); t++) sim->apply_input(0, walk);
        if (dev_.juice >= 0) p.juice = dev_.juice;
        if (dev_.sanity >= 0) p.sanity = dev_.sanity;
        if (dev_.health >= 0) p.health = dev_.health;
        p.inv[0] = 2; p.inv[1] = 1; p.inv[2] = 1;
        if (dev_.entity >= 0) {
            Entity e;
            e.id = 999;
            e.type = (EntityType)dev_.entity;
            Vec2 f = yaw_forward(cmd_.yaw);
            e.pos = Vec2{p.pos.x, p.pos.z} + f * dev_.entity_dist;
            e.yaw = cmd_.yaw + PI; // facing the player
            e.state = e.type == EntityType::Hound ? EntState::Stare : EntState::Wander;
            e.anim = 1.0f;
            e.speed = 1.0f;
            sim->ents.push_back(e);
            sim->freeze_ents = true; // hold still for the camera
        }
    }
    if (dev_.screen == "pause") paused_ = true;
}

// =================================================================== menus
void App::go(Screen s) {
    stack_.push_back(s);
    editing_ = false;
    build_menu();
}

void App::back() {
    editing_ = false;
    if (stack_.size() > 1) stack_.pop_back();
    if (screen() == Screen::Connecting) stack_.pop_back();
    save_settings();
    build_menu();
}

void App::build_menu() {
    Menu m;
    auto button = [&](const std::string& l, std::function<void()> f, const std::string& hint = "") {
        Item it; it.kind = Item::Button; it.label = l; it.on = f; it.hint = hint; m.items.push_back(it);
    };
    auto spacer = [&]() { Item it; it.kind = Item::Spacer; m.items.push_back(it); };
    auto label = [&](const std::string& l) { Item it; it.kind = Item::Label; it.label = l; m.items.push_back(it); };

    Screen sc = paused_ && screen() == Screen::Playing ? Screen::Playing : screen();
    switch (sc) {
        case Screen::Title:
            m.title = "BACKROOMS IRIS";
            m.subtitle = "CO-OP HORROR  -  " IRIS_VERSION;
            button("SINGLE PLAYER", [this] { go(Screen::Single); });
            button("MULTIPLAYER", [this] { go(Screen::Multi); });
            button("SETTINGS", [this] { go(Screen::Settings); });
            button("CONTROLS", [this] { go(Screen::Controls); });
            button("QUIT", [this] { in_.quit = true; });
            break;
        case Screen::Single: {
            m.title = "SINGLE PLAYER";
            m.subtitle = "FIND THE EXIT. DON'T GET FOUND.";
            Item lv; lv.kind = Item::Choice; lv.label = "START IN";
            for (auto n : LEVEL_NAMES) lv.choices.push_back(n);
            lv.choice = &single_level_;
            m.items.push_back(lv);
            Item sd; sd.kind = Item::Text; sd.label = "SEED"; sd.text = &single_seed_; sd.charset = "0123456789"; sd.maxlen = 10;
            sd.hint = "EMPTY = RANDOM WORLD";
            m.items.push_back(sd);
            spacer();
            button("START", [this] { start_single(); });
            button("BACK", [this] { back(); });
            break;
        }
        case Screen::Multi: {
            m.title = "MULTIPLAYER";
            m.subtitle = "UP TO 4 PLAYERS  -  PC AND VITA";
            Item nm; nm.kind = Item::Text; nm.label = "YOUR NAME"; nm.text = &cfg_.name;
            nm.charset = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"; nm.maxlen = 14;
            m.items.push_back(nm);
            spacer();
            button("HOST GAME", [this] { go(Screen::Host); }, "OTHERS JOIN YOU OVER LAN");
            button("JOIN GAME", [this] { go(Screen::Join); }, "FIND GAMES ON YOUR NETWORK");
            spacer();
            button("BACK", [this] { back(); });
            break;
        }
        case Screen::Host: {
            m.title = "HOST GAME";
            m.subtitle = "UDP PORT " + std::to_string(IRIS_DEFAULT_PORT);
            Item lv; lv.kind = Item::Choice; lv.label = "START IN";
            for (auto n : LEVEL_NAMES) lv.choices.push_back(n);
            lv.choice = &host_level_;
            m.items.push_back(lv);
            spacer();
            button("START HOSTING", [this] { start_host(); });
            button("BACK", [this] { back(); });
            break;
        }
        case Screen::Join: {
            m.title = "JOIN GAME";
            m.subtitle = browser_.found.empty() ? "SEARCHING YOUR NETWORK..." : "GAMES ON YOUR NETWORK";
            for (const FoundServer& f : browser_.found) {
                std::string addr = net_addr_str(f.addr);
                char buf[160];
                std::snprintf(buf, sizeof buf, "%s  %d/%d", f.name.c_str(), f.players, f.max_players);
                std::string ip = addr.substr(0, addr.find(':'));
                button(buf, [this, ip] { start_join(ip); }, f.level + "  " + addr + (f.protocol != IRIS_PROTOCOL_VERSION ? "  (OTHER VERSION)" : ""));
            }
            if (browser_.found.empty()) label("NOTHING YET  -  IS THE HOST ON THE SAME WIFI?");
            spacer();
            Item ip; ip.kind = Item::Text; ip.label = "DIRECT IP"; ip.text = &join_ip_; ip.charset = "0123456789."; ip.maxlen = 15;
            m.items.push_back(ip);
            button("CONNECT TO IP", [this] { start_join(join_ip_); });
            button("BACK", [this] { back(); });
            break;
        }
        case Screen::Settings: {
            m.title = "SETTINGS";
            auto slider = [&](const std::string& l, float* v, float lo, float hi, float step) {
                Item it; it.kind = Item::Slider; it.label = l; it.value = v; it.vmin = lo; it.vmax = hi; it.vstep = step; m.items.push_back(it);
            };
            auto toggle = [&](const std::string& l, bool* b, std::function<void()> on = nullptr) {
                Item it; it.kind = Item::Toggle; it.label = l; it.flag = b; it.on = on; m.items.push_back(it);
            };
            slider("MOUSE SENSITIVITY", &cfg_.mouse_sens, 0.2f, 3.0f, 0.1f);
            slider("STICK SENSITIVITY", &cfg_.stick_sens, 0.2f, 3.0f, 0.1f);
            toggle("INVERT LOOK", &cfg_.invert_y);
            slider("FIELD OF VIEW", &cfg_.fov, 50, 110, 5);
            toggle("HEAD BOB", &cfg_.head_bob);
            if (!platform_is_vita()) {
                toggle("FULLSCREEN", &cfg_.fullscreen, [this] { apply_display_settings(); });
                toggle("VSYNC", &cfg_.vsync, [this] { apply_display_settings(); });
            }
            toggle("SHOW FPS", &cfg_.show_fps);
            spacer();
            button("BACK", [this] { back(); });
            break;
        }
        case Screen::Controls:
            m.title = "CONTROLS";
            button("BACK", [this] { back(); });
            break;
        case Screen::Connecting:
            m.title = "CONNECTING";
            button("CANCEL", [this] { leave_game(); });
            break;
        case Screen::Playing: // pause menu
            m.title = "PAUSED";
            m.subtitle = session_ && session_->view().multiplayer ? "THE GAME KEEPS GOING WITHOUT YOU" : "";
            button("RESUME", [this] { paused_ = false; build_menu(); });
            button("SETTINGS", [this] { go(Screen::Settings); });
            button("CONTROLS", [this] { go(Screen::Controls); });
            button("QUIT TO MENU", [this] { leave_game(); });
            break;
    }
    // keep the selection on a usable item
    m.sel = std::min(menu_.title == m.title ? menu_.sel : 0, (int)m.items.size() - 1);
    while (m.sel < (int)m.items.size() - 1 && (m.items[(size_t)m.sel].kind == Item::Spacer || m.items[(size_t)m.sel].kind == Item::Label)) m.sel++;
    menu_ = m;
}

void App::menu_input() {
    if (!error_.empty()) {
        if (in_.accept || in_.back || in_.click) error_.clear();
        return;
    }
    auto& items = menu_.items;
    if (items.empty()) return;
    Item& it = items[(size_t)menu_.sel];

    if (editing_ && it.kind == Item::Text) {
        std::string& s = *it.text;
        cursor_ = std::min(cursor_, s.size());
        for (char c : in_.text) {
            char u = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
            if (it.charset.find(u) != std::string::npos && (int)s.size() < it.maxlen) s.insert(cursor_++, 1, u);
        }
        if (in_.backspace && cursor_ > 0) s.erase(--cursor_, 1);
        if (in_.left && cursor_ > 0) cursor_--;
        if (in_.right) {
            if (cursor_ < s.size()) cursor_++;
            else if ((int)s.size() < it.maxlen) { s.push_back(it.charset[0] == ' ' ? 'A' : it.charset[0]); cursor_ = s.size() - 1; }
        }
        // gamepad-friendly: up/down cycles the character under the cursor
        if ((in_.up || in_.down) && cursor_ < s.size()) {
            size_t p = it.charset.find(s[cursor_]);
            if (p == std::string::npos) p = 0;
            p = (p + it.charset.size() + (in_.up ? 1 : it.charset.size() - 1)) % it.charset.size();
            s[cursor_] = it.charset[p];
        } else if ((in_.up || in_.down) && cursor_ == s.size() && (int)s.size() < it.maxlen) {
            s.push_back(it.charset[in_.up ? (it.charset[0] == ' ' ? 1 : 0) : it.charset.size() - 1]);
        }
        if (in_.accept || in_.back || in_.click) { editing_ = false; save_settings(); }
        return;
    }

    auto move = [&](int d) {
        int n = (int)items.size();
        for (int k = 0; k < n; k++) {
            menu_.sel = (menu_.sel + d + n) % n;
            Item::Kind kd = items[(size_t)menu_.sel].kind;
            if (kd != Item::Spacer && kd != Item::Label) break;
        }
    };
    if (in_.up) move(-1);
    if (in_.down) move(1);

    // mouse hover / click
    float s = ui();
    float y0 = ren_.height * 0.36f, row = 13 * s;
    float pick = (in_.mouse_y - y0) / row;
    int hover = (int)std::floor(pick);
    bool mouse_on = !in_.using_gamepad && hover >= 0 && hover < (int)items.size() && in_.mouse_x > ren_.width * 0.2f &&
                    in_.mouse_x < ren_.width * 0.8f && items[(size_t)hover].kind != Item::Spacer && items[(size_t)hover].kind != Item::Label;
    if (mouse_on && in_.mouse_moved) menu_.sel = hover;

    Item& cur = items[(size_t)menu_.sel];
    int dir = in_.left ? -1 : (in_.right ? 1 : 0);
    bool activate = in_.accept || (in_.click && mouse_on && hover == menu_.sel);
    switch (cur.kind) {
        case Item::Button: if (activate && cur.on) { auto f = cur.on; f(); return; } break;
        case Item::Choice:
            if (dir || activate) {
                int n = (int)cur.choices.size();
                *cur.choice = (*cur.choice + (dir ? dir : 1) + n) % n;
            }
            break;
        case Item::Slider:
            if (dir) *cur.value = clampf(std::round((*cur.value + dir * cur.vstep) / cur.vstep) * cur.vstep, cur.vmin, cur.vmax);
            if (in_.click && mouse_on) { // click on the bar to set it
                float bx = ren_.width * 0.5f + 10 * s, bw = 80 * s;
                float t = clampf((in_.mouse_x - bx) / bw, 0, 1);
                *cur.value = clampf(std::round((cur.vmin + t * (cur.vmax - cur.vmin)) / cur.vstep) * cur.vstep, cur.vmin, cur.vmax);
            }
            break;
        case Item::Toggle:
            if (dir || activate) { *cur.flag = !*cur.flag; if (cur.on) cur.on(); }
            break;
        case Item::Text:
            if (activate) { editing_ = true; cursor_ = cur.text->size(); }
            break;
        default: break;
    }
    if (in_.back) {
        if (screen() == Screen::Playing && paused_) { paused_ = false; build_menu(); }
        else if (screen() != Screen::Title) back();
    }
}

void App::draw_menu_background() {
    // a slow drift through a fixed Level 0 behind the menus
    if (!menu_world_loaded_ || ren_.level() != LevelId::L0) { ren_.set_level(menu_world_); menu_world_loaded_ = true; }
    float t = (float)now_ * 0.25f;
    Camera cam;
    cam.eye = {CELL * 0.5f + std::sin(t * 0.3f) * 2.0f, 1.6f, CELL * 0.5f - t * 1.2f};
    cam.yaw = std::sin(t * 0.2f) * 0.5f;
    cam.pitch = -0.05f;
    cam.fov_deg = 70;
    ren_.update_chunks(menu_world_, cam.eye, 2);
    static std::unordered_set<uint32_t> none;
    ren_.draw_world(menu_world_, cam, (float)now_, 1.0f, none, {}, {}, {});
}

void App::draw_menu() {
    float s = ui();
    float W = (float)ren_.width, H = (float)ren_.height;
    ren_.rect(0, 0, W, H, rgba(0, 0, 0, 0.55f));
    ren_.sprite(SPR_VIGNETTE, 0, 0, W, H, rgba(1, 1, 1, 0.9f));
    // title, with a little VHS colour bleed
    float tp = s * 4;
    float ty = H * 0.12f;
    float jit = (hash01(1, (int)(now_ * 12), 0, 1) < 0.06f) ? s * 2 : 0;
    ren_.text(W / 2 - s + jit, ty, tp, menu_.title, rgba(1, 0.2f, 0.15f, 0.5f), 1);
    ren_.text(W / 2 + s, ty, tp, menu_.title, rgba(0.2f, 0.6f, 1, 0.4f), 1);
    ren_.text(W / 2, ty, tp, menu_.title, rgba(0.95f, 0.9f, 0.7f), 1);
    if (!menu_.subtitle.empty()) ren_.text(W / 2, ty + 9 * tp, s, menu_.subtitle, rgba(0.85f, 0.8f, 0.6f, 0.8f), 1);

    float y0 = H * 0.36f, row = 13 * s;
    if (screen() == Screen::Controls) {
        struct Row { const char* action; const char* kb; const char* pad; };
        static const Row rows[] = {
            {"MOVE", "W A S D", "LEFT STICK"}, {"LOOK", "MOUSE", "RIGHT STICK"}, {"SPRINT", "SHIFT", "R / R2"},
            {"CROUCH", "CTRL / C (HOLD)", "CIRCLE / B (TOGGLE)"}, {"PICK UP", "E", "CROSS / A"},
            {"ALMOND WATER", "1", "D-PAD LEFT"}, {"ENERGY DRINK", "2", "D-PAD UP"}, {"MEMORY JUICE", "3", "D-PAD RIGHT"},
            {"PAUSE", "ESC", "START"}, {"SCREENSHOT", "F12", "-"}, {"FULLSCREEN", "F11", "-"},
        };
        float cx[3] = {W * 0.14f, W * 0.42f, W * 0.68f};
        ren_.text(cx[1], y0 - row * 1.4f, s, "KEYBOARD + MOUSE", rgba(0.9f, 0.75f, 0.4f));
        ren_.text(cx[2], y0 - row * 1.4f, s, platform_is_vita() ? "PS VITA" : "GAMEPAD / PS VITA", rgba(0.9f, 0.75f, 0.4f));
        for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
            float y = y0 + i * row * 0.85f;
            ren_.text(cx[0], y, s, rows[i].action, rgba(0.95f, 0.92f, 0.8f));
            ren_.text(cx[1], y, s, rows[i].kb, rgba(0.8f, 0.78f, 0.7f));
            ren_.text(cx[2], y, s, rows[i].pad, rgba(0.8f, 0.78f, 0.7f));
        }
        y0 += 13.5f * row * 0.85f;
        ren_.text(W / 2, y0 - row * 1.1f, s, "SANITY DRAINS IN THE DARK AND NEAR THEM. STAY CLOSE TO YOUR FRIENDS.", rgba(0.7f, 0.65f, 0.55f), 1);
    }
    if (screen() == Screen::Connecting) {
        const char* dots[] = {"", ".", "..", "..."};
        ren_.text(W / 2, y0 - row * 2, s * 2, std::string("PLEASE WAIT") + dots[(int)(now_ * 3) % 4], rgba(0.9f, 0.85f, 0.7f), 1);
    }
    for (size_t i = 0; i < menu_.items.size(); i++) {
        const Item& it = menu_.items[i];
        float y = y0 + i * row;
        bool sel = (int)i == menu_.sel;
        if (it.kind == Item::Spacer) continue;
        uint32_t col = sel ? rgba(1, 0.95f, 0.65f) : rgba(0.78f, 0.74f, 0.62f);
        if (it.kind == Item::Label) { ren_.text(W / 2, y, s, it.label, rgba(0.6f, 0.58f, 0.5f), 1); continue; }
        if (sel) {
            ren_.rect(W * 0.2f, y - 3 * s, W * 0.6f, 11 * s, rgba(0.9f, 0.8f, 0.4f, 0.12f));
            ren_.rect(W * 0.2f, y - 3 * s, 2 * s, 11 * s, rgba(1, 0.9f, 0.5f, 0.9f));
        }
        if (it.kind == Item::Button) {
            ren_.text(W / 2, y, s * (sel ? 1.0f : 1.0f), it.label, col, 1);
        } else {
            ren_.text(W / 2 - 10 * s, y, s, it.label, col, 2);
            std::string val;
            if (it.kind == Item::Choice) val = "< " + it.choices[(size_t)*it.choice] + " >";
            if (it.kind == Item::Toggle) val = *it.flag ? "ON" : "OFF";
            if (it.kind == Item::Text) {
                val = *it.text;
                if (editing_ && sel) {
                    if ((int)(now_ * 3) % 2 == 0) {
                        float cxp = W / 2 + 10 * s + cursor_ * 6 * s;
                        ren_.rect(cxp, y + 8 * s, 5 * s, s, col);
                    }
                } else if (val.empty()) val = "-";
            }
            if (it.kind == Item::Slider) {
                float bx = W / 2 + 10 * s, bw = 80 * s;
                float t = (*it.value - it.vmin) / (it.vmax - it.vmin);
                ren_.rect(bx, y + 2 * s, bw, 3 * s, rgba(1, 1, 1, 0.15f));
                ren_.rect(bx, y + 2 * s, bw * t, 3 * s, col);
                char buf[16];
                std::snprintf(buf, sizeof buf, it.vstep >= 1 ? "%.0f" : "%.1f", *it.value);
                ren_.text(bx + bw + 6 * s, y, s, buf, col);
            } else {
                ren_.text(W / 2 + 10 * s, y, s, val, col);
            }
        }
        if (sel && !it.hint.empty()) ren_.text(W / 2, y + 8 * s, s * 0.999f, "", col, 1);
    }
    // help line at the bottom
    const Item* cur = menu_.items.empty() ? nullptr : &menu_.items[(size_t)menu_.sel];
    std::string help;
    if (editing_) help = key_hint("TYPE  -  ENTER: DONE", "{ }: CHANGE LETTER  ` \\: MOVE  CROSS: DONE");
    else if (cur && !cur->hint.empty()) help = cur->hint;
    else help = key_hint("ARROWS / MOUSE: CHOOSE  -  ENTER: SELECT  -  ESC: BACK", "{ }: CHOOSE  -  CROSS: SELECT  -  CIRCLE: BACK");
    ren_.text(W / 2, H - 16 * s, s, help, rgba(0.7f, 0.66f, 0.55f, 0.9f), 1);
    if (!error_.empty()) {
        ren_.rect(0, H * 0.4f, W, H * 0.2f, rgba(0, 0, 0, 0.9f));
        ren_.text(W / 2, H * 0.45f, s * 1.5f, error_, rgba(1, 0.4f, 0.3f), 1);
        ren_.text(W / 2, H * 0.53f, s, key_hint("PRESS ENTER", "PRESS CROSS"), rgba(0.8f, 0.75f, 0.6f), 1);
    }
}

// =================================================================== starting / leaving
void App::start_single() {
    cfg_.start_level = single_level_;
    save_settings();
    uint32_t seed = single_seed_.empty() ? (uint32_t)std::time(nullptr) * 2654435761u + 7 : (uint32_t)std::strtoul(single_seed_.c_str(), nullptr, 10);
    if (seed == 0) seed = 1;
    session_ = make_single_player((LevelId)single_level_, seed, cfg_.name);
    std::printf("[iris] single player: %s, seed %u\n", LEVEL_NAMES[single_level_], seed);
    stack_ = {Screen::Title, Screen::Playing};
    paused_ = false;
    epoch_ = 0xFFFFFFFF;
    build_menu();
}

void App::start_host() {
    std::string err;
    session_ = make_multiplayer("", IRIS_DEFAULT_PORT, cfg_.name, true, (LevelId)host_level_, now_, &err);
    if (!session_) { error_ = err.empty() ? "COULDN'T HOST" : err; return; }
    stack_ = {Screen::Title, Screen::Connecting};
    paused_ = false;
    epoch_ = 0xFFFFFFFF;
    build_menu();
}

void App::start_join(const std::string& addr) {
    std::string err;
    cfg_.last_ip = addr;
    save_settings();
    session_ = make_multiplayer(addr, IRIS_DEFAULT_PORT, cfg_.name, false, LevelId::L0, now_, &err);
    if (!session_) { error_ = err.empty() ? "COULDN'T CONNECT" : err; return; }
    stack_.push_back(Screen::Connecting);
    paused_ = false;
    epoch_ = 0xFFFFFFFF;
    build_menu();
}

void App::leave_game() {
    session_.reset();
    paused_ = false;
    stack_ = {Screen::Title};
    menu_world_loaded_ = false;
    platform_set_mouse_capture(false);
    build_menu();
}

// =================================================================== in-game
void App::game_input() {
    const GameView& v = session_->view();
    bool alive = v.me.alive;
    if (in_.pause && !paused_) { paused_ = true; build_menu(); return; }
    // look: mouse is applied every frame (no tick latency), stick is rate-based
    float inv = cfg_.invert_y ? -1.0f : 1.0f;
    cmd_.yaw -= in_.mouse_dx * 0.0022f * cfg_.mouse_sens;
    cmd_.pitch -= in_.mouse_dy * 0.0022f * cfg_.mouse_sens * inv;
    auto curve = [](float x) { return x * std::fabs(x); };
    cmd_.yaw -= curve(in_.look_x) * 2.8f * cfg_.stick_sens * (float)frame_dt_;
    cmd_.pitch -= curve(in_.look_y) * 1.9f * cfg_.stick_sens * inv * (float)frame_dt_;
    cmd_.pitch = clampf(cmd_.pitch, -1.45f, 1.45f);
    cmd_.yaw = std::remainder(cmd_.yaw, 2 * PI);
    cmd_.move_x = in_.move_x;
    cmd_.move_z = in_.move_z;
    if (dev_.autowalk) cmd_.move_z = 0.8f;
    if (in_.crouch_toggle) crouch_toggle_ = !crouch_toggle_;
    if (in_.sprint && in_.using_gamepad) crouch_toggle_ = false;
    cmd_.buttons = (uint8_t)((in_.sprint ? BTN_SPRINT : 0) | ((in_.crouch || crouch_toggle_) ? BTN_CROUCH : 0));
    if (alive) {
        if (in_.interact) cmd_.pickup_ctr++;
        for (int i = 0; i < 3; i++) if (in_.use[i]) cmd_.use_ctr[i]++;
    } else if (!v.multiplayer && (in_.accept || in_.interact)) {
        session_->request_respawn();
    }
}

void App::draw_fx_overlays(const ScreenFx& fx) {
    float W = (float)ren_.width, H = (float)ren_.height;
    if (fx.tint[3] > 0) ren_.rect(0, 0, W, H, rgba(fx.tint[0], fx.tint[1], fx.tint[2], fx.tint[3]));
    if (fx.grain > 0) {
        float ox = hash01(3, (int)(now_ * 30), 0, 1), oy = hash01(3, (int)(now_ * 30), 1, 1);
        ren_.tiled(SPR_NOISE, 0, 0, W, H, rgba(1, 1, 1, fx.grain * 0.3f), 128 * ui() * 0.5f, ox, oy);
    }
    if (fx.vignette > 0) {
        ren_.sprite(SPR_VIGNETTE, 0, 0, W, H, rgba(1, 1, 1, std::min(1.0f, fx.vignette)));
        if (fx.vignette > 0.6f) ren_.sprite(SPR_VIGNETTE, -W * 0.1f, -H * 0.1f, W * 1.2f, H * 1.2f, rgba(1, 1, 1, (fx.vignette - 0.6f) * 2.0f));
    }
    if (fx.red_flash > 0) ren_.sprite(SPR_VIGNETTE, 0, 0, W, H, rgba(0.8f, 0.05f, 0.02f, fx.red_flash), 0, false);
    for (const auto& sp : fx.spiders) {
        float sz = sp.size * H;
        ren_.sprite(SPR_SPIDER, sp.x * W - sz / 2, sp.y * H - sz / 2, sz, sz, rgba(1, 1, 1, 0.95f), sp.rot);
    }
    if (fx.whisper_alpha > 0.01f && !fx.whisper.empty()) {
        float s = ui();
        float j = (hash01(9, (int)(now_ * 20), 0, 1) - 0.5f) * s;
        ren_.text(fx.wx * W + j, fx.wy * H, s * 1.5f, fx.whisper, rgba(0.9f, 0.85f, 0.85f, fx.whisper_alpha), 1);
    }
    if (fx.blackout > 0) ren_.rect(0, 0, W, H, rgba(0, 0, 0, fx.blackout));
}

void App::draw_hud(const GameView& v, const ScreenFx& fx) {
    float s = ui();
    float W = (float)ren_.width, H = (float)ren_.height;
    const PlayerState& me = v.me;
    bool vita = platform_is_vita() || in_.using_gamepad;

    if (me.alive) {
        // ----- bars, bottom left
        float bx = 10 * s, by = H - 40 * s, bw = 70 * s, bh = 3 * s;
        auto bar = [&](float y, const char* name, float frac, uint32_t col, float shake) {
            float jx = shake > 0 ? (hash01(5, (int)(now_ * 25), (int)y, 1) - 0.5f) * shake * s * 2 : 0;
            ren_.text(bx + jx, y - 1 * s, s, name, rgba(0.85f, 0.82f, 0.72f, 0.85f));
            float x = bx + 46 * s + jx;
            ren_.rect(x - s, y - s, bw + 2 * s, bh + 2 * s, rgba(0, 0, 0, 0.55f));
            ren_.rect(x, y, bw * clampf(frac, 0, 1), bh, col);
        };
        bar(by, "HEALTH", me.health / 100, rgba(0.85f, 0.2f, 0.15f, 0.9f), 0);
        bar(by + 10 * s, "STAMINA", me.stamina, me.exhausted ? rgba(0.7f, 0.3f, 0.2f, 0.9f) : (me.boost > 0 ? rgba(0.4f, 1, 0.3f, 0.9f) : rgba(0.95f, 0.85f, 0.5f, 0.9f)), 0);
        bar(by + 20 * s, "SANITY", me.sanity / 100, rgba(0.6f, 0.45f, 0.95f, 0.9f), me.sanity < 30 ? (30 - me.sanity) / 30 : 0);

        // ----- inventory, bottom right
        static const Sprite icons[3] = {SPR_ALMOND, SPR_ENERGY, SPR_JUICE};
        const char* kb[3] = {"1", "2", "3"};
        const char* pad[3] = {"`", "{", "\\"};
        for (int i = 0; i < 3; i++) {
            float sz = 22 * s;
            float x = W - (3 - i) * (sz + 6 * s) - 4 * s, y = H - sz - 14 * s;
            bool has = me.inv[i] > 0;
            ren_.rect(x, y, sz, sz, rgba(0, 0, 0, has ? 0.5f : 0.3f));
            ren_.sprite(icons[i], x + 2 * s, y + 2 * s, sz - 4 * s, sz - 4 * s, rgba(1, 1, 1, has ? 1.0f : 0.25f));
            if (has) ren_.text(x + sz - 2 * s, y + sz - 8 * s, s, std::to_string(me.inv[i]), rgba(1, 1, 1), 2);
            ren_.text(x + sz / 2, y + sz + 3 * s, s, vita ? pad[i] : kb[i], rgba(0.85f, 0.8f, 0.65f, 0.8f), 1);
        }
        if (me.boost > 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "ENERGY %.0f", std::ceil(me.boost));
            ren_.text(W - 10 * s, H - 54 * s, s, buf, rgba(0.5f, 1, 0.4f), 2);
        }

        // ----- pick-up prompt
        ItemType t = ItemType::None;
        {
            int pcx = floor_to_int(me.pos.x / CELL), pcz = floor_to_int(me.pos.z / CELL);
            float best = PICKUP_RANGE;
            for (int cz = pcz - 1; cz <= pcz + 1; cz++)
                for (int cx = pcx - 1; cx <= pcx + 1; cx++) {
                    ItemType it = v.world.item(cx, cz);
                    if (it == ItemType::None || (v.picked && v.picked->count(World::item_id(cx, cz)))) continue;
                    float d = (v.world.item_pos(cx, cz) - Vec2{me.pos.x, me.pos.z}).len();
                    if (d < best) { best = d; t = it; }
                }
        }
        if (t != ItemType::None) {
            std::string p = (vita ? std::string(platform_is_vita() ? "CROSS" : "A") : std::string("E")) + "  PICK UP " + item_name((int)t - 1);
            ren_.rect(W / 2 - 1 * s, H / 2 - 1 * s, 2 * s, 2 * s, rgba(1, 1, 1, 0.8f));
            ren_.text(W / 2, H * 0.62f, s, p, rgba(1, 0.95f, 0.75f), 1);
        }
    }

    // ----- level title when you arrive
    double lt = now_ - level_title_t_;
    if (lt < 6.0 && !paused_) {
        float a = (float)std::min(std::min(lt / 1.0, 1.0), (6.0 - lt) / 1.5);
        const LevelInfo& li = v.world.info();
        ren_.text(W / 2, H * 0.28f, s * 3, li.code, rgba(0.95f, 0.9f, 0.7f, a), 1);
        ren_.text(W / 2, H * 0.28f + 26 * s, s * 1.5f, li.name, rgba(0.85f, 0.8f, 0.6f, a * 0.9f), 1);
    }

    // ----- event feed, top right
    for (size_t i = 0; i < feed_.size(); i++) {
        float age = (float)(now_ - feed_[i].t);
        float a = clampf(6.0f - age, 0, 1);
        ren_.text(W - 8 * s, 8 * s + i * 9 * s, s, feed_[i].text, rgba(0.9f, 0.85f, 0.7f, a * 0.9f), 2);
    }
    if (v.multiplayer) {
        for (size_t i = 0; i < v.roster.size(); i++) ren_.text(8 * s, 8 * s + i * 9 * s, s, v.roster[i], rgba(0.8f, 0.78f, 0.7f, 0.7f));
    }
    // name tags over other players
    for (const DrawPlayer& p : v.others) {
        float sx, sy;
        Vec3 head = p.pos + Vec3{0, 2.0f, 0};
        float d = (p.pos - me.pos).len();
        if (d < 25 && ren_.project(head, sx, sy) && !v.world.segment_blocked({me.pos.x, me.pos.z}, {p.pos.x, p.pos.z}))
            ren_.text(sx, sy, s, p.alive ? p.name : p.name + " (DOWN)", rgba(1, 1, 1, clampf(1.5f - d / 25, 0, 0.9f)), 1);
    }

    // ----- dead
    if (!me.alive) {
        ren_.rect(0, 0, W, H, rgba(0.05f, 0, 0, 0.55f));
        ren_.text(W / 2, H * 0.38f, s * 2, death_text(me.cause), rgba(0.95f, 0.3f, 0.25f), 1);
        std::string sub = v.multiplayer ? "RESPAWNING IN " + std::to_string((int)std::ceil(std::max(0.0f, v.respawn_t)))
                                        : key_hint("PRESS ENTER TO TRY AGAIN", platform_is_vita() ? "PRESS CROSS TO TRY AGAIN" : "PRESS A TO TRY AGAIN");
        ren_.text(W / 2, H * 0.38f + 24 * s, s, sub, rgba(0.85f, 0.8f, 0.7f), 1);
    }
    // ----- escaped (single player)
    double et = now_ - escaped_t_;
    if (et < 8.0) {
        float a = (float)std::min(std::min(et / 1.0, 1.0), (8.0 - et) / 1.5);
        ren_.rect(0, 0, W, H, rgba(0.9f, 0.9f, 0.85f, a * 0.85f));
        ren_.text(W / 2, H * 0.4f, s * 2, "YOU FOUND A WAY OUT", rgba(0.1f, 0.1f, 0.1f, a), 1);
        ren_.text(W / 2, H * 0.4f + 22 * s, s, "...THE CARPET IS DAMP AGAIN.", rgba(0.2f, 0.2f, 0.2f, a), 1);
    }
    if (cfg_.show_fps || debug_) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%.0f FPS  %.0f,%.0f%s", fps_, me.pos.x, me.pos.z, v.multiplayer ? "" : "");
        if (v.multiplayer) std::snprintf(buf, sizeof buf, "%.0f FPS  PING %.0f MS", fps_, v.ping_ms);
        ren_.text(W - 8 * s, H - 8 * s - 54 * s - 10 * s, s, buf, rgba(0.6f, 1, 0.6f, 0.8f), 2);
    }
    (void)fx;
}

void App::draw_game() {
    const GameView& v = session_->view();
    // level changes: new textures + chunks, and the title card
    if (v.epoch != epoch_) {
        if (!needs_load_) {
            needs_load_ = true; // draw one "loading" frame first so the pause is explained
            ren_.begin_2d();
            ren_.rect(0, 0, (float)ren_.width, (float)ren_.height, rgba(0, 0, 0, 1));
            ren_.text(ren_.width / 2.0f, ren_.height / 2.0f, ui() * 2, std::string("ENTERING ") + v.world.info().code, rgba(0.9f, 0.85f, 0.7f), 1);
            ren_.end_2d();
            return;
        }
        needs_load_ = false;
        epoch_ = v.epoch;
        ren_.set_level(v.world);
        menu_world_loaded_ = false;
        level_title_t_ = now_;
        if (v.escaped && !v.multiplayer) { escaped_t_ = now_; session_->clear_escaped(); }
        Vec3 eye = v.me.pos + Vec3{0, v.me.eye_height(), 0};
        ren_.update_chunks(v.world, eye, -1);
    }

    float a = v.alpha;
    Vec3 pos = v.me_prev.pos + (v.me.pos - v.me_prev.pos) * a;
    float eye_h = lerpf(v.me_prev.eye_height(), v.me.eye_height(), a);
    float bob = lerpf(v.me_prev.bob, v.me.bob, a);
    float bob_amp = cfg_.head_bob ? clampf(v.me.speed / WALK_SPEED, 0, 1.5f) * (1.0f - v.me.crouch * 0.5f) : 0;
    Vec2 right = yaw_right(cmd_.yaw);
    float side = std::cos(bob * 0.5f) * 0.022f * bob_amp;
    Camera cam;
    cam.eye = pos + Vec3{right.x * side, eye_h + std::sin(bob) * 0.03f * bob_amp, right.z * side};
    if (!v.me.alive) cam.eye.y = 0.35f; // you're on the floor
    cam.yaw = cmd_.yaw;
    cam.pitch = cmd_.pitch;

    ScreenFx fx = compute_fx(v.me, cam.eye, cam.yaw, (float)now_, (int)v.epoch * 7919 + v.my_id);
    cam.roll = fx.roll;
    cam.fov_deg = cfg_.fov + fx.fov_add;
    float fog = fx.fog_mult;
    if (v.world.info().dark) fog *= 1.0f + 0.06f * std::sin((float)now_ * 7.3f) * std::sin((float)now_ * 2.1f); // your light flickers

    ren_.update_chunks(v.world, cam.eye, rs_.chunk_builds_per_frame);
    static const std::unordered_set<uint32_t> none;
    ren_.draw_world(v.world, cam, (float)now_, fog, v.picked ? *v.picked : none, v.ents, v.others, fx.phantoms);
    ren_.post_warp(fx.wobble, fx.ghost, (float)now_);

    ren_.begin_2d();
    draw_fx_overlays(fx);
    if (dev_.hud) draw_hud(v, fx);
    if (paused_) draw_menu();
    ren_.end_2d();
}

// =================================================================== frame loop
void App::run() {
    double start = platform_time(), last = start;
    int shot_frames = 0;
    bool dev_applied = false;
    for (;;) {
        platform_poll(in_);
        if (in_.quit) break;
        now_ = platform_time();
        frame_dt_ = std::min(now_ - last, 0.25);
        last = now_;
        if (in_.toggle_debug) debug_ = !debug_;
        if (in_.toggle_fullscreen) { cfg_.fullscreen = !cfg_.fullscreen; apply_display_settings(); save_settings(); }

        bool playing = session_ && screen() == Screen::Playing;
        platform_set_mouse_capture(playing && !paused_ && dev_.shot_path.empty());

        // connecting -> playing / failed
        if (session_ && screen() == Screen::Connecting) {
            session_->update(cmd_, now_, (float)frame_dt_, false);
            std::string why;
            if (session_->failed(&why)) { leave_game(); error_ = why; }
            else if (session_->ready()) { stack_ = {Screen::Title, Screen::Playing}; build_menu(); }
        }
        if (screen() == Screen::Join) {
            if (now_ - browse_t_ > 1.0) { browser_.scan(now_); browse_t_ = now_; }
            size_t before = browser_.found.size();
            browser_.scan_poll(now_);
            if (browser_.found.size() != before || (int)(now_ * 2) % 2 == 0) {
                int sel = menu_.sel;
                build_menu();
                menu_.sel = std::min(sel, (int)menu_.items.size() - 1);
            }
        }

        if (playing) {
            if (!paused_) game_input();
            else menu_input();
            if (session_) {
                InputCmd c = cmd_;
                if (paused_) { c.move_x = c.move_z = 0; c.buttons = 0; }
                session_->update(c, now_, (float)frame_dt_, paused_ && !session_->view().multiplayer);
                // dev hooks: tweak the local player once the session exists (screenshots)
                if (!dev_applied && !dev_.shot_path.empty()) dev_applied = true;
                std::string why;
                if (session_->failed(&why)) { leave_game(); error_ = why; }
                else for (auto& e : session_->take_events()) {
                    feed_.push_back({e, now_});
                    if (feed_.size() > 5) feed_.erase(feed_.begin());
                }
            }
        } else if (screen() != Screen::Connecting) {
            menu_input();
        } else if (in_.back) {
            leave_game();
        }

        ren_.begin_frame(in_.width, in_.height);
        if (session_ && screen() == Screen::Playing) {
            draw_game();
        } else {
            draw_menu_background();
            ren_.begin_2d();
            draw_menu();
            ren_.end_2d();
        }
        platform_swap();

        if (in_.screenshot) {
            std::string path = data_dir_ + "iris_" + timestamp() + ".bmp";
            if (platform_save_screenshot(path)) {
                std::printf("[iris] screenshot: %s\n", path.c_str());
                feed_.push_back({"SCREENSHOT SAVED", now_});
            }
        }
        if (!dev_.shot_path.empty() && now_ - start >= dev_.shot_after && ++shot_frames > 3 && (!session_ || ren_.chunks_ready() || dev_.shot_after > 0)) {
            bool ok = platform_save_screenshot(dev_.shot_path);
            std::printf("[iris] %s %s\n", ok ? "saved" : "FAILED", dev_.shot_path.c_str());
            break;
        }

        frames_++;
        if (now_ - fps_t_ >= 1.0) {
            fps_ = (float)(frames_ / (now_ - fps_t_));
            if (debug_ && session_) {
                const GameView& v = session_->view();
                std::printf("[iris] %.0f fps | %s | pos %.1f %.1f | hp %.0f san %.0f | chunks %d | ents %zu\n", fps_,
                            v.world.info().code, v.me.pos.x, v.me.pos.z, v.me.health, v.me.sanity, ren_.chunks_loaded(), v.ents.size());
            }
            frames_ = 0;
            fps_t_ = now_;
        }
    }
}

} // namespace iris
