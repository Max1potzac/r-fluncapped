#include "game/hover.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "core/rtti.h"
#include "runtime.h"

#include <array>
#include <cstring>

namespace rfu {

namespace {

// libPVZ2.so offsets, with the first instructions expected there.
constexpr std::uint64_t kTouchMoved = 0x1556e1c;  // WidgetManager::TouchMoved(touch)
constexpr std::array<std::uint32_t, 3> kTouchMovedCode{0xD10283FF, 0x6D0523E9, 0xF90033F7};
constexpr std::uint64_t kWidgetAt = 0x163e82c;    // WidgetManager::GetWidgetAt(x, y, flags, bool* blocked, 0, 0)
constexpr std::array<std::uint32_t, 3> kWidgetAtCode{0xD10243FF, 0xA9036FFC, 0xA90467FA};

// WidgetManager fields
constexpr std::uint64_t kCaptured = 0xd0;  // the widget the touch went down on
constexpr std::uint64_t kFlagsMode = 0xc4, kFlagsAdd = 0xf0, kFlagsRemove = 0xf4, kFlagsBase = 0x260;
// Widget
constexpr std::uint64_t kDisabled = 0xb4;
constexpr std::uint64_t kGetAbsPos = 0xd0, kWidgetTouchMoved = 0x268;  // vtable offsets
// Touch: +0x10 x, +0x14 y, +0x18 x and +0x1c y where it went down, +0x30 phase; 0x38 bytes.
constexpr std::uint64_t kTouchSize = 0x38;

bool code_matches(Runtime& rt, GuestAddr addr, const std::array<std::uint32_t, 3>& code) {
    for (std::size_t i = 0; i < code.size(); ++i)
        if (rt.mem().read<std::uint32_t>(addr + 4 * i) != code[i]) return false;
    return true;
}

}  // namespace

bool install_hover_collect(Runtime& rt, const Module& pvz2) {
    GuestAddr touch_moved = pvz2.base() + kTouchMoved, widget_at = pvz2.base() + kWidgetAt;
    auto board = find_vtable(rt.mem(), pvz2, "Board");
    if (!code_matches(rt, touch_moved, kTouchMovedCode) || !code_matches(rt, widget_at, kWidgetAtCode) || !board) {
        log::warn("hover to collect: this libPVZ2.so is a build it doesn't know; left off");
        return false;
    }
    GuestAddr board_vtable = board->address;

    // Scratch memory for the calls, and GetAbsPos(widget) called with its Point result's address
    // in x1: it returns the Point through x8, which Runtime::call can't set.
    GuestAddr scratch = rt.heap().alloc_zeroed(0x100);
    GuestAddr blocked = scratch, pos = scratch + 0x10, touch = scratch + 0x20, abs_pos = scratch + 0x80;
    const std::uint32_t abs_pos_code[] = {
        0xAA0103E8,  // mov x8, x1
        0xF9400010,  // ldr x16, [x0]
        0xF9406A10,  // ldr x16, [x16, #0xd0]
        0xD61F0200,  // br x16
    };
    static_assert(kGetAbsPos == 0xd0);
    for (std::size_t i = 0; i < 4; ++i) rt.mem().write<std::uint32_t>(abs_pos + 4 * i, abs_pos_code[i]);

    hook(rt, touch_moved, "hover to collect", [=, &rt](CallContext& c) {
        GuestAddr wm = c.x(0), in = c.x(1);
        auto& mem = rt.mem();
        if (mem.read<std::uint64_t>(wm + kCaptured) != 0) return;  // a touch that went down: the game's own way
        std::int32_t x = mem.read<std::int32_t>(in + 0x10), y = mem.read<std::int32_t>(in + 0x14);
        std::uint32_t flags = mem.read<std::uint32_t>(wm + kFlagsBase);
        if (!mem.read<std::uint8_t>(wm + kFlagsMode))
            flags = (mem.read<std::uint32_t>(wm + kFlagsAdd) | flags) & ~mem.read<std::uint32_t>(wm + kFlagsRemove);
        mem.write<std::uint8_t>(blocked, 0);
        GuestAddr widget = rt.call(widget_at, {wm, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), flags, blocked, 0, 0});
        // Only the lawn: other widgets (buttons, the seed bank) react to moves in their own ways.
        if (!widget || mem.read<std::uint64_t>(widget) != board_vtable || mem.read<std::uint8_t>(widget + kDisabled)) return;
        rt.call(abs_pos, {widget, pos});
        std::int32_t px = mem.read<std::int32_t>(pos), py = mem.read<std::int32_t>(pos + 4);
        std::memcpy(mem.host(touch), mem.host(in), kTouchSize);
        mem.write<std::int32_t>(touch + 0x10, x - px);
        mem.write<std::int32_t>(touch + 0x14, y - py);
        mem.write<std::int32_t>(touch + 0x18, mem.read<std::int32_t>(in + 0x18) - px);
        mem.write<std::int32_t>(touch + 0x1c, mem.read<std::int32_t>(in + 0x1c) - py);
        GuestAddr fn = mem.read<std::uint64_t>(board_vtable + kWidgetTouchMoved);
        rt.call(fn, {widget, touch});
    });
    log::info("hover to collect: on");
    return true;
}

}  // namespace rfu
