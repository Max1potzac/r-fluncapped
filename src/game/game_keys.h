#pragma once

// Game keys: the keybinds that press something of the game's own, wherever the level's layout
// put it. "seed <n>" presses the n-th packet of the seed bank (counted from the top), and
// "shovel", "plantfood", "fastforward" and "pause" those buttons, as a finger going down on them
// while the key is held.
//
// The seed bank (SeedBankNew) and the buttons (ShovelUI, PlantfoodUI, FastForwardButton,
// PauseButton) aren't Sexy widgets but objects of the game's newer UI, whose positions are
// relative to parents named by handles. Hooks on their per-frame updates keep the live
// instances; a key asks the game itself for the packet's or the button's rectangle on screen and sends a touch at its middle into LawnApp's touch
// handlers, the way UI_ProcessEvents delivers a finger's.
//
// Like hover to collect, the addresses are those of the libPVZ2 build Reflourished 1.4.2 and
// AltverZ 1.8.3 share, checked first; on another build game keys stay off.
//
// Keybind hints (--key-hints) put each game key's name on what it presses, through the same
// rectangles, drawn over each frame by Window::draw_hints.
//
// The controller (--controller) plays the lawn the way PvZ1's console version does: a cursor on
// the grid's tiles that the d-pad moves, and buttons that tap the packet, the shovel or plant food
// and then the tile. The tiles come from the Board's grid (origin and tile size in world units)
// and the zoom it draws the lawn with, the same maths as the game's own (Board, 0xab23d0); the
// cursor is a box drawn with the hints. R3 halves the game's speed (slowmode), and right-stick
// flicks pick the level's powerups, which X then uses on the tile. Off the lawn, the pause menu's sliders and buttons are
// picked with the d-pad, the seed chooser's packets and slots with the d-pad and LB/RB, and
// elsewhere the right stick moves a cursor that A touches with.

#include "platform/window.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rfu {

class Runtime;
class Module;

// The screens the controller's buttons are bound on, each on its own: moving an action on one
// leaves the same button alone on the others. Lawn is a level in play (the tile box, or the free
// cursor there); Chooser the seed chooser; Menu the pause menu, the settings and the title screen;
// Map the world map; Other everywhere else (the cursor).
enum class PadScreen { Lawn, Chooser, Menu, Map, Other };
constexpr std::size_t kPadScreens = 5;

// --pad-binds: the button for each action, as "plant=a,dig=b,chooser.take=x,map.shop=lb". An
// action without a screen is the lawn's (plant=a,dig=b,use=x,plantfood=y,prevseed=lb,nextseed=rb,
// vacuum=lt,vacuum2=rt,fastforward=l3,pause=start,slowmode=r3,freecursor=back); the others:
//   chooser.  take=a remove=b letsrock=x viewlawn=y prevslot=lb nextslot=rb reload=lt almanac=rt pause=start freecursor=back
//   menu.     press=a back=b start=start prevbanner=lb nextbanner=rb
//   map.      touch=a back=b shop=y almanac=lt zengarden=rt settings=start
//   other.    touch=a back=b pause=start fastforward=l3
// An action is the button that does it by default on its screen (A for plant...); a bound action's
// default button does nothing on that screen unless it's bound to something too.
struct PadBind {
    PadScreen screen = PadScreen::Lawn;
    platform::PadButton action;  // the action, as its default button on the screen
    platform::PadButton button;  // the button that does it
};
// Empty, with error set, when the text isn't binds.
std::vector<PadBind> parse_pad_binds(const std::string& text, std::string& error);

// --pad-layout: buttons that tap a spot of the screen on one screen, in place of what they'd do
// there, for a button rfu doesn't know (a fan mod's own, or one an update moved). A file of lines
//   <screen> <button> tap <x>% <y>%
// with the screen as above or "any" (all of them), x and y from the top left; # starts a comment.
// Later lines (and files) win over earlier ones for the same screen and button.
struct PadTap {
    PadScreen screen;
    platform::PadButton button;
    float x, y;  // fractions of the screen
};
// The taps, and an error for each line that isn't one (with its number).
std::vector<PadTap> parse_pad_layout(const std::string& text, std::vector<std::string>& errors);

// --map-buttons: where the world map's buttons are, for a layout rfu doesn't know, as
// "settings=137:38,almanac=222:38,zengarden=395:38,shop=-75:53". x and y are pixels of the screen
// scaled to 768 high, from its top left; a negative x counts from the right edge. A button left
// out keeps the position rfu knows for the game's layout. Empty, with error set, when the text
// isn't positions.
struct MapButtons {
    std::optional<std::pair<float, float>> settings, almanac, zen_garden, shop;
};
MapButtons parse_map_buttons(const std::string& text, std::string& error);

struct GameKeyOptions {
    std::vector<platform::KeyBind> hint_binds;  // the binds whose keys are shown on what they press (--key-hints)
    bool controller = false;                    // the pad's controls on the lawn (--controller)
    bool pad_hints = false;                     // and its buttons shown on what they press
    std::vector<PadBind> pad_binds;             // the pad's buttons, rebound (--pad-binds)
    std::vector<PadTap> pad_taps;               // and tapping spots of the screen (--pad-layout)
    MapButtons map_buttons;                     // the world map's buttons, moved (--map-buttons)
};

struct GameKeyHandlers {
    std::function<void(const platform::GameKeyInput&)> key;  // GameKeyInput
    std::function<bool(const platform::PadInput&)> pad;      // PadInput; true when it's Android's back
    std::function<void()> frame;                             // once a frame, between frames
    std::function<void()> mouse;                             // the mouse (or a finger) was used
    std::function<platform::HintOverlay()> overlay;          // drawn over the frame on screen
};

// The handlers, called on the thread that runs the game's frames; empty when this libPVZ2.so is a
// build it doesn't know.
GameKeyHandlers install_game_keys(Runtime& rt, const Module& pvz2, const GameKeyOptions& options = {});

}  // namespace rfu
