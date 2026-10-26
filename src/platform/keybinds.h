#pragma once

// Keybinds: keys of the PC keyboard that act as a finger on a spot of the game's surface, or as
// Android's back button. Read from a text file (--keybinds), one bind per line:
//
//   <key> tap <x> <y>     touch (x, y) while the key is held; a number is pixels of the game's
//                         surface (--resolution), a number with % a fraction of its size
//   <key> back            Android's back button
//
// <key> is SDL's name for it (A, 1, Space, F1, Left Shift, Keypad 1...), with _ for a space.
// '#' starts a comment. Binds are off while the game shows its keyboard, so typing still works.

#include <string>
#include <vector>

namespace rfu::platform {

struct KeyBind {
    enum class Action { Tap, Back };
    std::string key;
    Action action = Action::Tap;
    float x = 0, y = 0;
    bool x_fraction = false, y_fraction = false;  // x, y are fractions (0-1) of the surface

    // The point on a surface of the given size.
    float surface_x(int width) const { return x_fraction ? x * width : x; }
    float surface_y(int height) const { return y_fraction ? y * height : y; }
};

// Parses a keybinds file; each line it can't read is reported in `errors` ("line 3: ...").
std::vector<KeyBind> parse_keybinds(const std::string& text, std::vector<std::string>& errors);

}  // namespace rfu::platform
