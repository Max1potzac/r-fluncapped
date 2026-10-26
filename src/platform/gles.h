#pragma once

// OpenGL ES 2.0 for the guest: each GL function libPVZ2 imports is a shim that calls the
// host's GLES implementation (the driver's ES profile, Mesa, or ANGLE on Windows) with guest
// pointers translated to host ones.

namespace rfu {
class ShimRegistry;
}

namespace rfu::platform {

void register_gles(ShimRegistry& reg);

// Loads the host's GL entry points. Call once, with the game's context current on the
// calling thread, before the guest makes any GL call.
void load_gles(void* (*get_proc)(const char* name));

}  // namespace rfu::platform
