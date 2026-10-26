#include "core/module.h"

#include "core/log.h"

#include <fmt/ranges.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace rfu {

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
}

template <typename T>
T at(const std::vector<std::uint8_t>& file, std::uint64_t offset) {
    if (offset + sizeof(T) > file.size()) throw std::runtime_error("truncated ELF");
    T v;
    std::memcpy(&v, file.data() + offset, sizeof(T));
    return v;
}

// Number of dynamic symbols, from DT_HASH (nchain) or by walking DT_GNU_HASH to the last chain.
std::uint64_t count_symbols(const GuestMemory& mem, GuestAddr hash, GuestAddr gnu_hash) {
    if (hash) return mem.read<std::uint32_t>(hash + 4);
    if (!gnu_hash) return 0;
    auto nbuckets = mem.read<std::uint32_t>(gnu_hash);
    auto symoffset = mem.read<std::uint32_t>(gnu_hash + 4);
    auto bloom_size = mem.read<std::uint32_t>(gnu_hash + 8);
    GuestAddr buckets = gnu_hash + 16 + 8ull * bloom_size;
    GuestAddr chains = buckets + 4ull * nbuckets;
    std::uint32_t last = 0;
    for (std::uint32_t i = 0; i < nbuckets; ++i) last = std::max(last, mem.read<std::uint32_t>(buckets + 4ull * i));
    if (last < symoffset) return symoffset;
    while (!(mem.read<std::uint32_t>(chains + 4ull * (last - symoffset)) & 1)) ++last;
    return last + 1;
}

}  // namespace

Module Module::load(GuestMemory& mem, const std::filesystem::path& path, GuestAddr base) {
    auto file = read_file(path);
    auto eh = at<elf::Ehdr>(file, 0);
    if (std::memcmp(eh.ident, "\x7f" "ELF", 4) != 0 || eh.ident[4] != 2)
        throw std::runtime_error(path.string() + " is not a 64-bit ELF file");
    if (eh.machine != elf::kMachineAArch64 || eh.type != elf::kTypeDyn)
        throw std::runtime_error(path.string() + " is not an AArch64 shared library");

    Module m;
    m.name_ = path.filename().string();
    m.base_ = base;

    std::vector<elf::Phdr> phdrs;
    for (std::uint16_t i = 0; i < eh.phnum; ++i) phdrs.push_back(at<elf::Phdr>(file, eh.phoff + std::uint64_t(i) * eh.phentsize));

    GuestAddr dynamic = 0;
    for (const auto& ph : phdrs) {
        if (ph.type == elf::kPtLoad) {
            GuestAddr seg = base + ph.vaddr;
            mem.commit(seg, ph.memsz);
            if (ph.filesz) {
                if (ph.offset + ph.filesz > file.size()) throw std::runtime_error("segment past end of file");
                mem.write_bytes(seg, file.data() + ph.offset, ph.filesz);
            }
            std::memset(mem.host(seg + ph.filesz), 0, ph.memsz - ph.filesz);
            m.end_ = std::max(m.end_, seg + ph.memsz);
            m.segments_.push_back({seg, ph.memsz, ph.flags});
        } else if (ph.type == elf::kPtDynamic) {
            dynamic = base + ph.vaddr;
        } else if (ph.type == elf::kPtTls) {
            log::warn("{}: has a TLS segment, which isn't supported yet", m.name_);
        }
    }
    if (!dynamic) throw std::runtime_error(m.name_ + " has no dynamic section");

    // Keep a copy of the program headers right after the image for dl_iterate_phdr.
    m.phdr_addr_ = (m.end_ + 0xFFF) & ~0xFFFull;
    m.phdr_count_ = eh.phnum;
    mem.commit(m.phdr_addr_, sizeof(elf::Phdr) * phdrs.size());
    mem.write_bytes(m.phdr_addr_, phdrs.data(), sizeof(elf::Phdr) * phdrs.size());
    m.end_ = m.phdr_addr_ + sizeof(elf::Phdr) * phdrs.size();

    GuestAddr strtab = 0, symtab = 0, hash = 0, gnu_hash = 0, rela = 0, jmprel = 0;
    std::uint64_t rela_size = 0, jmprel_size = 0;
    std::vector<std::uint64_t> needed_offsets;
    std::optional<std::uint64_t> soname_offset;
    for (GuestAddr d = dynamic;; d += sizeof(elf::Dyn)) {
        auto dyn = mem.read<elf::Dyn>(d);
        if (dyn.tag == elf::kDtNull) break;
        switch (dyn.tag) {
        case elf::kDtNeeded: needed_offsets.push_back(dyn.val); break;
        case elf::kDtSoName: soname_offset = dyn.val; break;
        case elf::kDtStrTab: strtab = base + dyn.val; break;
        case elf::kDtSymTab: symtab = base + dyn.val; break;
        case elf::kDtHash: hash = base + dyn.val; break;
        case elf::kDtGnuHash: gnu_hash = base + dyn.val; break;
        case elf::kDtRela: rela = base + dyn.val; break;
        case elf::kDtRelaSz: rela_size = dyn.val; break;
        case elf::kDtJmpRel: jmprel = base + dyn.val; break;
        case elf::kDtPltRelSz: jmprel_size = dyn.val; break;
        case elf::kDtInit: m.init_ = base + dyn.val; break;
        case elf::kDtFini: m.fini_ = base + dyn.val; break;
        case elf::kDtInitArray: m.init_array_ = base + dyn.val; break;
        case elf::kDtInitArraySz: m.init_array_count_ = dyn.val / 8; break;
        case elf::kDtFiniArray: m.fini_array_ = base + dyn.val; break;
        case elf::kDtFiniArraySz: m.fini_array_count_ = dyn.val / 8; break;
        default: break;
        }
    }
    for (auto off : needed_offsets) m.needed_.push_back(mem.read_cstr(strtab + off));
    m.soname_ = soname_offset ? mem.read_cstr(strtab + *soname_offset) : m.name_;

    std::uint64_t nsyms = count_symbols(mem, hash, gnu_hash);
    for (std::uint64_t i = 0; i < nsyms; ++i) {
        auto s = mem.read<elf::Sym>(symtab + i * sizeof(elf::Sym));
        Symbol sym;
        sym.name = mem.read_cstr(strtab + s.name);
        sym.defined = s.shndx != elf::kShnUndef;
        sym.value = sym.defined ? base + s.value : 0;
        sym.size = s.size;
        sym.bind = elf::sym_bind(s.info);
        sym.type = elf::sym_type(s.info);
        if (sym.defined && !sym.name.empty() && sym.bind != elf::kStbLocal) m.exports_.emplace(sym.name, m.symbols_.size());
        m.symbols_.push_back(std::move(sym));
    }

    for (std::uint64_t off = 0; off < rela_size; off += sizeof(elf::Rela)) m.relocations_.push_back(mem.read<elf::Rela>(rela + off));
    for (std::uint64_t off = 0; off < jmprel_size; off += sizeof(elf::Rela)) m.relocations_.push_back(mem.read<elf::Rela>(jmprel + off));

    log::info("loaded {} at {:#x}-{:#x}: {} symbols, {} relocations, needs [{}]", m.name_, m.base_, m.end_, m.symbols_.size(),
              m.relocations_.size(), fmt::join(m.needed_, ", "));
    return m;
}

std::optional<GuestAddr> Module::find_export(const std::string& name) const {
    auto it = exports_.find(name);
    if (it == exports_.end()) return std::nullopt;
    return symbols_[it->second].value;
}

std::vector<GuestAddr> Module::initializers(const GuestMemory& mem) const {
    std::vector<GuestAddr> out;
    if (init_) out.push_back(init_);
    for (std::uint64_t i = 0; i < init_array_count_; ++i) {
        auto f = mem.read<GuestAddr>(init_array_ + 8 * i);
        if (f && f != ~0ull) out.push_back(f);
    }
    return out;
}

std::vector<GuestAddr> Module::finalizers(const GuestMemory& mem) const {
    std::vector<GuestAddr> out;
    for (std::uint64_t i = fini_array_count_; i-- > 0;) {
        auto f = mem.read<GuestAddr>(fini_array_ + 8 * i);
        if (f && f != ~0ull) out.push_back(f);
    }
    if (fini_) out.push_back(fini_);
    return out;
}

}  // namespace rfu
