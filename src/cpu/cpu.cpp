#include "cpu/cpu.h"

#include "core/log.h"
#include "runtime.h"

#include <dynarmic/interface/exclusive_monitor.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <mutex>

namespace rfu {

// --- CallContext -----------------------------------------------------------------------

GuestMemory& CallContext::mem() const { return rt_.mem(); }
GuestThread& CallContext::thread() const { return cpu_.thread(); }

std::uint64_t CallContext::x(int i) const { return cpu_.jit().GetRegister(static_cast<std::size_t>(i)); }
double CallContext::d(int i) const { return std::bit_cast<double>(cpu_.jit().GetVector(static_cast<std::size_t>(i))[0]); }
float CallContext::s(int i) const {
    return std::bit_cast<float>(static_cast<std::uint32_t>(cpu_.jit().GetVector(static_cast<std::size_t>(i))[0]));
}
std::uint64_t CallContext::stack_arg(int i) const { return mem().read<std::uint64_t>(sp() + 8ull * static_cast<unsigned>(i)); }
std::uint64_t CallContext::sp() const { return cpu_.jit().GetSP(); }

void CallContext::ret(std::uint64_t v) { cpu_.jit().SetRegister(0, v); }
void CallContext::ret_d(double v) { cpu_.jit().SetVector(0, {std::bit_cast<std::uint64_t>(v), 0}); }
void CallContext::ret_f(float v) { cpu_.jit().SetVector(0, {std::bit_cast<std::uint32_t>(v), 0}); }
void CallContext::ret_q(std::uint64_t lo, std::uint64_t hi) { cpu_.jit().SetVector(0, {lo, hi}); }

char* CallContext::str(int i) const { return ptr<char>(i); }

void CallContext::fail(int err, std::uint64_t result) {
    rt_.set_errno(err);
    ret(result);
}

// --- Cpu -------------------------------------------------------------------------------

Cpu::Cpu(Runtime& rt, GuestThread& thread, std::uint32_t processor_id) : rt_(rt), thread_(thread) {
    Dynarmic::A64::UserConfig config;
    config.callbacks = this;
    config.processor_id = processor_id;
    config.global_monitor = &rt.monitor();
    config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(rt.mem().base());
    config.fastmem_address_space_bits = GuestMemory::kAddressBits;
    config.silently_mirror_fastmem = false;
    config.tpidr_el0 = &thread.tpidr_el0;
    config.tpidrro_el0 = &tpidrro_el0_;
    config.enable_cycle_counting = false;
    config.wall_clock_cntpct = true;
    config.define_unpredictable_behaviour = true;
    config.code_cache_size = 256ull << 20;
    jit_ = std::make_unique<Dynarmic::A64::Jit>(config);
}

Cpu::~Cpu() = default;

Cpu::State Cpu::save() const {
    return {jit_->GetRegisters(), jit_->GetVectors(), jit_->GetSP(), jit_->GetPC(), jit_->GetFpcr(), jit_->GetFpsr(), jit_->GetPstate()};
}

void Cpu::restore(const State& s) {
    jit_->SetRegisters(s.x);
    jit_->SetVectors(s.v);
    jit_->SetSP(s.sp);
    jit_->SetPC(s.pc);
    jit_->SetFpcr(s.fpcr);
    jit_->SetFpsr(s.fpsr);
    jit_->SetPstate(s.pstate);
}

std::uint64_t Cpu::call(GuestAddr fn, const std::vector<std::uint64_t>& args) {

    std::optional<State> outer;
    GuestAddr sp = thread_.stack_hi;
    if (depth_ > 0) {
        // Called from a shim: the interrupted guest frame owns everything above its SP.
        outer = save();
        sp = jit_->GetSP() & ~0xFull;
    }

    // AAPCS64: the first eight integer arguments in x0-x7, the rest in 8-byte stack slots.
    if (args.size() > 8) {
        std::size_t slots = args.size() - 8;
        sp = (sp - 8 * slots) & ~0xFull;
        for (std::size_t i = 0; i < slots; ++i) rt_.mem().write<std::uint64_t>(sp + 8 * i, args[8 + i]);
    }
    for (std::size_t i = 0; i < args.size() && i < 8; ++i) jit_->SetRegister(i, args[i]);
    jit_->SetRegister(29, 0);
    jit_->SetRegister(30, rt_.return_thunk());
    jit_->SetSP(sp);
    jit_->SetPC(fn);
    if (!outer) jit_->SetFpcr(0);

    struct DepthGuard {
        std::size_t& d;
        explicit DepthGuard(std::size_t& depth) : d(depth) { ++d; }
        ~DepthGuard() { --d; }
    } guard(depth_);

    for (;;) {
        pending_svc_.reset();
        jit_->Run();
        if (!fault_.empty()) {
            std::string message = std::move(fault_);
            fault_.clear();
            if (outer) restore(*outer);
            throw GuestFault(message);
        }
        if (!pending_svc_) continue;  // halted for cache invalidation and the like
        if (*pending_svc_ == kReturnSvc) break;
        rt_.dispatch(*pending_svc_, *this);
    }

    std::uint64_t result = jit_->GetRegister(0);
    if (outer) restore(*outer);
    return result;
}

void Cpu::fault(std::string message) {
    if (fault_.empty()) fault_ = std::move(message);
    jit_->HaltExecution();
}

template <typename T>
T Cpu::read(std::uint64_t vaddr) {
    if (vaddr >= GuestMemory::kSize || !rt_.mem().is_committed(vaddr, sizeof(T))) {
        fault(fmt::format("read of {} bytes from unmapped address {:#x} (pc {:#x})", sizeof(T), vaddr, jit_->GetPC()));
        return T{};
    }
    return rt_.mem().read<T>(vaddr);
}

template <typename T>
void Cpu::write(std::uint64_t vaddr, T v) {
    if (vaddr >= GuestMemory::kSize || !rt_.mem().is_committed(vaddr, sizeof(T))) {
        fault(fmt::format("write of {} bytes to unmapped address {:#x} (pc {:#x})", sizeof(T), vaddr, jit_->GetPC()));
        return;
    }
    rt_.mem().write(vaddr, v);
}

template <typename T>
bool Cpu::cas(std::uint64_t vaddr, T v, T expected) {
    if (vaddr >= GuestMemory::kSize || !rt_.mem().is_committed(vaddr, sizeof(T))) {
        fault(fmt::format("exclusive write to unmapped address {:#x}", vaddr));
        return false;
    }
    std::atomic_ref<T> ref(*rt_.mem().host_as<T>(vaddr));
    return ref.compare_exchange_strong(expected, v);
}

bool Cpu::MemoryWriteExclusive128(std::uint64_t a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) {
    // No portable 128-bit CAS; a global lock is good enough for the rare LDXP/STXP pairs.
    static std::mutex lock;
    std::lock_guard guard(lock);
    if (read<Dynarmic::A64::Vector>(a) != e) return false;
    write(a, v);
    return true;
}

std::optional<std::uint32_t> Cpu::MemoryReadCode(std::uint64_t vaddr) {
    if (vaddr >= GuestMemory::kSize || !rt_.mem().is_committed(vaddr, 4)) return std::nullopt;
    return rt_.mem().read<std::uint32_t>(vaddr);
}

void Cpu::InterpreterFallback(std::uint64_t pc, std::size_t num_instructions) {
    std::uint32_t insn = rt_.mem().is_committed(pc, 4) ? rt_.mem().read<std::uint32_t>(pc) : 0;

    // System registers dynarmic leaves to us. Android lets EL0 read these.
    std::optional<std::uint64_t> value;
    switch (insn & ~0x1Fu) {
    case 0xD53BE040: value = GetCNTPCT(); break;  // mrs xN, cntvct_el0
    case 0xD53BE000: value = 600'000'000; break;  // mrs xN, cntfrq_el0
    case 0xD5380000: value = 0x410FD083; break;   // mrs xN, midr_el1 (Cortex-A72)
    case 0xD53800A0: value = 0x80000000; break;   // mrs xN, mpidr_el1
    }
    if (value && num_instructions == 1) {
        std::uint32_t rt = insn & 0x1F;
        if (rt != 31) jit_->SetRegister(rt, *value);
        jit_->SetPC(pc + 4);
        return;
    }
    fault(fmt::format("instruction {:08x} at {:#x} isn't supported by the JIT ({} instructions)", insn, pc, num_instructions));
}

void Cpu::CallSVC(std::uint32_t swi) {
    pending_svc_ = swi;
    jit_->HaltExecution();
}

void Cpu::ExceptionRaised(std::uint64_t pc, Dynarmic::A64::Exception exception) {
    using Dynarmic::A64::Exception;
    switch (exception) {
    case Exception::WaitForInterrupt:
    case Exception::WaitForEvent:
    case Exception::SendEvent:
    case Exception::SendEventLocal:
    case Exception::Yield:
        return;
    case Exception::Breakpoint:
        fault(fmt::format("breakpoint (brk) at {:#x}", pc));
        return;
    case Exception::NoExecuteFault:
        fault(fmt::format("jump to non-executable address {:#x}", pc));
        return;
    default:
        fault(fmt::format("undefined instruction at {:#x} (exception {})", pc, static_cast<int>(exception)));
        return;
    }
}

void Cpu::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, std::uint64_t value) {
    // Code was patched (RFLib installs inline hooks this way). IC IVAU names one cache line;
    // the "invalidate all" forms drop everything.
    if (op == Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU)
        rt_.invalidate_code(value & ~0x3Full, 64);
    else
        jit_->ClearCache();
}

std::uint64_t Cpu::GetCNTPCT() {
    // CNTFRQ_EL0 is dynarmic's default 600 MHz.
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<std::uint64_t>(ns) * 3 / 5;
}

}  // namespace rfu
