#pragma once

#include "core/guest_memory.h"

#include <string>
#include <unordered_map>

namespace rfu {

class CallContext;
class Runtime;

// A host implementation of an imported guest function. Arguments and the return value
// travel through the CallContext.
using ShimFn = void (*)(CallContext&);

// An imported data symbol (stdin/stdout, SL_IID_ENGINE...): `size` bytes of guest memory
// that `init` fills in once, when the symbol is first resolved.
struct DataShim {
    std::uint64_t size;
    void (*init)(Runtime& rt, GuestAddr addr);
};

class ShimRegistry {
public:
    void add(const std::string& name, ShimFn fn) { functions_[name] = fn; }
    void add_data(const std::string& name, DataShim shim) { data_[name] = shim; }

    const ShimFn* function(const std::string& name) const;
    const DataShim* data(const std::string& name) const;
    std::size_t size() const { return functions_.size() + data_.size(); }

private:
    std::unordered_map<std::string, ShimFn> functions_;
    std::unordered_map<std::string, DataShim> data_;
};

// Registers every shim the runtime implements. Each register_* lives with its shims.
void register_all_shims(ShimRegistry& reg);

void register_libc_memory(ShimRegistry& reg);
void register_libc_string(ShimRegistry& reg);
void register_libc_stdio(ShimRegistry& reg);
void register_libc_math(ShimRegistry& reg);
void register_libc_misc(ShimRegistry& reg);
void register_libc_net(ShimRegistry& reg);
void register_libc_pthread(ShimRegistry& reg);
void register_libdl(ShimRegistry& reg);
void register_libz(ShimRegistry& reg);

}  // namespace rfu
