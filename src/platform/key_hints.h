#pragma once

// Keybind hints: a small badge with a key's name drawn over what the key presses (the seed
// packet's corner, the shovel's...), so the binds can be seen without opening the keybinds file.
// The badges are blocks of solid colour (a border, a dark back and a 5x7 pixel font), which
// Window::draw_hints fills with scissored clears, leaving the game's GL state alone. The controller's
// tile cursor is drawn the same way, as an outlined box.

#include <cstdint>
#include <string>
#include <vector>

namespace rfu::platform {

// The pad's face button symbols (PlayStation), as characters of a label's text.
constexpr char kGlyphCross = 5, kGlyphCircle = 6, kGlyphSquare = 7, kGlyphTriangle = 8;

struct HintLabel {
    enum class Anchor { TopLeft, BottomRight };  // the badge's corner that sits on (x, y)
    std::string text;
    int x = 0, y = 0;  // pixels of the game's surface, from the top left
    Anchor anchor = Anchor::TopLeft;
    std::uint32_t text_rgb = 0xFFFFFF;
};

// An outlined rectangle: the controller's cursor.
struct HintBox {
    int x = 0, y = 0, w = 0, h = 0;  // pixels of the game's surface, from the top left
    std::uint32_t rgb = 0xFFFFFF;
};

struct HintOverlay {
    std::vector<HintLabel> labels;
    std::vector<HintBox> boxes;
    bool empty() const { return labels.empty() && boxes.empty(); }
};

struct HintFill {
    int x, y, w, h;  // pixels of the game's surface, from the top left
    std::uint32_t rgb;
};

constexpr std::uint32_t kHintBorder = 0xF5CC40, kHintBack = 0x291A0D;

// What a badge shows for a key, from its name in a keybinds file: "F", "1", "SPC", "KP1", "LSHF"...
std::string hint_text(const std::string& key);

// The rectangles of one badge, in drawing order, sized for the surface's height and kept on it.
std::vector<HintFill> layout_hint(const HintLabel& label, int surface_width, int surface_height);

// The rectangles of a box's outline (with a dark edge outside), sized for the surface's height.
std::vector<HintFill> layout_box(const HintBox& box, int surface_height);

}  // namespace rfu::platform
