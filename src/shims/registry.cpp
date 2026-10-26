#include "shims/registry.h"

namespace rfu {

const ShimFn* ShimRegistry::function(const std::string& name) const {
    auto it = functions_.find(name);
    return it == functions_.end() ? nullptr : &it->second;
}

const DataShim* ShimRegistry::data(const std::string& name) const {
    auto it = data_.find(name);
    return it == data_.end() ? nullptr : &it->second;
}

void register_all_shims(ShimRegistry& reg) {
    register_libc_memory(reg);
    register_libc_string(reg);
    register_libc_stdio(reg);
    register_libc_math(reg);
    register_libc_misc(reg);
    register_libc_net(reg);
    register_libc_pthread(reg);
    register_libdl(reg);
    register_libz(reg);
}

}  // namespace rfu
