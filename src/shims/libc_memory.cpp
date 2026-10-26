// malloc and friends, and the mmap family (backed by the guest heap).

#include "core/log.h"
#include "runtime.h"
#include "shims/common.h"

#include <cstring>
#include <fstream>
#include <unordered_map>

namespace rfu {

namespace {

struct Mappings {
    std::mutex mutex;
    std::unordered_map<GuestAddr, std::uint64_t> blocks;  // mmap'd address -> length
};

constexpr std::uint64_t kMapFailed = ~0ull;
constexpr int kMapAnonymous = 0x20;

}  // namespace

void register_libc_memory(ShimRegistry& reg) {
    reg.add("malloc", [](CallContext& c) { c.ret(c.rt().heap().alloc(c.x(0))); });
    reg.add("free", [](CallContext& c) { c.rt().heap().free(c.x(0)); });
    reg.add("calloc", [](CallContext& c) {
        std::uint64_t n = c.x(0), size = c.x(1);
        if (size && n > ~0ull / size) return c.fail(android::kENOMEM, 0);
        c.ret(c.rt().heap().alloc_zeroed(n * size));
    });
    reg.add("realloc", [](CallContext& c) { c.ret(c.rt().heap().realloc(c.x(0), c.x(1))); });
    reg.add("memalign", [](CallContext& c) { c.ret(c.rt().heap().alloc_aligned(c.x(0), c.x(1))); });
    reg.add("posix_memalign", [](CallContext& c) {
        std::uint64_t align = c.x(1);
        if (align < 8 || (align & (align - 1))) return c.ret(android::kEINVAL);
        c.mem().write<GuestAddr>(c.x(0), c.rt().heap().alloc_aligned(align, c.x(2)));
        c.ret(0);
    });
    reg.add("malloc_usable_size", [](CallContext& c) { c.ret(c.x(0) ? c.rt().heap().usable_size(c.x(0)) : 0); });

    // mmap(addr, length, prot, flags, fd, offset). Every mapping is fresh heap memory: the
    // game only maps anonymous memory or reads files through it.
    reg.add("mmap", [](CallContext& c) {
        std::uint64_t length = c.x(1);
        int flags = static_cast<int>(c.x(3));
        int fd = static_cast<int>(c.x(4));
        if (!length) return c.fail(android::kEINVAL, kMapFailed);
        std::uint64_t rounded = (length + 0xFFF) & ~0xFFFull;
        GuestAddr a = c.rt().heap().alloc_aligned(0x1000, rounded);
        std::memset(c.mem().host(a), 0, rounded);
        if (!(flags & kMapAnonymous) && fd >= 0) {
            if (guest_pread(c.rt(), fd, c.mem().host(a), length, static_cast<std::int64_t>(c.x(5))) < 0) {
                c.rt().heap().free(a);
                return c.fail(android::kEBADF, kMapFailed);
            }
        }
        auto& maps = c.rt().state<Mappings>();
        std::lock_guard lock(maps.mutex);
        maps.blocks[a] = rounded;
        c.ret(a);
    });
    reg.add("munmap", [](CallContext& c) {
        auto& maps = c.rt().state<Mappings>();
        std::lock_guard lock(maps.mutex);
        auto it = maps.blocks.find(c.x(0));
        if (it == maps.blocks.end()) {
            log::debug("munmap of {:#x} which wasn't mapped whole; ignored", c.x(0));
            return c.ret(0);
        }
        c.rt().heap().free(it->first);
        maps.blocks.erase(it);
        c.ret(0);
    });
    // Everything is already readable, writable and executable. A protection change is how
    // code gets patched, though, so drop any translations of the range.
    reg.add("mprotect", [](CallContext& c) {
        c.rt().invalidate_code(c.x(0), c.x(1));
        c.ret(0);
    });
    reg.add("madvise", [](CallContext& c) { c.ret(0); });
    reg.add("mlock", [](CallContext& c) { c.ret(0); });
}

}  // namespace rfu
