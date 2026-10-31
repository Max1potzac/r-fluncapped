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
// cursor is a box drawn with the hints.

#include "platform/window.h"

#include <functional>
#include <vector>

namespace rfu {

class Runtime;
class Module;

struct GameKeyOptions {
    std::vector<platform::KeyBind> hint_binds;  // the binds whose keys are shown on what they press (--key-hints)
    bool controller = false;                    // the pad's controls on the lawn (--controller)
    bool pad_hints = false;                     // and its buttons shown on what they press
};

struct GameKeyHandlers {
    std::function<void(const platform::GameKeyInput&)> key;  // GameKeyInput
    std::function<void(const platform::PadInput&)> pad;      // PadInput
    std::function<void()> frame;                             // once a frame, between frames
    std::function<platform::HintOverlay()> overlay;          // drawn over the frame on screen
};

// The handlers, called on the thread that runs the game's frames; empty when this libPVZ2.so is a
// build it doesn't know.
GameKeyHandlers install_game_keys(Runtime& rt, const Module& pvz2, const GameKeyOptions& options = {});

}  // namespace rfu
