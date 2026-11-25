#include "sdl_gamepad.h"

#include <cmath>

namespace iris {
namespace {

SDL_GameController* g_pad = nullptr;
bool g_crouch_toggle = false;

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
                case SDL_CONTROLLER_BUTTON_B: g_crouch_toggle = !g_crouch_toggle; break; // Circle
                case SDL_CONTROLLER_BUTTON_START: in.pause = true; break;
                case SDL_CONTROLLER_BUTTON_BACK: in.toggle_debug = true; break;         // Select
                default: break;
            }
            break;
        default: break;
    }
}

void gamepad_poll(InputState& in) {
    if (!g_pad) return;
    float lx = axis(SDL_CONTROLLER_AXIS_LEFTX), ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
    float rx = axis(SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(SDL_CONTROLLER_AXIS_RIGHTY);
    deadzone(lx, ly, 0.22f);
    deadzone(rx, ry, 0.18f);
    if (lx != 0 || ly != 0 || rx != 0 || ry != 0) in.using_gamepad = true;
    // the stick wins over keys only when it's actually pushed
    if (lx != 0 || ly != 0) {
        in.move_x = lx;
        in.move_z = -ly;
    }
    in.look_x = rx;
    in.look_y = ry;
    // R / R2 to sprint (Vita has no analog triggers, so accept the shoulder too)
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
        axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.4f ||
        SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSTICK))
        in.sprint = true;
    if (g_crouch_toggle) in.crouch = true;
    if (in.sprint && in.using_gamepad) g_crouch_toggle = false;
}

void gamepad_shutdown() {
    if (g_pad) SDL_GameControllerClose(g_pad);
    g_pad = nullptr;
}

} // namespace iris
