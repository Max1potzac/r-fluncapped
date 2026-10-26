#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>

namespace rfu {

using GuestAddr = std::uint64_t;

// The guest's whole address space is one host reservation, so a guest address is just an
// offset from base(). Dynarmic uses the same reservation as its fastmem arena.
//
// Layout (all guest addresses):
//   0x0000'0000 .. 0x1000'0000   never mapped: catches null and small-offset dereferences
//   0x1000'0000 ..               loaded modules (Module::load picks the next free slot)
//   kHeapBase  .. +kHeapSize     guest heap (GuestHeap)
//   kStackBase ..                one stack per guest thread
//   kThunkBase ..                import thunks: `svc #n; ret` per imported symbol
class GuestMemory {
public:
    static constexpr unsigned kAddressBits = 36;  // 64 GiB
    static constexpr std::uint64_t kSize = 1ull << kAddressBits;
    static constexpr std::uint64_t kPageSize = 0x1000;

    static constexpr GuestAddr kModuleBase = 0x1000'0000;
    static constexpr GuestAddr kHeapBase = 0x4'0000'0000;
    static constexpr std::uint64_t kHeapSize = 0x2'0000'0000;  // 8 GiB of address space
    static constexpr GuestAddr kStackBase = 0x8'0000'0000;
    static constexpr std::uint64_t kStackSize = 0x10'0000;  // 1 MiB per thread
    static constexpr GuestAddr kThunkBase = 0xF'0000'0000;

    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    std::uint8_t* base() const { return base_; }

    // Makes [addr, addr + size) readable and writable (rounded out to pages).
    void commit(GuestAddr addr, std::uint64_t size);
    bool is_committed(GuestAddr addr, std::uint64_t size) const;

    std::uint8_t* host(GuestAddr addr) const { return base_ + addr; }
    template <typename T>
    T* host_as(GuestAddr addr) const { return reinterpret_cast<T*>(base_ + addr); }
    GuestAddr guest(const void* host_ptr) const {
        return static_cast<GuestAddr>(static_cast<const std::uint8_t*>(host_ptr) - base_);
    }

    template <typename T>
    T read(GuestAddr addr) const {
        static_assert(std::is_trivially_copyable_v<T>);
        T value;
        std::memcpy(&value, base_ + addr, sizeof(T));
        return value;
    }
    template <typename T>
    void write(GuestAddr addr, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::memcpy(base_ + addr, &value, sizeof(T));
    }
    void read_bytes(GuestAddr addr, void* out, std::size_t n) const { std::memcpy(out, base_ + addr, n); }
    void write_bytes(GuestAddr addr, const void* data, std::size_t n) { std::memcpy(base_ + addr, data, n); }

    std::string read_cstr(GuestAddr addr, std::size_t max = 1 << 20) const;
    std::u32string read_wstr(GuestAddr addr, std::size_t max = 1 << 20) const;  // Android wchar_t is 32-bit

private:
    std::uint8_t* base_ = nullptr;
    mutable std::mutex mutex_;
    std::map<GuestAddr, GuestAddr> committed_;  // start -> end, page aligned, non-overlapping
};

}  // namespace rfu
