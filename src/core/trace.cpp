#include "core/trace.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "core/rtti.h"
#include "runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>

namespace rfu {

namespace {

std::atomic<bool> g_tracing{false};

struct Traced {
    std::string names;  // "CollectableSun#49" (a function shared by classes or slots lists each)
    std::vector<GuestAddr> vtables;  // calls count when `this` is an object of these classes
    std::atomic<std::uint64_t> calls{0};
};

std::map<GuestAddr, std::shared_ptr<Traced>>& traced() {
    static std::map<GuestAddr, std::shared_ptr<Traced>> m;
    return m;
}

double ms_since_start() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool worth_logging(std::uint64_t n) {
    if (n <= 3) return true;
    while (n % 10 == 0) n /= 10;
    return n == 1;
}

}  // namespace

bool tracing() { return g_tracing.load(std::memory_order_relaxed); }

bool trace_class(Runtime& rt, const Module& module, const std::string& class_name) {
    auto vt = find_vtable(rt.mem(), module, class_name);
    if (!vt) {
        log::warn("trace: no class {} in {}", class_name, module.name());
        return false;
    }
    ms_since_start();
    g_tracing = true;
    log::info("trace: {} vtable at {}, {} functions", class_name, rt.describe(vt->address), vt->functions.size());
    int hooked = 0;
    for (std::size_t slot = 0; slot < vt->functions.size(); ++slot) {
        GuestAddr fn = vt->functions[slot];
        std::string name = fmt::format("{}#{}", class_name, slot);
        auto& entry = traced()[fn];
        if (entry) {
            entry->names += " " + name;
            entry->vtables.push_back(vt->address);  // set before the game runs, so unlocked
            continue;
        }
        entry = std::make_shared<Traced>();
        entry->names = name;
        entry->vtables.push_back(vt->address);
        try {
            hook(rt, fn, "trace " + name, [&rt, fn, t = entry](CallContext& c) {
                // Functions shared with other classes (the base class's) run for their objects too.
                if (!c.mem().is_committed(c.x(0), 8)) return;
                GuestAddr vptr = c.mem().read<std::uint64_t>(c.x(0));
                if (std::find(t->vtables.begin(), t->vtables.end(), vptr) == t->vtables.end()) return;
                std::uint64_t n = ++t->calls;
                if (!worth_logging(n)) return;
                log::info("trace {:.0f}ms [t{}] {} ({}) call {}: this={:#x} x1={:#x} x2={:#x} from {}", ms_since_start(), c.thread().id,
                          t->names, rt.describe(fn), n, c.x(0), c.x(1), c.x(2), rt.describe(c.x(30)));
            });
            ++hooked;
        } catch (const std::exception& e) {
            log::info("trace: {} not traced: {}", name, e.what());
        }
    }
    log::info("trace: {} functions of {} traced", hooked, class_name);
    return true;
}

}  // namespace rfu
