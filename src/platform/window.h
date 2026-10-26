#pragma once

// The game's window: an SDL2 window with an OpenGL ES 2 context, and its input.

#include <cstdint>
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
};

// Input, in the shape the game's Java side reports it.
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
using Input = std::variant<TouchInput, KeyInput, TextInput, BackInput, ResizeInput, FocusInput, QuitInput>;

class Window {
public:
    explicit Window(const WindowOptions& options);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Size of the drawable surface in pixels.
    int width() const;
    int height() const;

    // Makes the GL context current on the calling thread.
    void make_current();
    void swap();

    // Collects pending input. Mouse buttons become touches (left button = pointer 1),
    // Escape and the mouse's back button become the Android back button.
    std::vector<Input> poll();

    // The game asked for (or dismissed) the soft keyboard: route typed text to it.
    void set_text_input(bool on);

private:
    SDL_Window* window_ = nullptr;
    void* context_ = nullptr;
    bool mouse_down_ = false;
};

}  // namespace rfu::platform
