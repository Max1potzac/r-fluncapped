#pragma once

#include "core/elf.h"
#include "core/guest_memory.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rfu {

// One Android shared library mapped into guest memory. Loading maps its segments and parses
// its dynamic section; relocation is the Linker's job, because it needs every module's symbols.
class Module {
public:
    struct Symbol {
        std::string name;
        GuestAddr value;  // absolute guest address (0 for undefined)
        std::uint64_t size;
        std::uint8_t bind;
        std::uint8_t type;
        bool defined;
    };

    struct Segment {
        GuestAddr start;
        std::uint64_t size;
        std::uint32_t flags;  // PF_X = 1, PF_W = 2, PF_R = 4
    };

    static Module load(GuestMemory& mem, const std::filesystem::path& path, GuestAddr base);

    const std::string& name() const { return name_; }      // file name, e.g. "libRFLib.so"
    const std::string& soname() const { return soname_; }  // DT_SONAME, e.g. "libRFLib64.so"
    GuestAddr base() const { return base_; }
    GuestAddr end() const { return end_; }
    bool contains(GuestAddr a) const { return a >= base_ && a < end_; }

    const std::vector<Segment>& segments() const { return segments_; }
    const std::vector<std::string>& needed() const { return needed_; }
    const std::vector<Symbol>& symbols() const { return symbols_; }
    const std::vector<elf::Rela>& relocations() const { return relocations_; }
    std::optional<GuestAddr> find_export(const std::string& name) const;

    // Program headers copied into guest memory, for dl_iterate_phdr.
    GuestAddr phdr_addr() const { return phdr_addr_; }
    std::uint16_t phdr_count() const { return phdr_count_; }

    // Initialisers in the order the Android linker runs them: DT_INIT, then DT_INIT_ARRAY.
    std::vector<GuestAddr> initializers(const GuestMemory& mem) const;
    std::vector<GuestAddr> finalizers(const GuestMemory& mem) const;

private:
    std::string name_;
    std::string soname_;
    GuestAddr base_ = 0;
    GuestAddr end_ = 0;
    GuestAddr phdr_addr_ = 0;
    std::uint16_t phdr_count_ = 0;
    std::vector<Segment> segments_;
    std::vector<std::string> needed_;
    std::vector<Symbol> symbols_;
    std::unordered_map<std::string, std::size_t> exports_;
    std::vector<elf::Rela> relocations_;
    GuestAddr init_ = 0, fini_ = 0;
    GuestAddr init_array_ = 0, fini_array_ = 0;
    std::uint64_t init_array_count_ = 0, fini_array_count_ = 0;
};

}  // namespace rfu
