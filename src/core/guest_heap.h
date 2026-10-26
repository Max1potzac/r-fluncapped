#pragma once

#include "core/guest_memory.h"

#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace rfu {

// malloc/free for guest code, carved out of GuestMemory's heap range.
//
// Small blocks come from per-size-class free lists, large ones from a size-ordered free map.
// Every block has a 16-byte header right before the pointer handed to the guest:
//   u64 capacity   usable bytes
//   u64 offset     0 for a normal block; for an over-aligned block, the distance back to
//                  the real block it was carved from
class GuestHeap {
public:
    explicit GuestHeap(GuestMemory& mem);

    GuestAddr alloc(std::uint64_t size);
    GuestAddr alloc_zeroed(std::uint64_t size);
    GuestAddr alloc_aligned(std::uint64_t alignment, std::uint64_t size);
    GuestAddr realloc(GuestAddr ptr, std::uint64_t size);
    void free(GuestAddr ptr);
    std::uint64_t usable_size(GuestAddr ptr) const;

    // Copies a host string into a fresh guest allocation (NUL terminated).
    GuestAddr strdup(std::string_view s);

    std::uint64_t bytes_in_use() const { return in_use_; }

private:
    static constexpr std::uint64_t kHeader = 16;
    static constexpr std::size_t kClasses = 24;  // 16 B .. 128 KiB

    static std::size_t class_of(std::uint64_t size);
    static std::uint64_t class_size(std::size_t cls);
    GuestAddr carve(std::uint64_t total);

    GuestMemory& mem_;
    mutable std::mutex mutex_;
    GuestAddr top_;
    GuestAddr committed_end_;
    std::array<std::vector<GuestAddr>, kClasses> small_free_;
    std::multimap<std::uint64_t, GuestAddr> large_free_;  // capacity -> block
    std::uint64_t in_use_ = 0;
};

}  // namespace rfu
