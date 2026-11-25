// Backrooms Iris - PS Vita platform: vitaGL for rendering, SDL2 for input (and audio later).
// Milestone 2 is when this gets tested for real (Vita3K, then hardware).
#include <SDL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include "../platform.h"
#include "../sdl_gamepad.h"

// newlib heap for our own allocations; vitaGL manages GPU memory separately.
extern "C" {
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;
}

namespace iris {
namespace {
Uint64 g_t0 = 0;
}

bool platform_init(const PlatformConfig&) {
    // run the CPU/GPU at full clocks
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // 960x544 native, 2x MSAA, keep 16 MB of RAM headroom before vitaGL spills to it
    vglInitExtended(0, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_2X);

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) != 0) return false;
    gamepad_init();
    g_t0 = SDL_GetPerformanceCounter();
    return true;
}

void platform_poll(InputState& in) {
    in.mouse_dx = in.mouse_dy = 0;
    in.quit = in.screenshot = in.toggle_fullscreen = in.toggle_debug = in.pause = false;
    in.move_x = in.move_z = in.look_x = in.look_y = 0;
    in.sprint = in.crouch = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        gamepad_event(e, in);
        if (e.type == SDL_QUIT) in.quit = true;
    }
    gamepad_poll(in);
    in.using_gamepad = true;
    in.mouse_captured = false;
    in.width = 960;
    in.height = 544;
}

void platform_swap() { vglSwapBuffers(GL_FALSE); }

double platform_time() {
    return (double)(SDL_GetPerformanceCounter() - g_t0) / (double)SDL_GetPerformanceFrequency();
}

void platform_set_mouse_capture(bool) {}
bool platform_save_screenshot(const std::string&) { return false; }
const char* platform_name() { return "PS Vita"; }
std::string platform_data_dir() { return "ux0:data/BackroomsIris/"; }

void platform_shutdown() {
    gamepad_shutdown();
    SDL_Quit();
    vglEnd();
    sceKernelExitProcess(0);
}

} // namespace iris
