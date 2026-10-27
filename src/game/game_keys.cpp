#include "game/game_keys.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "core/rtti.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
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
constexpr std::uint64_t kApp = 0x25a4618;  // LawnApp* (the app singleton)
constexpr Known kAppTouchBegan{0x14e8568, {0xF941C000, 0x1401B9C9, 0xF941C000}};  // LawnApp::TouchBegan(touch): wm->TouchBegan
constexpr Known kAppTouchEnded{0x14e8570, {0xF941C000, 0x1401BAB4, 0xF941C000}};
constexpr Known kScreenRect{0x5c1788, {0xD10143FF, 0xA9015FF8, 0xA90257F6}};  // UI object: its Rect on screen, through x8
// Handles: the registry, a handle's object pointer (through x8), and the calls that look it up.
constexpr Known kRegistry{0x14ead90, {0xD10143FF, 0xF90013F5, 0xA9034FF4}};
constexpr Known kHandleRef{0x14f39cc, {0xA9BD57F6, 0xA9014FF4, 0xA9027BFD}};
constexpr Known kRefIsNull{0x14d926c, {0xF9400008, 0xF100011F, 0x1A9F17E0}};
constexpr Known kRefType{0x14d9178, {0xF9400008, 0xD371F900, 0xD65F03C0}};
constexpr Known kTypeTable{0x14f31a4, {0x91006008, 0x7213003F, 0x9A800109}};
constexpr Known kRefResolve{0x14ede00, {0xF81C0FF7, 0xA90157F6, 0xA9024FF4}};
constexpr Known kRefFree{0x14d9154, {0xA9BF7BFD, 0x910003FD, 0xAA1F03E1}};

constexpr std::size_t kUpdateSlot = 13;  // the UI objects' per-frame update, in the vtable
constexpr std::uint64_t kChildren = 0x78;  // UI object: std::vector of its children's handles
constexpr std::uint64_t kCaptured = 0xd0, kWidgetManager = 0x380;  // WidgetManager in LawnApp, its captured widget
constexpr int kPointer = 98;  // the touch's id, clear of the mouse (1), hover (99) and tap keybinds (100 and up)
constexpr auto kStale = std::chrono::seconds(1);  // an instance not updated for this long is gone

struct Rect {
    std::int32_t x, y, w, h;
};

struct Seen {
    std::atomic<GuestAddr> object{0};
    std::atomic<std::int64_t> when{0};  // steady_clock ticks
};

std::int64_t now_ticks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

// The single buttons a game key presses, by their UI classes.
struct ButtonClass {
    platform::KeyBind::Action action;
    const char* name;
};
constexpr ButtonClass kButtons[] = {
    {platform::KeyBind::Action::Shovel, "ShovelUI"},
    {platform::KeyBind::Action::PlantFood, "PlantfoodUI"},
    {platform::KeyBind::Action::FastForward, "FastForwardButton"},
    {platform::KeyBind::Action::Pause, "PauseButton"},
};
constexpr std::size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

class GameKeys : public std::enable_shared_from_this<GameKeys> {
public:
    GameKeys(Runtime& rt, const Module& pvz2) : rt_(rt), base_(pvz2.base()) {}

    bool install(const Module& pvz2) {
        for (const Known* k : {&kAppTouchBegan, &kAppTouchEnded, &kScreenRect, &kRegistry, &kHandleRef, &kRefIsNull, &kRefType, &kTypeTable, &kRefResolve, &kRefFree})
            for (std::size_t i = 0; i < k->code.size(); ++i)
                if (rt_.mem().read<std::uint32_t>(base_ + k->offset + 4 * i) != k->code[i]) return false;
        auto bank = find_vtable(rt_.mem(), pvz2, "SeedBankNew");
        auto packet = find_vtable(rt_.mem(), pvz2, "SeedPacket");
        if (!bank || !packet || bank->functions.size() <= kUpdateSlot) return false;
        bank_vtable_ = bank->address;
        packet_vtable_ = packet->address;
        std::array<std::optional<VTable>, kButtonCount> buttons;
        for (std::size_t i = 0; i < kButtonCount; ++i) {
            buttons[i] = find_vtable(rt_.mem(), pvz2, kButtons[i].name);
            if (!buttons[i] || buttons[i]->functions.size() <= kUpdateSlot) return false;
            button_vtables_[i] = buttons[i]->address;
        }

        // Scratch memory: the touch, a Rect, a handle's reference, and two stubs that call a
        // function returning through x8 (which Runtime::call can't set) with x8 = the last argument.
        scratch_ = rt_.heap().alloc_zeroed(0x100);
        touch_ = scratch_;
        rect_ = scratch_ + 0x40;
        ref_ = scratch_ + 0x50;
        rect_stub_ = scratch_ + 0x80;
        ref_stub_ = scratch_ + 0xa0;
        write_stub(rect_stub_, 0xAA0103E8, base_ + kScreenRect.offset);  // mov x8, x1
        write_stub(ref_stub_, 0xAA0203E8, base_ + kHandleRef.offset);    // mov x8, x2

        watch(bank->functions[kUpdateSlot], bank_vtable_, bank_, "game keys: seed bank");
        for (std::size_t i = 0; i < kButtonCount; ++i)
            watch(buttons[i]->functions[kUpdateSlot], button_vtables_[i], buttons_[i], std::string("game keys: ") + kButtons[i].name);
        return true;
    }

    void key(const platform::GameKeyInput& k) {
        if (!k.down) {
            if (pressed_) send(3);
            pressed_ = false;
            return;
        }
        if (pressed_) send(3);  // another game key still down: lift that finger first
        pressed_ = false;
        auto target = k.action == platform::KeyBind::Action::Seed ? packet_rect(k.slot) : button_rect(k.action);
        if (!target) return;
        GuestAddr app = rt_.mem().read<std::uint64_t>(base_ + kApp);
        if (!app) return;
        GuestAddr wm = rt_.mem().read<std::uint64_t>(app + kWidgetManager);
        if (!wm || rt_.mem().read<std::uint64_t>(wm + kCaptured)) return;  // a finger or the mouse is down already
        log::debug("game keys: {} at {}, {} ({}x{})", platform::action_name(k.action), target->x, target->y, target->w, target->h);
        x_ = target->x + target->w / 2;
        y_ = target->y + target->h / 2;
        pressed_ = true;
        send(0);
    }

private:
    void write_stub(GuestAddr at, std::uint32_t mov_x8, GuestAddr target) {
        const std::uint32_t code[] = {mov_x8, 0x58000070 /* ldr x16, #12 */, 0xD61F0200 /* br x16 */, 0xD503201F /* nop */};
        for (std::size_t i = 0; i < 4; ++i) rt_.mem().write<std::uint32_t>(at + 4 * i, code[i]);
        rt_.mem().write<std::uint64_t>(at + 0x10, target);
        rt_.invalidate_code(at, 0x18);
    }

    void watch(GuestAddr update, GuestAddr vtable, Seen& seen, const std::string& name) {
        hook(rt_, update, name, [self = shared_from_this(), vtable, &seen](CallContext& c) {
            auto& rt = self->rt_;
            GuestAddr object = c.x(0);
            if (rt.mem().read<std::uint64_t>(object) != vtable) return;
            seen.object = object;
            seen.when = now_ticks();
        });
    }

    // The instance last updated, if it's still about and still what it was.
    GuestAddr live(const Seen& seen, GuestAddr vtable) const {
        GuestAddr object = seen.object;
        auto age = std::chrono::steady_clock::duration(now_ticks() - seen.when);
        if (!object || age > kStale || rt_.mem().read<std::uint64_t>(object) != vtable) return 0;
        return object;
    }

    Rect screen_rect(GuestAddr object) {
        rt_.call(rect_stub_, {object, rect_});
        return {rt_.mem().read<std::int32_t>(rect_), rt_.mem().read<std::int32_t>(rect_ + 4), rt_.mem().read<std::int32_t>(rect_ + 8),
                rt_.mem().read<std::int32_t>(rect_ + 12)};
    }

    // The object a handle names, or 0, looked up the way the game's own code does.
    GuestAddr resolve(GuestAddr handle) {
        GuestAddr registry = rt_.call(base_ + kRegistry.offset);
        rt_.call(ref_stub_, {registry, handle, ref_});
        GuestAddr object = 0;
        if (!(rt_.call(base_ + kRefIsNull.offset, {ref_}) & 1)) {
            std::uint64_t type = rt_.call(base_ + kRefType.offset, {ref_});
            GuestAddr table = rt_.call(base_ + kTypeTable.offset, {registry, type & 0xffffffff});
            if (table) object = rt_.call(base_ + kRefResolve.offset, {table, ref_});
        }
        rt_.call(base_ + kRefFree.offset, {ref_});
        return object;
    }

    std::optional<Rect> button_rect(platform::KeyBind::Action action) {
        std::size_t i = 0;
        while (i < kButtonCount && kButtons[i].action != action) ++i;
        if (i == kButtonCount) return std::nullopt;
        GuestAddr button = live(buttons_[i], button_vtables_[i]);
        if (!button) return std::nullopt;
        Rect r = screen_rect(button);
        if (r.w <= 0 || r.h <= 0) return std::nullopt;
        return r;
    }

    // The slot-th packet (from 1), top to bottom, then left to right.
    std::optional<Rect> packet_rect(int slot) {
        GuestAddr bank = live(bank_, bank_vtable_);
        if (!bank) return std::nullopt;
        auto& mem = rt_.mem();
        GuestAddr begin = mem.read<std::uint64_t>(bank + kChildren), end = mem.read<std::uint64_t>(bank + kChildren + 8);
        if (end < begin || end - begin > 64 * 8) return std::nullopt;
        std::vector<Rect> packets;
        for (GuestAddr h = begin; h < end; h += 8) {
            GuestAddr packet = resolve(h);
            if (!packet || mem.read<std::uint64_t>(packet) != packet_vtable_) continue;
            Rect r = screen_rect(packet);
            if (r.w > 0 && r.h > 0) packets.push_back(r);
        }
        if (slot < 1 || static_cast<std::size_t>(slot) > packets.size()) return std::nullopt;
        std::stable_sort(packets.begin(), packets.end(), [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        return packets[slot - 1];
    }

    // A touch at (x_, y_) in the given phase (0 down, 3 up), into LawnApp's handlers.
    void send(int phase) {
        GuestAddr app = rt_.mem().read<std::uint64_t>(base_ + kApp);
        if (!app) return;
        auto& mem = rt_.mem();
        std::memset(mem.host(touch_), 0, 0x38);
        mem.write<std::int32_t>(touch_, kPointer);
        mem.write<std::int32_t>(touch_ + 0x10, x_);
        mem.write<std::int32_t>(touch_ + 0x14, y_);
        mem.write<std::int32_t>(touch_ + 0x18, x_);
        mem.write<std::int32_t>(touch_ + 0x1c, y_);
        mem.write<std::int32_t>(touch_ + 0x20, 1);
        mem.write<std::int32_t>(touch_ + 0x24, 8);
        mem.write<std::int32_t>(touch_ + 0x30, phase);
        rt_.call(base_ + (phase == 0 ? kAppTouchBegan : kAppTouchEnded).offset, {app, touch_});
    }

    Runtime& rt_;
    GuestAddr base_;
    GuestAddr bank_vtable_ = 0, packet_vtable_ = 0;
    std::array<GuestAddr, kButtonCount> button_vtables_{};
    GuestAddr scratch_ = 0, touch_ = 0, rect_ = 0, ref_ = 0, rect_stub_ = 0, ref_stub_ = 0;
    Seen bank_;
    std::array<Seen, kButtonCount> buttons_;
    bool pressed_ = false;
    std::int32_t x_ = 0, y_ = 0;
};

}  // namespace

std::function<void(const platform::GameKeyInput&)> install_game_keys(Runtime& rt, const Module& pvz2) {
    auto keys = std::make_shared<GameKeys>(rt, pvz2);
    if (!keys->install(pvz2)) {
        log::warn("game keys (seed, shovel, plant food...): this libPVZ2.so is a build they don't know; left off");
        return {};
    }
    log::info("game keys (seed, shovel, plant food...): on");
    return [keys](const platform::GameKeyInput& k) { keys->key(k); };
}

}  // namespace rfu
