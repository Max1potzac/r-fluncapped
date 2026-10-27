#pragma once

// Finding a C++ class's virtual function table by its RTTI name, in a loaded library whose
// code has no symbols (libPVZ2's game classes): the name string ("14CollectableSun"), the
// type_info that points at it, and the vtable whose type_info slot points at that.

#include "core/guest_memory.h"

#include <optional>
#include <string>
#include <vector>

namespace rfu {

class GuestMemory;
class Module;

struct VTable {
    GuestAddr address = 0;              // the first virtual function's slot
    std::vector<GuestAddr> functions;   // slot by slot, up to the first non-code entry
};

std::optional<VTable> find_vtable(const GuestMemory& mem, const Module& module, const std::string& class_name);

}  // namespace rfu
