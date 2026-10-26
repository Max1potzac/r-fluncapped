#pragma once

// The game's window: an SDL2 window with an OpenGL ES 2 context, and its input.

#include "platform/keybinds.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

struct SDL_Window;

namespace rfu::platform {

struct WindowOptions {
    std::string title = "Plants vs. Zombies 2";
    int width = 1280, height = 720;  // the client area, in pixels
    bool fullscreen = false;
    bool angle = false;  // use ANGLE's libEGL / libGLESv2 even when the driver has an ES profile
    bool vsync = true;
    // The size of the surface the game draws on, fixed for the run (the game reads it once, at
    // startup, and picks its 768 or 1536 asset set from it). It is scaled to fit the window.
    // 0x0: the desktop's shape, 720 pixels tall for low_assets, else 1080 to 1440.
    int game_width = 0, game_height = 0;
    bool low_assets = false;
    std::vector<KeyBind> keybinds;
    bool hover = false;  // send the mouse's moves with no button down too (kHoverPointer)
    std::filesystem::path icon;  // a PNG for the window's (and taskbar's) icon
};

// Input, in the shape the game's Java side reports it.
// The pointer id of the mouse moving with no button down (WindowOptions::hover): the game
// sees a finger sliding over the screen, which collects sun, coins and plant food it passes.
constexpr int kHoverPointer = 99;

struct TouchInput {
    enum Phase { Began = 0, Moved = 1, Stationary = 2, Ended = 3, Cancelled = 4 } phase;
    int id;          // 1-based pointer id
    float x, y;      // pixels from the top left of the surface
};
struct KeyInput {
    int android_keycode;
    std::uint32_t unicode;
    bool down;
    int repeat;
};
struct TextInput {
    enum Action { Commit = 0, Compose = 1, Finish = 2, Backspace = 3 } action;
    std::string utf8;
};
struct BackInput {};
struct ResizeInput {
    int width, height;
};
struct FocusInput {
    bool focused;
};
struct QuitInput {};
// A keybind for something of the game's own (KeyBind::Action::Seed or Shovel) going down or up.
struct GameKeyInput {
    KeyBind::Action action;
    int slot;
    bool down;
};
using Input = std::variant<TouchInput, KeyInput, TextInput, BackInput, ResizeInput, FocusInput, QuitInput, GameKeyInput>;

class Window {
public:
    explicit Window(const WindowOptions& options);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Size of the game's surface in pixels.
    int width() const { return game_w_; }
    int height() const { return game_h_; }

    // Makes the GL context current on the calling thread.
    void make_current();
    // Shows the game's surface, scaled to fit the window, and gets ready for the next frame.
    void swap();
    // Binds the game's surface (for reading it back).
    void bind_surface();

    // Collects pending input. Mouse buttons become touches (left button = pointer 1),
    // Escape and the mouse's back button become the Android back button.
    std::vector<Input> poll();

    // The game asked for (or dismissed) the soft keyboard: route typed text to it.
    void set_text_input(bool on);

private:
    void create_surface();
    void set_icon(const std::filesystem::path& file);
    void drawable_size(int& w, int& h) const;
    // Window points to the game's surface pixels, through the letterbox swap() draws.
    void to_surface(float wx, float wy, float& x, float& y) const;

    SDL_Window* window_ = nullptr;
    int game_w_ = 0, game_h_ = 0;
    unsigned fbo_ = 0;  // 0: the game draws straight to the window, which then has the game's size
    void* context_ = nullptr;
    bool mouse_down_ = false;
    bool text_input_ = false;
    bool hover_ = false;
    std::vector<std::pair<int, KeyBind>> binds_;  // by SDL key code
};

}  // namespace rfu::platform
