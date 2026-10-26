#include "platform/window.h"

#include "core/log.h"
#include "platform/gles.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <stdexcept>

namespace rfu::platform {

namespace {

// Android key codes (android.view.KeyEvent) for the keys a PC keyboard has.
int android_keycode(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 29 + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return 7 + (k - SDLK_0);
    switch (k) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return 66;
    case SDLK_BACKSPACE: return 67;
    case SDLK_TAB: return 61;
    case SDLK_SPACE: return 62;
    case SDLK_LEFT: return 21;
    case SDLK_RIGHT: return 22;
    case SDLK_UP: return 19;
    case SDLK_DOWN: return 20;
    case SDLK_DELETE: return 112;
    case SDLK_COMMA: return 55;
    case SDLK_PERIOD: return 56;
    case SDLK_MINUS: return 69;
    case SDLK_EQUALS: return 70;
    case SDLK_SLASH: return 76;
    case SDLK_LSHIFT: return 59;
    case SDLK_RSHIFT: return 60;
    case SDLK_PAGEUP: return 92;
    case SDLK_PAGEDOWN: return 93;
    case SDLK_HOME: return 122;
    case SDLK_END: return 123;
    default: return 0;
    }
}

[[noreturn]] void sdl_fail(const char* what) { throw std::runtime_error(std::string(what) + ": " + SDL_GetError()); }

}  // namespace

Window::Window(const WindowOptions& options) {
    SDL_SetMainReady();
    if (options.angle) SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#ifdef SDL_HINT_VIDEO_X11_FORCE_EGL
    // On Linux, EGL is where Mesa's ES contexts come from.
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) sdl_fail("SDL_Init");

    // What the game's GLSurfaceView asks EGL for: ES 2, RGB888, a 16-bit depth buffer.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (options.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window_ = SDL_CreateWindow(options.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, options.width, options.height, flags);
    if (!window_) sdl_fail("SDL_CreateWindow");
    context_ = SDL_GL_CreateContext(window_);
    if (!context_) sdl_fail("couldn't create an OpenGL ES 2 context (on Windows, try --angle with ANGLE's DLLs next to rfu)");
    SDL_GL_MakeCurrent(window_, context_);
    SDL_GL_SetSwapInterval(options.vsync ? 1 : 0);
    load_gles([](const char* name) { return SDL_GL_GetProcAddress(name); });
    SDL_StopTextInput();
}

Window::~Window() {
    if (context_) SDL_GL_DeleteContext(context_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

int Window::width() const {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window_, &w, &h);
    return w;
}

int Window::height() const {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window_, &w, &h);
    return h;
}

void Window::make_current() { SDL_GL_MakeCurrent(window_, context_); }
void Window::swap() { SDL_GL_SwapWindow(window_); }

void Window::set_text_input(bool on) {
    if (on) SDL_StartTextInput();
    else SDL_StopTextInput();
}

std::vector<Input> Window::poll() {
    std::vector<Input> out;
    // Mouse coordinates are in window points; the game wants surface pixels.
    int ww = 1, wh = 1;
    SDL_GetWindowSize(window_, &ww, &wh);
    float sx = static_cast<float>(width()) / std::max(ww, 1), sy = static_cast<float>(height()) / std::max(wh, 1);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: out.push_back(QuitInput{}); break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) out.push_back(ResizeInput{width(), height()});
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) out.push_back(FocusInput{true});
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) out.push_back(FocusInput{false});
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.which == SDL_TOUCH_MOUSEID) break;  // the finger events below have it
            if (ev.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = true;
                out.push_back(TouchInput{TouchInput::Began, 1, ev.button.x * sx, ev.button.y * sy});
            } else if (ev.button.button == SDL_BUTTON_X1) {
                out.push_back(BackInput{});
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.which == SDL_TOUCH_MOUSEID) break;
            if (ev.button.button == SDL_BUTTON_LEFT && mouse_down_) {
                mouse_down_ = false;
                out.push_back(TouchInput{TouchInput::Ended, 1, ev.button.x * sx, ev.button.y * sy});
            }
            break;
        case SDL_MOUSEMOTION:
            if (ev.motion.which == SDL_TOUCH_MOUSEID || !mouse_down_) break;
            out.push_back(TouchInput{TouchInput::Moved, 1, ev.motion.x * sx, ev.motion.y * sy});
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP: {
            auto phase = ev.type == SDL_FINGERDOWN ? TouchInput::Began : ev.type == SDL_FINGERUP ? TouchInput::Ended : TouchInput::Moved;
            // Pointer ids 2 and up, so a finger never collides with the mouse.
            int id = 2 + static_cast<int>(ev.tfinger.fingerId & 0xFFFF);
            out.push_back(TouchInput{phase, id, ev.tfinger.x * width(), ev.tfinger.y * height()});
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            bool down = ev.type == SDL_KEYDOWN;
            if (ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_AC_BACK) {
                if (down && !ev.key.repeat) out.push_back(BackInput{});
                break;
            }
            if (ev.key.keysym.sym == SDLK_F11 && down) {
                bool fs = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(window_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            int code = android_keycode(ev.key.keysym.sym);
            if (code) {
                // KeyEvent.getUnicodeChar(): printable keys give their character, Enter '\n'.
                std::uint32_t unicode = code == 66 ? '\n' : ev.key.keysym.sym >= 0x20 && ev.key.keysym.sym < 0x7F ? static_cast<std::uint32_t>(ev.key.keysym.sym) : 0;
                out.push_back(KeyInput{code, unicode, down, ev.key.repeat});
            }
            break;
        }
        case SDL_TEXTINPUT: out.push_back(TextInput{TextInput::Commit, ev.text.text}); break;
        default: break;
        }
    }
    return out;
}

}  // namespace rfu::platform
