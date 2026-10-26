#include "runtime.h"

#include "core/log.h"

#include <algorithm>
#include <dynarmic/interface/exclusive_monitor.h>

#include <random>

namespace rfu {

namespace {

constexpr std::uint32_t kSvcRet = 0xD65F03C0;  // ret
constexpr std::uint32_t kBrk = 0xD4200000;     // brk #0

constexpr std::uint32_t svc(std::uint32_t imm) { return 0xD4000001 | (imm << 5); }

GuestAddr thunk_addr(std::uint32_t index) { return GuestMemory::kThunkBase + 8ull * (index + 1); }

// Called for imports nothing implements: say so once, then behave like a function that
// returns 0, which is what most of the game's optional calls tolerate.
void missing_import(CallContext& c) {
    if (c.thunk().calls.load() == 1) log::warn("unimplemented import {} called (from {:#x})", c.thunk().name, c.x(30));
    c.ret(0);
}

struct CurrentThread {
    Runtime* rt = nullptr;
    std::shared_ptr<GuestThread> thread;
};
thread_local CurrentThread tl_current;

}  // namespace

Runtime::Runtime(RuntimeOptions options)
    : options_(std::move(options)),
      heap_(mem_),
      linker_(*this),
      monitor_(std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxProcessors)),
      thunks_(std::make_unique<Thunk[]>(kMaxThunks)) {
    // Thunk page: slot 0 is where guest functions called by the host return to.
    mem_.commit(GuestMemory::kThunkBase, 8ull * (kMaxThunks + 1));
    mem_.write(GuestMemory::kThunkBase, svc(Cpu::kReturnSvc));
    mem_.write(GuestMemory::kThunkBase + 4, kBrk);

    register_all_shims(shims_);
    if (options_.extra_shims) options_.extra_shims(shims_);
    vfs_.add_virtual("/proc/self/maps", [this] { return proc_maps(); });
    vfs_.add_virtual("/proc/self/cmdline", [this] { return options_.package + std::string(1, '\0'); });
    vfs_.add_virtual("/proc/self/status", [this] {
        return fmt::format("Name:\t{}\nState:\tR (running)\nTgid:\t4242\nPid:\t4242\nPPid:\t1\nTracerPid:\t0\n"
                           "VmSize:\t{} kB\nVmRSS:\t{} kB\nThreads:\t1\n",
                           options_.package.substr(0, 15), GuestMemory::kHeapSize >> 10, heap_.bytes_in_use() >> 10);
    });
    vfs_.add_virtual("/proc/self/statm", [this] {
        std::uint64_t pages = heap_.bytes_in_use() / GuestMemory::kPageSize;
        return fmt::format("{} {} 0 0 0 {} 0\n", GuestMemory::kHeapSize / GuestMemory::kPageSize, pages, pages);
    });
    // Where Android installs an app's libraries (the path /proc/self/maps and dladdr report):
    // mods read files they keep next to their code from there.
    if (!options_.lib_dir.empty()) vfs_.mount(fmt::format("/data/app/{}-1/lib/arm64", options_.package), options_.lib_dir);
}

Runtime::~Runtime() {
    if (tl_current.rt == this) tl_current = {};
}

// --- threads -------------------------------------------------------------------------------

std::shared_ptr<GuestThread> Runtime::create_thread(std::uint64_t stack_size) {
    stack_size = (std::max<std::uint64_t>(stack_size, 64 * 1024) + 0xFFFF) & ~0xFFFFull;
    auto t = std::make_shared<GuestThread>();
    {
        std::lock_guard lock(thread_mutex_);
        t->id = next_thread_id_++;
        // An uncommitted guard page below every stack catches overflows.
        t->stack_lo = next_stack_ + GuestMemory::kPageSize;
        t->stack_hi = t->stack_lo + stack_size;
        next_stack_ = t->stack_hi;
        if (next_stack_ >= GuestMemory::kThunkBase) throw std::runtime_error("out of guest stack space");
        threads_.push_back(t);
    }
    mem_.commit(t->stack_lo, stack_size);

    // bionic keeps per-thread slots at TPIDR_EL0: [-1] bionic_tls, [0] DTV, [1] thread id,
    // ..., [5] the stack protector cookie (read by every function compiled with
    // -fstack-protector as [tpidr_el0 + 0x28]).
    GuestAddr block = heap_.alloc_zeroed(0x200);
    t->tls = block + 0x40;
    t->tpidr_el0 = t->tls;
    t->errno_addr = block + 0x100;
    t->handle = heap_.alloc_zeroed(0x100);
    mem_.write<std::uint32_t>(t->handle + 0x10, t->id);  // pthread_internal_t::tid
    mem_.write(t->tls + 8, t->handle);
    static std::mt19937_64 rng{std::random_device{}()};
    mem_.write<std::uint64_t>(t->tls + 0x28, rng() & ~0xFFull);

    t->cpu = std::make_unique<Cpu>(*this, *t, (t->id - 1) % kMaxProcessors);
    return t;
}

void Runtime::bind_thread(std::shared_ptr<GuestThread> t) { tl_current = {this, std::move(t)}; }

void Runtime::unbind_thread() {
    if (tl_current.rt != this) return;
    std::lock_guard lock(thread_mutex_);
    std::erase(threads_, tl_current.thread);
    tl_current = {};
}

GuestThread& Runtime::current_thread() {
    if (tl_current.rt != this || !tl_current.thread) bind_thread(create_thread());
    return *tl_current.thread;
}

GuestThread* Runtime::thread_by_handle(GuestAddr handle) {
    std::lock_guard lock(thread_mutex_);
    for (auto& t : threads_)
        if (t->handle == handle) return t.get();
    return nullptr;
}

std::uint64_t Runtime::call(GuestAddr fn, const std::vector<std::uint64_t>& args) {
    return current_thread().cpu->call(fn, args);
}

void Runtime::invalidate_code(GuestAddr addr, std::uint64_t size) {
    std::lock_guard lock(thread_mutex_);
    for (auto& t : threads_)
        if (t->cpu) t->cpu->invalidate(addr, size);
}

// --- thunks --------------------------------------------------------------------------------

GuestAddr Runtime::make_thunk(std::string name, ShimFn fn) {
    std::uint32_t index = thunk_count_.load();
    if (index >= kMaxThunks) throw std::runtime_error("out of import thunks");
    thunks_[index].name = std::move(name);
    thunks_[index].fn = fn;
    GuestAddr addr = thunk_addr(index);
    mem_.write(addr, svc(index));
    mem_.write(addr + 4, kSvcRet);
    thunk_count_.store(index + 1);
    return addr;
}

std::optional<GuestAddr> Runtime::resolve_shim(const std::string& name) {
    std::lock_guard lock(thunk_mutex_);
    if (auto it = bound_.find(name); it != bound_.end()) return it->second;
    GuestAddr addr;
    if (auto fn = shims_.function(name)) {
        addr = make_thunk(name, *fn);
    } else if (auto data = shims_.data(name)) {
        addr = heap_.alloc_aligned(16, data->size);
        std::memset(mem_.host(addr), 0, data->size);
        data->init(*this, addr);
    } else {
        return std::nullopt;
    }
    bound_.emplace(name, addr);
    return addr;
}

GuestAddr Runtime::missing_thunk(const std::string& name) {
    std::lock_guard lock(thunk_mutex_);
    std::string key = "?" + name;
    if (auto it = bound_.find(key); it != bound_.end()) return it->second;
    GuestAddr addr = make_thunk(name, missing_import);
    bound_.emplace(key, addr);
    return addr;
}

GuestAddr Runtime::make_hook_thunk(std::string name, std::function<void(CallContext&)> fn) {
    std::lock_guard lock(thunk_mutex_);
    GuestAddr addr = make_thunk(std::move(name), nullptr);
    thunks_[(addr - thunk_addr(0)) / 8].hook = std::move(fn);
    return addr;
}

const Thunk* Runtime::thunk(std::uint32_t index) const {
    return index < thunk_count_.load() ? &thunks_[index] : nullptr;
}

void Runtime::dispatch(std::uint32_t index, Cpu& cpu) {
    const Thunk* t = thunk(index);
    if (!t) throw GuestFault(fmt::format("svc #{:#x} at {:#x} isn't an import thunk", index, cpu.jit().GetPC() - 4));
    const_cast<Thunk*>(t)->calls.fetch_add(1, std::memory_order_relaxed);
    CallContext ctx(cpu, *this, *t);
    if (log::enabled(log::Level::Trace))
        log::trace("[t{}] {}({:#x}, {:#x}, {:#x}) from {:#x}", cpu.thread().id, t->name, ctx.x(0), ctx.x(1), ctx.x(2), ctx.x(30));
    // Shims can call back into the guest and so nest; keep the outer import's state.
    GuestThread& thread = cpu.thread();
    const Thunk* outer = thread.in_import.load(std::memory_order_relaxed);
    GuestAddr arg = thread.import_arg, lr = thread.import_lr, fp = thread.import_fp;
    thread.import_arg = ctx.x(0);
    thread.import_lr = ctx.x(30);
    thread.import_fp = ctx.x(29);
    thread.in_import = t;
    struct Restore {
        GuestThread& thread;
        const Thunk* t;
        GuestAddr arg, lr, fp;
        ~Restore() {
            thread.in_import = t;
            thread.import_arg = arg;
            thread.import_lr = lr;
            thread.import_fp = fp;
        }
    } restore{thread, outer, arg, lr, fp};
    if (t->hook)
        t->hook(ctx);
    else
        t->fn(ctx);
}

std::string Runtime::describe(GuestAddr addr) const {
    if (const Module* m = linker_.module_at(addr)) return fmt::format("{}+{:#x}", m->name(), addr - m->base());
    return fmt::format("{:#x}", addr);
}

std::vector<std::string> Runtime::thread_report() {
    std::vector<std::shared_ptr<GuestThread>> threads;
    {
        std::lock_guard lock(thread_mutex_);
        threads = threads_;
    }
    std::vector<std::string> lines;
    for (const auto& t : threads) {
        const Thunk* in = t->in_import.load();
        if (!in) {
            lines.push_back(fmt::format("thread {}: running guest code", t->id));
            continue;
        }
        std::string line = fmt::format("thread {}: in {}({:#x}), called from {}", t->id, in->name, t->import_arg.load(),
                                       describe(t->import_lr));
        GuestAddr fp = t->import_fp;
        for (int depth = 0; depth < 12 && fp >= t->stack_lo && fp + 16 <= t->stack_hi && fp % 8 == 0; ++depth) {
            GuestAddr ret = mem_.read<std::uint64_t>(fp + 8);
            GuestAddr next = mem_.read<std::uint64_t>(fp);
            if (!ret) break;
            line += " <- " + describe(ret);
            if (next <= fp) break;
            fp = next;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

// --- process lifetime ----------------------------------------------------------------------

void Runtime::run_initializers() {
    auto order = linker_.init_order();
    while (initialized_modules_ < order.size()) {
        Module* m = order[initialized_modules_++];
        auto inits = m->initializers(mem_);
        log::info("running {} initializers of {}", inits.size(), m->name());
        for (GuestAddr f : inits) {
            log::debug("  {}+{:#x}", m->name(), f - m->base());
            try {
                call(f);
            } catch (const GuestFault& e) {
                throw GuestFault(fmt::format("{} (in initializer {}+{:#x})", e.what(), m->name(), f - m->base()));
            }
        }
    }
}

void Runtime::add_atexit(GuestAddr fn, GuestAddr arg, GuestAddr dso) {
    std::lock_guard lock(atexit_mutex_);
    atexit_.push_back({fn, arg, dso});
}

void Runtime::run_atexit(GuestAddr dso) {
    for (;;) {
        AtExit entry;
        {
            std::lock_guard lock(atexit_mutex_);
            auto it = std::find_if(atexit_.rbegin(), atexit_.rend(), [&](const AtExit& e) { return !dso || e.dso == dso; });
            if (it == atexit_.rend()) return;
            entry = *it;
            atexit_.erase(std::next(it).base());
        }
        call(entry.fn, {entry.arg});
    }
}

void Runtime::exit(int code) {
    log::info("guest called exit({})", code);
    run_atexit(0);
    throw GuestExit{code};
}

void Runtime::set_errno(int err) { mem_.write<std::int32_t>(current_thread().errno_addr, err); }

std::string Runtime::proc_maps() const {
    std::string out;
    for (const auto& m : linker_.modules()) {
        out += fmt::format("{:x}-{:x} r-xp 00000000 fd:00 0                          /data/app/{}-1/lib/arm64/{}\n",
                           m->base(), (m->end() + 0xFFF) & ~0xFFFull, options_.package, m->name());
    }
    out += fmt::format("{:x}-{:x} rw-p 00000000 00:00 0                          [anon:libc_malloc]\n", GuestMemory::kHeapBase,
                       GuestMemory::kHeapBase + GuestMemory::kHeapSize);
    return out;
}

}  // namespace rfu
