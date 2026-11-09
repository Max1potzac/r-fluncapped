#include "core/rtti.h"

#include "core/module.h"

#include <cstring>

namespace rfu {

namespace {

bool executable(const Module& m, GuestAddr a) {
    for (const auto& s : m.segments())
        if ((s.flags & 1) && a >= s.start && a < s.start + s.size) return true;
    return false;
}

// Every 8-byte-aligned slot in the module's data segments holding `value`.
std::vector<GuestAddr> slots_holding(const GuestMemory& mem, const Module& m, GuestAddr value) {
    std::vector<GuestAddr> out;
    for (const auto& s : m.segments()) {
        if (s.flags & 1) continue;
        GuestAddr lo = (s.start + 7) & ~7ull, hi = s.start + s.size;
        for (GuestAddr a = lo; a + 8 <= hi; a += 8)
            if (mem.read<std::uint64_t>(a) == value) out.push_back(a);
    }
    return out;
}

}  // namespace

std::optional<VTable> find_vtable(const GuestMemory& mem, const Module& m, const std::string& class_name) {
    // "Sexy::Slider", a class in a namespace, is "N4Sexy6SliderE".
    std::string mangled;
    std::size_t colon = class_name.find("::");
    if (colon == std::string::npos) {
        mangled = std::to_string(class_name.size()) + class_name;
    } else {
        mangled = "N";
        for (std::size_t from = 0;;) {
            std::size_t to = class_name.find("::", from);
            std::string part = class_name.substr(from, to == std::string::npos ? std::string::npos : to - from);
            mangled += std::to_string(part.size()) + part;
            if (to == std::string::npos) break;
            from = to + 2;
        }
        mangled += "E";
    }
    for (const auto& s : m.segments()) {  // the name is in .rodata, often in the code segment
        const char* data = reinterpret_cast<const char*>(mem.host(s.start));
        for (std::uint64_t i = 1; i + mangled.size() < s.size; ++i) {
            if (data[i] != mangled[0] || std::memcmp(data + i, mangled.data(), mangled.size() + 1) != 0) continue;
            // "14CollectableSun" but not "N4Sexy14CollectableSun" or "214CollectableSun"
            char before = data[i - 1];
            if (before != 0) continue;
            GuestAddr name = s.start + i;
            for (GuestAddr name_slot : slots_holding(mem, m, name)) {
                GuestAddr type_info = name_slot - 8;
                for (GuestAddr ti_slot : slots_holding(mem, m, type_info)) {
                    if (mem.read<std::uint64_t>(ti_slot - 8) != 0) continue;  // offset to top: the primary vtable
                    VTable vt;
                    vt.address = ti_slot + 8;
                    for (GuestAddr a = vt.address;; a += 8) {
                        GuestAddr fn = mem.read<std::uint64_t>(a);
                        if (!executable(m, fn)) break;
                        vt.functions.push_back(fn);
                    }
                    if (!vt.functions.empty()) return vt;
                }
            }
        }
    }
    return std::nullopt;
}

}  // namespace rfu
