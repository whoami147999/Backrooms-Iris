// Backrooms Iris - platform layer. Everything OS/console specific hides behind this.
// Implementations: platform/pc (SDL2 window + desktop OpenGL), platform/vita (vitaGL).
#pragma once
#include <cstdint>
#include <string>

namespace iris {

struct InputState {
    // ----- held / continuous
    float move_x = 0, move_z = 0;     // -1..1 (keyboard or left stick)
    float mouse_dx = 0, mouse_dy = 0; // relative motion while the mouse is captured, pixels
    float look_x = 0, look_y = 0;     // right stick, -1..1, dead-zoned
    bool sprint = false;
    bool crouch = false;              // held crouch (keyboard)

    // ----- one-shot (true for the frame they happened)
    bool up = false, down = false, left = false, right = false; // menu navigation (with repeat)
    bool accept = false, back = false;  // Enter / A(Cross), Esc / B(Circle)
    bool pause = false;                 // Esc / Start
    bool interact = false;              // E / A(Cross)
    bool crouch_toggle = false;         // B(Circle) on a gamepad
    bool use_item = false;              // left click / Square / D-pad up: use the selected item (or swing)
    bool light = false;                 // F / Triangle: flashlight
    bool slot_prev = false, slot_next = false; // mouse wheel / D-pad left right
    int slot_num = -1;                  // 1-9 keys: pick a hotbar slot directly
    bool sandbox_menu = false;          // Tab / Select
    bool backspace = false;
    bool click = false;                 // left mouse button (menus)
    bool screenshot = false, toggle_fullscreen = false, toggle_debug = false, quit = false;
    std::string text;                   // typed characters this frame

    float mouse_x = 0, mouse_y = 0;     // absolute mouse position (menus)
    bool mouse_moved = false;
    bool mouse_captured = false;
    bool using_gamepad = false;         // last input came from a gamepad (changes the hints)
    int width = 0, height = 0;          // drawable size in pixels
};

struct PlatformConfig {
    const char* title = "Backrooms Iris";
    int width = 1280, height = 720;
    bool fullscreen = false;
    bool vsync = true;
    bool hidden = false;          // dev: render without showing a window (screenshots)
    int msaa = 4;
};

bool platform_init(const PlatformConfig& cfg);
void platform_poll(InputState& in);   // fills `in`, clears one-shot events from last call
void platform_swap();
double platform_time();               // seconds, monotonic
void platform_set_mouse_capture(bool on);
void platform_set_fullscreen(bool on);
void platform_set_vsync(bool on);
bool platform_save_screenshot(const std::string& path); // false if unsupported
const char* platform_name();
bool platform_is_vita();
std::string platform_data_dir();      // writable dir for screenshots/settings, ends with '/'
void platform_shutdown();

} // namespace iris
