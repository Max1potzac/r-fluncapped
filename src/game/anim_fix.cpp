#include "game/anim_fix.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "core/rtti.h"
#include "runtime.h"

#include <array>
#include <memory>
#include <vector>

namespace rfu {

namespace {

// libPVZ2.so offsets, with the instructions expected there.
constexpr std::uint64_t kPlantTick = 0x12738f0;  // Plant's per-frame call (its vtable's slot 29)
constexpr std::array<std::uint32_t, 3> kPlantTickCode{0xD10343FF, 0xFD004BE8, 0xF9004FF7};
constexpr std::size_t kPlantTickSlot = 29;
// In it: if (game->board->running || plant->always_update) Plant::Update(plant)
constexpr std::uint64_t kRunningCheck = 0x1273d44;
constexpr std::array<std::uint32_t, 3> kRunningCheckCode{0xF944D508, 0x394C2D08, 0x35000068};
constexpr std::uint64_t kGame = 0x2593760;  // the object holding the board
constexpr std::uint64_t kBoard = 0x9a8, kBoardRunning = 0x30b;

// Plant
constexpr std::uint64_t kFramework = 0x80, kAlive = 0xc0, kState = 0x9c, kAlwaysUpdate = 0x128;
constexpr std::uint64_t kStatus = 0x1e0, kEffects = 0x198;  // as Plant::Update checks them (stunned...)
constexpr std::uint32_t kStateDying = 3;
constexpr std::size_t kSpecialUpdateSlot = 36;  // PlantFramework: the plant's own animation states

// Plant::FindTarget(range...): whether a zombie is in reach. While the fix runs a special update
// it answers no, or a Citron fires at the zombies still waiting off the lawn.
constexpr std::uint64_t kHasTarget = 0x1272850;
constexpr std::array<std::uint32_t, 3> kHasTargetCode{0xD100C3FF, 0xA9014FF4, 0xA9027BFD};

// The plants whose animations wait on their special update. Citron's states: 13 charging ->
// 10 ready, waiting for a target -> 11, 12 firing -> 13.
constexpr const char* kPlants[] = {"PlantCitron"};

bool code_matches(Runtime& rt, GuestAddr addr, const std::array<std::uint32_t, 3>& code) {
    for (std::size_t i = 0; i < code.size(); ++i)
        if (rt.mem().read<std::uint32_t>(addr + 4 * i) != code[i]) return false;
    return true;
}

}  // namespace

bool install_animation_fix(Runtime& rt, const Module& pvz2) {
    GuestAddr base = pvz2.base();
    auto plant = find_vtable(rt.mem(), pvz2, "Plant");
    bool known = plant && plant->functions.size() > kPlantTickSlot && plant->functions[kPlantTickSlot] == base + kPlantTick &&
                 code_matches(rt, base + kPlantTick, kPlantTickCode) && code_matches(rt, base + kRunningCheck, kRunningCheckCode);
    known = known && code_matches(rt, base + kHasTarget, kHasTargetCode);
    struct Fixed {
        GuestAddr vtable, special_update;
    };
    std::vector<Fixed> fixed;
    for (const char* name : kPlants) {
        auto vt = find_vtable(rt.mem(), pvz2, name);
        if (!vt || vt->functions.size() <= kSpecialUpdateSlot) known = false;
        else fixed.push_back({vt->address, vt->functions[kSpecialUpdateSlot]});
    }
    if (!known) {
        log::warn("frozen animations fix: this libPVZ2.so is a build it doesn't know; left off");
        return false;
    }

    // Set while a special update the game skipped runs (on the game's thread).
    auto forced = std::make_shared<bool>(false);
    hook(rt, base + kHasTarget, "frozen animations fix: no target", [forced](CallContext& c) {
        if (*forced) return_now(c, 0);
    });
    hook(rt, base + kPlantTick, "frozen animations fix", [&rt, base, fixed, forced](CallContext& c) {
        auto& mem = rt.mem();
        GuestAddr p = c.x(0);
        GuestAddr framework = mem.read<std::uint64_t>(p + kFramework);
        if (!framework) return;
        GuestAddr vtable = mem.read<std::uint64_t>(framework);
        const Fixed* f = nullptr;
        for (const auto& candidate : fixed)
            if (candidate.vtable == vtable) f = &candidate;
        if (!f) return;
        // Only when the game is about to skip the update itself.
        GuestAddr game = mem.read<std::uint64_t>(base + kGame);
        GuestAddr board = game ? mem.read<std::uint64_t>(game + kBoard) : 0;
        if (!board || mem.read<std::uint8_t>(board + kBoardRunning) || mem.read<std::uint8_t>(p + kAlwaysUpdate)) return;
        // The checks Plant::Update makes before the special update.
        if (!mem.read<std::uint8_t>(p + kAlive) || mem.read<std::uint32_t>(p + kState) == kStateDying) return;
        GuestAddr status = mem.read<std::uint64_t>(p + kStatus), effects = mem.read<std::uint64_t>(p + kEffects);
        if (!status || !effects || mem.read<std::uint8_t>(status + 1) || mem.read<std::uint8_t>(status + 6) || mem.read<std::uint8_t>(effects + 9)) return;
        *forced = true;
        rt.call(f->special_update, {framework});
        *forced = false;
    });
    log::info("frozen animations fix: on");
    return true;
}

}  // namespace rfu
