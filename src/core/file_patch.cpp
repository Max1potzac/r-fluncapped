#include "core/file_patch.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rfu {

std::size_t FilePatch::read(std::uint64_t pos, void* out, std::size_t n, const ReadBase& read_base) const {
    if (pos >= size) return 0;
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, size - pos));
    auto* dst = static_cast<std::uint8_t*>(out);
    // The base file, then zeros past its end.
    std::size_t from_base = pos < base_size ? static_cast<std::size_t>(std::min<std::uint64_t>(n, base_size - pos)) : 0;
    std::size_t got = from_base ? read_base(pos, dst, from_base) : 0;
    if (got < from_base) from_base = got;
    std::memset(dst + from_base, 0, n - from_base);
    // The writes that overlap [pos, pos + n).
    std::uint64_t end = pos + n;
    auto it = std::upper_bound(writes.begin(), writes.end(), pos, [](std::uint64_t p, const Write& w) { return p < w.offset + w.bytes.size(); });
    for (; it != writes.end() && it->offset < end; ++it) {
        std::uint64_t a = std::max(pos, it->offset), b = std::min<std::uint64_t>(end, it->offset + it->bytes.size());
        if (a < b) std::memcpy(dst + (a - pos), it->bytes.data() + (a - it->offset), static_cast<std::size_t>(b - a));
    }
    return n;
}

std::shared_ptr<FilePatch> parse_file_patch(const std::string& data, std::string& error) {
    std::size_t at = 0;
    auto take = [&](void* out, std::size_t n) {
        if (data.size() - at < n) return false;
        std::memcpy(out, data.data() + at, n);
        at += n;
        return true;
    };
    auto u32 = [&](std::uint32_t& v) {
        std::uint8_t b[4];
        if (!take(b, 4)) return false;
        v = b[0] | b[1] << 8 | b[2] << 16 | static_cast<std::uint32_t>(b[3]) << 24;
        return true;
    };
    auto u64 = [&](std::uint64_t& v) {
        std::uint32_t lo, hi;
        if (!u32(lo) || !u32(hi)) return false;
        v = lo | static_cast<std::uint64_t>(hi) << 32;
        return true;
    };
    char magic[8];
    std::uint32_t version = 0, count = 0;
    auto p = std::make_shared<FilePatch>();
    if (!take(magic, 8) || std::memcmp(magic, "RFUPATCH", 8) != 0) {
        error = "not a patch file (no RFUPATCH)";
        return nullptr;
    }
    if (!u32(version) || version != 1) {
        error = "patch file version " + std::to_string(version) + " isn't known (1 is)";
        return nullptr;
    }
    if (!u32(count) || !u64(p->base_size) || !u64(p->size)) {
        error = "patch file is cut short";
        return nullptr;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        FilePatch::Write w;
        std::uint64_t len = 0;
        if (!u64(w.offset) || !u64(len) || data.size() - at < len) {
            error = "patch file is cut short";
            return nullptr;
        }
        w.bytes.assign(data.data() + at, static_cast<std::size_t>(len));
        at += static_cast<std::size_t>(len);
        if (w.offset + len > p->size) {
            error = "a write runs past the patched file's end";
            return nullptr;
        }
        p->writes.push_back(std::move(w));
    }
    std::sort(p->writes.begin(), p->writes.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    for (std::size_t i = 1; i < p->writes.size(); ++i)
        if (p->writes[i - 1].offset + p->writes[i - 1].bytes.size() > p->writes[i].offset) {
            error = "writes overlap";
            return nullptr;
        }
    if (p->size < p->base_size) {
        error = "the patched file is smaller than the original";
        return nullptr;
    }
    return p;
}

std::shared_ptr<FilePatch> load_file_patch(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "can't read it";
        return nullptr;
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_file_patch(data, error);
}

}  // namespace rfu
