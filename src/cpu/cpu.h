#pragma once

#include "core/guest_memory.h"

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace rfu {

class Runtime;
class Cpu;
struct Thunk;

// Raised when guest code faults, hits an undefined instruction, or calls abort().
class GuestFault : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Raised by exit(): unwinds every host frame back to whoever started the guest.
struct GuestExit {
    int code;
};

// A guest thread: its stack, its TLS block (TPIDR_EL0) and the CPU that runs it.
struct GuestThread {
    std::uint32_t id = 0;          // what gettid() reports
    GuestAddr handle = 0;          // what pthread_self() reports: a small guest block
    GuestAddr stack_lo = 0, stack_hi = 0;
    GuestAddr tls = 0;             // bionic TLS slots; TPIDR_EL0 points here
    GuestAddr errno_addr = 0;
    std::uint64_t tpidr_el0 = 0;
    std::unique_ptr<Cpu> cpu;
    std::unordered_map<std::uint32_t, GuestAddr> specific;  // pthread_key -> value
};

// What a shim sees: the calling guest context's registers, following AAPCS64.
class CallContext {
public:
    CallContext(Cpu& cpu, Runtime& rt, const Thunk& thunk) : cpu_(cpu), rt_(rt), thunk_(thunk) {}
    Runtime& rt() const { return rt_; }
    Cpu& cpu() const { return cpu_; }
    GuestMemory& mem() const;
    GuestThread& thread() const;
    const Thunk& thunk() const { return thunk_; }

    std::uint64_t x(int i) const;  // integer/pointer argument i (0-7), or register x0-x30
    double d(int i) const;         // floating-point argument i as double
    float s(int i) const;          // floating-point argument i as float
    std::uint64_t stack_arg(int i) const;  // i-th 8-byte slot of stack-passed arguments
    std::uint64_t sp() const;

    void ret(std::uint64_t v);
    void ret_d(double v);
    void ret_f(float v);
    void ret_q(std::uint64_t lo, std::uint64_t hi);  // 128-bit value in q0 (long double)

    // Host view of a guest pointer argument. Null stays null.
    char* str(int i) const;
    template <typename T = std::uint8_t>
    T* ptr(int i) const {
        auto a = x(i);
        return a ? mem().host_as<T>(a) : nullptr;
    }

    // Sets errno for the calling thread and returns `result` (usually -1).
    void fail(int err, std::uint64_t result = ~0ull);

private:
    Cpu& cpu_;
    Runtime& rt_;
    const Thunk& thunk_;
};

// One dynarmic JIT per guest thread. Guest calls to imported functions land on thunks
// (`svc #n; ret`); CallSVC stops the JIT, and run() handles the shim outside the JIT so the
// shim can freely read and write registers or call back into the guest.
class Cpu final : public Dynarmic::A64::UserCallbacks {
public:
    static constexpr std::uint32_t kReturnSvc = 0xFFFF;

    Cpu(Runtime& rt, GuestThread& thread, std::uint32_t processor_id);
    ~Cpu() override;

    // Calls guest function `fn` with up to 8 integer arguments and returns x0. Safe to call
    // from inside a shim (the interrupted guest state is saved and restored).
    std::uint64_t call(GuestAddr fn, const std::vector<std::uint64_t>& args);

    Dynarmic::A64::Jit& jit() { return *jit_; }
    GuestThread& thread() { return thread_; }
    Runtime& rt() { return rt_; }
    std::size_t depth() const { return depth_; }
    void invalidate(GuestAddr addr, std::size_t size) { jit_->InvalidateCacheRange(addr, size); }

    // Dynarmic callbacks. Plain loads and stores go straight to host memory (fastmem); these
    // only see what fastmem couldn't handle: unmapped addresses and exclusive accesses.
    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t vaddr) override;
    std::uint8_t MemoryRead8(std::uint64_t vaddr) override { return read<std::uint8_t>(vaddr); }
    std::uint16_t MemoryRead16(std::uint64_t vaddr) override { return read<std::uint16_t>(vaddr); }
    std::uint32_t MemoryRead32(std::uint64_t vaddr) override { return read<std::uint32_t>(vaddr); }
    std::uint64_t MemoryRead64(std::uint64_t vaddr) override { return read<std::uint64_t>(vaddr); }
    Dynarmic::A64::Vector MemoryRead128(std::uint64_t vaddr) override { return read<Dynarmic::A64::Vector>(vaddr); }
    void MemoryWrite8(std::uint64_t vaddr, std::uint8_t v) override { write(vaddr, v); }
    void MemoryWrite16(std::uint64_t vaddr, std::uint16_t v) override { write(vaddr, v); }
    void MemoryWrite32(std::uint64_t vaddr, std::uint32_t v) override { write(vaddr, v); }
    void MemoryWrite64(std::uint64_t vaddr, std::uint64_t v) override { write(vaddr, v); }
    void MemoryWrite128(std::uint64_t vaddr, Dynarmic::A64::Vector v) override { write(vaddr, v); }
    bool MemoryWriteExclusive8(std::uint64_t a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(std::uint64_t a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(std::uint64_t a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(std::uint64_t a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(std::uint64_t a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override;
    void InterpreterFallback(std::uint64_t pc, std::size_t num_instructions) override;
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint64_t pc, Dynarmic::A64::Exception exception) override;
    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, std::uint64_t value) override;
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return ~0ull; }
    std::uint64_t GetCNTPCT() override;

private:
    struct State {
        std::array<std::uint64_t, 31> x;
        std::array<Dynarmic::A64::Vector, 32> v;
        std::uint64_t sp, pc;
        std::uint32_t fpcr, fpsr, pstate;
    };
    State save() const;
    void restore(const State& s);

    template <typename T>
    T read(std::uint64_t vaddr);
    template <typename T>
    void write(std::uint64_t vaddr, T v);
    template <typename T>
    bool cas(std::uint64_t vaddr, T v, T expected);
    void fault(std::string message);

    Runtime& rt_;
    GuestThread& thread_;
    std::uint64_t tpidrro_el0_ = 0;
    std::unique_ptr<Dynarmic::A64::Jit> jit_;
    std::size_t depth_ = 0;
    std::optional<std::uint32_t> pending_svc_;
    std::string fault_;
};

}  // namespace rfu
