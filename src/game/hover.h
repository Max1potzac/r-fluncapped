#pragma once

// Hover to collect: sun, coins and plant food are picked up when the mouse passes over them,
// no click needed (as in PvZ2 Gardendless).
//
// On a phone a finger sliding over a sun collects it: the lawn (the game's Board widget) hands
// every touch move to the collectables under it. With hover on, the window sends the mouse's
// moves with no button down as moves of their own pointer (platform::kHoverPointer), but the
// game's WidgetManager drops moves of a touch that never went down. The hook here passes those
// to the lawn under the cursor the way the WidgetManager passes a sliding finger's.
//
// The addresses are those of the libPVZ2 build Reflourished 1.4.2 and AltverZ 1.8.3 share; the
// code at each is checked first, and on another build hover stays off.

namespace rfu {

class Runtime;
class Module;

bool install_hover_collect(Runtime& rt, const Module& pvz2);

}  // namespace rfu
