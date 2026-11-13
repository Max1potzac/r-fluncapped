#pragma once

// An APK's table of contents, for AssetManager.openFd(): where each stored (uncompressed)
// entry's bytes start in the file and how long they are. PvZ2 China ships its packages in the
// APK's assets/ folder, stored, and reads them straight out of the APK at those offsets, as
// Android lets it.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rfu::android {

class ApkIndex {
public:
    struct Entry {
        std::uint64_t offset = 0;  // of the entry's data in the APK
        std::uint64_t size = 0;    // uncompressed
        bool stored = false;       // false: compressed, so openFd() fails on it as on Android
        std::uint64_t compressed_size = 0;
        int method = 0;            // 0 stored, 8 deflate
    };

    // Reads the zip's central directory (and each local header, for the data offset). Throws
    // std::runtime_error if the file isn't a zip.
    explicit ApkIndex(const std::filesystem::path& apk);

    std::optional<Entry> find(const std::string& name) const;  // "assets/init.rsb.smf"
    std::size_t size() const { return entries_.size(); }

    // An entry's contents, inflated if need be; nullopt if it's missing or can't be read.
    std::optional<std::vector<std::uint8_t>> read(const std::string& name) const;

    // The DER bytes of the certificate the APK was signed with (v1 signature: the first certificate
    // in META-INF/<name>.RSA, .DSA or .EC), which is what PackageInfo.signatures[0] holds. Empty if
    // there's none.
    std::vector<std::uint8_t> signing_certificate() const;

private:
    std::filesystem::path path_;
    std::map<std::string, Entry> entries_;
};

}  // namespace rfu::android
