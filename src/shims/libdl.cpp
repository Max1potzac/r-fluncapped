// <dlfcn.h> and dl_iterate_phdr. Handles are small guest blocks naming a loaded module or a
// system library (whose symbols are the shims).

#include "core/log.h"
#include "shims/common.h"

#include <unordered_map>

namespace rfu {

namespace {

struct Handles {
    std::mutex mutex;
    std::unordered_map<GuestAddr, Module*> modules;   // nullptr = a system library
    std::unordered_map<std::string, GuestAddr> by_name;
    std::string error;
};

constexpr GuestAddr kRtldDefault = 0, kRtldNext = ~0ull;

void set_error(CallContext& c, std::string msg) {
    log::debug("dl error: {}", msg);
    auto& h = c.rt().state<Handles>();
    std::lock_guard lock(h.mutex);
    h.error = std::move(msg);
}

GuestAddr handle_for(Runtime& rt, const std::string& key, Module* m) {
    auto& h = rt.state<Handles>();
    std::lock_guard lock(h.mutex);
    auto [it, inserted] = h.by_name.emplace(key, 0);
    if (inserted) {
        it->second = rt.heap().alloc_zeroed(16);
        h.modules[it->second] = m;
    }
    return it->second;
}

}  // namespace

void register_libdl(ShimRegistry& reg) {
    reg.add("dlopen", [](CallContext& c) {
        if (!c.x(0)) return c.ret(handle_for(c.rt(), "", nullptr));
        std::string name = c.str(0);
        std::string base = std::filesystem::path(name).filename().string();
        Module* m = c.rt().linker().find_module(base);
        if (!m) {
            auto path = c.rt().options().lib_dir / base;
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                log::info("dlopen({}): loading {}", name, path_to_utf8(path));
                m = &c.rt().linker().load(path);
                c.rt().run_initializers();
            }
        }
        if (m) return c.ret(handle_for(c.rt(), m->name(), m));
        // Android system libraries: their functions are the shims.
        static const char* system_libs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libandroid.so",
                                            "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libOpenSLES.so"};
        for (const char* lib : system_libs)
            if (base == lib) return c.ret(handle_for(c.rt(), base, nullptr));
        set_error(c, "dlopen failed: library \"" + name + "\" not found");
        c.ret(0);
    });
    reg.add("dlsym", [](CallContext& c) {
        GuestAddr handle = c.x(0);
        std::string name = c.str(1);
        std::optional<GuestAddr> addr;
        Module* m = nullptr;
        bool known = handle == kRtldDefault || handle == kRtldNext;
        if (!known) {
            auto& h = c.rt().state<Handles>();
            std::lock_guard lock(h.mutex);
            auto it = h.modules.find(handle);
            known = it != h.modules.end();
            if (known) m = it->second;
        }
        if (!known) {
            set_error(c, "dlsym: invalid handle");
            return c.ret(0);
        }
        if (m)
            addr = m->find_export(name);
        else
            addr = c.rt().linker().find_symbol(name);
        if (!addr) {
            set_error(c, "undefined symbol: " + name);
            return c.ret(0);
        }
        c.ret(*addr);
    });
    // dladdr(addr, Dl_info*): which module an address is in. Mods use it to find their own
    // library file, and with it the files they keep next to it.
    reg.add("dladdr", [](CallContext& c) {
        GuestAddr addr = c.x(0), info = c.x(1);
        for (const auto& m : c.rt().linker().modules()) {
            if (addr < m->base() || addr >= m->end()) continue;
            // struct Dl_info { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; }
            std::string path = fmt::format("/data/app/{}-1/lib/arm64/{}", c.rt().options().package, m->name());
            c.mem().write<GuestAddr>(info, intern_string(c.rt(), path));
            c.mem().write<GuestAddr>(info + 8, m->base());
            c.mem().write<GuestAddr>(info + 16, 0);
            c.mem().write<GuestAddr>(info + 24, 0);
            return c.ret(1);
        }
        c.ret(0);
    });
    reg.add("dlclose", [](CallContext& c) { c.ret(0); });
    reg.add("dlerror", [](CallContext& c) {
        auto& h = c.rt().state<Handles>();
        std::string msg;
        {
            std::lock_guard lock(h.mutex);
            msg.swap(h.error);
        }
        c.ret(msg.empty() ? 0 : intern_string(c.rt(), msg));
    });

    // dl_iterate_phdr(callback, data). The unwinder in libc++_shared finds .eh_frame_hdr
    // this way, so C++ exceptions in guest code depend on it.
    reg.add("dl_iterate_phdr", [](CallContext& c) {
        // struct dl_phdr_info { addr; name; phdr; u16 phnum; u64 adds; u64 subs; size_t tls_modid; void* tls_data; }
        constexpr std::uint64_t kInfoSize = 64;
        GuestAddr cb = c.x(0), data = c.x(1);
        std::vector<Module*> modules;
        for (const auto& m : c.rt().linker().modules()) modules.push_back(m.get());
        GuestAddr info = c.rt().heap().alloc_zeroed(kInfoSize);
        std::uint64_t result = 0;
        for (Module* m : modules) {
            std::memset(c.mem().host(info), 0, kInfoSize);
            c.mem().write<GuestAddr>(info, m->base());
            c.mem().write<GuestAddr>(info + 8, intern_string(c.rt(), m->name()));
            c.mem().write<GuestAddr>(info + 16, m->phdr_addr());
            c.mem().write<std::uint16_t>(info + 24, m->phdr_count());
            c.mem().write<std::uint64_t>(info + 32, modules.size());
            result = c.cpu().call(cb, {info, kInfoSize, data});
            if (static_cast<std::int32_t>(result)) break;
        }
        c.rt().heap().free(info);
        c.ret(result & 0xFFFFFFFF);
    });
}

}  // namespace rfu
