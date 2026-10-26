#include "core/guest_memory.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace rfu {

GuestMemory::GuestMemory() {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, kSize, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) throw std::runtime_error("could not reserve the guest address space");
#else
    void* p = mmap(nullptr, kSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) throw std::runtime_error("could not reserve the guest address space");
#endif
    base_ = static_cast<std::uint8_t*>(p);
}

GuestMemory::~GuestMemory() {
#ifdef _WIN32
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, kSize);
#endif
}

void GuestMemory::commit(GuestAddr addr, std::uint64_t size) {
    GuestAddr start = addr & ~(kPageSize - 1);
    GuestAddr end = (addr + size + kPageSize - 1) & ~(kPageSize - 1);
    if (end > kSize || start >= end) throw std::runtime_error("guest commit out of range");
    std::lock_guard lock(mutex_);
#ifdef _WIN32
    // Guest code lives in this memory too, but it is only ever read by the recompiler, never
    // executed directly, so read/write is enough.
    if (!VirtualAlloc(base_ + start, end - start, MEM_COMMIT, PAGE_READWRITE))
        throw std::runtime_error("guest commit failed");
#else
    if (mprotect(base_ + start, end - start, PROT_READ | PROT_WRITE) != 0)
        throw std::runtime_error("guest commit failed");
#endif
    // Merge into the committed set.
    auto it = committed_.upper_bound(start);
    if (it != committed_.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= start) {
            start = prev->first;
            end = std::max(end, prev->second);
            it = committed_.erase(prev);
        }
    }
    while (it != committed_.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = committed_.erase(it);
    }
    committed_[start] = end;
}

bool GuestMemory::is_committed(GuestAddr addr, std::uint64_t size) const {
    std::lock_guard lock(mutex_);
    auto it = committed_.upper_bound(addr);
    if (it == committed_.begin()) return false;
    --it;
    return addr >= it->first && addr + size <= it->second;
}

std::string GuestMemory::read_cstr(GuestAddr addr, std::size_t max) const {
    const char* p = reinterpret_cast<const char*>(base_ + addr);
    std::size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(p, n);
}

std::u32string GuestMemory::read_wstr(GuestAddr addr, std::size_t max) const {
    std::u32string out;
    for (std::size_t i = 0; i < max; ++i) {
        char32_t c = read<char32_t>(addr + 4 * i);
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

}  // namespace rfu
