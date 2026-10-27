#pragma once

// Frozen plant animations: some plants stop on the last frame of an animation when the level
// isn't running, before the first wave of a Last Stand level and after the last zombie dies.
// A Citron that finishes charging stays on its last charging frame instead of going on to its
// charged idle. (A Chomper or Toadstool that eats the last zombie stops mid-chew too; their
// state machines target zombies, and aren't covered until they can be tested.)
//
// Those switches happen in the plant's own update (its PlantFramework's special update, which
// moves the plant from one animation state to the next once an animation ends), and the game
// skips every plant's update while the board isn't running. Plants still animate and still
// get their per-frame call then; a hook on it runs just the special update for the plants that
// need it, when the game skipped the rest, with no zombie in reach as far as it can tell.
//
// Like hover to collect, the addresses are those of the libPVZ2 build Reflourished 1.4.2 and
// AltverZ 1.8.3 share, checked first; on another build the fix stays off.

namespace rfu {

class Runtime;
class Module;

bool install_animation_fix(Runtime& rt, const Module& pvz2);

}  // namespace rfu
