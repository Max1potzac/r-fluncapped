#include "core/linker.h"

#include "core/log.h"
#include "runtime.h"

#include <deque>
#include <functional>

namespace rfu {

namespace {
constexpr std::uint64_t kModuleAlign = 0x10'0000;
constexpr std::uint8_t kSttObject = 1;
}  // namespace

Module* Linker::find_module(const std::string& name) const {
    std::string base = std::filesystem::path(name).filename().string();
    for (const auto& m : modules_)
        if (m->name() == base || m->soname() == base) return m.get();
    return nullptr;
}

const Module* Linker::module_at(GuestAddr addr) const {
    for (const auto& m : modules_)
        if (m->contains(addr)) return m.get();
    return nullptr;
}

GuestAddr Linker::next_base() const {
    GuestAddr end = GuestMemory::kModuleBase;
    for (const auto& m : modules_) end = std::max(end, m->end() + kModuleAlign);
    return (end + kModuleAlign - 1) & ~(kModuleAlign - 1);
}

Module& Linker::load_one(const std::filesystem::path& path, std::vector<Module*>& fresh) {
    if (Module* m = find_module(path.filename().string())) return *m;
    auto m = std::make_unique<Module>(Module::load(rt_.mem(), path, next_base()));
    Module& ref = *m;
    modules_.push_back(std::move(m));
    fresh.push_back(&ref);
    return ref;
}

Module& Linker::load(const std::filesystem::path& path) {
    std::vector<Module*> fresh;
    Module& root = load_one(path, fresh);

    // Breadth-first over DT_NEEDED, like bionic's local group. Libraries that aren't next to
    // the root (libc.so, libGLESv2.so...) are system libraries: the shims stand in for them.
    std::vector<Module*> scope{&root};
    for (std::size_t i = 0; i < scope.size(); ++i) {
        for (const auto& name : scope[i]->needed()) {
            Module* dep = find_module(name);
            if (!dep) {
                auto candidate = path.parent_path() / name;
                if (!std::filesystem::exists(candidate)) continue;
                dep = &load_one(candidate, fresh);
            }
            if (std::find(scope.begin(), scope.end(), dep) == scope.end()) scope.push_back(dep);
        }
    }

    for (Module* m : fresh) {
        relocate_with(*m, scope);
    }

    // Initializers run dependencies first.
    std::vector<Module*> visited;
    std::function<void(Module*)> visit = [&](Module* m) {
        if (std::find(visited.begin(), visited.end(), m) != visited.end()) return;
        visited.push_back(m);
        for (const auto& name : m->needed())
            if (Module* dep = find_module(name)) visit(dep);
        if (std::find(fresh.begin(), fresh.end(), m) != fresh.end()) init_order_.push_back(m);
    };
    visit(&root);
    return root;
}

GuestAddr Linker::resolve_import(Module& m, const Module::Symbol& sym, const std::vector<Module*>& scope) {
    for (Module* candidate : scope)
        if (auto a = candidate->find_export(sym.name)) return *a;
    if (auto a = rt_.resolve_shim(sym.name)) return *a;
    if (sym.bind == elf::kStbWeak) return 0;

    missing_.insert(sym.name);
    if (sym.type == kSttObject) {
        log::warn("{}: imported data {} has no shim; giving it a zeroed block", m.name(), sym.name);
        GuestAddr block = rt_.heap().alloc_zeroed(std::max<std::uint64_t>(sym.size, 256));
        return block;
    }
    return rt_.missing_thunk(sym.name);
}

void Linker::relocate_with(Module& m, const std::vector<Module*>& scope) {
    auto& mem = rt_.mem();
    std::unordered_map<std::uint32_t, GuestAddr> resolved;
    auto symbol_value = [&](std::uint32_t index) -> GuestAddr {
        if (index == 0) return 0;
        if (auto it = resolved.find(index); it != resolved.end()) return it->second;
        const auto& sym = m.symbols().at(index);
        GuestAddr value = (sym.defined && sym.bind == elf::kStbLocal) ? sym.value : resolve_import(m, sym, scope);
        resolved.emplace(index, value);
        return value;
    };

    std::size_t unknown = 0;
    for (const auto& r : m.relocations()) {
        GuestAddr where = m.base() + r.offset;
        switch (elf::rela_type(r.info)) {
        case elf::kRelRelative:
            mem.write<std::uint64_t>(where, m.base() + static_cast<std::uint64_t>(r.addend));
            break;
        case elf::kRelAbs64:
        case elf::kRelGlobDat:
        case elf::kRelJumpSlot:
            mem.write<std::uint64_t>(where, symbol_value(elf::rela_sym(r.info)) + static_cast<std::uint64_t>(r.addend));
            break;
        case 0:
            break;
        default:
            ++unknown;
            break;
        }
    }
    if (unknown) log::warn("{}: {} relocations of unsupported types were skipped", m.name(), unknown);
}

std::optional<GuestAddr> Linker::find_symbol(const std::string& name) {
    for (const auto& m : modules_)
        if (auto a = m->find_export(name)) return a;
    return rt_.resolve_shim(name);
}

}  // namespace rfu
