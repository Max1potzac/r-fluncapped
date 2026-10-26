#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rfu {

// UTF-8 <-> std::filesystem::path, the same on Windows and elsewhere.
std::string path_to_utf8(const std::filesystem::path& p);
std::filesystem::path path_from_utf8(const std::string& s);

// The guest's view of the file system. Android paths are translated to host paths through
// mount points; a few files that only exist on Android (/proc/self/maps) are generated.
class Vfs {
public:
    using Generator = std::function<std::string()>;

    // Makes host directory `host` visible at guest path `guest` (e.g. "/sdcard/Android/obb").
    void mount(std::string guest, std::filesystem::path host);
    void add_virtual(std::string guest, Generator gen);
    // Lays host directory `host` over guest directory `guest`, read-only: a file the overlay
    // has is what the game reads there, and its files are added to directory listings. Writes
    // still go to the mount underneath, so the game's own files stay as they were. Overlays
    // added later win over earlier ones (mod load order).
    void overlay(std::string guest, std::filesystem::path host);

    // Normalises a guest path against the guest's working directory.
    std::string absolute(const std::string& guest_path) const;
    std::optional<std::filesystem::path> to_host(const std::string& guest_path) const;
    // Where reading `guest_path` goes: the topmost overlay that has it, else to_host().
    std::optional<std::filesystem::path> to_host_read(const std::string& guest_path) const;
    // The host directories that make up a guest directory, mount first, then each overlay
    // that has it, bottom to top (for listing it).
    std::vector<std::filesystem::path> layers(const std::string& guest_path) const;
    std::optional<std::string> to_guest(const std::filesystem::path& host_path) const;
    const Generator* virtual_file(const std::string& guest_path) const;
    // True for "/" and the directories above a mount point ("/storage" when
    // "/storage/emulated/0" is mounted): they exist for the guest without a host directory.
    bool is_mount_parent(const std::string& guest_path) const;

    const std::string& cwd() const { return cwd_; }
    void set_cwd(std::string path) { cwd_ = absolute(path); }

private:
    struct Mount {
        std::string guest;
        std::filesystem::path host;
    };
    std::vector<Mount> mounts_;  // longest guest prefix first
    std::vector<Mount> overlays_;  // in the order added: the last one wins
    std::vector<std::pair<std::string, Generator>> virtual_;
    std::string cwd_ = "/";
};

}  // namespace rfu
