#pragma once

// The subset of the ELF64 format the loader needs. Defined here rather than taken from
// <elf.h>, which Windows doesn't have.

#include <cstdint>

namespace rfu::elf {

struct Ehdr {
    std::uint8_t ident[16];
    std::uint16_t type;
    std::uint16_t machine;
    std::uint32_t version;
    std::uint64_t entry;
    std::uint64_t phoff;
    std::uint64_t shoff;
    std::uint32_t flags;
    std::uint16_t ehsize;
    std::uint16_t phentsize;
    std::uint16_t phnum;
    std::uint16_t shentsize;
    std::uint16_t shnum;
    std::uint16_t shstrndx;
};

struct Phdr {
    std::uint32_t type;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t paddr;
    std::uint64_t filesz;
    std::uint64_t memsz;
    std::uint64_t align;
};

struct Dyn {
    std::int64_t tag;
    std::uint64_t val;
};

struct Sym {
    std::uint32_t name;
    std::uint8_t info;
    std::uint8_t other;
    std::uint16_t shndx;
    std::uint64_t value;
    std::uint64_t size;
};

struct Rela {
    std::uint64_t offset;
    std::uint64_t info;
    std::int64_t addend;
};

constexpr std::uint16_t kTypeDyn = 3;
constexpr std::uint16_t kMachineAArch64 = 183;

constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint32_t kPtTls = 7;

constexpr std::int64_t kDtNull = 0;
constexpr std::int64_t kDtNeeded = 1;
constexpr std::int64_t kDtPltRelSz = 2;
constexpr std::int64_t kDtHash = 4;
constexpr std::int64_t kDtStrTab = 5;
constexpr std::int64_t kDtSymTab = 6;
constexpr std::int64_t kDtRela = 7;
constexpr std::int64_t kDtRelaSz = 8;
constexpr std::int64_t kDtStrSz = 10;
constexpr std::int64_t kDtSoName = 14;
constexpr std::int64_t kDtJmpRel = 23;
constexpr std::int64_t kDtInit = 12;
constexpr std::int64_t kDtFini = 13;
constexpr std::int64_t kDtInitArray = 25;
constexpr std::int64_t kDtFiniArray = 26;
constexpr std::int64_t kDtInitArraySz = 27;
constexpr std::int64_t kDtFiniArraySz = 28;
constexpr std::int64_t kDtGnuHash = 0x6ffffef5;

constexpr std::uint32_t kRelAbs64 = 257;
constexpr std::uint32_t kRelGlobDat = 1025;
constexpr std::uint32_t kRelJumpSlot = 1026;
constexpr std::uint32_t kRelRelative = 1027;

constexpr std::uint16_t kShnUndef = 0;
constexpr std::uint8_t kStbLocal = 0;
constexpr std::uint8_t kStbGlobal = 1;
constexpr std::uint8_t kStbWeak = 2;
constexpr std::uint8_t kSttFunc = 2;

inline std::uint32_t rela_sym(std::uint64_t info) { return static_cast<std::uint32_t>(info >> 32); }
inline std::uint32_t rela_type(std::uint64_t info) { return static_cast<std::uint32_t>(info); }
inline std::uint8_t sym_bind(std::uint8_t info) { return info >> 4; }
inline std::uint8_t sym_type(std::uint8_t info) { return info & 0xF; }

}  // namespace rfu::elf
