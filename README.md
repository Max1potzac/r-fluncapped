# r-fluncapped

A native Windows runtime for Plants vs. Zombies 2 Reflourished.

The plan: load the game's own Android libraries (`libPVZ2.so`, `libRFLib.so`) on the PC through an
ARM64 recompiler and a shim layer, so the game is fully playable early, then replace its code with
native C++ one function at a time.

This repository contains only original code. It ships no game code or assets: the runtime loads
them from your own copy of the game.
