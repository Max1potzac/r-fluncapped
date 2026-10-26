#include "core/vfs.h"

#include <algorithm>

namespace rfu {

namespace {

// Collapses "//", "." and ".." in an absolute guest path.
std::string normalise(const std::string& path) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        i = j + 1;
    }
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

bool has_prefix(const std::string& path, const std::string& prefix) {
    if (prefix == "/") return true;
    return path.size() >= prefix.size() && path.compare(0, prefix.size(), prefix) == 0 &&
           (path.size() == prefix.size() || path[prefix.size()] == '/');
}

}  // namespace

std::string path_to_utf8(const std::filesystem::path& p) {
    auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path path_from_utf8(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

void Vfs::mount(std::string guest, std::filesystem::path host) {
    mounts_.push_back({normalise(guest), std::move(host)});
    std::sort(mounts_.begin(), mounts_.end(), [](const Mount& a, const Mount& b) { return a.guest.size() > b.guest.size(); });
}

void Vfs::add_virtual(std::string guest, Generator gen) { virtual_.emplace_back(normalise(guest), std::move(gen)); }

std::string Vfs::absolute(const std::string& guest_path) const {
    if (!guest_path.empty() && guest_path[0] == '/') return normalise(guest_path);
    return normalise(cwd_ + "/" + guest_path);
}

std::optional<std::filesystem::path> Vfs::to_host(const std::string& guest_path) const {
    std::string path = absolute(guest_path);
    for (const auto& m : mounts_) {
        if (!has_prefix(path, m.guest)) continue;
        std::string rest = path.substr(m.guest == "/" ? 0 : m.guest.size());
        std::filesystem::path host = m.host;
        if (!rest.empty() && rest != "/") host /= path_from_utf8(rest.substr(1));
        return host;
    }
    return std::nullopt;
}

std::optional<std::string> Vfs::to_guest(const std::filesystem::path& host_path) const {
    auto host = std::filesystem::weakly_canonical(host_path);
    for (const auto& m : mounts_) {
        auto root = std::filesystem::weakly_canonical(m.host);
        auto rel = host.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") continue;
        std::string tail = rel == "." ? "" : "/" + path_to_utf8(rel);
        return normalise(m.guest + tail);
    }
    return std::nullopt;
}

const Vfs::Generator* Vfs::virtual_file(const std::string& guest_path) const {
    std::string path = absolute(guest_path);
    for (const auto& [name, gen] : virtual_)
        if (name == path) return &gen;
    return nullptr;
}

}  // namespace rfu
