#pragma once

// A file patch: a host file as the guest sees it with bytes put over parts of it, and maybe more
// after its end. The launcher makes one for the OBB (--obb-patch) when mods replace files inside
// it (a localization's fonts): the resource groups that hold them are written again, after the
// end of the OBB, and the RSB header is changed to point at the new copies. The OBB itself stays
// as it is.
//
// The file (written by the launcher's ObbPatch):
//   "RFUPATCH", u32 version (1), u32 count, u64 base size, u64 patched size,
//   then count times: u64 offset, u64 length, the bytes.
// All little-endian. The base size is the OBB's, so a patch made for another OBB is refused.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rfu {

struct FilePatch {
    struct Write {
        std::uint64_t offset;
        std::string bytes;
    };
    std::uint64_t base_size = 0;  // the host file's size
    std::uint64_t size = 0;       // the patched file's: the base's, or more
    std::vector<Write> writes;    // by offset, none overlapping

    using ReadBase = std::function<std::size_t(std::uint64_t offset, void* out, std::size_t n)>;
    // Copies up to n bytes from `pos` of the patched file into `out` and returns how many:
    // the base file's bytes (read through read_base; zeros past its end), with the writes over them.
    std::size_t read(std::uint64_t pos, void* out, std::size_t n, const ReadBase& read_base) const;
};

// Reads a patch file. Null, with error set, when it isn't one (or is cut short).
std::shared_ptr<FilePatch> load_file_patch(const std::filesystem::path& file, std::string& error);
// The same, from its bytes.
std::shared_ptr<FilePatch> parse_file_patch(const std::string& data, std::string& error);

}  // namespace rfu
