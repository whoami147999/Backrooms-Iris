// Backrooms Iris - platform layer. Everything OS/console specific hides behind this.
// Implementations: platform/pc (SDL2 window + desktop OpenGL), platform/vita (vitaGL).
#pragma once
#include <cstdint>
#include <string>

namespace iris {

struct InputState {
    // movement, -1..1 (keyboard or left stick)
    float move_x = 0, move_z = 0;
    // mouse motion since last poll, in pixels
    float mouse_dx = 0, mouse_dy = 0;
    // right stick, -1..1, already dead-zoned
    float look_x = 0, look_y = 0;
    bool sprint = false, crouch = false;

    // one-shot events
    bool quit = false;
    bool screenshot = false;
    bool toggle_fullscreen = false;
    bool toggle_debug = false;
    bool pause = false;

    bool mouse_captured = false;
    bool using_gamepad = false;   // last input came from a gamepad
    int width = 0, height = 0;    // drawable size in pixels
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
bool platform_save_screenshot(const std::string& path); // false if unsupported
const char* platform_name();
std::string platform_data_dir();      // writable dir for screenshots/settings, ends with '/'
void platform_shutdown();

} // namespace iris
