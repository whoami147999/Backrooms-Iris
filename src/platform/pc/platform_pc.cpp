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
    SDL_StartTextInput();
    return true;
}

void platform_set_mouse_capture(bool on) {
    g_captured = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
}

void platform_poll(InputState& in) {
    InputState prev = in;
    in = InputState{};
    in.using_gamepad = prev.using_gamepad;
    in.mouse_x = prev.mouse_x;
    in.mouse_y = prev.mouse_y;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        gamepad_event(e, in);
        switch (e.type) {
            case SDL_QUIT: in.quit = true; break;
            case SDL_MOUSEMOTION:
                in.mouse_moved = true;
                if (g_captured) {
                    in.mouse_dx += (float)e.motion.xrel;
                    in.mouse_dy += (float)e.motion.yrel;
                } else {
                    // menus use absolute positions in drawable pixels (HiDPI aware)
                    int ww, wh, dw, dh;
                    SDL_GetWindowSize(g_win, &ww, &wh);
                    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
                    in.mouse_x = e.motion.x * (float)dw / (float)(ww > 0 ? ww : 1);
                    in.mouse_y = e.motion.y * (float)dh / (float)(wh > 0 ? wh : 1);
                }
                if (e.motion.xrel || e.motion.yrel) in.using_gamepad = false;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) { in.click = true; if (g_captured) in.use_item = true; }
                if (e.button.button == SDL_BUTTON_RIGHT && !g_captured) in.back = true; // menus: right click goes back
                in.using_gamepad = false;
                break;
            case SDL_MOUSEWHEEL:
                if (e.wheel.y > 0) in.slot_prev = true;
                if (e.wheel.y < 0) in.slot_next = true;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && g_captured) in.pause = true;
                break;
            case SDL_TEXTINPUT:
                in.text += e.text.text;
                break;
            case SDL_KEYDOWN: {
                in.using_gamepad = false;
                SDL_Keycode k = e.key.keysym.sym;
                // navigation and editing keys repeat when held
                switch (k) {
                    case SDLK_UP: in.up = true; break;
                    case SDLK_DOWN: in.down = true; break;
                    case SDLK_LEFT: in.left = true; break;
                    case SDLK_RIGHT: in.right = true; break;
                    case SDLK_BACKSPACE: in.backspace = true; break;
                    default: break;
                }
                if (e.key.repeat) break;
                switch (k) {
                    case SDLK_ESCAPE: in.back = true; in.pause = true; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        if (e.key.keysym.mod & KMOD_ALT) in.toggle_fullscreen = true;
                        else in.accept = true;
                        break;
                    case SDLK_e: in.interact = true; break;
                    case SDLK_f: in.light = true; break;
                    case SDLK_q: in.use_item = true; break;
                    case SDLK_TAB: in.sandbox_menu = true; break;
                    case SDLK_F11: in.toggle_fullscreen = true; break;
                    case SDLK_F12: in.screenshot = true; break;
                    case SDLK_F3: in.toggle_debug = true; break;
                    case SDLK_g: in.emote = 1; break;
                    case SDLK_h: in.emote = 2; break;
                    case SDLK_j: in.emote = 3; break;
                    case SDLK_k: in.emote = 4; break;
                    default:
                        if (k >= SDLK_1 && k <= SDLK_9) in.slot_num = (int)(k - SDLK_1);
                        break;
                }
                break;
            }
            default: break;
        }
    }

    const Uint8* k = SDL_GetKeyboardState(nullptr);
    if (g_captured) {
        in.move_x = (float)((k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]));
        in.move_z = (float)((k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) - (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]));
        in.sprint = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
        in.crouch = k[SDL_SCANCODE_LCTRL] || k[SDL_SCANCODE_C];
        in.fire_held = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
        in.talk = k[SDL_SCANCODE_V] != 0;
    }
    gamepad_poll(in, platform_time());

    in.mouse_captured = g_captured;
    SDL_GL_GetDrawableSize(g_win, &in.width, &in.height);
}

void platform_set_fullscreen(bool on) {
    g_fullscreen = on;
    SDL_SetWindowFullscreen(g_win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void platform_set_vsync(bool on) {
    if (on) { if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1); }
    else SDL_GL_SetSwapInterval(0);
}

bool platform_is_vita() { return false; }

void platform_boot_stage(const char*) {}
void platform_thread_background(int) {}
std::string platform_last_crash() { return {}; }

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
