#pragma once

#include "core/guest_heap.h"
#include "core/guest_memory.h"
#include "core/linker.h"
#include "core/vfs.h"
#include "cpu/cpu.h"
#include "shims/registry.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace Dynarmic {
class ExclusiveMonitor;
}

namespace rfu {

// One entry of the thunk table: guest code calling thunk n ends up in fn.
struct Thunk {
    std::string name;
    ShimFn fn;
    std::atomic<std::uint64_t> calls{0};
};

struct RuntimeOptions {
    std::filesystem::path lib_dir;          // where libPVZ2.so and friends live
    std::string package = "com.ea.game.pvz2_row";
};

// Owns everything the guest sees: memory, heap, loaded modules, threads and shims.
class Runtime {
public:
    static constexpr std::uint32_t kMaxThunks = 0xFFF0;
    static constexpr std::uint32_t kMaxProcessors = 64;

    explicit Runtime(RuntimeOptions options);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    const RuntimeOptions& options() const { return options_; }
    GuestMemory& mem() { return mem_; }
    GuestHeap& heap() { return heap_; }
    Linker& linker() { return linker_; }
    Vfs& vfs() { return vfs_; }
    const ShimRegistry& shims() const { return shims_; }
    Dynarmic::ExclusiveMonitor& monitor() { return *monitor_; }

    // --- threads -----------------------------------------------------------------------
    // The guest thread for the calling host thread, created on first use.
    GuestThread& current_thread();
    // Creates a guest thread (stack, TLS, CPU) without binding it to a host thread yet.
    std::shared_ptr<GuestThread> create_thread(std::uint64_t stack_size = GuestMemory::kStackSize);
    // Binds `t` to the calling host thread (used by pthread_create's host thread).
    void bind_thread(std::shared_ptr<GuestThread> t);
    void unbind_thread();
    GuestThread* thread_by_handle(GuestAddr handle);

    // Calls a guest function on the calling thread.
    std::uint64_t call(GuestAddr fn, const std::vector<std::uint64_t>& args = {});

    // --- thunks ------------------------------------------------------------------------
    GuestAddr return_thunk() const { return GuestMemory::kThunkBase; }
    GuestAddr make_thunk(std::string name, ShimFn fn);
    // Address a guest import named `name` should bind to: a shim's thunk or data block, or
    // nullopt when no shim exists.
    std::optional<GuestAddr> resolve_shim(const std::string& name);
    GuestAddr missing_thunk(const std::string& name);
    void dispatch(std::uint32_t index, Cpu& cpu);
    const Thunk* thunk(std::uint32_t index) const;
    std::uint32_t thunk_count() const { return thunk_count_.load(); }
    bool is_thunk(GuestAddr a) const { return a >= GuestMemory::kThunkBase && a < GuestMemory::kThunkBase + 8ull * (kMaxThunks + 1); }

    // Drops translated code for [addr, addr + size) on every thread (code was patched).
    void invalidate_code(GuestAddr addr, std::uint64_t size);

    // --- process lifetime ----------------------------------------------------------------
    // Runs the initializers of every module loaded since the last call, dependencies first.
    void run_initializers();
    void add_atexit(GuestAddr fn, GuestAddr arg, GuestAddr dso);
    void run_atexit(GuestAddr dso);  // dso 0 = everything
    [[noreturn]] void exit(int code);

    void set_errno(int err);

    // Per-subsystem state for shims (open files, mutex tables...), created on first use.
    template <typename T>
    T& state() {
        std::lock_guard lock(state_mutex_);
        auto& slot = state_[std::type_index(typeid(T))];
        if (!slot) slot = std::make_shared<T>();
        return *static_cast<T*>(slot.get());
    }

    // Text of /proc/self/maps as the guest should see it.
    std::string proc_maps() const;

private:
    RuntimeOptions options_;
    GuestMemory mem_;
    GuestHeap heap_;
    Vfs vfs_;
    ShimRegistry shims_;
    Linker linker_;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> monitor_;

    std::unique_ptr<Thunk[]> thunks_;  // kMaxThunks entries, filled up to thunk_count_
    std::atomic<std::uint32_t> thunk_count_{0};
    std::unordered_map<std::string, GuestAddr> bound_;  // import name -> thunk or data address
    std::mutex thunk_mutex_;

    std::mutex thread_mutex_;
    std::vector<std::shared_ptr<GuestThread>> threads_;
    std::uint32_t next_thread_id_ = 1;
    GuestAddr next_stack_ = GuestMemory::kStackBase;

    std::size_t initialized_modules_ = 0;

    struct AtExit {
        GuestAddr fn, arg, dso;
    };
    std::mutex atexit_mutex_;
    std::vector<AtExit> atexit_;

    std::mutex state_mutex_;
    std::unordered_map<std::type_index, std::shared_ptr<void>> state_;
};

}  // namespace rfu
