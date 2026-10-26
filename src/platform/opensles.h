#pragma once

// OpenSL ES for the guest: the engine, an output mix, and PCM buffer-queue audio players
// played through SDL audio. That is the part of OpenSL ES the game's sound engine (Wwise)
// uses on Android.

namespace rfu {
class ShimRegistry;
}

namespace rfu::platform {

void register_opensles(ShimRegistry& reg);

}  // namespace rfu::platform
