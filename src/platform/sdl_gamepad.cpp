#include "sdl_gamepad.h"

#include <cmath>

namespace iris {
namespace {

SDL_GameController* g_pad = nullptr;
// stick-as-dpad repeat for menus
int g_nav_dir = 0;
double g_nav_next = 0;

float axis(SDL_GameControllerAxis a) {
    if (!g_pad) return 0;
    return SDL_GameControllerGetAxis(g_pad, a) / 32767.0f;
}

// radial dead zone with rescale, so small stick drift doesn't move you
void deadzone(float& x, float& y, float dz) {
    float m = std::sqrt(x * x + y * y);
    if (m < dz) { x = y = 0; return; }
    float s = (std::fmin(m, 1.0f) - dz) / (1.0f - dz) / m;
    x *= s;
    y *= s;
}

void open_first() {
    if (g_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) return;
        }
    }
}

} // namespace

void gamepad_init() {
    SDL_GameControllerEventState(SDL_ENABLE);
    open_first();
}

bool gamepad_connected() { return g_pad != nullptr; }

void gamepad_event(const SDL_Event& e, InputState& in) {
    switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED: open_first(); break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad);
                g_pad = nullptr;
                open_first();
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN:
            in.using_gamepad = true;
            switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A: in.accept = true; in.interact = true; break;          // Cross
                case SDL_CONTROLLER_BUTTON_B: in.back = true; in.crouch_toggle = true; break;       // Circle
                case SDL_CONTROLLER_BUTTON_X: in.use_item = true; break;                            // Square
                case SDL_CONTROLLER_BUTTON_Y: in.light = true; break;                               // Triangle
                case SDL_CONTROLLER_BUTTON_START: in.pause = true; break;
                case SDL_CONTROLLER_BUTTON_BACK: in.sandbox_menu = true; break;                     // Select
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: in.use_item = true; break;                 // L
                case SDL_CONTROLLER_BUTTON_DPAD_UP: in.up = true; in.use_item = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in.down = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in.left = true; in.slot_prev = true; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in.right = true; in.slot_next = true; break;
                default: break;
            }
            break;
        default: break;
    }
}

void gamepad_poll(InputState& in, double now) {
    if (!g_pad) return;
    float lx = axis(SDL_CONTROLLER_AXIS_LEFTX), ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
    float rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
    // menu navigation with the left stick, with key-repeat style timing
    int dir = 0;
    if (ly < -0.6f) dir = 1; else if (ly > 0.6f) dir = 2; else if (lx < -0.6f) dir = 3; else if (lx > 0.6f) dir = 4;
    if (dir != g_nav_dir) { g_nav_dir = dir; g_nav_next = now; }
    if (dir && now >= g_nav_next) {
        if (dir == 1) in.up = true; else if (dir == 2) in.down = true; else if (dir == 3) in.left = true; else in.right = true;
        g_nav_next = now + (g_nav_next == now ? 0.35 : 0.14);
    }
    deadzone(lx, ly, 0.22f);
    deadzone(rx, ry, 0.18f);
    if (lx != 0 || ly != 0 || rx != 0 || ry != 0) in.using_gamepad = true;
    if (lx != 0 || ly != 0) { in.move_x = lx; in.move_z = -ly; }
    in.look_x = rx;
    in.look_y = ry;
    // R / R2 / L3 to sprint (Vita has no analog triggers, so accept the shoulder too)
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
        axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.4f || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))
        in.sprint = true;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_X) || SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        in.fire_held = true;
}

void gamepad_shutdown() {
    if (g_pad) SDL_GameControllerClose(g_pad);
    g_pad = nullptr;
}

} // namespace iris
