#include "android/apk.h"

#include <zlib.h>

#include <fstream>
#include <stdexcept>
#include <vector>

namespace rfu::android {

namespace {

std::uint64_t le(const std::uint8_t* p, int n) {
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = v << 8 | p[i];
    return v;
}

}  // namespace

ApkIndex::ApkIndex(const std::filesystem::path& apk) : path_(apk) {
    std::ifstream in(apk, std::ios::binary);
    if (!in) throw std::runtime_error("can't read " + apk.string());
    in.seekg(0, std::ios::end);
    std::uint64_t file_size = static_cast<std::uint64_t>(in.tellg());
    auto read = [&](std::uint64_t at, std::size_t n) {
        std::vector<std::uint8_t> b(n);
        in.clear();
        in.seekg(static_cast<std::streamoff>(at));
        in.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(in.gcount()) != n) throw std::runtime_error(apk.string() + " is cut short");
        return b;
    };

    // The end of central directory record is in the last 64 KiB + 22 bytes.
    std::uint64_t tail_size = std::min<std::uint64_t>(file_size, 65536 + 22);
    auto tail = read(file_size - tail_size, tail_size);
    std::int64_t eocd = -1;
    for (std::int64_t i = static_cast<std::int64_t>(tail_size) - 22; i >= 0; --i)
        if (le(&tail[i], 4) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) throw std::runtime_error(apk.string() + " isn't a zip (no end of central directory)");
    std::uint64_t count = le(&tail[eocd + 10], 2), cd_size = le(&tail[eocd + 12], 4), cd_offset = le(&tail[eocd + 16], 4);
    // Zip64: the record before the end record points at the zip64 end record.
    if ((cd_offset == 0xFFFFFFFF || count == 0xFFFF) && eocd >= 20 && le(&tail[eocd - 20], 4) == 0x07064b50) {
        auto z = read(le(&tail[eocd - 20 + 8], 8), 56);
        if (le(&z[0], 4) == 0x06064b50) {
            count = le(&z[32], 8);
            cd_size = le(&z[40], 8);
            cd_offset = le(&z[48], 8);
        }
    }
    auto cd = read(cd_offset, cd_size);
    std::size_t p = 0;
    for (std::uint64_t i = 0; i < count && p + 46 <= cd.size(); ++i) {
        if (le(&cd[p], 4) != 0x02014b50) throw std::runtime_error(apk.string() + ": bad central directory");
        std::uint64_t method = le(&cd[p + 10], 2);
        std::uint64_t csize = le(&cd[p + 20], 4), usize = le(&cd[p + 24], 4);
        std::size_t name_len = le(&cd[p + 28], 2), extra_len = le(&cd[p + 30], 2), comment_len = le(&cd[p + 32], 2);
        std::uint64_t local = le(&cd[p + 42], 4);
        std::string name(reinterpret_cast<const char*>(&cd[p + 46]), name_len);
        // Zip64 extra field: the sizes and offset that didn't fit, in that order.
        for (std::size_t e = p + 46 + name_len; e + 4 <= p + 46 + name_len + extra_len;) {
            std::size_t id = le(&cd[e], 2), len = le(&cd[e + 2], 2), q = e + 4;
            if (id == 1) {
                if (usize == 0xFFFFFFFF && q + 8 <= e + 4 + len) usize = le(&cd[q], 8), q += 8;
                if (csize == 0xFFFFFFFF && q + 8 <= e + 4 + len) csize = le(&cd[q], 8), q += 8;
                if (local == 0xFFFFFFFF && q + 8 <= e + 4 + len) local = le(&cd[q], 8), q += 8;
            }
            e += 4 + len;
        }
        p += 46 + name_len + extra_len + comment_len;
        if (name.empty() || name.back() == '/') continue;
        auto lh = read(local, 30);
        if (le(&lh[0], 4) != 0x04034b50) continue;
        Entry entry;
        entry.offset = local + 30 + le(&lh[26], 2) + le(&lh[28], 2);
        entry.size = usize;
        entry.stored = method == 0 && csize == usize;
        entry.compressed_size = csize;
        entry.method = static_cast<int>(method);
        entries_.emplace(std::move(name), entry);
    }
}

std::optional<ApkIndex::Entry> ApkIndex::find(const std::string& name) const {
    auto it = entries_.find(name);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

std::optional<std::vector<std::uint8_t>> ApkIndex::read(const std::string& name) const {
    auto e = find(name);
    if (!e || (e->method != 0 && e->method != 8)) return std::nullopt;
    std::ifstream in(path_, std::ios::binary);
    std::vector<std::uint8_t> raw(e->compressed_size);
    in.seekg(static_cast<std::streamoff>(e->offset));
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (static_cast<std::uint64_t>(in.gcount()) != raw.size()) return std::nullopt;
    if (e->method == 0) return raw;
    std::vector<std::uint8_t> out(e->size);
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return std::nullopt;
    z.next_in = raw.data();
    z.avail_in = static_cast<uInt>(raw.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (r != Z_STREAM_END) return std::nullopt;
    return out;
}

namespace {

// One DER element: where its contents start and end, and where the whole element ends.
struct Der {
    std::uint8_t tag = 0;
    std::size_t start = 0, content = 0, end = 0;
};

std::optional<Der> der_at(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 2 > b.size()) return std::nullopt;
    Der d;
    d.tag = b[p];
    d.start = p;
    std::size_t len = b[p + 1];
    p += 2;
    if (len & 0x80) {
        int n = len & 0x7F;
        if (n == 0 || n > 4 || p + n > b.size()) return std::nullopt;
        len = 0;
        for (int i = 0; i < n; ++i) len = len << 8 | b[p++];
    }
    if (p + len > b.size()) return std::nullopt;
    d.content = p;
    d.end = p + len;
    return d;
}

}  // namespace

std::vector<std::uint8_t> ApkIndex::signing_certificate() const {
    for (const auto& [name, e] : entries_) {
        if (!name.starts_with("META-INF/")) continue;
        if (!name.ends_with(".RSA") && !name.ends_with(".DSA") && !name.ends_with(".EC")) continue;
        auto b = read(name);
        if (!b) continue;
        // ContentInfo { contentType OID, [0] SignedData { version, digestAlgorithms, contentInfo,
        // [0] certificates, ... } }
        auto ci = der_at(*b, 0);
        if (!ci || ci->tag != 0x30) continue;
        auto oid = der_at(*b, ci->content);
        auto wrap = oid ? der_at(*b, oid->end) : std::nullopt;
        auto sd = wrap && wrap->tag == 0xA0 ? der_at(*b, wrap->content) : std::nullopt;
        if (!sd || sd->tag != 0x30) continue;
        std::size_t p = sd->content;
        for (int i = 0; i < 3; ++i) {
            auto skip = der_at(*b, p);
            if (!skip) break;
            p = skip->end;
        }
        auto certs = der_at(*b, p);
        if (!certs || certs->tag != 0xA0) continue;
        auto cert = der_at(*b, certs->content);
        if (!cert || cert->tag != 0x30) continue;
        return {b->begin() + static_cast<std::ptrdiff_t>(cert->start), b->begin() + static_cast<std::ptrdiff_t>(cert->end)};
    }
    return {};
}

}  // namespace rfu::android
