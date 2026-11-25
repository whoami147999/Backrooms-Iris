// Backrooms Iris - PS Vita platform: vitaGL for rendering, SDL2 for input (and audio later).
//
// Every boot writes ux0:data/BackroomsIris/log.txt (unbuffered), so after a crash the last
// line says how far the game got.
#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "../../client/font.h"
#include "../platform.h"
#include "../sdl_gamepad.h"

extern "C" {
// newlib heap for our own allocations. vitaGL takes most of what's left for GPU memory, so
// this stays moderate (the game itself needs well under 100 MB).
unsigned int _newlib_heap_size_user = 128 * 1024 * 1024;
// main thread stack (the default is small)
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

namespace iris {
namespace {
Uint64 g_t0 = 0;

bool file_exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// A text screen drawn straight into a display framebuffer (no GPU, no vitaGL), for errors that
// happen before or instead of graphics. Waits for a button, then quits the app.
[[noreturn]] void fatal_screen(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) std::printf("[iris] FATAL: %s\n", l.c_str());
    const int W = 960, H = 544, S = 3;
    void* base = nullptr;
    SceUID mem = sceKernelAllocMemBlock("iris_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 2 * 1024 * 1024, nullptr);
    if (mem >= 0 && sceKernelGetMemBlockBase(mem, &base) >= 0 && base) {
        uint32_t* px = (uint32_t*)base;
        for (int i = 0; i < W * H; i++) px[i] = 0xFF101418u; // 0xAABBGGRR
        std::vector<uint8_t> atlas = font_atlas();
        int y = 110;
        for (size_t li = 0; li < lines.size(); li++) {
            uint32_t col = li == 0 ? 0xFF5050F0u : 0xFF60D8F0u; // first line red, rest yellow
            int x = 60;
            for (char ch : lines[li]) {
                int code = std::toupper((unsigned char)ch) - 32;
                if (code > 0 && code < FONT_COLS * FONT_ROWS) {
                    int ox = (code % FONT_COLS) * FONT_CELL, oy = (code / FONT_COLS) * FONT_CELL;
                    for (int gy = 0; gy < 7; gy++)
                        for (int gx = 0; gx < 5; gx++) {
                            if (atlas[((size_t)(oy + gy) * FONT_ATLAS_W + ox + gx) * 4 + 3] < 128) continue;
                            for (int sy = 0; sy < S; sy++)
                                for (int sx = 0; sx < S; sx++) {
                                    int X = x + gx * S + sx, Y = y + gy * S + sy;
                                    if (X < W && Y < H) px[Y * W + X] = col;
                                }
                        }
                }
                x += 6 * S;
            }
            y += li == 0 ? 50 : 30;
        }
        SceDisplayFrameBuf fb;
        fb.size = sizeof(fb);
        fb.base = base;
        fb.pitch = W;
        fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
        fb.width = W;
        fb.height = H;
        sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
        sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
        SceCtrlData pad;
        int frames = 0;
        for (;;) {
            sceCtrlPeekBufferPositive(0, &pad, 1);
            if (++frames > 60 && (pad.buttons & (SCE_CTRL_START | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE))) break;
            sceDisplayWaitVblankStart();
        }
    }
    sceKernelExitProcess(0);
    std::abort();
}

void on_terminate() {
    std::printf("[iris] FATAL: uncaught exception / std::terminate (out of memory?)\n");
    sceKernelExitProcess(0);
    std::abort();
}

void open_log() {
    sceIoMkdir("ux0:data/BackroomsIris", 0777);
    if (std::freopen("ux0:data/BackroomsIris/log.txt", "w", stdout)) std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::freopen("ux0:data/BackroomsIris/log.txt", "a", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
}
} // namespace

bool platform_init(const PlatformConfig&) {
    std::set_terminate(on_terminate);
    std::printf("[iris] vita: boot\n");

    // vitaGL compiles its shaders on the console with Sony's shader compiler, which can't be
    // shipped with homebrew: it has to be extracted from the firmware with ShaRKF00D. Without
    // it vitaGL crashes on the first draw (C2-12828-1), so check first and say so.
    if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx"))
        fatal_screen({"BACKROOMS IRIS CAN'T START",
                      "LIBSHACCCG.SUPRX IS MISSING.",
                      "THE GRAPHICS NEED SONY'S SHADER COMPILER:",
                      "1. INSTALL SHARKF00D (FROM VITADB)",
                      "2. OPEN IT AND LET IT EXTRACT LIBSHACCCG.SUPRX",
                      "3. START BACKROOMS IRIS AGAIN",
                      "",
                      "PRESS X OR START TO EXIT"});
    std::printf("[iris] vita: libshacccg found\n");

    // run the CPU/GPU at full clocks
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // 960x544 native, 2x MSAA, keep 16 MB of RAM headroom before vitaGL spills to it
    std::printf("[iris] vita: vglInitExtended\n");
    vglInitExtended(0, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_2X);
    std::printf("[iris] vita: vitaGL ready\n");

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) != 0) {
        std::printf("[iris] vita: SDL_Init failed: %s\n", SDL_GetError());
        fatal_screen({"BACKROOMS IRIS CAN'T START", "SDL_INIT FAILED (INPUT)", "", "PRESS X OR START TO EXIT"});
    }
    gamepad_init();
    g_t0 = SDL_GetPerformanceCounter();
    std::printf("[iris] vita: SDL ready\n");
    return true;
}

void platform_poll(InputState& in) {
    InputState prev = in;
    in = InputState{};
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        gamepad_event(e, in);
        if (e.type == SDL_QUIT) in.quit = true;
    }
    gamepad_poll(in, platform_time());
    in.using_gamepad = true;
    in.mouse_captured = false;
    in.width = 960;
    in.height = 544;
    (void)prev;
}

void platform_set_fullscreen(bool) {}
void platform_set_vsync(bool) {}
bool platform_is_vita() { return true; }

void platform_swap() { vglSwapBuffers(GL_FALSE); }

double platform_time() {
    return (double)(SDL_GetPerformanceCounter() - g_t0) / (double)SDL_GetPerformanceFrequency();
}

void platform_set_mouse_capture(bool) {}
bool platform_save_screenshot(const std::string&) { return false; }
const char* platform_name() { return "PS Vita"; }
std::string platform_data_dir() {
    static bool logging = false;
    if (!logging) { open_log(); logging = true; } // first call happens at the very start of App::init
    return "ux0:data/BackroomsIris/";
}

void platform_shutdown() {
    gamepad_shutdown();
    SDL_Quit();
    vglEnd();
    sceKernelExitProcess(0);
}

} // namespace iris
