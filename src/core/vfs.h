#pragma once

#include "core/file_patch.h"

#include <filesystem>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
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
    // added later win over earlier ones (mod load order). "/sdcard/..." and
    // "/storage/emulated/0/..." are the same folder to it, as on Android.
    void overlay(std::string guest, std::filesystem::path host);
    // The game wrote guest file `guest_path` (opened it to write, renamed a file onto it, removed
    // it): from now on reading it takes the game's own file, not an overlay's. An overlay is made
    // before the game starts, from what the game had then (the CDN archive with mods' strings in
    // it), so once the game downloads newer content the overlay's copy is out of date: the next
    // launch makes it again from the new file. True the first time for a path an overlay has.
    bool written(const std::string& guest_path);

    // Normalises a guest path against the guest's working directory.
    std::string absolute(const std::string& guest_path) const;
    std::optional<std::filesystem::path> to_host(const std::string& guest_path) const;
    // Where reading `guest_path` goes: the topmost overlay that has it, else to_host().
    std::optional<std::filesystem::path> to_host_read(const std::string& guest_path) const;
    // The host directories that make up a guest directory, mount first, then each overlay
    // that has it, bottom to top (for listing it).
    std::vector<std::filesystem::path> layers(const std::string& guest_path) const;
    // The names of the mounts sitting directly in guest directory `guest_path` whose host side
    // exists (a single file mounted there, such as the OBB): its listing shows them too.
    std::vector<std::string> mounted_children(const std::string& guest_path) const;
    std::optional<std::string> to_guest(const std::filesystem::path& host_path) const;
    const Generator* virtual_file(const std::string& guest_path) const;
    // True for "/" and the directories above a mount point ("/storage" when
    // "/storage/emulated/0" is mounted): they exist for the guest without a host directory.
    bool is_mount_parent(const std::string& guest_path) const;

    // Reads of host file `host` (wherever the guest opens it) see it patched: the OBB with a
    // mod's files in it (--obb-patch).
    void patch(std::filesystem::path host, std::shared_ptr<const FilePatch> p) { patches_.emplace_back(std::move(host), std::move(p)); }
    std::shared_ptr<const FilePatch> patch_of(const std::filesystem::path& host) const {
        for (const auto& [h, p] : patches_)
            if (h == host) return p;
        return nullptr;
    }

    const std::string& cwd() const { return cwd_; }
    void set_cwd(std::string path) { cwd_ = absolute(path); }

private:
    struct Mount {
        std::string guest;
        std::filesystem::path host;
    };
    std::vector<Mount> mounts_;  // longest guest prefix first
    std::vector<Mount> overlays_;  // in the order added: the last one wins
    mutable std::mutex written_mutex_;
    std::set<std::string> written_;  // guest paths the game has written since it started
    std::vector<std::pair<std::string, Generator>> virtual_;
    std::vector<std::pair<std::filesystem::path, std::shared_ptr<const FilePatch>>> patches_;
    std::string cwd_ = "/";
};

}  // namespace rfu
