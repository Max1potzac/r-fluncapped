#pragma once

// Keybinds: keys of the PC keyboard that act as a finger on a spot of the game's surface, or as
// Android's back button. Read from a text file (--keybinds), one bind per line:
//
//   <key> tap <x> <y>     touch (x, y) while the key is held; a number is pixels of the game's
//                         surface (--resolution), a number with % a fraction of its size
//   <key> back            Android's back button
//   <key> seed <n>        the n-th seed packet of the seed bank, counted from 1 at the top
//   <key> shovel          the shovel
//   <key> plantfood       the plant food button (then click a plant)
//   <key> fastforward     the fast-forward button
//   <key> pause           the pause button
//   <key> scroll <dir> [speed]
//                         scrolls what's under the mouse left, right, up or down while the key is
//                         held (the world map, the almanac's lists...): a finger dragging the other
//                         way, `speed` pixels a frame, or with % a fraction of the surface's height
//                         (default 2%)
//
// seed and the buttons are found wherever the level put them (src/game/game_keys.h).
// <key> is SDL's name for it (A, 1, Space, F1, Left Shift, Keypad 1...), with _ for a space.
// '#' starts a comment. Binds are off while the game shows its keyboard, so typing still works.

#include <string>
#include <vector>

namespace rfu::platform {

struct KeyBind {
    enum class Action { Tap, Back, Seed, Shovel, PlantFood, FastForward, Pause, Scroll };
    std::string key;
    Action action = Action::Tap;
    float x = 0, y = 0;
    bool x_fraction = false, y_fraction = false;  // x, y are fractions (0-1) of the surface
    int slot = 0;  // Seed: the packet, from 1
    int dir_x = 0, dir_y = 0;  // Scroll: the way the view goes (-1, 0 or 1)
    float speed = 0.02f;       // Scroll: per frame, pixels or (speed_fraction) a fraction of the height
    bool speed_fraction = true;

    // The point on a surface of the given size.
    float surface_x(int width) const { return x_fraction ? x * width : x; }
    float surface_y(int height) const { return y_fraction ? y * height : y; }
};

// Seed and the buttons: the actions src/game/game_keys.cpp carries out (GameKeyInput).
bool is_game_action(KeyBind::Action action);
// The action's word in a keybinds file ("seed", "plantfood"...).
const char* action_name(KeyBind::Action action);

// A scroll keybind's finger: down at a point (the mouse's), then each frame `speed` pixels
// against the bind's direction; at the edge it lifts and goes down at the point again. A
// point right at the edge moves back just far enough for a short stroke.
class ScrollDrag {
public:
    struct Step {
        enum Phase { Down, Move, Up } phase;
        float x, y;
    };
    ScrollDrag(const KeyBind& bind, int width, int height, float x, float y);
    // The steps for one frame: the first call puts the finger down.
    std::vector<Step> frame();
    // Lifts the finger (nothing when it's already up).
    std::vector<Step> release();

private:
    int w_, h_;
    float dx_, dy_;
    float start_x_, start_y_;
    float x_ = 0, y_ = 0;
    bool down_ = false;
};

// Parses a keybinds file; each line it can't read is reported in `errors` ("line 3: ...").
std::vector<KeyBind> parse_keybinds(const std::string& text, std::vector<std::string>& errors);

}  // namespace rfu::platform
