#include "android/file_picker.h"

#include "android/apk.h"
#include "core/log.h"
#include "core/vfs.h"

#include <zlib.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <set>
#include <vector>


namespace rfu::android::file_picker {

namespace fs = std::filesystem;

namespace {

// SimpleDateFormat("yyyyMMdd_HHmmss") of the local time now.
std::string timestamp() {
    std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", &tm);
    return buf;
}

// FilePickerHelper.safeName: the last part of a name, with nothing that names a folder.
std::string safe_name(const std::string& name) {
    std::string s = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty() || s == "." || s == "..") return "imported.dat";
    return s;
}

bool copy_to(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) log::warn("file picker: couldn't copy {} to {}: {}", path_to_utf8(from), path_to_utf8(to), ec.message());
    return !ec;
}

void put16(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}

}  // namespace

std::string what(const std::string& rel) {
    if (rel.find("levels/") != std::string::npos) return "custom level";
    if (rel.find("pp.dat") != std::string::npos) return "save";
    if (rel.find("quest") != std::string::npos) return "quest progress";
    return "file";
}

std::string import_extension(const std::string& rel) {
    std::string ext = path_to_utf8(path_from_utf8(rel).extension());
    return ext == ".json" ? "json" : "";
}

std::optional<fs::path> inside(const fs::path& files, const std::string& rel) {
    fs::path out = files;
    for (const auto& part : path_from_utf8(rel).relative_path()) {
        if (part == "..") return std::nullopt;
        if (part != "." && !part.empty()) out /= part;
    }
    return out;
}

std::optional<fs::path> import_file(const fs::path& picked, const fs::path& files, const std::string& rel) {
    auto to = inside(files, rel);
    if (!to || !copy_to(picked, *to)) return std::nullopt;
    log::info("file picker: imported {} as {}", path_to_utf8(picked), path_to_utf8(*to));
    return to;
}

std::optional<fs::path> import_file_to_dir(const fs::path& picked, const fs::path& files, const std::string& rel) {
    auto dir = inside(files, rel);
    if (!dir) return std::nullopt;
    fs::path to = *dir / path_from_utf8(safe_name(path_to_utf8(picked.filename())));
    if (!copy_to(picked, to)) return std::nullopt;
    log::info("file picker: imported {} into {}", path_to_utf8(picked), path_to_utf8(*dir));
    return to;
}

std::optional<fs::path> import_zip(const fs::path& picked, const fs::path& files, const std::string& rel) {
    auto dir = inside(files, rel);
    if (!dir) return std::nullopt;
    try {
        ApkIndex zip(picked);
        std::error_code ec;
        fs::create_directories(*dir, ec);
        std::set<std::string> replaced;
        for (const auto& name : zip.names()) {
            auto to = inside(*dir, name);
            if (!to || name.empty()) {
                log::warn("file picker: skipping unsafe zip entry {}", name);
                continue;
            }
            // The first time a top-level folder turns up, the old one of that name goes.
            if (auto slash = name.find('/'); slash != std::string::npos && slash > 0)
                if (std::string top = name.substr(0, slash); replaced.insert(top).second) fs::remove_all(*dir / path_from_utf8(top), ec);
            if (name.back() == '/') {
                fs::create_directories(*to, ec);
                continue;
            }
            auto data = zip.read(name);
            if (!data) {
                log::warn("file picker: couldn't unpack {} from {}", name, path_to_utf8(picked));
                continue;
            }
            fs::create_directories(to->parent_path(), ec);
            std::ofstream(*to, std::ios::binary).write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
        }
    } catch (const std::exception& e) {
        log::warn("file picker: {} isn't a zip I can read: {}", path_to_utf8(picked), e.what());
        return std::nullopt;
    }
    log::info("file picker: unpacked {} into {}", path_to_utf8(picked), path_to_utf8(*dir));
    return dir;
}

std::string export_file_name(const std::string& rel) {
    fs::path p = path_from_utf8(rel);
    std::string stem = path_to_utf8(p.stem()), ext = path_to_utf8(p.extension());
    if (stem.empty()) return "export_" + timestamp() + ext;
    return stem + "_" + timestamp() + ext;
}

std::string export_folder_name(const std::string& rel) {
    std::string name = path_to_utf8(path_from_utf8(rel).filename());
    if (name.empty()) name = path_to_utf8(path_from_utf8(rel).parent_path().filename());
    return (name.empty() ? "export" : name) + "_" + timestamp() + ".zip";
}

bool export_file(const fs::path& files, const std::string& rel, const fs::path& to) {
    auto from = inside(files, rel);
    std::error_code ec;
    if (!from || !fs::is_regular_file(*from, ec)) {
        log::warn("file picker: nothing to export at {}", rel);
        return false;
    }
    if (!copy_to(*from, to)) return false;
    log::info("file picker: exported {} to {}", path_to_utf8(*from), path_to_utf8(to));
    return true;
}

bool export_folder(const fs::path& files, const std::string& rel, const fs::path& to) {
    auto from = inside(files, rel);
    std::error_code ec;
    if (!from || !fs::is_directory(*from, ec)) {
        log::warn("file picker: no folder to export at {}", rel);
        return false;
    }
    // A plain (stored) zip of the folder, its name as the top folder in the zip, as zipFolder does.
    std::string top = path_to_utf8(from->filename());
    std::vector<std::uint8_t> out, central;
    std::uint32_t count = 0;
    auto add = [&](const std::string& name, const std::vector<std::uint8_t>& data) {
        std::uint32_t crc = static_cast<std::uint32_t>(crc32(0, data.data(), static_cast<uInt>(data.size())));
        std::uint32_t offset = static_cast<std::uint32_t>(out.size()), size = static_cast<std::uint32_t>(data.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0x0800);  // names are UTF-8
        put16(out, 0);       // stored
        put32(out, 0);       // time, date
        put32(out, crc);
        put32(out, size);
        put32(out, size);
        put16(out, static_cast<std::uint32_t>(name.size()));
        put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0x0800);
        put16(central, 0);
        put32(central, 0);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, static_cast<std::uint32_t>(name.size()));
        put32(central, 0);  // extra and comment lengths
        put32(central, 0);  // disk, internal attributes
        put32(central, name.back() == '/' ? 0x10 : 0);
        put32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
        ++count;
    };
    add(top + "/", {});
    for (auto it = fs::recursive_directory_iterator(*from, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string name = top + "/" + path_to_utf8(it->path().lexically_relative(*from));
        if (it->is_directory(ec)) {
            add(name + "/", {});
            continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        add(name, data);
    }
    std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size()), cd_size = static_cast<std::uint32_t>(central.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054b50);
    put32(out, 0);
    put16(out, count);
    put16(out, count);
    put32(out, cd_size);
    put32(out, cd_offset);
    put16(out, 0);
    std::ofstream f(to, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!f) {
        log::warn("file picker: couldn't write {}", path_to_utf8(to));
        return false;
    }
    log::info("file picker: exported {} to {}", path_to_utf8(*from), path_to_utf8(to));
    return true;
}

}  // namespace rfu::android::file_picker
