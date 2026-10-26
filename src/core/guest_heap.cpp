#include "core/guest_heap.h"

#include <algorithm>
#include <stdexcept>

namespace rfu {

namespace {
struct Header {
    std::uint64_t capacity;
    std::uint64_t offset;
};
constexpr std::uint64_t kCommitChunk = 16ull << 20;
}  // namespace

GuestHeap::GuestHeap(GuestMemory& mem)
    : mem_(mem), top_(GuestMemory::kHeapBase), committed_end_(GuestMemory::kHeapBase) {}

std::size_t GuestHeap::class_of(std::uint64_t size) {
    std::size_t cls = 0;
    while (cls < kClasses && class_size(cls) < size) ++cls;
    return cls;
}

std::uint64_t GuestHeap::class_size(std::size_t cls) { return 16ull << cls; }

GuestAddr GuestHeap::carve(std::uint64_t total) {
    GuestAddr block = top_;
    top_ += total;
    if (top_ > GuestMemory::kHeapBase + GuestMemory::kHeapSize) throw std::runtime_error("guest heap exhausted");
    if (top_ > committed_end_) {
        std::uint64_t grow = ((top_ - committed_end_) + kCommitChunk - 1) / kCommitChunk * kCommitChunk;
        mem_.commit(committed_end_, grow);
        committed_end_ += grow;
    }
    return block;
}

GuestAddr GuestHeap::alloc(std::uint64_t size) {
    if (size == 0) size = 1;
    size = (size + 15) & ~15ull;
    std::lock_guard lock(mutex_);
    std::size_t cls = class_of(size);
    GuestAddr ptr;
    std::uint64_t capacity;
    if (cls < kClasses) {
        capacity = class_size(cls);
        auto& list = small_free_[cls];
        if (!list.empty()) {
            ptr = list.back();
            list.pop_back();
        } else {
            ptr = carve(kHeader + capacity) + kHeader;
        }
    } else {
        auto it = large_free_.lower_bound(size);
        if (it != large_free_.end() && it->first <= size * 2) {
            capacity = it->first;
            ptr = it->second;
            large_free_.erase(it);
        } else {
            capacity = (size + 0xFFF) & ~0xFFFull;
            ptr = carve(kHeader + capacity) + kHeader;
        }
    }
    mem_.write(ptr - kHeader, Header{capacity, 0});
    in_use_ += capacity;
    return ptr;
}

GuestAddr GuestHeap::alloc_zeroed(std::uint64_t size) {
    GuestAddr p = alloc(size);
    std::memset(mem_.host(p), 0, size);
    return p;
}

GuestAddr GuestHeap::alloc_aligned(std::uint64_t alignment, std::uint64_t size) {
    if (alignment <= 16) return alloc(size);
    GuestAddr raw = alloc(size + alignment + kHeader);
    GuestAddr p = (raw + kHeader + alignment - 1) & ~(alignment - 1);
    mem_.write(p - kHeader, Header{size, p - raw});
    return p;
}

std::uint64_t GuestHeap::usable_size(GuestAddr ptr) const {
    return mem_.read<Header>(ptr - kHeader).capacity;
}

GuestAddr GuestHeap::realloc(GuestAddr ptr, std::uint64_t size) {
    if (!ptr) return alloc(size);
    if (size == 0) {
        free(ptr);
        return 0;
    }
    std::uint64_t old = usable_size(ptr);
    if (size <= old && mem_.read<Header>(ptr - kHeader).offset == 0) return ptr;
    GuestAddr fresh = alloc(size);
    std::memcpy(mem_.host(fresh), mem_.host(ptr), std::min(old, size));
    free(ptr);
    return fresh;
}

void GuestHeap::free(GuestAddr ptr) {
    if (!ptr) return;
    Header h = mem_.read<Header>(ptr - kHeader);
    if (h.offset) {
        free(ptr - h.offset);
        return;
    }
    std::lock_guard lock(mutex_);
    in_use_ -= h.capacity;
    std::size_t cls = class_of(h.capacity);
    if (cls < kClasses && class_size(cls) == h.capacity)
        small_free_[cls].push_back(ptr);
    else
        large_free_.emplace(h.capacity, ptr);
}

GuestAddr GuestHeap::strdup(std::string_view s) {
    GuestAddr p = alloc(s.size() + 1);
    std::memcpy(mem_.host(p), s.data(), s.size());
    mem_.host(p)[s.size()] = 0;
    return p;
}

}  // namespace rfu
