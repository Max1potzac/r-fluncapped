#include "platform/window.h"

#include "core/log.h"
#include "platform/gles.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#define SDL_USE_BUILTIN_OPENGL_DEFINITIONS 1
#define GL_GLES_PROTOTYPES 0
#include <SDL_opengles2.h>

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

constexpr GLenum kReadFramebuffer = 0x8CA8, kRgba8 = 0x8058, kDepth24Stencil8 = 0x88F0;

// What swap() needs beyond ES 2: glBlitFramebuffer (ES 3, or ANGLE's / NV's extension in ES 2).
struct Present {
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer;
    PFNGLGENRENDERBUFFERSPROC GenRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC BindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETFLOATVPROC GetFloatv;
    PFNGLGETBOOLEANVPROC GetBooleanv;
    PFNGLISENABLEDPROC IsEnabled;
    PFNGLENABLEPROC Enable;
    PFNGLDISABLEPROC Disable;
    PFNGLCLEARCOLORPROC ClearColor;
    PFNGLCOLORMASKPROC ColorMask;
    PFNGLCLEARPROC Clear;
    void(GL_APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
} gl;

template <typename F>
void load(F& f, const char* name) {
    f = reinterpret_cast<F>(SDL_GL_GetProcAddress(name));
}

// The surface size the game gets when none was asked for: the desktop's shape at a height that
// picks the asset set (the game uses its 1536 assets from 1080 pixels up, 768 below).
void default_game_size(SDL_Window* window, bool low_assets, int& w, int& h) {
    SDL_DisplayMode mode{};
    int display = std::max(SDL_GetWindowDisplayIndex(window), 0);
    double aspect = 16.0 / 9.0;
    int desktop_h = 1080;
    if (SDL_GetDesktopDisplayMode(display, &mode) == 0 && mode.w > 0 && mode.h > 0) {
        if (mode.w >= mode.h) aspect = static_cast<double>(mode.w) / mode.h;
        desktop_h = std::min(mode.w, mode.h);
    }
    h = low_assets ? 720 : std::clamp(desktop_h, 1080, 1440);
    w = static_cast<int>(h * aspect + 0.5) & ~1;
}

}  // namespace

Window::Window(const WindowOptions& options) {
    SDL_SetMainReady();
    if (options.angle) SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#ifdef SDL_HINT_VIDEO_X11_FORCE_EGL
    // On Linux, EGL is where Mesa's ES contexts come from.
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) sdl_fail("SDL_Init");

    // What the game's GLSurfaceView asks EGL for: ES 2, RGB888, a 16-bit depth buffer. ES 3
    // is asked for first, for glBlitFramebuffer; the game only uses ES 2 calls.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
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
    if (!context_) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        context_ = SDL_GL_CreateContext(window_);
    }
    if (!context_) sdl_fail("couldn't create an OpenGL ES 2 context (on Windows, try --angle with ANGLE's DLLs next to rfu)");
    SDL_GL_MakeCurrent(window_, context_);
    SDL_GL_SetSwapInterval(options.vsync ? 1 : 0);
    load_gles([](const char* name) { return SDL_GL_GetProcAddress(name); });
    SDL_StopTextInput();

    game_w_ = options.game_width;
    game_h_ = options.game_height;
    if (game_w_ <= 0 || game_h_ <= 0) default_game_size(window_, options.low_assets, game_w_, game_h_);
    create_surface();
}

void Window::create_surface() {
    load(gl.GenFramebuffers, "glGenFramebuffers");
    load(gl.BindFramebuffer, "glBindFramebuffer");
    load(gl.GenRenderbuffers, "glGenRenderbuffers");
    load(gl.BindRenderbuffer, "glBindRenderbuffer");
    load(gl.RenderbufferStorage, "glRenderbufferStorage");
    load(gl.FramebufferRenderbuffer, "glFramebufferRenderbuffer");
    load(gl.CheckFramebufferStatus, "glCheckFramebufferStatus");
    load(gl.GetIntegerv, "glGetIntegerv");
    load(gl.GetFloatv, "glGetFloatv");
    load(gl.GetBooleanv, "glGetBooleanv");
    load(gl.IsEnabled, "glIsEnabled");
    load(gl.Enable, "glEnable");
    load(gl.Disable, "glDisable");
    load(gl.ClearColor, "glClearColor");
    load(gl.ColorMask, "glColorMask");
    load(gl.Clear, "glClear");
    for (const char* name : {"glBlitFramebuffer", "glBlitFramebufferANGLE", "glBlitFramebufferNV"}) {
        load(gl.BlitFramebuffer, name);
        if (gl.BlitFramebuffer) break;
    }
    if (gl.BlitFramebuffer) {
        GLuint fbo = 0, color = 0, depth = 0;
        gl.GenFramebuffers(1, &fbo);
        gl.GenRenderbuffers(1, &color);
        gl.GenRenderbuffers(1, &depth);
        gl.BindRenderbuffer(GL_RENDERBUFFER, color);
        gl.RenderbufferStorage(GL_RENDERBUFFER, kRgba8, game_w_, game_h_);
        gl.BindRenderbuffer(GL_RENDERBUFFER, depth);
        gl.RenderbufferStorage(GL_RENDERBUFFER, kDepth24Stencil8, game_w_, game_h_);
        gl.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
        gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
        if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            fbo_ = fbo;
            set_default_framebuffer(fbo_);
            log::info("game surface {}x{}, scaled to the window", game_w_, game_h_);
            return;
        }
        gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
        log::warn("couldn't create a {}x{} offscreen surface", game_w_, game_h_);
    }
    // Without one the game draws straight to the window, at the window's size.
    drawable_size(game_w_, game_h_);
    log::warn("the GL driver can't scale a surface (no glBlitFramebuffer): the game draws at the window's size, {}x{}, "
              "and the window shouldn't change size", game_w_, game_h_);
}

Window::~Window() {
    if (context_) SDL_GL_DeleteContext(context_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}

void Window::drawable_size(int& w, int& h) const {
    SDL_GL_GetDrawableSize(window_, &w, &h);
    w = std::max(w, 1);
    h = std::max(h, 1);
}

// The largest rectangle with the game's shape that fits the drawable, centred.
static void letterbox(int dw, int dh, int gw, int gh, int& x, int& y, int& w, int& h) {
    double s = std::min(static_cast<double>(dw) / gw, static_cast<double>(dh) / gh);
    w = static_cast<int>(gw * s + 0.5);
    h = static_cast<int>(gh * s + 0.5);
    x = (dw - w) / 2;
    y = (dh - h) / 2;
}

void Window::to_surface(float wx, float wy, float& x, float& y) const {
    int ww = 1, wh = 1, dw, dh;
    SDL_GetWindowSize(window_, &ww, &wh);
    drawable_size(dw, dh);
    float px = wx * dw / std::max(ww, 1), py = wy * dh / std::max(wh, 1);  // points to pixels
    if (!fbo_) {
        x = px;
        y = py;
        return;
    }
    int lx, ly, lw, lh;
    letterbox(dw, dh, game_w_, game_h_, lx, ly, lw, lh);
    x = std::clamp((px - lx) * game_w_ / std::max(lw, 1), 0.0f, static_cast<float>(game_w_ - 1));
    y = std::clamp((py - ly) * game_h_ / std::max(lh, 1), 0.0f, static_cast<float>(game_h_ - 1));
}

void Window::make_current() { SDL_GL_MakeCurrent(window_, context_); }

void Window::bind_surface() {
    if (gl.BindFramebuffer) gl.BindFramebuffer(GL_FRAMEBUFFER, fbo_);
}
void Window::swap() {
    if (!fbo_) return SDL_GL_SwapWindow(window_);
    // Scale the game's surface into the window, leaving the game's GL state as it was.
    GLint bound = 0;
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    GLboolean scissor = gl.IsEnabled(GL_SCISSOR_TEST), mask[4];
    GLfloat clear[4];
    gl.GetBooleanv(GL_COLOR_WRITEMASK, mask);
    gl.GetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.ClearColor(0, 0, 0, 1);

    int dw, dh, x, y, w, h;
    drawable_size(dw, dh);
    letterbox(dw, dh, game_w_, game_h_, x, y, w, h);
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    gl.BindFramebuffer(kReadFramebuffer, fbo_);
    gl.BlitFramebuffer(0, 0, game_w_, game_h_, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    SDL_GL_SwapWindow(window_);

    gl.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(bound));
    gl.ClearColor(clear[0], clear[1], clear[2], clear[3]);
    gl.ColorMask(mask[0], mask[1], mask[2], mask[3]);
    if (scissor) gl.Enable(GL_SCISSOR_TEST);
}

void Window::set_text_input(bool on) {
    if (on) SDL_StartTextInput();
    else SDL_StopTextInput();
}

std::vector<Input> Window::poll() {
    std::vector<Input> out;
    // Mouse coordinates are in window points; the game wants pixels of its surface.
    auto touch = [&](TouchInput::Phase phase, int id, float wx, float wy) {
        float x, y;
        to_surface(wx, wy, x, y);
        out.push_back(TouchInput{phase, id, x, y});
    };
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT: out.push_back(QuitInput{}); break;
        case SDL_WINDOWEVENT:
            // With its own surface the game keeps its size; only a game drawing straight to
            // the window has to follow it.
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !fbo_) {
                drawable_size(game_w_, game_h_);
                out.push_back(ResizeInput{game_w_, game_h_});
            }
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) out.push_back(FocusInput{true});
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) out.push_back(FocusInput{false});
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.which == SDL_TOUCH_MOUSEID) break;  // the finger events below have it
            if (ev.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = true;
                touch(TouchInput::Began, 1, static_cast<float>(ev.button.x), static_cast<float>(ev.button.y));
            } else if (ev.button.button == SDL_BUTTON_X1) {
                out.push_back(BackInput{});
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.which == SDL_TOUCH_MOUSEID) break;
            if (ev.button.button == SDL_BUTTON_LEFT && mouse_down_) {
                mouse_down_ = false;
                touch(TouchInput::Ended, 1, static_cast<float>(ev.button.x), static_cast<float>(ev.button.y));
            }
            break;
        case SDL_MOUSEMOTION:
            if (ev.motion.which == SDL_TOUCH_MOUSEID || !mouse_down_) break;
            touch(TouchInput::Moved, 1, static_cast<float>(ev.motion.x), static_cast<float>(ev.motion.y));
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP: {
            auto phase = ev.type == SDL_FINGERDOWN ? TouchInput::Began : ev.type == SDL_FINGERUP ? TouchInput::Ended : TouchInput::Moved;
            // Pointer ids 2 and up, so a finger never collides with the mouse.
            int id = 2 + static_cast<int>(ev.tfinger.fingerId & 0xFFFF);
            int ww = 1, wh = 1;
            SDL_GetWindowSize(window_, &ww, &wh);
            touch(phase, id, ev.tfinger.x * ww, ev.tfinger.y * wh);
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
