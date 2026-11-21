#pragma once

// Flag zombies in the level preview: before a level starts, the zombies waiting on the street show
// what the level holds. With this on, the level's flag zombie (its stage's FlagZombieTypeName:
// mummy_flag in Egypt, pirate_flag on the Pirate Seas...) stands a little ahead of them, once for
// each flag the level has, counted the way the game's flag meter counts them (every
// FlagWaveInterval waves, the last wave and any flag waves listed). Nothing changes in the level:
// they leave with the rest of the street when it starts. A level that suppresses its flag zombie
// (the wave manager's SuppressFlagZombie) shows none.
//
// The street is laid out by the intro's PlaceStreetZombies, which fills a grid of spots (9 across,
// 10 down, some blocked by the scenery) with the level's zombie types; a hook at its end places
// the flag zombies with the game's own call for one street zombie, three spots to the left of the
// leftmost one, at most five to a column.
//
// Like hover to collect, the addresses are those of the libPVZ2 build Reflourished 1.4.2 and
// AltverZ 1.8.3 share, checked first; on another build it stays off.

namespace rfu {

class Runtime;
class Module;

bool install_preview_flags(Runtime& rt, const Module& pvz2);

// The rows (0 to 9) of `count` flag zombies in one column, spread evenly. Exposed for the tests.
int preview_flag_row(int index, int count);

}  // namespace rfu
