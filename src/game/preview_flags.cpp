#include "game/preview_flags.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rfu {

namespace {

// libPVZ2.so offsets, with the first instructions expected there.
struct Known {
    std::uint64_t offset;
    std::array<std::uint32_t, 3> code;
};
// PlaceStreetZombies (the intro's, on its street layout), at its end: the street in [sp, #0x18],
// the grid of spots at sp + 0x90 (9 columns of 10 ints: 5 blocked, 3 a zombie, 1 beside one).
constexpr Known kStreetPlaced{0x6f1eb8, {0xF9402BE0, 0xB4000060, 0xF9002FE0}};
constexpr std::uint64_t kStreetAt = 0x18, kGridAt = 0x90;
constexpr int kColumns = 9, kRows = 10, kRowStride = 40;
constexpr std::int32_t kZombieSpot = 3;
// (street, const RtWeakPtr<ZombieType>&, column, row): adds one street zombie at a spot, the
// column counting from the street's left edge (a negative one further left).
constexpr Known kPlaceAt{0x6f2918, {0xD10143FF, 0xF9000BF7, 0xA90257F6}};
// The board; its wave manager (or null); the manager's flag count; its properties.
constexpr Known kBoard{0xce4d8c, {0xB000C168, 0xF9448D08, 0xF9400108}};
constexpr Known kWaveManager{0xaa1ba8, {0xF81E0FF3, 0xA9017BFD, 0x910043FD}};
constexpr Known kFlagCount{0x8d6494, {0xF81C0FF7, 0xA90157F6, 0xA9024FF4}};
constexpr Known kWaveProps{0x8d330c, {0xD10103FF, 0xF9000BF5, 0xA9024FF4}};
constexpr std::uint64_t kSuppressFlagZombie = 0x65;  // bool, in WaveManagerProperties
// The board's modules (at board + 0x3e0), looked up by RtClass: the StageModule's, and the
// stage's properties (a handle in the module), whose FlagZombieTypeName is a std::string.
constexpr Known kModuleOf{0xa84a1c, {0xD10183FF, 0xF9000BF9, 0xA9025FF8}};
constexpr Known kStageModuleClass{0x8201fc, {0xF81D0FF5, 0xA9014FF4, 0xA9027BFD}};
constexpr Known kStagePropsClass{0x81d9b0, {0xF81D0FF5, 0xA9014FF4, 0xA9027BFD}};
constexpr std::uint64_t kModules = 0x3e0, kStageProps = 0x10, kFlagZombieTypeName = 0xe0;
constexpr std::uint64_t kIsASlot = 0x20;  // RtObject::IsA in the vtable
// RtWeakPtr: parse "RTID(name@sheet)" (x0 a std::string, the handle through x8), and look one up,
// as plant_behaviours does.
constexpr Known kParseRtid{0x14daf88, {0xD104C3FF, 0xA90D6FFC, 0xA90E67FA}};
constexpr Known kRegistry{0x14ead90, {0xD10143FF, 0xF90013F5, 0xA9034FF4}};
constexpr Known kHandleRef{0x14f39cc, {0xA9BD57F6, 0xA9014FF4, 0xA9027BFD}};
constexpr Known kRefIsNull{0x14d926c, {0xF9400008, 0xF100011F, 0x1A9F17E0}};
constexpr Known kRefType{0x14d9178, {0xF9400008, 0xD371F900, 0xD65F03C0}};
constexpr Known kTypeTable{0x14f31a4, {0x91006008, 0x7213003F, 0x9A800109}};
constexpr Known kRefResolve{0x14ede00, {0xF81C0FF7, 0xA90157F6, 0xA9024FF4}};
constexpr Known kRefFree{0x14d9154, {0xA9BF7BFD, 0x910003FD, 0xAA1F03E1}};

constexpr int kAhead = 3;      // columns between the leftmost zombie and the flags
constexpr int kPerColumn = 5;  // flag zombies to a column, before the next one further left
constexpr int kMaxFlags = 20;

class PreviewFlags : public std::enable_shared_from_this<PreviewFlags> {
public:
    PreviewFlags(Runtime& rt, GuestAddr base) : rt_(rt), base_(base) { ref_ = rt.heap().alloc_zeroed(0x20); }

    void install() {
        auto self = shared_from_this();
        hook(rt_, base_ + kStreetPlaced.offset, "preview flags", [self](CallContext& c) { self->on_street(c); });
    }

private:
    std::uint64_t call(const Known& k, const std::vector<std::uint64_t>& args = {}, std::optional<std::uint64_t> x8 = std::nullopt) {
        return rt_.call(base_ + k.offset, args, x8);
    }

    // A libc++ std::string's text: short ones inline (size << 1 in the first byte), long ones
    // through a pointer (capacity | 1, size, data).
    std::string read_string(GuestAddr s) const {
        auto& mem = rt_.mem();
        std::uint8_t first = mem.read<std::uint8_t>(s);
        std::size_t size = (first & 1) ? mem.read<std::uint64_t>(s + 8) : first >> 1;
        GuestAddr data = (first & 1) ? mem.read<std::uint64_t>(s + 16) : s + 1;
        if (size > 256) return {};
        std::string out(size, '\0');
        for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>(mem.read<std::uint8_t>(data + i));
        return out;
    }

    // The object a handle names, or 0.
    GuestAddr resolve(GuestAddr handle) {
        GuestAddr registry = call(kRegistry);
        call(kHandleRef, {registry, handle}, ref_);
        GuestAddr object = 0;
        if (!(call(kRefIsNull, {ref_}) & 1)) {
            std::uint64_t type = call(kRefType, {ref_});
            GuestAddr table = call(kTypeTable, {registry, type & 0xffffffff});
            if (table) object = call(kRefResolve, {table, ref_});
        }
        call(kRefFree, {ref_});
        return object;
    }

    // A zombie type's handle, made the first time (and kept: the game copies it).
    GuestAddr zombie_type(const std::string& name) {
        auto found = types_.find(name);
        if (found != types_.end()) return found->second;
        auto& mem = rt_.mem();
        std::string rtid = "RTID(" + name + "@ZombieTypes)";
        // A libc++ std::string, long: capacity | 1, size, data.
        GuestAddr text = rt_.heap().alloc_zeroed(rtid.size() + 1), str = rt_.heap().alloc_zeroed(24);
        std::memcpy(mem.host(text), rtid.data(), rtid.size());
        mem.write<std::uint64_t>(str, (rtid.size() + 1) | 1);
        mem.write<std::uint64_t>(str + 8, rtid.size());
        mem.write<std::uint64_t>(str + 16, text);
        GuestAddr handle = rt_.heap().alloc_zeroed(8);
        call(kParseRtid, {str}, handle);
        if (!resolve(handle)) handle = 0;
        types_[name] = handle;
        return handle;
    }

    // The stage's flag zombie, empty when there's none to show.
    std::string flag_zombie(GuestAddr board) {
        GuestAddr stage = call(kModuleOf, {rt_.mem().read<std::uint64_t>(board + kModules), call(kStageModuleClass)});
        if (!stage) return {};
        GuestAddr props = resolve(stage + kStageProps);
        if (!props || !is_a(props, call(kStagePropsClass))) return {};
        return read_string(props + kFlagZombieTypeName);
    }

    bool is_a(GuestAddr object, GuestAddr cls) {
        GuestAddr fn = rt_.mem().read<std::uint64_t>(rt_.mem().read<std::uint64_t>(object) + kIsASlot);
        return rt_.call(fn, {object, cls}) & 1;
    }

    void on_street(CallContext& c) {
        auto& mem = rt_.mem();
        GuestAddr street = mem.read<std::uint64_t>(c.sp() + kStreetAt);
        GuestAddr board = call(kBoard);
        GuestAddr waves = board ? call(kWaveManager, {board}) : 0;
        if (!street || !waves) return;
        int flags = static_cast<std::int32_t>(call(kFlagCount, {waves}));
        GuestAddr wave_props = call(kWaveProps, {waves});
        if (flags <= 0 || !wave_props || mem.read<std::uint8_t>(wave_props + kSuppressFlagZombie)) return;
        std::string name = flag_zombie(board);
        GuestAddr type = name.empty() ? 0 : zombie_type(name);
        if (!type) {
            log::warn("preview flags: the level's flag zombie ({}) isn't a zombie type it can find", name.empty() ? "none" : name);
            return;
        }

        // The leftmost column with a zombie in it; the street's fourth when there's none.
        int leftmost = 3;
        GuestAddr grid = c.sp() + kGridAt;
        for (int x = kColumns - 1; x >= 0; --x)
            for (int y = 0; y < kRows; ++y)
                if (mem.read<std::int32_t>(grid + x * kRowStride + y * 4) == kZombieSpot) leftmost = x;

        flags = std::min(flags, kMaxFlags);
        for (int i = 0; i < flags; ++i) {
            int column = i / kPerColumn, in_column = std::min(kPerColumn, flags - column * kPerColumn);
            auto x = static_cast<std::int64_t>(leftmost - kAhead * (column + 1));
            auto y = static_cast<std::int64_t>(preview_flag_row(i % kPerColumn, in_column));
            call(kPlaceAt, {street, type, static_cast<std::uint64_t>(x), static_cast<std::uint64_t>(y)});
        }
        log::info("preview flags: {} {} ahead of the street's zombies", flags, name);
    }

    Runtime& rt_;
    GuestAddr base_;
    GuestAddr ref_ = 0;
    std::map<std::string, GuestAddr> types_;
};

bool code_matches(Runtime& rt, GuestAddr addr, const std::array<std::uint32_t, 3>& code) {
    for (std::size_t i = 0; i < code.size(); ++i)
        if (rt.mem().read<std::uint32_t>(addr + 4 * i) != code[i]) return false;
    return true;
}

}  // namespace

int preview_flag_row(int index, int count) {
    if (count <= 0) return kRows / 2;
    return std::min(kRows - 1, (2 * index + 1) * kRows / (2 * count));
}

bool install_preview_flags(Runtime& rt, const Module& pvz2) {
    GuestAddr base = pvz2.base();
    for (const Known* k : {&kStreetPlaced, &kPlaceAt, &kBoard, &kWaveManager, &kFlagCount, &kWaveProps, &kModuleOf, &kStageModuleClass,
                           &kStagePropsClass, &kParseRtid, &kRegistry, &kHandleRef, &kRefIsNull, &kRefType, &kTypeTable, &kRefResolve, &kRefFree})
        if (!code_matches(rt, base + k->offset, k->code)) {
            log::warn("preview flags: this libPVZ2.so is a build it doesn't know ({:#x} differs); left off", k->offset);
            return false;
        }
    std::make_shared<PreviewFlags>(rt, base)->install();
    return true;
}

}  // namespace rfu
