#pragma once

// Keybind hints: a small badge with a key's name drawn over what the key presses (the seed
// packet's corner, the shovel's...), so the binds can be seen without opening the keybinds file.
// The badges are blocks of solid colour (a border, a dark back and a 5x7 pixel font), which
// Window::draw_hints fills with scissored clears, leaving the game's GL state alone.

#include <string>
#include <vector>

namespace rfu::platform {

struct HintLabel {
    enum class Anchor { TopLeft, BottomRight };  // the badge's corner that sits on (x, y)
    std::string text;
    int x = 0, y = 0;  // pixels of the game's surface, from the top left
    Anchor anchor = Anchor::TopLeft;
};

struct HintFill {
    enum class Color { Border, Back, Text };
    int x, y, w, h;  // pixels of the game's surface, from the top left
    Color color;
};

// What a badge shows for a key, from its name in a keybinds file: "F", "1", "SPC", "KP1", "LSHF"...
std::string hint_text(const std::string& key);

// The rectangles of one badge, in drawing order, sized for the surface's height and kept on it.
std::vector<HintFill> layout_hint(const HintLabel& label, int surface_width, int surface_height);

}  // namespace rfu::platform
