// zlib. The guest's z_stream (LP64 layout) is mirrored into a host z_stream around each
// call; the host one can't be used directly because uLong is 32-bit on Windows.

#include "core/log.h"
#include "shims/common.h"

#include <zlib.h>

#include <cstring>
#include <unordered_map>

namespace rfu {

namespace {

// Guest z_stream field offsets.
constexpr int kNextIn = 0, kAvailIn = 8, kTotalIn = 16, kNextOut = 24, kAvailOut = 32, kTotalOut = 40, kMsg = 48,
              kState = 56, kDataType = 88, kAdler = 96;

struct Streams {
    std::mutex mutex;
    std::unordered_map<GuestAddr, std::unique_ptr<z_stream>> streams;
    std::unordered_map<GuestAddr, gzFile> gz;
};

z_stream* host_stream(CallContext& c, GuestAddr g, bool create) {
    auto& s = c.rt().state<Streams>();
    std::lock_guard lock(s.mutex);
    auto& slot = s.streams[g];
    if (!slot && create) slot = std::make_unique<z_stream>();
    return slot.get();
}

void drop_stream(CallContext& c, GuestAddr g) {
    auto& s = c.rt().state<Streams>();
    std::lock_guard lock(s.mutex);
    s.streams.erase(g);
}

void to_host(CallContext& c, GuestAddr g, z_stream& h) {
    auto& mem = c.mem();
    GuestAddr in = mem.read<GuestAddr>(g + kNextIn), out = mem.read<GuestAddr>(g + kNextOut);
    h.next_in = in ? mem.host(in) : nullptr;
    h.avail_in = mem.read<std::uint32_t>(g + kAvailIn);
    h.next_out = out ? mem.host(out) : nullptr;
    h.avail_out = mem.read<std::uint32_t>(g + kAvailOut);
}

void to_guest(CallContext& c, GuestAddr g, const z_stream& h) {
    auto& mem = c.mem();
    std::uint64_t in_before = mem.read<std::uint32_t>(g + kAvailIn), out_before = mem.read<std::uint32_t>(g + kAvailOut);
    mem.write<GuestAddr>(g + kNextIn, h.next_in ? mem.guest(h.next_in) : 0);
    mem.write<std::uint32_t>(g + kAvailIn, h.avail_in);
    mem.write<GuestAddr>(g + kNextOut, h.next_out ? mem.guest(h.next_out) : 0);
    mem.write<std::uint32_t>(g + kAvailOut, h.avail_out);
    mem.write(g + kTotalIn, mem.read<std::uint64_t>(g + kTotalIn) + in_before - h.avail_in);
    mem.write(g + kTotalOut, mem.read<std::uint64_t>(g + kTotalOut) + out_before - h.avail_out);
    mem.write<GuestAddr>(g + kMsg, h.msg ? intern_string(c.rt(), h.msg) : 0);
    mem.write<std::int32_t>(g + kDataType, h.data_type);
    mem.write<std::uint64_t>(g + kAdler, h.adler);
}

// Starts a guest stream: clears the counters and marks state non-null like zlib does.
z_stream* begin(CallContext& c, GuestAddr g) {
    z_stream* h = host_stream(c, g, true);
    *h = {};
    c.mem().write<std::uint64_t>(g + kTotalIn, 0);
    c.mem().write<std::uint64_t>(g + kTotalOut, 0);
    c.mem().write<GuestAddr>(g + kMsg, 0);
    c.mem().write<GuestAddr>(g + kState, g);
    to_host(c, g, *h);
    return h;
}

template <typename F>
void with_stream(CallContext& c, F f) {
    GuestAddr g = c.x(0);
    z_stream* h = host_stream(c, g, false);
    if (!h) return c.ret(static_cast<std::uint64_t>(static_cast<std::int64_t>(Z_STREAM_ERROR)));
    to_host(c, g, *h);
    int r = f(*h);
    to_guest(c, g, *h);
    c.ret(static_cast<std::uint64_t>(static_cast<std::int64_t>(r)));
}

void ret_int(CallContext& c, int r) { c.ret(static_cast<std::uint64_t>(static_cast<std::int64_t>(r))); }

}  // namespace

void register_libz(ShimRegistry& reg) {
    reg.add("zlibVersion", [](CallContext& c) { c.ret(intern_string(c.rt(), zlibVersion())); });
    reg.add("adler32", [](CallContext& c) {
        c.ret(c.x(1) ? adler32(static_cast<uLong>(c.x(0)), c.ptr(1), static_cast<uInt>(c.x(2))) : adler32(0, nullptr, 0));
    });
    reg.add("crc32", [](CallContext& c) {
        c.ret(c.x(1) ? crc32(static_cast<uLong>(c.x(0)), c.ptr(1), static_cast<uInt>(c.x(2))) : crc32(0, nullptr, 0));
    });

    reg.add("inflateInit_", [](CallContext& c) {
        z_stream* h = begin(c, c.x(0));
        int r = inflateInit(h);
        to_guest(c, c.x(0), *h);
        ret_int(c, r);
    });
    reg.add("inflateInit2_", [](CallContext& c) {
        z_stream* h = begin(c, c.x(0));
        int r = inflateInit2(h, static_cast<int>(c.x(1)));
        to_guest(c, c.x(0), *h);
        ret_int(c, r);
    });
    reg.add("inflate", [](CallContext& c) { with_stream(c, [&](z_stream& h) { return inflate(&h, static_cast<int>(c.x(1))); }); });
    reg.add("inflateReset", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0) + kTotalIn, 0);
        c.mem().write<std::uint64_t>(c.x(0) + kTotalOut, 0);
        with_stream(c, [](z_stream& h) { return inflateReset(&h); });
    });
    reg.add("inflateEnd", [](CallContext& c) {
        with_stream(c, [](z_stream& h) { return inflateEnd(&h); });
        c.mem().write<GuestAddr>(c.x(0) + kState, 0);
        drop_stream(c, c.x(0));
    });
    // deflateInit2_(strm, level, method, windowBits, memLevel, strategy, version, size)
    reg.add("deflateInit2_", [](CallContext& c) {
        z_stream* h = begin(c, c.x(0));
        int r = deflateInit2(h, static_cast<int>(c.x(1)), static_cast<int>(c.x(2)), static_cast<int>(c.x(3)), static_cast<int>(c.x(4)),
                             static_cast<int>(c.x(5)));
        to_guest(c, c.x(0), *h);
        ret_int(c, r);
    });
    reg.add("deflate", [](CallContext& c) { with_stream(c, [&](z_stream& h) { return deflate(&h, static_cast<int>(c.x(1))); }); });
    reg.add("deflateReset", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0) + kTotalIn, 0);
        c.mem().write<std::uint64_t>(c.x(0) + kTotalOut, 0);
        with_stream(c, [](z_stream& h) { return deflateReset(&h); });
    });
    reg.add("deflateEnd", [](CallContext& c) {
        with_stream(c, [](z_stream& h) { return deflateEnd(&h); });
        c.mem().write<GuestAddr>(c.x(0) + kState, 0);
        drop_stream(c, c.x(0));
    });

    reg.add("gzopen", [](CallContext& c) {
        bool reading = std::strchr(c.str(1), 'r') != nullptr;
        auto path = c.rt().vfs().absolute(c.str(0));
        auto host = reading ? c.rt().vfs().to_host_read(path) : c.rt().vfs().to_host(path);
        if (!host) return c.fail(android::kENOENT, 0);
#ifdef _WIN32
        gzFile f = gzopen_w(host->c_str(), c.str(1));
#else
        gzFile f = gzopen(host->c_str(), c.str(1));
#endif
        if (!f) return c.fail(android::kENOENT, 0);
        GuestAddr handle = c.rt().heap().alloc_zeroed(16);
        auto& s = c.rt().state<Streams>();
        std::lock_guard lock(s.mutex);
        s.gz[handle] = f;
        c.ret(handle);
    });
    reg.add("gzread", [](CallContext& c) {
        auto& s = c.rt().state<Streams>();
        gzFile f;
        {
            std::lock_guard lock(s.mutex);
            auto it = s.gz.find(c.x(0));
            if (it == s.gz.end()) return ret_int(c, -1);
            f = it->second;
        }
        ret_int(c, gzread(f, c.ptr(1), static_cast<unsigned>(c.x(2))));
    });
    reg.add("gzclose", [](CallContext& c) {
        auto& s = c.rt().state<Streams>();
        gzFile f;
        {
            std::lock_guard lock(s.mutex);
            auto it = s.gz.find(c.x(0));
            if (it == s.gz.end()) return ret_int(c, Z_STREAM_ERROR);
            f = it->second;
            s.gz.erase(it);
        }
        c.rt().heap().free(c.x(0));
        ret_int(c, gzclose(f));
    });
}

}  // namespace rfu
