#include "app.h"

#include <cctype>

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

std::vector<std::string> level_names() {
    std::vector<std::string> v;
    for (int i = 0; i < (int)LevelId::COUNT; i++) v.push_back(level_label((LevelId)i));
    return v;
}

std::string timestamp() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", std::localtime(&t));
    return buf;
}

uint32_t random_seed() { return (uint32_t)std::time(nullptr) * 2654435761u + 7; }

} // namespace

// =================================================================== setup
// 2x on a 544p Vita / 720p, 4x on 1080p: always crisp, always readable
float App::ui() const { return std::max(1.0f, std::floor(ren_.height / 270.0f)); }

std::string App::key_hint(const char* kb, const char* pad) const {
    return in_.using_gamepad || platform_is_vita() ? pad : kb;
}

bool App::in_game() const {
    return session_ && std::find(stack_.begin(), stack_.end(), Screen::Playing) != stack_.end();
}

bool App::init(const DevOptions& dev) {
    dev_ = dev;
    data_dir_ = platform_data_dir();
    cfg_.load(data_dir_ + "settings.cfg");
    single_level_ = std::clamp(cfg_.start_level, 0, (int)LevelId::COUNT - 1);
    join_ip_ = cfg_.last_ip;
    if (dev.camcorder >= 0) cfg_.camcorder = dev.camcorder != 0;
    if (dev.film >= 0) cfg_.film = dev.film != 0;
    if (dev.retro >= 0) cfg_.retro = dev.retro != 0;
    if (platform_is_vita()) {
        crash_note_ = platform_last_crash();
        if (!crash_note_.empty()) {
            std::printf("[iris] last run crashed during '%s': safe mode on\n", crash_note_.c_str());
            cfg_.safe_mode = true;
            save_settings();
        }
    }
    const bool safe = (platform_is_vita() && cfg_.safe_mode) || dev.safe;

    PlatformConfig pc;
    pc.safe_mode = safe;
    pc.fullscreen = cfg_.fullscreen && dev.shot_path.empty();
    pc.vsync = cfg_.vsync;
    pc.hidden = !dev.shot_path.empty();
    if (!platform_init(pc)) return false;
    std::printf("[iris] Backrooms Iris %s on %s\n", IRIS_VERSION, platform_name());

    if (platform_is_vita()) {
        rs_.view_dist = 24.0f;
        rs_.texture_size = 512;     // sharper than before; generated once per level
        rs_.chunk_builds_per_frame = 1;
        rs_.post_effects = false;
        rs_.mesh_detail = 1;
    } else {
        rs_.texture_size = 1024;    // PC: double resolution textures
        rs_.mesh_detail = 2;        // and finer lighting grids
    }
    if (safe) { // the bare minimum: no worker thread, no mipmaps, small textures
        rs_.threaded = false;
        rs_.mipmaps = false;
        rs_.texture_size = 256;
        rs_.view_dist = 20.0f;
        rs_.mesh_detail = 1;
        rs_.chunk_builds_per_frame = 1;
    }
    rs_.film = cfg_.film;
    rs_.camcorder = cfg_.camcorder;
    rs_.atmosphere = cfg_.atmosphere;
    rs_.retro = cfg_.retro;
    if (safe) std::printf("[iris] safe mode: no sound\n");
    else if (dev.shot_path.empty()) { // (screenshots stay silent)
        platform_boot_stage("audio init");
        if (audio_.init()) audio_.set_master(cfg_.volume);
    }
    std::printf("[iris] renderer init\n");
    platform_boot_stage("renderer init");
    ren_.init(rs_);
    std::printf("[iris] net init\n");
    platform_boot_stage("network init");
    net_init();
    platform_boot_stage("menu setup");
    dev_setup();
    build_menu();
    std::printf("[iris] init done\n");
    stage("first frame");
    return true;
}

void App::shutdown() {
    audio_.shutdown();
    session_.reset();
    browser_.scan_stop();
    ren_.shutdown();
    save_settings();
    platform_boot_stage("exit");
    platform_shutdown();
}

void App::apply_display_settings() {
    platform_set_fullscreen(cfg_.fullscreen);
    platform_set_vsync(cfg_.vsync);
    ren_.set_style(cfg_.film, cfg_.camcorder, cfg_.atmosphere, cfg_.retro);
}

// Command-line shortcuts for testing: jump straight into a level / screen / state.
void App::dev_setup() {
    if (!dev_.name.empty()) cfg_.name = dev_.name;
    if (dev_.host) { host_level_ = std::max(0, dev_.level); if (dev_.difficulty >= 0) cfg_.difficulty = dev_.difficulty; start_host(); }
    else if (!dev_.join.empty()) start_join(dev_.join);
    else if (!dev_.screen.empty() && dev_.screen != "pause" && dev_.screen != "pause-settings") {
        const std::string& s = dev_.screen;
        if (s == "single") go(Screen::Single);
        else if (s == "sandbox") go(Screen::Sandbox);
        else if (s == "multi") go(Screen::Multi);
        else if (s == "join") { go(Screen::Multi); go(Screen::Join); }
        else if (s == "host") { go(Screen::Multi); go(Screen::Host); }
        else if (s == "settings") go(Screen::Settings);
        else if (s == "controls") go(Screen::Controls);
        return;
    } else if (dev_.level >= 0 || dev_.screen == "pause" || dev_.screen == "pause-settings") {
        start_session(make_single_player((LevelId)std::max(dev_.level, 0), dev_.seed ? dev_.seed : 1215, cfg_.name, dev_.sandbox,
                                         dev_.difficulty >= 0 ? (Difficulty)dev_.difficulty : (Difficulty)cfg_.difficulty));
        stack_ = {Screen::Title, Screen::Playing};
    }
    cmd_.yaw = dev_.yaw_deg * PI / 180.0f;
    cmd_.pitch = dev_.pitch_deg * PI / 180.0f;
    if (!session_) return;
    if (GameSim* sim = session_->sim()) { // poke the sim directly (screenshots / testing)
        PlayerState& p = sim->players[0].st;
        if (dev_.has_pos) { p.pos.x = dev_.pos_x; p.pos.z = dev_.pos_z; }
        if (dev_.nv) { p.inv[(int)ItemType::NightVision] = 1; p.nv_on = true; }
        if (dev_.at_camp) {
            int pcx = floor_to_int(p.pos.x / CELL), pcz = floor_to_int(p.pos.z / CELL), ccx, ccz;
            bool found = false;
            for (int r = 0; r < 40 && !found; r++)
                for (int dz = -r; dz <= r && !found; dz++)
                    for (int dx = -r; dx <= r && !found; dx++)
                        if (sim->world.camp(pcx + dx, pcz + dz, ccx, ccz)) {
                            found = true;
                            p.pos.x = (ccx + 0.5f) * CELL + 3.2f;
                            p.pos.z = (ccz + 1.0f) * CELL + 1.0f;
                            cmd_.yaw = std::atan2(1.0f, 0.5f);
                        }
        }
        if (dev_.at_exit >= 0) {
            Vec2 at;
            ExitDoor door;
            if (sim->nearest_exit({p.pos.x, p.pos.z}, at, &door)) {
                Vec2 s = door.pos + door.normal * dev_.at_exit;
                p.pos.x = s.x;
                p.pos.z = s.z;
                cmd_.yaw = std::atan2(door.normal.x, door.normal.z);
                std::printf("[iris] dev: exit door at %.1f,%.1f side %d\n", door.pos.x, door.pos.z, door.side);
            }
        }
        InputCmd walk = cmd_;
        walk.move_z = dev_.walk > 0 ? 1.0f : 0.0f;
        for (int t = 0; t < (int)(std::max(dev_.walk, 0.05f) * TICK_RATE); t++) sim->apply_input(0, walk);
        cmd_.pickup_ctr = walk.pickup_ctr;
        if (dev_.juice >= 0) p.juice = dev_.juice;
        if (dev_.sanity >= 0) p.sanity = dev_.sanity;
        if (dev_.health >= 0) p.health = dev_.health;
        p.inv[(int)ItemType::AlmondWater] = 2;
        p.inv[(int)ItemType::Flashlight] = 1;
        if (dev_.give_all) for (int i = 1; i < ITEM_COUNT; i++) p.inv[i] = item_info((ItemType)i).tool ? 1 : 3;
        if (dev_.light) p.light_on = true;
        if (dev_.select > 0) selected_ = (ItemType)dev_.select;
        if (dev_.entity >= 0) {
            Vec2 f = yaw_forward(cmd_.yaw);
            Entity& e = sim->spawn_entity((EntityType)dev_.entity, Vec2{p.pos.x, p.pos.z} + f * dev_.entity_dist, cmd_.yaw + PI);
            e.state = dev_.stun ? EntState::Stunned : (e.type == EntityType::Hound ? EntState::Stare : EntState::Wander);
            e.anim = 1.0f;
            e.speed = 1.0f;
            sim->freeze_ents = true; // hold still for the camera
        }
        if (!dev_.prop.empty()) {
            Vec2 f = yaw_forward(cmd_.yaw);
            Prop pr;
            pr.id = 901;
            pr.kind = dev_.prop == "lure" ? PropKind::Lure : PropKind::Glow;
            pr.pos = Vec2{p.pos.x, p.pos.z} + f * 2.2f;
            pr.life = 60;
            sim->props.push_back(pr);
        }
        if (dev_.flare > 0) {
            Vec2 f = yaw_forward(cmd_.yaw);
            Prop pr;
            pr.id = 900;
            pr.kind = PropKind::Flare;
            pr.pos = Vec2{p.pos.x, p.pos.z} + f * (float)dev_.flare;
            pr.life = FLARE_TIME;
            sim->props.push_back(pr);
        }
        session_->resync();
    }
    if (dev_.screen == "pause") { paused_ = true; build_menu(); }
    if (dev_.screen == "pause-settings") { paused_ = true; go(Screen::Settings); }
    if (dev_.sandbox_menu) sb_open_ = true;
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
    auto level_choice = [&](int* v) {
        Item lv; lv.kind = Item::Choice; lv.label = "START IN"; lv.choices = level_names(); lv.choice = v; m.items.push_back(lv);
    };
    auto diff_choice = [&]() {
        Item d; d.kind = Item::Choice; d.label = "DIFFICULTY"; d.choice = &cfg_.difficulty;
        for (int i = 0; i < (int)Difficulty::COUNT; i++) {
            d.choices.push_back(diff_info((Difficulty)i).name);
            d.hints.push_back(diff_info((Difficulty)i).blurb);
        }
        m.items.push_back(d);
    };

    switch (screen()) {
        case Screen::Title:
            m.title = "BACKROOMS IRIS";
            m.subtitle = "CO-OP HORROR  -  " IRIS_VERSION;
            if (!crash_note_.empty()) {
                std::string up = crash_note_;
                for (char& c : up) c = (char)std::toupper((unsigned char)c);
                m.subtitle = "SAFE MODE - LAST RUN CRASHED AT: " + up;
            } else if (platform_is_vita() && cfg_.safe_mode) m.subtitle = "SAFE MODE  -  " IRIS_VERSION;
            button("SINGLE PLAYER", [this] { go(Screen::Single); });
            button("SANDBOX", [this] { go(Screen::Sandbox); }, "SPAWN ENTITIES, ITEMS AND WEAPONS. NOTHING COUNTS.");
            button("MULTIPLAYER", [this] { go(Screen::Multi); });
            button("SETTINGS", [this] { go(Screen::Settings); });
            button("CONTROLS", [this] { go(Screen::Controls); });
            button("QUIT", [this] { in_.quit = true; });
            break;
        case Screen::Single: {
            m.title = "SINGLE PLAYER";
            m.subtitle = "FIND THE EXIT. DON'T GET FOUND.";
            level_choice(&single_level_);
            diff_choice();
            Item sd; sd.kind = Item::Text; sd.label = "SEED"; sd.text = &single_seed_; sd.charset = "0123456789"; sd.maxlen = 10;
            sd.hint = "EMPTY = RANDOM WORLD";
            m.items.push_back(sd);
            spacer();
            button("START", [this] { start_single(false); });
            button("BACK", [this] { back(); });
            break;
        }
        case Screen::Sandbox:
            m.title = "SANDBOX";
            m.subtitle = std::string("PRESS ") + key_hint("TAB", "SELECT") + " IN GAME TO SPAWN THINGS";
            level_choice(&sandbox_level_);
            diff_choice();
            spacer();
            button("START", [this] { start_single(true); });
            button("BACK", [this] { back(); });
            break;
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
        case Screen::Host:
            m.title = "HOST GAME";
            m.subtitle = "UDP PORT " + std::to_string(IRIS_DEFAULT_PORT);
            level_choice(&host_level_);
            diff_choice();
            spacer();
            if (!platform_is_vita()) {
                if (tail_.running && !tail_.self_ip.empty()) {
                    label("TAILSCALE: " + tail_.self_ip + "  (" + tail_.self_name + ")");
                    label("FRIENDS ON YOUR TAILNET SEE THIS GAME IN THEIR JOIN LIST");
                } else if (tail_.available) label("TAILSCALE IS INSTALLED BUT NOT CONNECTED");
                else label("ONLINE WITH FRIENDS: INSTALL TAILSCALE (SEE README)");
            }
            button("START HOSTING", [this] { start_host(); });
            button("BACK", [this] { back(); });
            break;
        case Screen::Join: {
            m.title = "JOIN GAME";
            m.subtitle = browser_.found.empty() ? "SEARCHING YOUR NETWORK..." : "GAMES ON YOUR NETWORK AND TAILNET";
            for (const FoundServer& f : browser_.found) {
                std::string addr = net_addr_str(f.addr);
                char buf[160];
                std::snprintf(buf, sizeof buf, "%s  %d/%d%s", f.name.c_str(), f.players, f.max_players, f.via.empty() ? "" : "  [TAILSCALE]");
                std::string ip = addr.substr(0, addr.find(':'));
                button(buf, [this, ip] { start_join(ip); },
                       f.level + "  " + (f.via.empty() ? addr : f.via + " " + addr) + (f.protocol != IRIS_PROTOCOL_VERSION ? "  (OTHER VERSION)" : ""));
            }
            if (browser_.found.empty()) label("NOTHING YET  -  SAME WIFI, OR BOTH ON TAILSCALE?");
            if (!platform_is_vita()) {
                int online = 0;
                for (const TailPeer& p : tail_.peers) online += p.online;
                if (tail_.running) label("TAILSCALE: " + tail_.self_ip + "  -  " + std::to_string(online) + " FRIEND PC(S) ONLINE");
                else if (tail_.available) label("TAILSCALE: NOT CONNECTED (RUN TAILSCALE UP)");
                else label("TAILSCALE: NOT INSTALLED (LAN ONLY)");
            }
            spacer();
            Item ip; ip.kind = Item::Text; ip.label = "IP OR NAME"; ip.text = &join_ip_;
            ip.charset = platform_is_vita() ? "0123456789." : "0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ-";
            ip.maxlen = 40;
            ip.hint = "A LAN IP, A TAILSCALE IP (100.X.X.X) OR A TAILSCALE MACHINE NAME";
            m.items.push_back(ip);
            button("CONNECT", [this] { start_join(join_ip_); });
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
            slider("VOLUME", &cfg_.volume, 0.0f, 1.0f, 0.1f);
            slider("MOUSE SENSITIVITY", &cfg_.mouse_sens, 0.2f, 3.0f, 0.1f);
            slider("STICK SENSITIVITY", &cfg_.stick_sens, 0.2f, 3.0f, 0.1f);
            toggle("INVERT LOOK", &cfg_.invert_y);
            slider("FIELD OF VIEW", &cfg_.fov, 50, 110, 5);
            toggle("HEAD BOB", &cfg_.head_bob);
            if (!platform_is_vita()) {
                toggle("FULLSCREEN", &cfg_.fullscreen, [this] { apply_display_settings(); });
                toggle("VSYNC", &cfg_.vsync, [this] { apply_display_settings(); });
            }
            toggle("RETRO PS1 LOOK", &cfg_.retro, [this] { apply_display_settings(); });
            m.items.back().hint = platform_is_vita() ? "CRISP PIXEL TEXTURES" : "CHUNKY PIXELS, DITHERING, WOBBLY VERTICES, CRISP TEXTURES";
            toggle("FILM EFFECTS", &cfg_.film, [this] { apply_display_settings(); });
            m.items.back().hint = platform_is_vita() ? "FILM GRAIN AND A DARK VIGNETTE" : "BLOOM, FILM GRAIN, LENS FRINGING, COLOUR GRADING";
            toggle("CAMCORDER LOOK", &cfg_.camcorder, [this] { apply_display_settings(); });
            m.items.back().hint = "FOUND FOOTAGE: SCANLINES, TRACKING NOISE, REC";
            toggle("LIGHT HALOS + DUST", &cfg_.atmosphere, [this] { apply_display_settings(); });
            m.items.back().hint = "GLOWING LAMPS, LIGHT SHAFTS, FLICKERING TUBES, DUST";
            toggle("SHOW FPS", &cfg_.show_fps);
            if (platform_is_vita()) {
                toggle("SAFE MODE", &cfg_.safe_mode, [this] { save_settings(); });
                m.items.back().hint = "NO SOUND, MSAA OR MIPMAPS. FOR CRASHES. RESTART THE GAME TO APPLY";
            }
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
            m.subtitle = session_ ? std::string(level_label(session_->view().world.id())) + "  -  " + diff_info(session_->view().world.difficulty()).name +
                                        (session_->view().multiplayer ? "  -  THE GAME KEEPS GOING" : "")
                                  : "";
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
    int sel_before = menu_.sel;
    bool ui_activate = (in_.accept || in_.click) && !editing_ && error_.empty();
    struct UiSound { App* a; int before; bool act; ~UiSound() {
        a->audio_.set_master(a->cfg_.volume);
        if (a->menu_.sel != before) a->audio_.play(SFX_UI_MOVE, 0.5f);
        else if (act) a->audio_.play(SFX_UI_SELECT, 0.6f);
    } } ui_sound{this, sel_before, ui_activate};
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
        if (in_.right && cursor_ < s.size()) cursor_++;
        else if (in_.right && in_.using_gamepad && (int)s.size() < it.maxlen) { // gamepad: grow the text
            s.push_back(it.charset[it.charset[0] == ' ' ? 1 : 0]);
            cursor_ = s.size() - 1;
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
    int hover = (int)std::floor((in_.mouse_y - y0 + 3 * s) / row);
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
    // a slow look around the Level 0 spawn room (it's open in every direction, so the
    // camera never ends up inside a wall)
    ren_.set_world(menu_world_);
    float t = (float)now_;
    Camera cam;
    cam.eye = {CELL * 0.5f + std::sin(t * 0.05f) * 0.6f, 1.6f + std::sin(t * 0.7f) * 0.01f, CELL * 0.5f + std::cos(t * 0.04f) * 0.6f};
    cam.yaw = t * 0.06f;
    cam.pitch = -0.04f + std::sin(t * 0.13f) * 0.04f;
    cam.fov_deg = 70;
    ren_.update_chunks(menu_world_, cam.eye, 2);
    static const std::unordered_set<uint32_t> none;
    ren_.draw_world(menu_world_, cam, (float)now_, 1.0f, none, {}, {}, {}, {});
}

void App::draw_menu() {
    float s = ui();
    float W = (float)ren_.width, H = (float)ren_.height;
    ren_.rect(0, 0, W, H, rgba(0, 0, 0, 0.55f));
    ren_.sprite(SPR_VIGNETTE, 0, 0, W, H, rgba(1, 1, 1, 0.9f));
    // title, with a little VHS colour bleed
    float tp = s * 4;
    float ty = H * 0.11f;
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
            {"CROUCH", "CTRL / C", "CIRCLE"}, {"PICK UP", "E", "CROSS"},
            {"USE ITEM / SWING", "LEFT CLICK / Q", "SQUARE / L / D-PAD UP"}, {"PICK ITEM", "WHEEL / 1-9", "D-PAD LEFT RIGHT"},
            {"FLASHLIGHT", "F", "TRIANGLE"}, {"SANDBOX MENU", "TAB", "SELECT"},
            {"PAUSE", "ESC", "START"}, {"SCREENSHOT", "F12", "-"},
        };
        float cx[3] = {W * 0.10f, W * 0.40f, W * 0.66f};
        float ry = row * 0.72f;
        float top = H * 0.27f;
        ren_.text(cx[1], top - ry * 1.3f, s, "KEYBOARD + MOUSE", rgba(0.9f, 0.75f, 0.4f));
        ren_.text(cx[2], top - ry * 1.3f, s, platform_is_vita() ? "PS VITA" : "GAMEPAD / PS VITA", rgba(0.9f, 0.75f, 0.4f));
        int n = (int)(sizeof(rows) / sizeof(rows[0]));
        for (int i = 0; i < n; i++) {
            float y = top + i * ry;
            ren_.text(cx[0], y, s, rows[i].action, rgba(0.95f, 0.92f, 0.8f));
            ren_.text(cx[1], y, s, rows[i].kb, rgba(0.8f, 0.78f, 0.7f));
            ren_.text(cx[2], y, s, rows[i].pad, rgba(0.8f, 0.78f, 0.7f));
        }
        ren_.text(W / 2, top + (n + 0.6f) * ry, s, "SANITY DRAINS IN THE DARK AND NEAR THEM. STAY CLOSE TO YOUR FRIENDS.", rgba(0.7f, 0.65f, 0.55f), 1);
        y0 = top + (n + 2.2f) * ry;
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
            ren_.text(W / 2, y, s, it.label, col, 1);
            continue;
        }
        ren_.text(W / 2 - 10 * s, y, s, it.label, col, 2);
        std::string val;
        if (it.kind == Item::Choice) val = "< " + it.choices[(size_t)*it.choice] + " >";
        if (it.kind == Item::Toggle) val = *it.flag ? "ON" : "OFF";
        if (it.kind == Item::Text) {
            val = *it.text;
            if (editing_ && sel) {
                if ((int)(now_ * 3) % 2 == 0) ren_.rect(W / 2 + 10 * s + cursor_ * 6 * s, y + 8 * s, 5 * s, s, col);
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
    // help line at the bottom
    const Item* cur = menu_.items.empty() ? nullptr : &menu_.items[(size_t)menu_.sel];
    std::string help;
    if (editing_) help = key_hint("TYPE  -  ENTER: DONE", "UP DOWN: CHANGE LETTER  LEFT RIGHT: MOVE  CROSS: DONE");
    else if (cur && cur->choice && !cur->hints.empty()) help = cur->hints[(size_t)std::clamp(*cur->choice, 0, (int)cur->hints.size() - 1)];
    else if (cur && !cur->hint.empty()) help = cur->hint;
    else help = key_hint("ARROWS / MOUSE: CHOOSE  -  ENTER: SELECT  -  ESC: BACK", "D-PAD: CHOOSE  -  CROSS: SELECT  -  CIRCLE: BACK");
    ren_.text(W / 2, H - 14 * s, s, help, rgba(0.7f, 0.66f, 0.55f, 0.9f), 1);
    if (!error_.empty()) {
        ren_.rect(0, H * 0.4f, W, H * 0.2f, rgba(0, 0, 0, 0.9f));
        ren_.text(W / 2, H * 0.45f, s * 1.5f, error_, rgba(1, 0.4f, 0.3f), 1);
        ren_.text(W / 2, H * 0.53f, s, key_hint("PRESS ENTER", "PRESS CROSS"), rgba(0.8f, 0.75f, 0.6f), 1);
    }
}

// =================================================================== starting / leaving
void App::start_session(std::unique_ptr<Session> s) {
    session_ = std::move(s);
    std::printf("[iris] session started\n");
    // a fresh game starts with fresh action counters (the sim also ignores the first command's)
    float yaw = cmd_.yaw;
    cmd_ = InputCmd{};
    cmd_.yaw = yaw;
    selected_ = ItemType::None;
    crouch_toggle_ = false;
    sb_open_ = false;
    paused_ = false;
    epoch_ = 0xFFFFFFFF;
    toast_seq_ = 0;
    toast_t_ = -100;
    feed_.clear();
}

void App::start_single(bool sandbox) {
    int level = sandbox ? sandbox_level_ : single_level_;
    if (!sandbox) cfg_.start_level = single_level_;
    save_settings();
    uint32_t seed = (sandbox || single_seed_.empty()) ? random_seed() : (uint32_t)std::strtoul(single_seed_.c_str(), nullptr, 10);
    if (seed == 0) seed = 1;
    start_session(make_single_player((LevelId)level, seed, cfg_.name, sandbox, (Difficulty)cfg_.difficulty));
    std::printf("[iris] %s: %s, %s, seed %u\n", sandbox ? "sandbox" : "single player", level_label((LevelId)level),
                diff_info((Difficulty)cfg_.difficulty).name, seed);
    stack_ = {Screen::Title, Screen::Playing};
    build_menu();
}

void App::start_host() {
    std::string err;
    save_settings();
    auto s = make_multiplayer("", IRIS_DEFAULT_PORT, cfg_.name, true, (LevelId)host_level_, (Difficulty)cfg_.difficulty, now_, &err);
    if (!s) { error_ = err.empty() ? "COULDN'T HOST" : err; return; }
    start_session(std::move(s));
    stack_ = {Screen::Title, Screen::Connecting};
    build_menu();
}

void App::start_join(const std::string& addr) {
    std::string err;
    cfg_.last_ip = addr;
    save_settings();
    auto s = make_multiplayer(addr, IRIS_DEFAULT_PORT, cfg_.name, false, LevelId::L0, Difficulty::Medium, now_, &err);
    if (!s) { error_ = err.empty() ? "COULDN'T CONNECT" : err; return; }
    start_session(std::move(s));
    stack_.push_back(Screen::Connecting);
    build_menu();
}

void App::leave_game() {
    audio_.stop_loops();
    session_.reset();
    paused_ = false;
    sb_open_ = false;
    stack_ = {Screen::Title};
    platform_set_mouse_capture(false);
    build_menu();
}

// =================================================================== in-game input
std::vector<ItemType> App::owned() const {
    std::vector<ItemType> out;
    if (!session_) return out;
    const PlayerState& me = session_->view().me;
    for (int i = 1; i < ITEM_COUNT; i++)
        if (me.inv[i] > 0) out.push_back((ItemType)i);
    return out;
}

void App::game_input() {
    const GameView& v = session_->view();
    bool alive = v.me.alive;
    if (in_.pause && !paused_) { paused_ = true; sb_open_ = false; build_menu(); return; }
    if (in_.sandbox_menu && session_->sim() && session_->sim()->sandbox) { sb_open_ = !sb_open_; sb_sel_ = 0; }
    if (sb_open_) { sandbox_input(); cmd_.move_x = cmd_.move_z = 0; cmd_.buttons = 0; return; }

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
    cmd_.buttons = (uint8_t)((in_.sprint ? BTN_SPRINT : 0) | ((in_.crouch || crouch_toggle_) ? BTN_CROUCH : 0) |
                             (in_.fire_held && selected_ == ItemType::Rifle ? BTN_FIRE : 0));
    cmd_.attack_item = is_weapon(selected_) ? (uint8_t)selected_ : 0;

    // hotbar: the items you own, in a fixed order
    std::vector<ItemType> have = owned();
    auto idx = std::find(have.begin(), have.end(), selected_);
    if (idx == have.end()) selected_ = have.empty() ? ItemType::None : have[0];
    if (!have.empty()) {
        int i = (int)(std::find(have.begin(), have.end(), selected_) - have.begin());
        if (in_.slot_next) i = (i + 1) % (int)have.size();
        if (in_.slot_prev) i = (i + (int)have.size() - 1) % (int)have.size();
        if (in_.slot_num >= 0 && in_.slot_num < (int)have.size()) i = in_.slot_num;
        selected_ = have[(size_t)i];
    }
    if (alive) {
        if (in_.interact) cmd_.pickup_ctr++;
        if (in_.light) cmd_.light_ctr++;
        if (in_.use_item && selected_ != ItemType::None) {
            if (is_weapon(selected_)) cmd_.attack_ctr++;
            else if (selected_ == ItemType::Flashlight) cmd_.light_ctr++;
            else if (selected_ == ItemType::Compass || selected_ == ItemType::Radio) {} // passive tools
            else { cmd_.use_item = (uint8_t)selected_; cmd_.use_ctr++; item_sound(selected_); }
        }
    } else if (!v.multiplayer && (in_.accept || in_.interact)) {
        session_->request_respawn();
    }
}

// =================================================================== sandbox
namespace {
const char* SB_TABS[3] = {"ENTITIES", "ITEMS", "TOOLS"};
const int SB_TOOLS = 8;
}

void App::sandbox_input() {
    GameSim* sim = session_->sim();
    if (!sim) { sb_open_ = false; return; }
    int count = sb_tab_ == 0 ? (int)EntityType::COUNT : sb_tab_ == 1 ? ITEM_COUNT - 1 : SB_TOOLS;
    if (in_.left) { sb_tab_ = (sb_tab_ + 2) % 3; sb_sel_ = 0; }
    if (in_.right) { sb_tab_ = (sb_tab_ + 1) % 3; sb_sel_ = 0; }
    if (in_.up) sb_sel_ = (sb_sel_ + count - 1) % count;
    if (in_.down) sb_sel_ = (sb_sel_ + 1) % count;
    if (in_.back) { sb_open_ = false; return; }
    if (!(in_.accept || in_.interact)) return;
    PlayerState& me = sim->players[0].st;
    Vec2 f = yaw_forward(cmd_.yaw);
    Vec2 here{me.pos.x, me.pos.z};
    // spawn in front, but never inside a wall: walk the ray until it's blocked
    auto ahead = [&](float dist) {
        Vec2 at = here;
        for (float d = 0.5f; d <= dist; d += 0.25f) {
            Vec2 c = here + f * d;
            if (sim->world.segment_blocked(here, c) || sim->world.solid(floor_to_int(c.x / CELL), floor_to_int(c.z / CELL))) break;
            at = c;
        }
        return at;
    };
    if (sb_tab_ == 0) {
        EntityType t = (EntityType)sb_sel_;
        sim->spawn_entity(t, ahead(4.5f), cmd_.yaw + PI);
        feed_.push_back({std::string("SPAWNED ") + entity_name(t), now_});
    } else if (sb_tab_ == 1) {
        ItemType t = (ItemType)(sb_sel_ + 1);
        sim->spawn_item(t, ahead(1.5f));
        feed_.push_back({std::string("SPAWNED ") + item_info(t).name, now_});
    } else {
        switch (sb_sel_) {
            case 0: sim->god = !sim->god; break;
            case 1: me.noclip = !me.noclip; break;
            case 2: sim->auto_spawn = !sim->auto_spawn; break;
            case 3: sim->freeze_ents = !sim->freeze_ents; break;
            case 4: sim->clear_entities(); feed_.push_back({"ENTITIES CLEARED", now_}); break;
            case 5: me.health = 100; me.sanity = 100; me.stamina = 1; me.juice = -1; feed_.push_back({"HEALED", now_}); break;
            case 6: {
                Vec2 ex;
                if (sim->nearest_exit(here, ex)) { me.pos.x = ex.x; me.pos.z = ex.z; session_->resync(); }
                break;
            }
            case 7: sim->next_level(); sb_open_ = false; break;
        }
    }
    if (feed_.size() > 5) feed_.erase(feed_.begin());
}

void App::draw_sandbox_menu() {
    GameSim* sim = session_->sim();
    if (!sim) return;
    float s = ui(), W = (float)ren_.width, H = (float)ren_.height;
    float pw = 200 * s, ph = 136 * s, px = W - pw - 8 * s, py = 8 * s;
    ren_.rect(px, py, pw, ph, rgba(0, 0, 0, 0.75f));
    ren_.rect(px, py, pw, s, rgba(1, 0.85f, 0.4f, 0.8f));
    float tx = px + 4 * s;
    for (int t = 0; t < 3; t++) {
        uint32_t c = t == sb_tab_ ? rgba(1, 0.92f, 0.6f) : rgba(0.6f, 0.58f, 0.5f);
        tx += ren_.text(tx, py + 4 * s, s, SB_TABS[t], c) + 8 * s;
    }
    float y = py + 16 * s;
    const int VIS = 11; // rows that fit: the list scrolls with the selection
    int count = sb_tab_ == 0 ? (int)EntityType::COUNT : sb_tab_ == 1 ? ITEM_COUNT - 1 : SB_TOOLS;
    int first = std::clamp(sb_sel_ - VIS / 2, 0, std::max(0, count - VIS));
    auto row = [&](int i, const std::string& label, const std::string& value, Sprite icon) {
        if (i < first || i >= first + VIS) return;
        bool sel = i == sb_sel_;
        if (sel) ren_.rect(px + 2 * s, y - 2 * s, pw - 4 * s, 10 * s, rgba(0.9f, 0.8f, 0.4f, 0.18f));
        float x = px + 5 * s;
        if (icon != SPR_WHITE) { ren_.sprite(icon, x, y - 2 * s, 9 * s, 9 * s, rgba(1, 1, 1)); x += 11 * s; }
        ren_.text(x, y, s, label, sel ? rgba(1, 0.95f, 0.7f) : rgba(0.82f, 0.78f, 0.68f));
        if (!value.empty()) ren_.text(px + pw - 5 * s, y, s, value, value == "ON" ? rgba(0.5f, 1, 0.5f) : rgba(0.7f, 0.6f, 0.55f), 2);
        y += 10 * s;
    };
    if (sb_tab_ == 0) {
        for (int i = 0; i < (int)EntityType::COUNT; i++) row(i, entity_name((EntityType)i), "", SPR_WHITE);
    } else if (sb_tab_ == 1) {
        for (int i = 1; i < ITEM_COUNT; i++) row(i - 1, item_info((ItemType)i).name, item_info((ItemType)i).sandbox_only ? "GUN" : "", item_sprite((ItemType)i));
        if (first > 0) ren_.text(px + pw - 6 * s, py + 16 * s, s, "^", rgba(1, 0.9f, 0.6f), 2);
        if (first + VIS < count) ren_.text(px + pw - 6 * s, py + ph - 20 * s, s, "V", rgba(1, 0.9f, 0.6f), 2);
    } else {
        const PlayerState& me = sim->players[0].st;
        row(0, "GOD MODE", sim->god ? "ON" : "OFF", SPR_WHITE);
        row(1, "NOCLIP", me.noclip ? "ON" : "OFF", SPR_WHITE);
        row(2, "AUTO SPAWNING", sim->auto_spawn ? "ON" : "OFF", SPR_WHITE);
        row(3, "FREEZE ENTITIES", sim->freeze_ents ? "ON" : "OFF", SPR_WHITE);
        row(4, "CLEAR ALL ENTITIES", "", SPR_WHITE);
        row(5, "HEAL ME", "", SPR_WHITE);
        row(6, "GO TO NEAREST EXIT", "", SPR_WHITE);
        row(7, "NEXT LEVEL", "", SPR_WHITE);
    }
    ren_.text(px + pw / 2, py + ph - 9 * s, s, key_hint("ENTER: SPAWN   TAB: CLOSE", "CROSS: SPAWN   SELECT: CLOSE"), rgba(0.65f, 0.6f, 0.5f), 1);
    (void)H;
}

// =================================================================== HUD
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
    if (fx.red_flash > 0) ren_.sprite(SPR_VIGNETTE, 0, 0, W, H, rgba(0.8f, 0.05f, 0.02f, fx.red_flash));
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
    bool pad = platform_is_vita() || in_.using_gamepad;

    if (me.alive) {
        // ----- held item, bottom right of the view (drawn first so the HUD sits on top)
        if (is_gun(selected_)) { // crosshair (the gun itself is a 3D view model now)
            ren_.rect(W / 2 - 4 * s, H / 2 - s * 0.5f, 3 * s, s, rgba(1, 1, 1, 0.8f));
            ren_.rect(W / 2 + 1 * s, H / 2 - s * 0.5f, 3 * s, s, rgba(1, 1, 1, 0.8f));
            ren_.rect(W / 2 - s * 0.5f, H / 2 - 4 * s, s, 3 * s, rgba(1, 1, 1, 0.8f));
            ren_.rect(W / 2 - s * 0.5f, H / 2 + 1 * s, s, 3 * s, rgba(1, 1, 1, 0.8f));
        }
        // compass: a needle toward the nearest exit door
        // compass (nerfed): only while you hold it, short range, a needle that won't sit still, no distance
        if (selected_ == ItemType::Compass && me.has(ItemType::Compass)) {
            if (now_ - compass_t_ > 2.0) {
                compass_t_ = now_;
                compass_ok_ = false;
                float best = 1e9f;
                int pcx = floor_to_int(me.pos.x / CELL), pcz = floor_to_int(me.pos.z / CELL);
                for (int cz = pcz - 16; cz <= pcz + 16; cz++)
                    for (int cx = pcx - 16; cx <= pcx + 16; cx++) {
                        ExitDoor d;
                        if (!v.world.exit_door(cx, cz, &d)) continue;
                        float dist = (d.pos - Vec2{me.pos.x, me.pos.z}).len();
                        if (dist < best) { best = dist; compass_to_ = d.pos; compass_ok_ = true; }
                    }
            }
            float cx = W / 2, cy = 22 * s, r = 12 * s;
            ren_.sprite(SPR_GLOW, cx - r * 1.4f, cy - r * 1.4f, r * 2.8f, r * 2.8f, rgba(0, 0, 0, 0.6f));
            if (compass_ok_) {
                Vec2 d = compass_to_ - Vec2{me.pos.x, me.pos.z};
                float ang = std::atan2(-d.x, -d.z) - cmd_.yaw; // relative to where you face
                float shake = 0.35f + clampf(1.0f - v.world.light_level(me.pos) * 2.0f, 0, 1) * 0.4f; // worse in the dark
                ang += (std::sin((float)now_ * 1.3f) * 0.6f + std::sin((float)now_ * 3.7f + 1.0f) * 0.4f) * shake;
                ren_.sprite(SPR_WHITE, cx - s * 0.75f, cy - r, 1.5f * s, 2 * r, rgba(0.5f, 0.5f, 0.5f, 0.8f), -ang);
                ren_.sprite(SPR_GLOW, cx + std::sin(-ang) * r - 3 * s, cy - std::cos(-ang) * r - 3 * s, 6 * s, 6 * s, rgba(0.4f, 1, 0.5f), 0, true);
                if (d.len() < 12.0f) ren_.text(cx, cy + r + 2 * s, s, "CLOSE", rgba(0.6f, 1, 0.65f, 0.85f), 1);
            } else {
                ren_.sprite(SPR_WHITE, cx - s * 0.75f, cy - r, 1.5f * s, 2 * r, rgba(0.6f, 0.6f, 0.6f), (float)now_ * 3.0f); // spinning: nothing near
                ren_.text(cx, cy + r + 2 * s, s, "...", rgba(0.7f, 0.7f, 0.7f, 0.7f), 1);
            }
        }
        // radio: static rises as something gets close
        if (me.has(ItemType::Radio)) {
            float nearest = 1e9f;
            for (const DrawEntity& e : v.ents) nearest = std::min(nearest, (e.pos - Vec2{me.pos.x, me.pos.z}).len());
            float sig = clampf(1.0f - nearest / 35.0f, 0, 1);
            if (sig > 0) {
                float ox = hash01(17, (int)(now_ * 30), 0, 1), oy = hash01(17, (int)(now_ * 30), 1, 1);
                ren_.tiled(SPR_NOISE, 0, 0, W, H, rgba(1, 1, 1, sig * sig * 0.18f), 128 * s * 0.5f, ox, oy);
            }
            float rx = W - 74 * s, ry = H - 62 * s;
            ren_.text(rx, ry, s, "RADIO", rgba(0.85f, 0.82f, 0.72f, 0.85f));
            for (int k = 0; k < 8; k++) {
                bool lit = k < (int)(sig * 8 + (hash01(19, (int)(now_ * 12), k, 1) - 0.5f) * 2 * sig);
                ren_.rect(rx + 32 * s + k * 4 * s, ry + 6 * s - k * 0.8f * s, 3 * s, 1 * s + k * 0.8f * s, lit ? rgba(1, 0.4f + 0.6f * (1 - sig), 0.2f, 0.9f) : rgba(0.3f, 0.3f, 0.3f, 0.6f));
            }
        }
        if (me.flash < 1.2f) ren_.rect(0, 0, W, H, rgba(1, 1, 0.95f, 1.0f - me.flash / 1.2f)); // flash bulb

        // ----- bars, bottom left
        float bx = 8 * s, by = H - 42 * s, bw = 64 * s, bh = 3 * s;
        auto bar = [&](float y, const char* name, float frac, uint32_t col, float shake) {
            float jx = shake > 0 ? (hash01(5, (int)(now_ * 25), (int)y, 1) - 0.5f) * shake * s * 2 : 0;
            ren_.text(bx + jx, y - 1 * s, s, name, rgba(0.85f, 0.82f, 0.72f, 0.85f));
            float x = bx + 46 * s + jx;
            ren_.rect(x - s, y - s, bw + 2 * s, bh + 2 * s, rgba(0, 0, 0, 0.55f));
            ren_.rect(x, y, bw * clampf(frac, 0, 1), bh, col);
        };
        bar(by, "HEALTH", me.health / 100, rgba(0.85f, 0.2f, 0.15f, 0.9f), 0);
        bar(by + 9 * s, "STAMINA", me.stamina, me.exhausted ? rgba(0.7f, 0.3f, 0.2f, 0.9f) : (me.boost > 0 ? rgba(0.4f, 1, 0.3f, 0.9f) : rgba(0.95f, 0.85f, 0.5f, 0.9f)), 0);
        bar(by + 18 * s, "SANITY", me.sanity / 100, rgba(0.6f, 0.45f, 0.95f, 0.9f), me.sanity < 30 ? (30 - me.sanity) / 30 : 0);
        if (me.has(ItemType::Flashlight) || me.has(ItemType::NightVision)) bar(by + 27 * s, "LIGHT", me.battery, me.light_on ? rgba(1, 0.97f, 0.8f, 0.9f) : rgba(0.6f, 0.6f, 0.55f, 0.7f), 0);
        // active effects
        std::string fxline;
        char buf[48];
        if (me.boost > 0) { std::snprintf(buf, sizeof buf, "ENERGY %.0f  ", std::ceil(me.boost)); fxline += buf; }
        if (me.lucky > 0) { std::snprintf(buf, sizeof buf, "LUCKY %.0f  ", std::ceil(me.lucky)); fxline += buf; }
        if (me.slow > 0) { std::snprintf(buf, sizeof buf, "SLOW %.0f  ", std::ceil(me.slow)); fxline += buf; }
        if (!fxline.empty()) ren_.text(bx, by - 11 * s, s, fxline, rgba(0.6f, 1, 0.6f, 0.9f));

        // ----- hotbar, bottom right
        std::vector<ItemType> have = owned();
        float sz = 18 * s, gap = 3 * s;
        float hx = W - have.size() * (sz + gap) - 6 * s, hy = H - sz - 8 * s;
        for (size_t i = 0; i < have.size(); i++) {
            ItemType t = have[i];
            float x = hx + i * (sz + gap);
            bool sel = t == selected_;
            ren_.rect(x - (sel ? s : 0), hy - (sel ? s : 0), sz + (sel ? 2 * s : 0), sz + (sel ? 2 * s : 0), sel ? rgba(1, 0.85f, 0.45f, 0.85f) : rgba(0, 0, 0, 0.5f));
            ren_.rect(x, hy, sz, sz, rgba(0.05f, 0.05f, 0.05f, 0.75f));
            ren_.sprite(item_sprite(t), x + s, hy + s, sz - 2 * s, sz - 2 * s, rgba(1, 1, 1));
            if (!item_info(t).tool && me.inv[(int)t] > 1) ren_.text(x + sz - s, hy + sz - 8 * s, s, std::to_string(me.inv[(int)t]), rgba(1, 1, 1), 2);
            if (!pad && i < 9) ren_.text(x + 2 * s, hy + 2 * s, s, std::to_string(i + 1), rgba(1, 1, 1, 0.5f));
        }
        if (selected_ != ItemType::None) {
            const ItemInfo& info = item_info(selected_);
            std::string use = pad ? (platform_is_vita() ? "SQUARE" : "X") : "CLICK";
            ren_.text(W - 6 * s, hy - 19 * s, s, info.name, rgba(1, 0.92f, 0.7f), 2);
            ren_.text(W - 6 * s, hy - 10 * s, s, use + ": " + (selected_ == ItemType::Crowbar ? "SWING" : is_gun(selected_) ? (selected_ == ItemType::Rifle ? "HOLD TO FIRE" : "SHOOT")
  : selected_ == ItemType::Flashlight ? "LIGHT" : selected_ == ItemType::Compass ? "HOLD IT" : selected_ == ItemType::Radio ? "ALWAYS ON"
  : (selected_ == ItemType::Firesalt || selected_ == ItemType::FlashBulb || selected_ == ItemType::MusicBox) ? "THROW"
  : selected_ == ItemType::GlowStick ? "DROP" : selected_ == ItemType::NightVision ? (me.nv_on ? "TURN OFF" : "TURN ON")
  : selected_ == ItemType::Chalk ? "MARK THE FLOOR" : "USE"),
                      rgba(0.8f, 0.76f, 0.65f, 0.85f), 2);
        }

        // ----- pick-up prompt (same rules as the sim: level items + dropped props)
        ItemType t = ItemType::None;
        float best = PICKUP_RANGE;
        int pcx = floor_to_int(me.pos.x / CELL), pcz = floor_to_int(me.pos.z / CELL);
        for (int cz = pcz - 1; cz <= pcz + 1; cz++)
            for (int cx = pcx - 1; cx <= pcx + 1; cx++) {
                ItemType it = v.world.item(cx, cz);
                if (it == ItemType::None || (v.picked && v.picked->count(World::item_id(cx, cz)))) continue;
                float d = (v.world.item_pos(cx, cz) - Vec2{me.pos.x, me.pos.z}).len();
                if (d < best) { best = d; t = it; }
            }
        for (const DrawProp& p : v.props) {
            if (p.kind != PropKind::Item) continue;
            float d = (p.pos - Vec2{me.pos.x, me.pos.z}).len();
            if (d < best) { best = d; t = p.item; }
        }
        // objective things take priority (same as the sim)
        std::string obj_prompt;
        for (const DrawProp& p : v.props) {
            float d = (p.pos - Vec2{me.pos.x, me.pos.z}).len();
            if (p.kind == PropKind::Tape && d < PICKUP_RANGE) obj_prompt = "PICK UP LOST TAPE";
            if (p.kind == PropKind::Breaker && d < 1.7f && p.life < 0.5f) obj_prompt = "SWITCH ON THE BREAKER";
        }
        if (!obj_prompt.empty()) {
            std::string key = pad ? (platform_is_vita() ? "CROSS" : "A") : "E";
            ren_.rect(W / 2 - s, H / 2 - s, 2 * s, 2 * s, rgba(1, 1, 1, 0.8f));
            ren_.text(W / 2, H * 0.60f, s, key + "  " + obj_prompt, rgba(0.7f, 0.9f, 1.0f), 1);
        } else if (t != ItemType::None) {
            std::string key = pad ? (platform_is_vita() ? "CROSS" : "A") : "E";
            ren_.rect(W / 2 - s, H / 2 - s, 2 * s, 2 * s, rgba(1, 1, 1, 0.8f));
            ren_.text(W / 2, H * 0.60f, s, key + "  PICK UP " + item_info(t).name, rgba(1, 0.95f, 0.75f), 1);
            ren_.text(W / 2, H * 0.60f + 9 * s, s, item_info(t).blurb, rgba(0.75f, 0.72f, 0.62f, 0.8f), 1);
        }
        // ----- personal messages ("PICKED UP ...", "CAN'T CARRY MORE ...")
        if (v.toast_seq != toast_seq_) { toast_seq_ = v.toast_seq; toast_ = v.toast; toast_t_ = now_; }
        double ta = now_ - toast_t_;
        if (ta < 2.5 && !toast_.empty()) ren_.text(W / 2, H * 0.70f, s, toast_, rgba(0.9f, 1, 0.8f, (float)std::min(1.0, 2.5 - ta)), 1);
    }

    // ----- level title when you arrive
    double lt = now_ - level_title_t_;
    if (lt < 6.0 && !paused_) {
        float a = (float)std::min(std::min(lt / 1.0, 1.0), (6.0 - lt) / 1.5);
        const LevelInfo& li = v.world.info();
        ren_.text(W / 2, H * 0.26f, s * 3, li.code, rgba(0.95f, 0.9f, 0.7f, a), 1);
        ren_.text(W / 2, H * 0.26f + 26 * s, s * 1.5f, li.name, rgba(0.85f, 0.8f, 0.6f, a * 0.9f), 1);
        ren_.text(W / 2, H * 0.26f + 40 * s, s, diff_info(v.world.difficulty()).name, rgba(0.75f, 0.45f, 0.35f, a * 0.8f), 1);
    }

    // ----- event feed, top right (top left in multiplayer is the player list)
    for (size_t i = 0; i < feed_.size(); i++) {
        float a = clampf(6.0f - (float)(now_ - feed_[i].t), 0, 1);
        if (a <= 0) continue;
        ren_.text(W - 8 * s, (cfg_.camcorder ? 28 * s : 8 * s) + i * 9 * s, s, feed_[i].text, rgba(0.9f, 0.85f, 0.7f, a * 0.9f), 2);
    }
    if (v.multiplayer)
        for (size_t i = 0; i < v.roster.size(); i++) ren_.text(8 * s, 8 * s + i * 9 * s, s, v.roster[i], rgba(0.8f, 0.78f, 0.7f, 0.7f));
    float oy = 8 * s + (v.multiplayer ? v.roster.size() * 9 * s + 4 * s : 0);
    if (session_->sim() && session_->sim()->sandbox) {
        GameSim* sim = session_->sim();
        std::string tag = "SANDBOX";
        if (sim->god) tag += "  GOD";
        if (me.noclip) tag += "  NOCLIP";
        ren_.text(8 * s, oy, s, tag + "  -  " + key_hint("TAB", "SELECT") + ": SPAWN MENU", rgba(1, 0.85f, 0.4f, 0.8f));
        oy += 12 * s;
    }
    // ----- objective + perks, top left
    if (v.objective.type != ObjType::None) {
        const Objective& o = v.objective;
        const PerkInfo& pk = perk_info(o.reward);
        std::string l2 = o.done ? std::string("FIND THE EXIT WHEN YOU'RE READY") : objective_text(o);
        std::string l3 = std::string(o.done ? "GOT: " : "REWARD: ") + pk.name + "  " + pk.text;
        float bw = std::max(ren_.text_w(s, l2), ren_.text_w(s, l3)) + 8 * s;
        ren_.rect(6 * s, oy - 3 * s, bw, 30 * s, rgba(0, 0, 0, 0.4f));
        ren_.text(9 * s, oy, s, o.done ? "OBJECTIVE COMPLETE" : "OBJECTIVE", o.done ? rgba(0.5f, 1, 0.55f, 0.9f) : rgba(0.95f, 0.8f, 0.45f, 0.9f));
        ren_.text(9 * s, oy + 9 * s, s, l2, rgba(0.95f, 0.93f, 0.85f, 0.9f));
        ren_.text(9 * s, oy + 18 * s, s, l3, rgba(0.6f, 0.85f, 1.0f, 0.75f));
        oy += 32 * s;
    }
    std::string perks;
    for (int k = 0; k < PERK_COUNT; k++)
        if (me.perks[k]) perks += std::string(perks.empty() ? "" : "  ") + perk_info((Perk)k).name + (me.perks[k] > 1 ? " X" + std::to_string(me.perks[k]) : "");
    if (!perks.empty()) ren_.text(9 * s, oy, s, perks, rgba(0.6f, 0.85f, 1.0f, 0.6f));
    // name tags over other players
    for (const DrawPlayer& p : v.others) {
        float sx, sy;
        float d = (p.pos - me.pos).len();
        if (d < 25 && ren_.project(p.pos + Vec3{0, 2.0f, 0}, sx, sy) && !v.world.segment_blocked({me.pos.x, me.pos.z}, {p.pos.x, p.pos.z}))
            ren_.text(sx, sy, s, p.alive ? p.name : p.name + " (DOWN)", rgba(1, 1, 1, clampf(1.5f - d / 25, 0, 0.9f)), 1);
    }

    // ----- dead
    if (!me.alive) {
        ren_.rect(0, 0, W, H, rgba(0.05f, 0, 0, 0.55f));
        ren_.text(W / 2, H * 0.38f, s * 2, death_text(me.cause), rgba(0.95f, 0.3f, 0.25f), 1);
        std::string sub = v.multiplayer ? "RESPAWNING IN " + std::to_string((int)std::ceil(std::max(0.0f, v.respawn_t)))
                                        : key_hint("PRESS ENTER TO TRY AGAIN", platform_is_vita() ? "PRESS CROSS TO TRY AGAIN" : "PRESS A TO TRY AGAIN");
        ren_.text(W / 2, H * 0.38f + 24 * s, s, sub, rgba(0.85f, 0.8f, 0.7f), 1);
        ren_.text(W / 2, H * 0.38f + 34 * s, s, "YOUR THINGS ARE WHERE YOU FELL", rgba(0.6f, 0.55f, 0.5f), 1);
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
        if (v.multiplayer) std::snprintf(buf, sizeof buf, "%.0f FPS  PING %.0f MS", fps_, v.ping_ms);
        else std::snprintf(buf, sizeof buf, "%.0f FPS  %.0f,%.0f", fps_, me.pos.x, me.pos.z);
        ren_.text(W / 2, 4 * s, s, buf, rgba(0.6f, 1, 0.6f, 0.8f), 1);
    }
    if (sb_open_) draw_sandbox_menu();
    (void)fx;
}

void App::draw_game(bool overlay_menu) {
    const GameView& v = session_->view();
    // level changes: one "entering" frame first (texture generation takes a moment), then load
    if (v.epoch != epoch_) {
        if (!needs_load_) {
            needs_load_ = true;
            ren_.begin_2d();
            ren_.rect(0, 0, (float)ren_.width, (float)ren_.height, rgba(0, 0, 0, 1));
            ren_.text(ren_.width / 2.0f, ren_.height / 2.0f, ui() * 2, std::string("ENTERING ") + v.world.info().code, rgba(0.9f, 0.85f, 0.7f), 1);
            static const char* TIPS[] = {
                "HOUNDS HEAR YOU. CROUCH, OR STARE THEM DOWN",
                "DON'T LOOK AT THE SMILE FOR TOO LONG",
                "BACTERIA TURN SLOWLY. CORNERS ARE YOUR FRIENDS",
                "WANDERERS LEFT CAMPS BEHIND. THEY LEFT SUPPLIES TOO",
                "CHALK THE FLOOR SO YOU STOP WALKING IN CIRCLES",
                "THE EXIT IS ALWAYS IN A WALL. LOOK FOR THE GREEN SIGN",
                "OBJECTIVES MAKE YOU STRONGER FOR THE WHOLE RUN",
                "A MUSIC BOX BUYS YOU TWENTY SECONDS",
                "MEMORY JUICE MAKES YOU FAST AND UNKILLABLE. THEN IT KILLS YOU",
                "FLASH BULBS STUN EVERYTHING THAT SEES THEM. INCLUDING YOU",
                "NIGHT VISION AND THE FLASHLIGHT SHARE ONE BATTERY",
            };
            const int nt = (int)(sizeof(TIPS) / sizeof(TIPS[0]));
            ren_.text(ren_.width / 2.0f, ren_.height / 2.0f + 22 * ui(), ui(), std::string("TIP: ") + TIPS[(v.epoch * 7 + 3) % (uint32_t)nt], rgba(0.65f, 0.62f, 0.55f), 1);
            ren_.end_2d();
            return;
        }
        needs_load_ = false;
        epoch_ = v.epoch;
        ren_.set_world(v.world);
        level_title_t_ = now_;
        if (v.escaped && !v.multiplayer) { escaped_t_ = now_; session_->clear_escaped(); }
        stage(std::string("level ") + v.world.info().number + " meshes");
        ren_.update_chunks(v.world, v.me.pos + Vec3{0, v.me.eye_height(), 0}, -1);
        std::printf("[iris] level %s loaded\n", v.world.info().number);
        stage(std::string("level ") + v.world.info().number + " playing");
    }
    ren_.set_world(v.world); // no-op unless something else (a menu) changed it

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
    if (!overlay_menu || session_->view().multiplayer) update_audio(v, cam.eye, cam.yaw);
    else audio_.stop_loops();
    cam.roll = fx.roll;
    cam.fov_deg = cfg_.fov + fx.fov_add;
    float fog = fx.fog_mult;
    const LevelInfo& li = v.world.info();
    bool dark = li.dark || li.ambient[0] < 0.15f;
    if (li.dark) fog *= 1.0f + 0.06f * std::sin((float)now_ * 7.3f) * std::sin((float)now_ * 2.1f);
    // a flashlight lets you see further into the dark
    float flick = v.me.battery < 0.15f ? (hash01(4, (int)(now_ * 14), 0, 1) < 0.25f ? 0.2f : 1.0f) : 1.0f;
    if (v.me.light_on && dark) fog *= li.dark ? 0.55f : 0.8f;
    if (v.me.nv_on && v.me.alive) fog *= 0.35f; // night vision sees through the dark

    ren_.update_chunks(v.world, cam.eye, rs_.chunk_builds_per_frame);
    static const std::unordered_set<uint32_t> none;
    ren_.draw_world(v.world, cam, (float)now_, fog, v.picked ? *v.picked : none, v.ents, v.others, v.props, fx.phantoms);
    ren_.post_warp(fx.wobble, fx.ghost, (float)now_);
    // what's in your hands, in 3D
    if (v.me.alive && dev_.hud) {
        ViewModel vm;
        if (is_weapon(selected_)) vm.item = selected_;
        else if (v.me.light_on) vm.item = ItemType::Flashlight;
        vm.light_on = v.me.light_on;
        vm.bob = v.me.bob;
        vm.kick = is_gun(selected_) ? clampf(1.0f - v.me.swing / 0.15f, 0, 1) : 0.0f;
        vm.swing = selected_ == ItemType::Crowbar && v.me.swing < 0.35f ? v.me.swing / 0.35f : 0.0f;
        float dyaw = std::remainder(cmd_.yaw - vm_last_yaw_, 2 * PI);
        vm_last_yaw_ = cmd_.yaw;
        vm_sway_ = lerpf(vm_sway_, clampf(dyaw * 0.6f, -0.03f, 0.03f), std::min(1.0f, (float)frame_dt_ * 10.0f));
        vm.sway_x = vm_sway_;
        vm.sway_y = -std::fabs(std::sin(v.me.bob * 0.5f)) * 0.01f * std::min(1.0f, v.me.speed / WALK_SPEED);
        vm.light = v.world.light_level(cam.eye) * 0.8f + 0.3f + (v.me.light_on ? 0.35f : 0.0f);
        ren_.draw_viewmodel(vm);
    }
    float beam = li.dark ? (li.ambient[0] < 0.4f ? 1.3f : 1.1f) : (dark ? 0.9f : 0.5f);
    if (v.me.light_on && v.me.alive) ren_.flashlight_overlay(beam, flick);

    // film look: grading per level, more fringing and grain as your mind goes
    FilmParams film;
    film.time = (float)now_;
    float insanity = clampf((60.0f - v.me.sanity) / 60.0f, 0, 1);
    film.grain = 0.045f + insanity * 0.08f + fx.grain * 0.2f;
    film.aberration = 1.2f + insanity * 4.0f + fx.wobble * 3.0f;
    film.vignette = 0.38f + insanity * 0.25f;
    film.bloom = li.ambient[0] > 0.4f ? 0.12f : (li.outdoor ? 0.35f : (v.me.light_on && li.dark ? 0.45f : 0.85f));
    film.saturation = v.world.greyscale() ? 0.0f : 0.88f - insanity * 0.3f; // TH3 SH4DY GR3Y has no colour at all
    film.contrast = 0.32f;
    film.distort = 0.06f + insanity * 0.08f;
    for (int k = 0; k < 3; k++) film.lift[k] = li.fog[k] * 0.12f + 0.006f;
    film.dim = ren_.flicker_dim();
    film.camcorder = cfg_.camcorder;
    if (cfg_.retro) { // PS1: ~360 lines of chunky pixels, 15-bit colour with ordered dither, a touch more colour
        film.pixel_h = 360;
        film.dither = 31;
        if (!v.world.greyscale()) film.saturation = std::min(1.0f, film.saturation + 0.12f);
        film.distort *= 0.5f;
    }
    if (v.me.nv_on && v.me.alive) {
        ren_.night_vision((float)now_);
        film.saturation = 0.25f;
        film.grain += 0.06f;
        film.bloom = 0.4f;
    }
    film_on_ = ren_.post_film(film);

    ren_.begin_2d();
    if (!film_on_) { // no shader (Vita): the cheap version of the same look
        float W = (float)ren_.width, H = (float)ren_.height;
        if (cfg_.film) {
            float ox = hash01(13, (int)(now_ * 24), 0, 1), oy = hash01(13, (int)(now_ * 24), 1, 1);
            ren_.tiled(SPR_NOISE, 0, 0, W, H, rgba(1, 1, 1, 0.05f), 128 * ui() * 0.5f, ox, oy);
            ren_.sprite(SPR_VIGNETTE, 0, 0, W, H, rgba(1, 1, 1, 0.55f));
        }
        if (cfg_.camcorder) ren_.tiled(SPR_SCANLINES, 0, 0, W, H, rgba(1, 1, 1, 0.6f), 4, 0, 0);
        if (ren_.flicker_dim() > 0) ren_.rect(0, 0, W, H, rgba(0, 0, 0, ren_.flicker_dim()));
    }
    draw_fx_overlays(fx);
    if (cfg_.camcorder && dev_.hud) { // REC and a running clock, top right
        float s = ui(), W = (float)ren_.width;
        int t = (int)v.level_time;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
        if (std::fmod(now_, 1.0) < 0.6) ren_.sprite(SPR_GLOW, W - 62 * s, 6 * s, 7 * s, 7 * s, rgba(1, 0.1f, 0.05f, 1));
        ren_.text(W - 52 * s, 7 * s, s, "REC", rgba(1, 0.95f, 0.9f, 0.9f));
        ren_.text(W - 8 * s, 16 * s, s, buf, rgba(1, 0.95f, 0.9f, 0.8f), 2);
    }
    if (dev_.hud) draw_hud(v, fx);
    if (overlay_menu) draw_menu();
    ren_.end_2d();
}

// =================================================================== frame loop
void App::run() {
    double start = platform_time(), last = start;
    int shot_frames = 0;
    for (;;) {
        platform_poll(in_);
        if (in_.quit) break;
        now_ = platform_time();
        frame_dt_ = std::min(now_ - last, 0.25);
        last = now_;
        if (in_.toggle_debug) debug_ = !debug_;
        if (in_.toggle_fullscreen) { cfg_.fullscreen = !cfg_.fullscreen; apply_display_settings(); save_settings(); }

        bool playing = session_ && screen() == Screen::Playing;
        bool game = in_game();               // also true in Settings/Controls opened from the pause menu
        platform_set_mouse_capture(playing && !paused_ && !sb_open_ && dev_.shot_path.empty());

        // connecting -> playing / failed
        if (session_ && screen() == Screen::Connecting) {
            session_->update(cmd_, now_, (float)frame_dt_, false);
            std::string why;
            if (session_->failed(&why)) { leave_game(); error_ = why; }
            else if (session_->ready()) { stack_ = {Screen::Title, Screen::Playing}; build_menu(); }
        }
        // Tailscale: ask who's on the tailnet every few seconds while the multiplayer screens are open
        if (!platform_is_vita() && (screen() == Screen::Join || screen() == Screen::Host || screen() == Screen::Multi)) {
            if (now_ - tail_t_ > 6.0) { tailscale_.refresh(); tail_t_ = now_; }
            if (!tailscale_.busy()) {
                TailStatus st = tailscale_.status();
                bool changed = st.running != tail_.running || st.self_ip != tail_.self_ip || st.peers.size() != tail_.peers.size() ||
                               st.available != tail_.available;
                tail_ = st;
                if (changed && screen() != Screen::Multi) { int sel = menu_.sel; build_menu(); menu_.sel = std::min(sel, (int)menu_.items.size() - 1); }
            }
        }
        if (screen() == Screen::Join) {
            if (now_ - browse_t_ > 1.0) {
                browser_.scan(now_);
                for (const TailPeer& p : tail_.peers) { // and knock on every friend's PC on the tailnet
                    NetAddr a;
                    if (p.online && net_resolve(p.ip, IRIS_DEFAULT_PORT, a)) browser_.scan_to(a, "TAILSCALE: " + p.name);
                }
                browse_t_ = now_;
            }
            size_t before = browser_.found.size();
            browser_.scan_poll(now_);
            if (browser_.found.size() != before) {
                int sel = menu_.sel;
                build_menu();
                menu_.sel = std::min(sel, (int)menu_.items.size() - 1);
            }
        }

        if (game) {
            if (playing && !paused_) game_input();
            else menu_input();
            if (session_) {
                InputCmd c = cmd_;
                bool frozen = paused_ || !playing;
                if (frozen) { c.move_x = c.move_z = 0; c.buttons = 0; }
                // keep the session running in submenus: multiplayer must keep talking to the server
                session_->update(c, now_, (float)frame_dt_, frozen && !session_->view().multiplayer);
                std::string why;
                if (session_->failed(&why)) { leave_game(); error_ = why; }
                else for (auto& e : session_->take_events()) {
                    if (e.rfind("ENTERED ", 0) == 0) continue; // the title card already says it
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
        if (session_ && in_game()) {
            draw_game(paused_ || screen() != Screen::Playing);
        } else {
            draw_menu_background();
            ren_.begin_2d();
            draw_menu();
            ren_.end_2d();
        }
        platform_swap();
        if (frames_total_ < 3 && ++frames_total_ <= 2) std::printf("[iris] frame %d drawn\n", frames_total_);
        if (stage_pending_ && now_ - stage_t_ > 4.0) { // survived a few seconds: whatever ran last was fine
            platform_boot_stage(in_game() ? "play-ok" : "menu-ok");
            stage_pending_ = false;
        }

        if (in_.screenshot) {
            std::string path = data_dir_ + "iris_" + timestamp() + ".bmp";
            if (platform_save_screenshot(path)) {
                std::printf("[iris] screenshot: %s\n", path.c_str());
                feed_.push_back({"SCREENSHOT SAVED", now_});
            }
        }
        if (!dev_.shot_path.empty() && now_ - start >= dev_.shot_after && ++shot_frames > 3) {
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


// =================================================================== sound
void App::item_sound(ItemType t) {
    switch (t) {
        case ItemType::AlmondWater: case ItemType::EnergyDrink: case ItemType::MemoryJuice: case ItemType::LiquidPain:
        case ItemType::CashewWater: case ItemType::LuckyMilk: case ItemType::SuperAlmond: case ItemType::LiquidSilence:
            audio_.play(SFX_DRINK, 0.7f, 0, 0.95f + hash01(5, (int)(now_ * 10), 0, 1) * 0.1f); break;
        case ItemType::RoyalRation: case ItemType::MothJelly: case ItemType::MedKit: audio_.play(SFX_EAT, 0.7f); break;
        case ItemType::Firesalt: case ItemType::FlashBulb: case ItemType::MusicBox: case ItemType::GlowStick: audio_.play(SFX_THROW, 0.7f); break;
        default: break;
    }
}

void App::play_at(Sfx s, Vec2 at, float vol, Vec3 eye, float yaw, const World& w, float pitch) {
    Vec2 e{eye.x, eye.z};
    Vec2 d = at - e;
    float dist = d.len();
    if (dist > 45.0f) return;
    Vec2 r = yaw_right(yaw);
    float pan = dist > 0.3f ? (d.x * r.x + d.z * r.z) / dist * 0.85f : 0.0f;
    bool blocked = dist > 1.0f && w.segment_blocked(e, at);
    float v = vol / (1.0f + dist * 0.22f) * (blocked ? 0.45f : 1.0f);
    audio_.play(s, v, pan, pitch, blocked ? 0.8f : 0.0f);
}

void App::update_audio(const GameView& v, Vec3 eye, float yaw) {
    if (!audio_.ok()) return;
    audio_.set_master(cfg_.volume);
    const PlayerState& me = v.me;
    const World& w = v.world;
    const LevelId id = w.id();
    Vec2 here{me.pos.x, me.pos.z};
    // ----- ambience: one main bed per level, a low dread drone under most of them
    Sfx amb = SFX_HUM;
    float av = 0.22f;
    switch (id) {
        case LevelId::L6: case LevelId::L8: case LevelId::L19: case LevelId::L51: case LevelId::L2: case LevelId::TSGBlack: case LevelId::TSGHope:
            amb = SFX_DRONE; av = 0.4f; break;
        case LevelId::L9: case LevelId::L11: case LevelId::L24: case LevelId::L10: amb = SFX_WIND; av = 0.35f; break;
        case LevelId::L7: case LevelId::L37: amb = SFX_WATER; av = 0.4f; break;
        case LevelId::LRun: amb = SFX_ALARM; av = 0.3f; break;
        default: break;
    }
    if (amb == SFX_HUM) av *= 1.0f - ren_.flicker_dim() * 2.0f; // the hum stutters with the lights
    audio_.loop(LOOP_AMB1, amb, id == LevelId::L3 ? av * 1.8f : av, 0, id == LevelId::L3 ? 0.5f : 1.0f); // Level 3: transformers drone
    bool safe = id == LevelId::L11 || id == LevelId::L37;
    audio_.loop(LOOP_AMB2, id == LevelId::L7 ? SFX_WIND : SFX_DRONE, safe ? 0.0f : (amb == SFX_DRONE ? 0.0f : 0.12f), 0, 0.98f);
    // ----- fire nearby (flares, fire barrels, lanterns) and the music box
    if (now_ - as_.scan_t > 0.5) {
        as_.scan_t = now_;
        as_.fire_d = 1e9f;
        int pcx = floor_to_int(here.x / CELL), pcz = floor_to_int(here.z / CELL);
        for (int cz = pcz - 4; cz <= pcz + 4; cz++)
            for (int cx = pcx - 4; cx <= pcx + 4; cx++) {
                Furn fs[MAX_FURN];
                bool owner = false;
                int n = w.furniture(cx, cz, fs, &owner);
                for (int k = 0; owner && k < n; k++)
                    if (fs[k].kind == FurnKind::FireBarrel) {
                        Vec2 c{(fs[k].x0 + fs[k].x1) * 0.5f, (fs[k].z0 + fs[k].z1) * 0.5f};
                        float d = (c - here).len();
                        if (d < as_.fire_d) { as_.fire_d = d; as_.fire_at = c; }
                    }
            }
    }
    float fire_d = as_.fire_d;
    Vec2 fire_at = as_.fire_at;
    Vec2 lure_at{0, 0};
    float lure_d = 1e9f;
    for (const DrawProp& p : v.props) {
        float d = (p.pos - here).len();
        if (p.kind == PropKind::Flare && d < fire_d) { fire_d = d; fire_at = p.pos; }
        if (p.kind == PropKind::Lure && d < lure_d) { lure_d = d; lure_at = p.pos; }
    }
    auto pan_of = [&](Vec2 at) {
        Vec2 d = at - here;
        float l = d.len();
        Vec2 r = yaw_right(yaw);
        return l > 0.3f ? (d.x * r.x + d.z * r.z) / l * 0.8f : 0.0f;
    };
    audio_.loop(LOOP_FIRE, SFX_FIRE, fire_d < 12 ? 0.6f / (1.0f + fire_d * 0.5f) : 0.0f, pan_of(fire_at));
    audio_.loop(LOOP_MUSIC, SFX_MUSICBOX, lure_d < 30 ? 0.7f / (1.0f + lure_d * 0.25f) : 0.0f, pan_of(lure_at));
    // ----- your body: heartbeat when you're in trouble, whispers on memory juice, radio static
    float trouble = std::max(clampf((35.0f - me.sanity) / 35.0f, 0, 1), clampf((30.0f - me.health) / 30.0f, 0, 1));
    audio_.loop(LOOP_HEART, SFX_HEART, me.alive ? trouble * 0.8f : 0.0f, 0, 1.0f + trouble * 0.6f);
    audio_.loop(LOOP_JUICE, SFX_WHISPERS, me.alive && me.juice >= 0 ? 0.25f + clampf(me.juice / JUICE_TIME, 0, 1) * 0.5f : 0.0f);
    float sig = 0;
    if (me.has(ItemType::Radio)) {
        float nearest = 1e9f;
        for (const DrawEntity& e : v.ents) nearest = std::min(nearest, (e.pos - here).len());
        sig = clampf(1.0f - nearest / 35.0f, 0, 1);
    }
    audio_.loop(LOOP_STATIC, SFX_STATIC, sig * sig * 0.5f);
    // ----- footsteps: by what you're walking on
    int step = (int)std::floor(me.bob / PI);
    if (step != as_.step && me.alive && me.speed > 0.4f) {
        int cx = floor_to_int(here.x / CELL), cz = floor_to_int(here.z / CELL);
        int fm = w.floor_mat(cx, cz);
        Sfx s = SFX_STEP_HARD;
        switch (id) {
            case LevelId::L0: case LevelId::L4: case LevelId::L6: case LevelId::L21: case LevelId::LFun: case LevelId::TSG: case LevelId::TSGBlack:
                s = SFX_STEP_CARPET; break;
            case LevelId::L5: case LevelId::TSGMansion: s = fm == 1 ? SFX_STEP_WOOD : SFX_STEP_CARPET; break;
            case LevelId::L2: s = fm == 1 ? SFX_STEP_METAL : SFX_STEP_HARD; break;
            case LevelId::L19: s = SFX_STEP_WOOD; break;
            case LevelId::L7: s = w.water(cx, cz) ? SFX_STEP_WATER : SFX_STEP_WOOD; break;
            case LevelId::L37: s = fm == 1 ? SFX_STEP_WATER : SFX_STEP_HARD; break;
            case LevelId::L1: s = fm == 1 ? SFX_STEP_WATER : SFX_STEP_HARD; break;
            case LevelId::L33: s = fm == 1 ? SFX_STEP_CARPET : SFX_STEP_HARD; break;
            case LevelId::L36: s = fm == 0 ? SFX_STEP_CARPET : SFX_STEP_HARD; break;
            default: break;
        }
        float vol = me.crouch > 0.5f ? 0.12f : (me.speed > WALK_SPEED * 1.2f ? 0.55f : 0.32f);
        if (me.silent > 0) vol = 0;
        audio_.play(s, vol, (step & 1) ? 0.12f : -0.12f, 0.9f + hash01(77, step, 0, 1) * 0.2f);
    }
    as_.step = step;
    for (const DrawPlayer& o : v.others) { // friends' footsteps
        int st = (int)std::floor(o.anim / PI);
        int& last = as_.other_step[o.id];
        if (st != last && o.alive) play_at(SFX_STEP_HARD, {o.pos.x, o.pos.z}, 0.3f, eye, yaw, w, 0.95f);
        last = st;
    }
    // ----- entities: idle noises, and a sting when something comes for you
    for (const DrawEntity& e : v.ents) {
        float d = (e.pos - here).len();
        if (d > 40) continue;
        int& last = as_.state[e.id];
        double& next = as_.next[e.id];
        int st = (int)e.state;
        if (st != last) {
            if (e.type == EntityType::Bacteria && e.state == EntState::Notice) { play_at(SFX_BACTERIA_SCREECH, e.pos, 1.2f, eye, yaw, w); if (d < 25) audio_.play(SFX_STING, 0.5f); }
            if (e.type == EntityType::Smiler && e.state == EntState::Charge) { play_at(SFX_SMILER_CHARGE, e.pos, 1.3f, eye, yaw, w); audio_.play(SFX_STING, 0.5f); }
            if (e.type == EntityType::Hound && e.state == EntState::Chase && last != (int)EntState::Stare) play_at(SFX_HOUND_GROWL, e.pos, 1.1f, eye, yaw, w, 1.15f);
            if (e.type == EntityType::SkinStealer && e.state == EntState::Chase && d < 25) audio_.play(SFX_STING, 0.4f);
            last = st;
        }
        if (now_ < next) continue;
        float r = hash01(e.id, (int)(now_ * 3), 9, 1);
        switch (e.type) {
            case EntityType::Bacteria: if (e.speed > 0.2f) play_at(SFX_BACTERIA_CLICK, e.pos, 0.9f, eye, yaw, w, 0.9f + r * 0.2f); next = now_ + 1.5 + r * 2.5; break;
            case EntityType::Hound:
                if (e.state == EntState::Chase || e.state == EntState::Stare) play_at(SFX_HOUND_GROWL, e.pos, 0.9f, eye, yaw, w, 0.9f + r * 0.2f);
                next = now_ + 2.5 + r * 2.5;
                break;
            case EntityType::Smiler: if (e.state == EntState::Stare) play_at(SFX_SMILER_WHISPER, e.pos, 0.8f, eye, yaw, w); next = now_ + 3.5 + r * 3; break;
            case EntityType::Partygoer: play_at(SFX_PARTY_GIGGLE, e.pos, e.state == EntState::Chase ? 1.0f : 0.6f, eye, yaw, w, 0.95f + r * 0.1f); next = now_ + 3 + r * 4; break;
            case EntityType::SkinStealer: play_at(SFX_SKIN_GROAN, e.pos, 0.8f, eye, yaw, w, 0.9f + r * 0.2f); next = now_ + 5 + r * 4; break;
            default: next = now_ + 5; break;
        }
    }
    if (as_.state.size() > 200) { as_.state.clear(); as_.next.clear(); }
    // ----- things you did (or that happened to you)
    if (me.swing < as_.swing - 0.05f) {
        if (selected_ == ItemType::Pistol) audio_.play(SFX_PISTOL, 0.9f, 0.1f, 0.95f + hash01(3, (int)(now_ * 50), 0, 1) * 0.1f);
        else if (selected_ == ItemType::Shotgun) audio_.play(SFX_SHOTGUN, 1.0f, 0.1f);
        else if (selected_ == ItemType::Rifle) audio_.play(SFX_RIFLE, 0.8f, 0.1f, 0.95f + hash01(3, (int)(now_ * 50), 0, 1) * 0.1f);
        else if (selected_ == ItemType::Crowbar) audio_.play(SFX_SWING, 0.7f);
    }
    as_.swing = me.swing;
    if (me.light_on != as_.light) audio_.play(SFX_CLICK, 0.6f);
    as_.light = me.light_on;
    if (me.flash < as_.flash - 0.2f) audio_.play(SFX_FLASH, 0.9f);
    as_.flash = me.flash;
    if (me.alive && me.health < as_.health - 3.0f) audio_.play(SFX_HURT, 0.8f);
    as_.health = me.health;
    if (!me.alive && as_.alive) audio_.play(SFX_DEATH, 0.9f);
    as_.alive = me.alive;
    if (v.epoch != as_.epoch) {
        if (as_.epoch != 0xFFFFFFFF) audio_.play(SFX_EXIT, 0.8f);
        as_.epoch = v.epoch;
        as_.state.clear();
        as_.next.clear();
    }
    if (v.toast_seq != as_.toast_seq) {
        const std::string& t = v.toast;
        if (t.rfind("PICKED UP", 0) == 0) audio_.play(SFX_PICKUP, 0.6f);
        else if (t.rfind("LOST TAPE", 0) == 0) audio_.play(SFX_TAPE, 0.8f);
        else if (t.rfind("BREAKER", 0) == 0) audio_.play(SFX_BREAKER, 0.8f);
        else if (t.rfind("OBJECTIVE DONE", 0) == 0) audio_.play(SFX_OBJECTIVE, 0.8f);
        else if (t.find("RABBIT") != std::string::npos) audio_.play(SFX_STING, 0.7f);
        as_.toast_seq = v.toast_seq;
    }
}

} // namespace iris
