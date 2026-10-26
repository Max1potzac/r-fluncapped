#pragma once

#include "core/module.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace rfu {

class Runtime;

// Loads Android shared libraries into guest memory and binds their imports, the way
// bionic's linker would: each module's undefined symbols are looked up breadth-first through
// its own dependency tree, and whatever no module defines is bound to a host shim.
class Linker {
public:
    explicit Linker(Runtime& rt) : rt_(rt) {}

    // Loads `path` and, recursively, its DT_NEEDED libraries that exist in `search_dir`
    // (system libraries such as libc.so are provided by shims instead). Relocates everything
    // newly loaded. Returns the module for `path`.
    Module& load(const std::filesystem::path& path);

    const std::vector<std::unique_ptr<Module>>& modules() const { return modules_; }
    Module* find_module(const std::string& name) const;  // by file name or soname
    const Module* module_at(GuestAddr addr) const;

    // Global lookup through every loaded module in load order, then the shims.
    std::optional<GuestAddr> find_symbol(const std::string& name);

    // Names of imports that resolved to the "unimplemented" stub, by module.
    const std::set<std::string>& missing() const { return missing_; }

    // Modules in the order their initializers must run (dependencies first).
    std::vector<Module*> init_order() const { return init_order_; }

private:
    Module& load_one(const std::filesystem::path& path, std::vector<Module*>& fresh);
    void relocate_with(Module& m, const std::vector<Module*>& scope);
    GuestAddr resolve_import(Module& m, const Module::Symbol& sym, const std::vector<Module*>& scope);
    GuestAddr next_base() const;

    Runtime& rt_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::vector<Module*> init_order_;
    std::set<std::string> missing_;
};

}  // namespace rfu
