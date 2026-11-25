// Backrooms Iris - PC platform (Linux, Windows): SDL2 window, desktop OpenGL context.
#include <SDL.h>

#include <cstdio>
#include <vector>

#include "../../client/gl.h"
#include "../platform.h"
#include "../sdl_gamepad.h"

namespace iris {
namespace {

SDL_Window* g_win = nullptr;
SDL_GLContext g_ctx = nullptr;
bool g_captured = false;
bool g_fullscreen = false;
Uint64 g_t0 = 0;

void load_gl_functions() {
    gl_load_extensions([](const char* name) { return SDL_GL_GetProcAddress(name); },
                       [](const char* ext) { return SDL_GL_ExtensionSupported(ext) == SDL_TRUE; });
}

} // namespace

bool platform_init(const PlatformConfig& cfg) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    // Desktop GL 2.1 compatibility context: the fixed-function + VBO subset we use is
    // also what vitaGL implements on the Vita, so the renderer is shared.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    if (cfg.msaa > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, cfg.msaa);
    }
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.hidden) flags |= SDL_WINDOW_HIDDEN;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    g_win = SDL_CreateWindow(cfg.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.width, cfg.height, flags);
    if (!g_win && cfg.msaa > 0) { // no multisampling available: try again without it
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
        g_win = SDL_CreateWindow(cfg.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, cfg.width, cfg.height, flags);
    }
    if (!g_win) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g_ctx = SDL_GL_CreateContext(g_win);
    if (!g_ctx) {
        std::fprintf(stderr, "OpenGL context failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_win, g_ctx);
    if (cfg.vsync && SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1); // adaptive, else plain
    if (!cfg.vsync) SDL_GL_SetSwapInterval(0);
    load_gl_functions();
    std::printf("[iris] GL %s | %s\n", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    g_fullscreen = cfg.fullscreen;
    g_t0 = SDL_GetPerformanceCounter();
    gamepad_init();
    if (!cfg.hidden) platform_set_mouse_capture(true);
    return true;
}

void platform_set_mouse_capture(bool on) {
    g_captured = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
}

void platform_poll(InputState& in) {
    in.mouse_dx = in.mouse_dy = 0;
    in.quit = in.screenshot = in.toggle_fullscreen = in.toggle_debug = in.pause = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        gamepad_event(e, in);
        switch (e.type) {
            case SDL_QUIT: in.quit = true; break;
            case SDL_MOUSEMOTION:
                if (g_captured) {
                    in.mouse_dx += (float)e.motion.xrel;
                    in.mouse_dy += (float)e.motion.yrel;
                    if (e.motion.xrel || e.motion.yrel) in.using_gamepad = false;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (!g_captured) platform_set_mouse_capture(true);
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) platform_set_mouse_capture(false);
                break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                in.using_gamepad = false;
                switch (e.key.keysym.sym) {
                    case SDLK_ESCAPE:
                        if (g_captured) platform_set_mouse_capture(false);
                        else in.quit = true; // second Esc quits
                        break;
                    case SDLK_F11: in.toggle_fullscreen = true; break;
                    case SDLK_RETURN:
                        if (e.key.keysym.mod & KMOD_ALT) in.toggle_fullscreen = true;
                        break;
                    case SDLK_F12: in.screenshot = true; break;
                    case SDLK_F3: in.toggle_debug = true; break;
                    default: break;
                }
                break;
            default: break;
        }
    }
    if (in.toggle_fullscreen) {
        g_fullscreen = !g_fullscreen;
        SDL_SetWindowFullscreen(g_win, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }

    const Uint8* k = SDL_GetKeyboardState(nullptr);
    in.move_x = (float)((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]));
    in.move_z = (float)((k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) - (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]));
    in.sprint = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
    in.crouch = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_C];
    in.look_x = in.look_y = 0;
    gamepad_poll(in);

    in.mouse_captured = g_captured;
    SDL_GL_GetDrawableSize(g_win, &in.width, &in.height);
}

void platform_swap() { SDL_GL_SwapWindow(g_win); }

double platform_time() {
    return (double)(SDL_GetPerformanceCounter() - g_t0) / (double)SDL_GetPerformanceFrequency();
}

bool platform_save_screenshot(const std::string& path) {
    int w, h;
    SDL_GL_GetDrawableSize(g_win, &w, &h);
    std::vector<unsigned char> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
    if (!s) return false;
    for (int y = 0; y < h; y++) // GL rows are bottom-up
        SDL_memcpy((Uint8*)s->pixels + (size_t)y * s->pitch, px.data() + (size_t)(h - 1 - y) * w * 3, (size_t)w * 3);
    bool ok = SDL_SaveBMP(s, path.c_str()) == 0;
    SDL_FreeSurface(s);
    return ok;
}

const char* platform_name() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "PC";
#endif
}

std::string platform_data_dir() {
    char* p = SDL_GetPrefPath("BackroomsIris", "BackroomsIris");
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}

void platform_shutdown() {
    gamepad_shutdown();
    if (g_ctx) SDL_GL_DeleteContext(g_ctx);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

} // namespace iris
