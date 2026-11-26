// Backrooms Iris - PS Vita platform: vitaGL for rendering, SDL2 for input (and audio later).
//
// Every boot writes ux0:data/BackroomsIris/log.txt (unbuffered), so after a crash the last
// line says how far the game got.
#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#ifndef SCE_KERNEL_CPU_MASK_USER_0
#define SCE_KERNEL_CPU_MASK_USER_0 (0x01 << 16)
#define SCE_KERNEL_CPU_MASK_USER_1 (0x02 << 16)
#define SCE_KERNEL_CPU_MASK_USER_2 (0x04 << 16)
#endif
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "../../client/font.h"
#include "../platform.h"
#include "../sdl_gamepad.h"

extern "C" {
// newlib heap for our own allocations. vitaGL takes most of what's left for GPU memory, so
// this stays moderate (the game itself needs well under 100 MB).
unsigned int _newlib_heap_size_user = 96 * 1024 * 1024;
// main thread stack (the default is small)
unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

namespace iris {
namespace {
Uint64 g_t0 = 0;
const char* BOOT_FILE = "ux0:data/BackroomsIris/boot.txt";
const char* LOG_FILE = "ux0:data/BackroomsIris/log.txt";

// Log straight through sceIo (open, append, close every line): survives any crash, and doesn't
// depend on newlib's stdout redirection (which left log.txt empty on real hardware).
void vlog(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    SceUID fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, buf, (SceSize)n);
    sceIoClose(fd);
}
std::string g_prev_stage;   // what boot.txt said when we started
bool g_prev_read = false;

void read_prev_stage() {
    if (g_prev_read) return;
    g_prev_read = true;
    SceUID fd = sceIoOpen(BOOT_FILE, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    char buf[96] = {0};
    int n = sceIoRead(fd, buf, sizeof buf - 1);
    sceIoClose(fd);
    if (n > 0) {
        buf[n] = 0;
        g_prev_stage = buf;
        while (!g_prev_stage.empty() && (g_prev_stage.back() == '\n' || g_prev_stage.back() == '\r' || g_prev_stage.back() == ' '))
            g_prev_stage.pop_back();
    }
}

// returns free user RAM in bytes (-1 if unknown)
int log_free_memory(const char* when) {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    int r = sceKernelGetFreeMemorySize(&info);
    if (r < 0) { vlog("[iris] vita: free memory %s: unknown (0x%08X)\n", when, (unsigned)r); return -1; }
    vlog("[iris] vita: free memory %s: user %d KB, cdram %d KB, phycont %d KB\n", when, info.size_user / 1024,
         info.size_cdram / 1024, info.size_phycont / 1024);
    return info.size_user;
}

bool file_exists(const char* path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

// A text screen drawn straight into a display framebuffer (no GPU, no vitaGL), for errors that
// happen before or instead of graphics. Waits for a button, then quits the app.
[[noreturn]] void fatal_screen(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) vlog("[iris] FATAL: %s\n", l.c_str());
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
    vlog("[iris] FATAL: uncaught exception / std::terminate (out of memory?)\n");
    sceKernelExitProcess(0);
    std::abort();
}

void open_log() {
    sceIoMkdir("ux0:data/BackroomsIris", 0777);
    read_prev_stage();
    SceUID fd = sceIoOpen(LOG_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); // start a fresh log
    if (fd >= 0) sceIoClose(fd);
    vlog("[iris] Backrooms Iris log\n");
    // the rest of the game logs with printf: send stdout to the same file (append mode)
    if (std::freopen(LOG_FILE, "a", stdout)) std::setvbuf(stdout, nullptr, _IONBF, 0);
}
} // namespace

bool platform_init(const PlatformConfig& cfg) {
    std::set_terminate(on_terminate);
    vlog("[iris] vita: boot%s\n", cfg.safe_mode ? " (SAFE MODE)" : "");
    if (!g_prev_stage.empty()) vlog("[iris] vita: last run ended at stage '%s'\n", g_prev_stage.c_str());
    int free_user = log_free_memory("at boot");
    platform_boot_stage("libshacccg check");

    // vitaGL compiles its shaders on the console with Sony's shader compiler, which can't be
    // shipped with homebrew: it has to be extracted from the firmware with ShaRKF00D. Without
    // it vitaGL crashes on the first draw (C2-12828-1), so check first and say so.
    const bool shacccg_data = file_exists("ur0:data/libshacccg.suprx");
    const bool shacccg_ext = file_exists("ur0:data/external/libshacccg.suprx");
    vlog("[iris] vita: libshacccg.suprx in ur0:data/ %s, in ur0:data/external/ %s\n", shacccg_data ? "YES" : "no",
         shacccg_ext ? "YES" : "no");
    if (!shacccg_data && !shacccg_ext)
        fatal_screen({"BACKROOMS IRIS CAN'T START",
                      "LIBSHACCCG.SUPRX IS MISSING.",
                      "THE GRAPHICS NEED SONY'S SHADER COMPILER:",
                      "1. INSTALL SHARKF00D (FROM VITADB)",
                      "2. OPEN IT AND LET IT EXTRACT LIBSHACCCG.SUPRX",
                      "3. START BACKROOMS IRIS AGAIN",
                      "",
                      "PRESS X OR START TO EXIT"});
    // Sanity-check the file without touching the module (vitaGL / vitaShaRK must be the first to
    // load it: loading + unloading it ourselves can leave it unusable for them, and then vitaGL
    // has no shader compiler and the first frame never appears).
    {
        const char* path = shacccg_data ? "ur0:data/libshacccg.suprx" : "ur0:data/external/libshacccg.suprx";
        SceIoStat st;
        long long size = sceIoGetstat(path, &st) >= 0 ? (long long)st.st_size : -1;
        vlog("[iris] vita: %s is %lld bytes\n", path, size);
        if (size >= 0 && size < 64 * 1024)
            fatal_screen({"BACKROOMS IRIS CAN'T START",
                          "LIBSHACCCG.SUPRX LOOKS BROKEN (TOO SMALL)",
                          "DELETE IT, RUN SHARKF00D AGAIN, THEN RESTART",
                          "", "PRESS X OR START TO EXIT"});
    }

    // the main thread (game + all GL calls) keeps CPU 0 to itself; workers go to cores 1 and 2
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);

    // run the CPU/GPU at full clocks
    platform_boot_stage("clocks");
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    // 960x544 native. vitaGL grabs all free RAM minus the threshold for the GPU, so the
    // threshold is what's left for thread stacks, SDL audio and the network. Safe mode: no MSAA
    // and twice the headroom.
    int headroom = (cfg.safe_mode ? 32 : 24) * 1024 * 1024;
    if (free_user > 0 && free_user < headroom * 3) headroom = free_user / 3; // little RAM: still leave vitaGL most of it
    platform_boot_stage("vitagl init");
    vlog("[iris] vita: vglInitExtended(%s, %d KB headroom)\n", cfg.safe_mode ? "no msaa" : "2x msaa", headroom / 1024);
    if (!vglInitExtended(0, 960, 544, headroom, cfg.safe_mode ? SCE_GXM_MULTISAMPLE_NONE : SCE_GXM_MULTISAMPLE_2X)) {
        vlog("[iris] vita: vglInitExtended failed\n");
        fatal_screen({"BACKROOMS IRIS CAN'T START", "VITAGL FAILED TO START (GPU / SHADER COMPILER)",
                      "RE-EXTRACT LIBSHACCCG.SUPRX WITH SHARKF00D", "", "PRESS X OR START TO EXIT"});
    }
    vlog("[iris] vita: vitaGL ready\n");
    log_free_memory("after vitaGL");
    platform_boot_stage("sdl init");

    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_TIMER) != 0) {
        vlog("[iris] vita: SDL_Init failed: %s\n", SDL_GetError());
        fatal_screen({"BACKROOMS IRIS CAN'T START", "SDL_INIT FAILED (INPUT)", "", "PRESS X OR START TO EXIT"});
    }
    gamepad_init();
    g_t0 = SDL_GetPerformanceCounter();
    vlog("[iris] vita: SDL ready\n");
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

// Background threads (chunk builder, sound synth): lower priority than the main thread and on
// their own core, so a busy worker can never starve the frame loop (the Vita's scheduler doesn't
// time-slice threads of equal priority).
void platform_thread_background(int core) {
    int r = sceKernelChangeThreadPriority(0, 0x10000100 + 16); // default user priority + 16 = lower
    if (r < 0) r = sceKernelChangeThreadPriority(0, 176);        // (same thing, absolute)
    int a = sceKernelChangeThreadCpuAffinityMask(0, core == 2 ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_1);
    vlog("[iris] vita: worker thread -> core %d (priority 0x%08X, affinity 0x%08X)\n", core, (unsigned)r, (unsigned)a);
}

void platform_boot_stage(const char* stage) {
    vlog("[iris] stage: %s\n", stage);
    SceUID fd = sceIoOpen(BOOT_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, stage, (SceSize)std::strlen(stage));
    sceIoClose(fd);
}

std::string platform_last_crash() {
    read_prev_stage();
    const std::string& s = g_prev_stage;
    if (s.empty() || s == "exit") return {};
    if (s.size() >= 3 && s.compare(s.size() - 3, 3, "-ok") == 0) return {};
    return s;
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
