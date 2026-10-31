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

#include "platform/window.h"

#include <functional>
#include <vector>

namespace rfu {

class Runtime;
class Module;

struct GameKeyHandlers {
    std::function<void(const platform::GameKeyInput&)> key;  // GameKeyInput
    std::function<std::vector<platform::HintLabel>()> hints;  // the keybind hints for the frame on screen
};

// The handlers, called on the thread that runs the game's frames; empty when this libPVZ2.so is a
// build it doesn't know. hints is set when hint_binds isn't empty: the binds whose keys it shows.
GameKeyHandlers install_game_keys(Runtime& rt, const Module& pvz2, const std::vector<platform::KeyBind>& hint_binds = {});

}  // namespace rfu
