#include "game/game_keys.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "core/rtti.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
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
constexpr Known kAppTouchMoved{0x14e8578, {0xF941C000, 0x1401BA28, 0xF81E0FF3}};
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
// LawnApp: the size the UI is laid out at (1365x768 on a 1280x720 surface), and the surface's.
constexpr std::uint64_t kLogicalSize = 0xf4, kSurfaceSize = 0x744;
// The lawn, libPVZ2's Board, hangs off the game singleton. Board: its position among the UI (ints),
// the grid's columns and rows, the grid's origin and size in world units (200, 160, 9 x 64, 5 x 76),
// and the zoom about a centre it's drawn with. It loses focus while a dialog is up (paused, the seed
// chooser...). LawnApp: world units to the UI's before the zoom.
constexpr std::uint64_t kGame = 0x2593760, kBoardOfGame = 0x9a8;
constexpr std::uint64_t kBoardPos = 0x44, kGridCols = 0xd8, kGridOrigin = 0x440, kGridSize = 0x448, kBoardZoom = 0x460, kBoardCentre = 0x464;
constexpr std::uint64_t kAppScale = 0x980;
// TimeMgr (a lazy singleton): each frame's step is the time since the last one (at most 0.04 s)
// times its scale, a float that's 1, or 1.5 while fast-forward's on.
constexpr std::uint64_t kTimeMgr = 0x256a010, kTimeScale = 0x30;
constexpr float kSlowScale = 0.5f;  // slowmode's
// PowerupManager: the powerup in use, a handle (null when there's none). A powerup's button
// (PowerupTimeUI, a child of PowerupHolderUI) names its PowerupType at +0x1a0, whose class name
// (PowerupSnowball, PowerupFlickZombie...) is a std::string at +0x20.
constexpr std::uint64_t kManagerActive = 0x28, kButtonType = 0x1a0, kTypeClass = 0x20;
// Board's world Rect to UI one, whose maths Lawn::ui copies, and the world x to column it checks against.
constexpr Known kWorldToUi{0xab23d0, {0xFC1D0FE8, 0xF90007F5, 0xA9014FF4}};
constexpr Known kWorldToColumn{0x9b17d0, {0xD0008F08, 0xBD47D101, 0xD0008F08}};
constexpr Known kBoardTouchMoved{0xab24c8, {0xD101C3FF, 0xF90023F5, 0xA9054FF4}};  // Board::TouchMoved(touch): the lawn hands it to the collectables
constexpr std::uint64_t kTouchMovedSlot = 0x268;  // its place in Board's vtable
constexpr Known kWidgetAt{0x163e82c, {0xD10243FF, 0xA9036FFC, 0xA90467FA}};  // WidgetManager::GetWidgetAt(x, y, flags, bool* blocked, 0, 0)
// Sexy::Widget: its children (a std::list: the end node, then the first's at +8), its parent,
// whether it's shown, and its x, y, width and height, relative to the parent. Sexy::Slider's value (0-1).
constexpr std::uint64_t kWidgetList = 0x8, kWidgetParent = 0x28, kWidgetRect = 0x44, kWidgetVisible = 0x70, kSliderValue = 0xc0;
constexpr std::uint64_t kFlagsMode = 0xc4, kFlagsAdd = 0xf0, kFlagsRemove = 0xf4, kFlagsBase = 0x260;  // WidgetManager: the flags a touch is tested with
constexpr std::uint64_t kBoardState = 0xe0;  // 1 and 3 before the level starts (the pan, the seed chooser), 5 in play
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

// The lawn's grid and how the game draws it, read fresh each frame (the zoom eases in at a level's start).
struct Lawn {
    int cols, rows;
    float ox, oy, tw, th;  // the grid's origin and a tile's size, in world units
    float zoom, cx, cy, bx, by, scale;
    float lw, lh, sw, sh;  // the UI's size and the surface's

    // A world point in the UI's units, as Board's own code computes it (kWorldToUi).
    float ui_x(float wx) const { return cx + std::floor(zoom * (std::trunc(scale * wx) - cx + bx)); }
    float ui_y(float wy) const { return cy + std::floor(zoom * (std::trunc(scale * wy) - cy + by)); }
};

// The pad's labels for its buttons, by type: Xbox's, PlayStation's, Switch's.
struct PadLabel {
    std::string text;
    std::uint32_t rgb;
};
PadLabel pad_label(platform::PadButton b, platform::PadType type) {
    using platform::PadButton;
    using platform::PadType;
    static const char kCross[] = {platform::kGlyphCross, 0}, kCircle[] = {platform::kGlyphCircle, 0};
    static const char kSquare[] = {platform::kGlyphSquare, 0}, kTriangle[] = {platform::kGlyphTriangle, 0};
    switch (type) {
    case PadType::PlayStation:
        switch (b) {
        case PadButton::A: return {kCross, 0x7CB2E8};
        case PadButton::B: return {kCircle, 0xFF6B6B};
        case PadButton::X: return {kSquare, 0xE68FD0};
        case PadButton::Y: return {kTriangle, 0x40D9A8};
        case PadButton::LB: return {"L1", 0xFFFFFF};
        case PadButton::RB: return {"R1", 0xFFFFFF};
        case PadButton::LT: return {"L2", 0xFFFFFF};
        case PadButton::RT: return {"R2", 0xFFFFFF};
        case PadButton::Start: return {"OPT", 0xFFFFFF};
        default: break;
        }
        break;
    case PadType::Switch:  // labelled the other way round from Xbox: the bottom button is B
        switch (b) {
        case PadButton::A: return {"B", 0xFFFFFF};
        case PadButton::B: return {"A", 0xFFFFFF};
        case PadButton::X: return {"Y", 0xFFFFFF};
        case PadButton::Y: return {"X", 0xFFFFFF};
        case PadButton::LB: return {"L", 0xFFFFFF};
        case PadButton::RB: return {"R", 0xFFFFFF};
        case PadButton::LT: return {"ZL", 0xFFFFFF};
        case PadButton::RT: return {"ZR", 0xFFFFFF};
        case PadButton::L3: return {"LS", 0xFFFFFF};
        case PadButton::Start: return {"+", 0xFFFFFF};
        default: break;
        }
        break;
    case PadType::Xbox:
        switch (b) {
        case PadButton::A: return {"A", 0x6CC24A};
        case PadButton::B: return {"B", 0xF0504A};
        case PadButton::X: return {"X", 0x4D9BF0};
        case PadButton::Y: return {"Y", 0xF5C400};
        default: break;
        }
        break;
    }
    switch (b) {
    case PadButton::LB: return {"LB", 0xFFFFFF};
    case PadButton::RB: return {"RB", 0xFFFFFF};
    case PadButton::LT: return {"LT", 0xFFFFFF};
    case PadButton::RT: return {"RT", 0xFFFFFF};
    case PadButton::L3: return {"L3", 0xFFFFFF};
    case PadButton::Start: return {"MENU", 0xFFFFFF};
    default: return {"", 0xFFFFFF};
    }
}

// A keybind hint: the bind's key, shown on the packet or button it presses.
struct HintBind {
    platform::KeyBind::Action action;
    int slot;
    std::string text;
};

class GameKeys : public std::enable_shared_from_this<GameKeys> {
public:
    GameKeys(Runtime& rt, const Module& pvz2, const std::vector<platform::KeyBind>& hint_binds, bool pad_hints, const std::vector<PadBind>& pad_binds)
        : rt_(rt), base_(pvz2.base()), pad_hints_(pad_hints) {
        // Each button does its own action, but an action that's rebound leaves its own button
        // idle unless a bind gives it something else.
        for (std::size_t i = 0; i < kPadButtons; ++i) action_of_[i] = static_cast<platform::PadButton>(i);
        for (const auto& b : pad_binds) action_of_[static_cast<std::size_t>(b.action)].reset();
        for (const auto& b : pad_binds) action_of_[static_cast<std::size_t>(b.button)] = b.action;
        for (const auto& b : hint_binds) {
            if (!platform::is_game_action(b.action)) continue;
            // A second key for the same thing isn't shown.
            bool dup = std::any_of(hints_.begin(), hints_.end(), [&](const HintBind& h) { return h.action == b.action && h.slot == b.slot; });
            std::string text = platform::hint_text(b.key);
            if (!dup && !text.empty()) hints_.push_back({b.action, b.slot, text});
        }
    }

    bool install(const Module& pvz2) {
        for (const Known* k : {&kAppTouchBegan, &kAppTouchMoved, &kAppTouchEnded, &kScreenRect, &kRegistry, &kHandleRef, &kRefIsNull, &kRefType, &kTypeTable, &kRefResolve, &kRefFree})
            if (!known(*k)) return false;
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
        blocked_ = scratch_ + 0xf0;
        write_stub(rect_stub_, 0xAA0103E8, base_ + kScreenRect.offset);  // mov x8, x1
        write_stub(ref_stub_, 0xAA0203E8, base_ + kHandleRef.offset);    // mov x8, x2

        watch(bank->functions[kUpdateSlot], bank_vtable_, bank_, "game keys: seed bank");
        // The seed chooser and its look at the lawn button (each drawn once a frame).
        for (auto [name, vtable, seen] : {std::tuple{"SeedChooser", &chooser_vtable_, &chooser_}, std::tuple{"ViewBoardOrZombiesButton", &view_vtable_, &view_},
                                          std::tuple{"ReloadLayoutButton", &reload_vtable_, &reload_}}) {
            auto vt = find_vtable(rt_.mem(), pvz2, name);
            if (!vt || vt->functions.size() <= kUpdateSlot) continue;
            *vtable = vt->address;
            watch(vt->functions[kUpdateSlot], vt->address, *seen, std::string("game keys: ") + name);
        }
        for (std::size_t i = 0; i < kButtonCount; ++i)
            watch(buttons[i]->functions[kUpdateSlot], button_vtables_[i], buttons_[i], std::string("game keys: ") + kButtons[i].name);
        return true;
    }

    // The controller's part: the lawn's grid, and the plant cursor that's up while a packet is picked.
    bool install_pad(const Module& pvz2) {
        if (!known(kWorldToUi) || !known(kWorldToColumn) || !known(kBoardTouchMoved) || !known(kWidgetAt)) return false;
        auto board = find_vtable(rt_.mem(), pvz2, "Board");
        auto cursor = find_vtable(rt_.mem(), pvz2, "PlantCursor");
        if (!board || !cursor || cursor->functions.empty()) return false;
        if (rt_.mem().read<std::uint64_t>(board->address + kTouchMovedSlot) != base_ + kBoardTouchMoved.offset) return false;
        board_vtable_ = board->address;
        cursor_vtable_ = cursor->address;
        watch(cursor->functions[0], cursor_vtable_, plant_cursor_, "controller: plant cursor");  // its per-frame call
        // The cursors of the plants that aim (Banana Launcher, Missile Toe, the launchers, Escape
        // Root): up from a tap on the plant until its target is tapped.
        for (const char* name : {"BananaTargetCursor", "MissileToeTargetCursor", "PlantLauncherTargetCursor", "EscapeRootCursor"}) {
            auto aim = find_vtable(rt_.mem(), pvz2, name);
            if (!aim || aim->functions.empty()) continue;
            aim_cursors_.emplace_back(std::piecewise_construct, std::forward_as_tuple(aim->address), std::forward_as_tuple());
        }
        if (auto map = find_vtable(rt_.mem(), pvz2, "WorldMap")) map_vtable_ = map->address;
        // The pause menu: its dialog, sliders and buttons (Sexy widgets).
        auto pause = find_vtable(rt_.mem(), pvz2, "PauseDialog");
        auto slider = find_vtable(rt_.mem(), pvz2, "Sexy::Slider");
        auto button = find_vtable(rt_.mem(), pvz2, "PVZ2UIButton");
        if (pause && slider && button) {
            pause_vtable_ = pause->address;
            slider_vtable_ = slider->address;
            button_vtable_ = button->address;
        } else {
            log::warn("controller: the pause menu's classes aren't there; its controls are off");
        }
        for (auto& a : aim_cursors_)
            watch(rt_.mem().read<std::uint64_t>(a.first), a.first, a.second, "controller: aiming cursor");
        // The powerups: the holder of their buttons and the manager (each updated once a frame),
        // and the flick's class, which wants a swipe rather than a finger held.
        auto holder = find_vtable(rt_.mem(), pvz2, "PowerupHolderUI");
        auto power_button = find_vtable(rt_.mem(), pvz2, "PowerupTimeUI");
        auto manager = find_vtable(rt_.mem(), pvz2, "PowerupManager");
        if (holder && power_button && manager && holder->functions.size() > kUpdateSlot && manager->functions.size() > kUpdateSlot) {
            holder_vtable_ = holder->address;
            power_button_vtable_ = power_button->address;
            manager_vtable_ = manager->address;
            watch(holder->functions[kUpdateSlot], holder_vtable_, holder_, "controller: powerups");
            watch(manager->functions[kUpdateSlot], manager_vtable_, manager_, "controller: powerup manager");
            if (auto flick = find_vtable(rt_.mem(), pvz2, "PowerupFlickZombie")) flick_vtable_ = flick->address;
        } else {
            log::warn("controller: the powerups' classes aren't there; their controls are off");
        }
        return true;
    }

    // A pad button. The lawn's controls, in the manner of PvZ1's console version: the d-pad or left
    // stick moves the cursor a tile, LB/RB pick the packet, A plants it (or taps the tile), B held a
    // second digs, Y feeds plant food (held two seconds, it buys one through the "+" by the plant
    // food bar), X taps the tile (a plant's own ability, or the target
    // once one asks for it; held a second in the first column, the mower), LT/RT sweep the lawn
    // for sun and coins, Start pauses, L3 fast-forwards, R3 turns slowmode on and off. The right
    // stick flicked left, up or right picks the first, second or third powerup; while one's in use,
    // X holds a finger on the tile (twice quickly, the flick's swipe).
    //
    // Off the lawn: on the pause menu the d-pad picks a slider or button (left and right move a
    // slider), A presses it and B or Start resume; on the seed chooser the d-pad picks a packet and
    // A takes it, LB/RB pick a slot and B empties it (held, all of them), Y looks at the lawn and X
    // held is "let's rock"; everywhere else the right stick moves a cursor, A touches where it is (held, it drags), the d-pad or left stick
    // scrolls, and B is Android's back. True: the input is Android's back button.
    bool pad(const platform::PadInput& pressed) {
        using platform::PadButton;
        if (pressed.button == PadButton::LStick || pressed.button == PadButton::RStick) {
            if (pressed.button == PadButton::RStick) {
                stick_x_ = pressed.x;
                stick_y_ = pressed.y;
                if (std::hypot(stick_x_, stick_y_) > kDeadZone) {
                    pad_type_ = pressed.type;
                    pad_active_ = true;
                }
                power_flick();
            }
            return false;
        }
        auto action = action_of_[static_cast<std::size_t>(pressed.button)];
        if (!action) return false;
        platform::PadInput p = pressed;
        p.button = *action;
        pad_type_ = p.type;
        pad_active_ = true;
        auto dir = std::find_if(dirs_.begin(), dirs_.end(), [&](const Dir& d) { return d.button == p.button; });
        if (dir != dirs_.end()) {
            dir->held = p.down;
            if (p.down) {
                Mode m = mode();
                if (m == Mode::Lawn) move(dir->dx, dir->dy);
                else if (m == Mode::Menu) menu_move(dir->dx, dir->dy);
                else if (m == Mode::Chooser) chooser_move(dir->dx, dir->dy);
                dir->next = now_ticks() + ticks(kRepeatDelay);
            }
            return false;
        }
        if (!p.down) {
            // Whatever went down, its finger comes up.
            if (p.button == PadButton::A && cursor_down_) {
                steps_.push_back({1, 3, static_cast<std::int32_t>(cursor_x_), static_cast<std::int32_t>(cursor_y_)});
                cursor_down_ = false;
            }
            chooser_release(p);
            lawn_pad(p);
            return false;
        }
        switch (mode()) {
        case Mode::Lawn: lawn_pad(p); return false;
        case Mode::Menu: return menu_pad(p);
        case Mode::Cursor: return cursor_pad(p);
        case Mode::Chooser: chooser_pad(p); return false;
        case Mode::None: return false;
        }
        return false;
    }

    // The lawn's controls (and the buttons let go anywhere).
    void lawn_pad(const platform::PadInput& p) {
        using platform::PadButton;
        if (p.button == PadButton::B) {
            b_since_ = p.down ? now_ticks() : 0;
            return;
        }
        auto lawn = this->lawn();
        if (p.button == PadButton::Y) {
            // On the way up, unless the hold already bought one.
            if (p.down) y_since_ = now_ticks();
            else if (y_since_ && lawn) {
                if (auto r = button_rect(platform::KeyBind::Action::PlantFood)) tap(r->x + r->w / 2, r->y + r->h / 2);
                tap_tile(*lawn);
            }
            if (!p.down) y_since_ = 0;
            return;
        }
        if (p.button == PadButton::X && (power_down_ || (p.down && lawn && active_powerup()))) {
            power_x(p.down, *lawn);
            return;
        }
        if (p.button == PadButton::X) {
            // Aiming: the finger that's held on the target lets go, and the plant fires.
            if (p.down && aim_down_) {
                steps_.push_back({1, 3, aim_x_, aim_y_});
                aim_down_ = false;
                aim_from_.reset();
                return;
            }
            if (p.down && lawn) aim_from_ = std::pair{col_, row_};
            // In the first column it waits to see if it's held for the mower.
            if (p.down && lawn && col_ == 0) x_since_ = now_ticks();
            else if (p.down && lawn) tap_tile(*lawn);
            else if (mower_down_) {
                // Let go of the mower: the game launched it if it was held long enough.
                steps_.push_back({1, 3, mower_x_, mower_y_});
                mower_down_ = false;
            } else if (x_since_ && lawn) tap_tile(*lawn);
            if (!p.down) x_since_ = 0;
            return;
        }
        if (!p.down) return;
        switch (p.button) {
        case PadButton::LB:
        case PadButton::RB: {
            int n = static_cast<int>(packet_rects().size());
            if (n == 0) break;
            slot_ = slot_ < 1 || slot_ > n ? 1 : (slot_ - 1 + (p.button == PadButton::RB ? 1 : n - 1)) % n + 1;
            break;
        }
        case PadButton::A:
            if (!lawn) break;
            if (aim_down_) {
                // Aiming is called off: the finger slides back onto the plant and lets go there.
                if (aim_from_) {
                    aim_x_ = tile_x(*lawn, aim_from_->first);
                    aim_y_ = tile_y(*lawn, aim_from_->second);
                    steps_.push_back({1, 1, aim_x_, aim_y_});
                }
                steps_.push_back({1, 3, aim_x_, aim_y_});
                aim_down_ = false;
                aim_from_.reset();
                break;
            }
            // The picked packet first, unless the game's plant cursor says one is picked already.
            if (slot_ > 0 && !live(plant_cursor_, cursor_vtable_, std::chrono::milliseconds(200))) {
                if (auto r = packet_rect(slot_)) tap(r->x + r->w / 2, r->y + r->h / 2);
            }
            tap_tile(*lawn);
            break;
        case PadButton::LT:
        case PadButton::RT:
            if (lawn && sweep_ < 0) sweep_ = 0;
            break;
        case PadButton::Start:
            if (auto r = button_rect(platform::KeyBind::Action::Pause)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        case PadButton::L3:
            if (auto r = button_rect(platform::KeyBind::Action::FastForward)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        case PadButton::R3:
            slow_ = !slow_;
            log::info("controller: slowmode {}", slow_ ? "on" : "off");
            break;
        default: break;
        }
    }

    // In a level, past the seed chooser: the Board's there and in its state for play or after.
    bool in_level() {
        auto& mem = rt_.mem();
        GuestAddr game = mem.read<std::uint64_t>(base_ + kGame);
        GuestAddr board = game ? mem.read<std::uint64_t>(game + kBoardOfGame) : 0;
        if (!board || !board_vtable_ || mem.read<std::uint64_t>(board) != board_vtable_) return false;
        return mem.read<std::int32_t>(board + kBoardState) > 3;
    }

    // Slowmode: the game's time scale (TimeMgr +0x30, which fast-forward sets to 1.5) is held at
    // half while it's on; it goes back to what the game had when it's turned off or the level's over.
    void slow_frame() {
        if (slow_ && !in_level()) slow_ = false;
        auto& mem = rt_.mem();
        GuestAddr time = mem.read<std::uint64_t>(base_ + kTimeMgr);
        if (!time) return;
        float scale = mem.read<float>(time + kTimeScale);
        if (slow_) {
            if (scale != kSlowScale) {
                game_scale_ = scale;  // fast-forward, pressed while slow, comes back when it's off
                mem.write<float>(time + kTimeScale, kSlowScale);
            }
        } else if (game_scale_) {
            if (scale == kSlowScale) mem.write<float>(time + kTimeScale, *game_scale_);
            game_scale_.reset();
        }
    }

    // Once a frame, between frames: held directions repeat, a held B digs after a second, and the
    // queued touches go out one step at a time.
    void frame() {
        auto now = now_ticks();
        for (auto& d : dirs_)
            if (d.held && now >= d.next) {
                Mode m = mode();
                if (m == Mode::Lawn) move(d.dx, d.dy);
                else if (m == Mode::Menu) menu_move(d.dx, d.dy);
                else if (m == Mode::Chooser) chooser_move(d.dx, d.dy);
                d.next = now + ticks(kRepeatEvery);
            }
        cursor_frame(now);
        chooser_frame(now);
        if (b_since_ && now - b_since_ >= ticks(kDigHold)) {
            b_since_ = 0;
            if (auto lawn = this->lawn()) {
                if (auto r = button_rect(platform::KeyBind::Action::Shovel)) tap(r->x + r->w / 2, r->y + r->h / 2);
                tap_tile(*lawn);
            }
        }
        if (y_since_ && now - y_since_ >= ticks(kBuyHold)) {
            y_since_ = 0;
            // The "+" opens a plant food (and its price) over itself; a tap on that buys it.
            if (lawn())
                if (auto bar = button_rect(platform::KeyBind::Action::PlantFood)) {
                    auto plus = buy_rect();
                    tap(plus->x, plus->y);
                    std::int32_t x = plus->x - bar->h * 12 / 100, y = plus->y - bar->h * 128 / 100;
                    steps_.push_back({kPopupWait, 0, x, y});
                    steps_.push_back({2, 3, x, y});
                }
        }
        if (x_since_ && now - x_since_ >= ticks(kMowerHold)) {
            x_since_ = 0;
            if (auto lawn = this->lawn()) {
                // A finger on the row's mower, left of the first column, held while X is: the
                // game's own dial fills, and the mower goes once it's full.
                float wx = lawn->ox - lawn->tw * 0.6f, wy = lawn->oy + lawn->th * (row_ + 0.5f);
                mower_x_ = static_cast<std::int32_t>(lawn->ui_x(wx));
                mower_y_ = static_cast<std::int32_t>(lawn->ui_y(wy));
                steps_.push_back({1, 0, mower_x_, mower_y_});
                mower_down_ = true;
            }
        }
        if (sweep_ >= 0) sweep();
        slow_frame();
        // The powerup ran out while X held its finger down: it comes up.
        if (power_down_ && !active_powerup()) {
            if (auto lawn = this->lawn()) steps_.push_back({1, 3, tile_x(*lawn, col_), tile_y(*lawn, row_)});
            power_down_ = false;
        }
        // A plant started aiming (its cursor is up): a finger goes down on the cursor's tile and
        // stays, as on a phone the reticle shows under a finger held on the lawn. If the game
        // drops the aiming by itself, the finger comes up.
        bool aims = aiming();
        if (aims && !aim_down_ && !mower_down_ && steps_.empty()) {
            if (auto lawn = this->lawn()) {
                aim_x_ = tile_x(*lawn, col_);
                aim_y_ = tile_y(*lawn, row_);
                steps_.push_back({1, 0, aim_x_, aim_y_});
                aim_down_ = true;
            }
        } else if (!aims && aim_down_ && steps_.empty()) {
            steps_.push_back({1, 3, aim_x_, aim_y_});
            aim_down_ = false;
        }
        if (!steps_.empty() && --steps_.front().wait <= 0) {
            Step step = steps_.front();
            steps_.pop_front();
            x_ = step.x;
            y_ = step.y;
            send(step.phase);
        }
    }

    // A game key is pressed or the mouse clicks: the keyboard and mouse are in use again, so the
    // pad's cursor and labels go (until the pad's used) and the keys' hints come back.
    void keyboard() { pad_active_ = false; }

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

    // What's drawn over the frame: the controller's cursor (once the pad is in use), and the hints
    // for whichever of the keyboard and the pad was used last.
    platform::HintOverlay overlay() {
        platform::HintOverlay out;
        if (!pad_active_) {
            out.labels = hints();
            return out;
        }
        auto lawn = this->lawn();
        if (!lawn) {
            off_lawn_overlay(out);
            return out;
        }
        float fx = lawn->sw / lawn->lw, fy = lawn->sh / lawn->lh;
        // The tile, in the surface's pixels; red while B is held to dig.
        float x0 = lawn->ox + lawn->tw * col_, y0 = lawn->oy + lawn->th * row_;
        int left = static_cast<int>(lawn->ui_x(x0) * fx), top = static_cast<int>(lawn->ui_y(y0) * fy);
        int right = static_cast<int>(lawn->ui_x(x0 + lawn->tw) * fx), bottom = static_cast<int>(lawn->ui_y(y0 + lawn->th) * fy);
        // Red while B is held to dig, orange while a plant aims.
        out.boxes.push_back({left, top, right - left, bottom - top, b_since_ ? 0xFF4040u : aiming() ? 0xFFA030u : 0xFFFFFFu, true});
        if (b_since_) {
            // The shovel's dial by the tile's top right corner, full when the plant comes up.
            float held = static_cast<float>(now_ticks() - b_since_) / ticks(kDigHold);
            int radius = std::max(6, (bottom - top) / 6);
            out.meters.push_back({right, top, radius, held, 0xFF4040u});
        }
        if (y_since_ && now_ticks() - y_since_ >= ticks(std::chrono::milliseconds(250))) {
            // Y held to buy: a dial above the "+", full when it's pressed.
            if (auto bar = button_rect(platform::KeyBind::Action::PlantFood)) {
                auto plus = buy_rect();
                float held = static_cast<float>(now_ticks() - y_since_) / ticks(kBuyHold);
                int radius = std::max(6, static_cast<int>(bar->h * fy) / 6);
                out.meters.push_back({static_cast<int>(plus->x * fx), static_cast<int>(bar->y * fy), radius, held, 0x60E040u});
            }
        }
        auto packets = packet_rects();
        if (slot_ >= 1 && static_cast<std::size_t>(slot_) <= packets.size()) {
            const Rect& r = packets[slot_ - 1];
            out.boxes.push_back({static_cast<int>(r.x * fx), static_cast<int>(r.y * fy), static_cast<int>(r.w * fx), static_cast<int>(r.h * fy), platform::kHintBorder});
        }
        if (!pad_hints_) return out;
        using platform::PadButton;
        auto label = [&](PadButton action, int x, int y, platform::HintLabel::Anchor anchor) {
            // The button that does the action now.
            std::size_t i = 0;
            while (i < kPadButtons && action_of_[i] != action) ++i;
            if (i == kPadButtons) return;
            PadLabel l = pad_label(static_cast<PadButton>(i), pad_type_);
            out.labels.push_back({l.text, x, y, anchor, l.rgb});
        };
        int n = static_cast<int>(packets.size());
        if (n > 1 && slot_ >= 1 && slot_ <= n) {
            const Rect& prev = packets[(slot_ + n - 2) % n];
            const Rect& next = packets[slot_ % n];
            label(PadButton::LB, static_cast<int>(prev.x * fx), static_cast<int>(prev.y * fy), platform::HintLabel::Anchor::TopLeft);
            // RB at the next packet's bottom, so it doesn't read as the same corner as LB's.
            label(PadButton::RB, static_cast<int>(next.x * fx), static_cast<int>((next.y + next.h) * fy), platform::HintLabel::Anchor::BottomLeft);
        }
        const std::pair<platform::KeyBind::Action, PadButton> kOnButtons[] = {
            {platform::KeyBind::Action::Shovel, PadButton::B}, {platform::KeyBind::Action::PlantFood, PadButton::Y},
            {platform::KeyBind::Action::FastForward, PadButton::L3}, {platform::KeyBind::Action::Pause, PadButton::Start}};
        for (auto [action, b] : kOnButtons)
            if (auto r = button_rect(action))
                label(b, static_cast<int>((r->x + r->w) * fx), static_cast<int>((r->y + r->h) * fy), platform::HintLabel::Anchor::BottomRight);
        // The right stick's flicks on the powerups they pick: left, up, right.
        auto powers = power_buttons();
        const char kArrows[] = {platform::kGlyphLeft, platform::kGlyphUp, platform::kGlyphRight};
        for (std::size_t i = 0; i < powers.size() && i < 3; ++i) {
            const Rect& r = powers[i].rect;
            out.labels.push_back({std::string("R") + kArrows[i], static_cast<int>(r.x * fx), static_cast<int>(r.y * fy), platform::HintLabel::Anchor::TopLeft, 0xFFFFFF});
        }
        return out;
    }

    // The hints for what's on screen now: seed keys at their packets' top left corners, the
    // buttons' keys at their bottom right ones.
    std::vector<platform::HintLabel> hints() {
        std::vector<platform::HintLabel> out;
        // The rectangles are in the UI's units; the hints are drawn in the surface's pixels.
        GuestAddr app = rt_.mem().read<std::uint64_t>(base_ + kApp);
        if (!app) return out;
        auto& mem = rt_.mem();
        std::int32_t lw = mem.read<std::int32_t>(app + kLogicalSize), lh = mem.read<std::int32_t>(app + kLogicalSize + 4);
        std::int32_t sw = mem.read<std::int32_t>(app + kSurfaceSize), sh = mem.read<std::int32_t>(app + kSurfaceSize + 4);
        if (lw <= 0 || lh <= 0 || sw <= 0 || sh <= 0) return out;
        auto px = [&](std::int32_t x) { return static_cast<int>(static_cast<std::int64_t>(x) * sw / lw); };
        auto py = [&](std::int32_t y) { return static_cast<int>(static_cast<std::int64_t>(y) * sh / lh); };
        std::optional<std::vector<Rect>> packets;
        for (const auto& h : hints_) {
            if (h.action == platform::KeyBind::Action::Seed) {
                if (!packets) packets = packet_rects();
                if (h.slot < 1 || static_cast<std::size_t>(h.slot) > packets->size()) continue;
                const Rect& r = (*packets)[h.slot - 1];
                out.push_back({h.text, px(r.x), py(r.y), platform::HintLabel::Anchor::TopLeft});
            } else if (auto r = button_rect(h.action)) {
                out.push_back({h.text, px(r->x + r->w), py(r->y + r->h), platform::HintLabel::Anchor::BottomRight});
            }
        }
        return out;
    }

private:
    static constexpr auto kRepeatDelay = std::chrono::milliseconds(300), kRepeatEvery = std::chrono::milliseconds(110);
    static constexpr auto kDigHold = std::chrono::seconds(1), kBuyHold = std::chrono::seconds(2);
    static constexpr auto kMowerHold = std::chrono::milliseconds(250);  // X held this long in the first column is for the mower
    static constexpr int kSweepStep = 40;  // between the sweep's points, in the UI's units
    static constexpr int kPopupWait = 12;  // frames for the plant food "+"'s offer to come up
    static std::int64_t ticks(std::chrono::steady_clock::duration d) { return d.count(); }

    bool known(const Known& k) const {
        for (std::size_t i = 0; i < k.code.size(); ++i)
            if (rt_.mem().read<std::uint32_t>(base_ + k.offset + 4 * i) != k.code[i]) return false;
        return true;
    }

    // The level's lawn, when there is one and nothing is in front of it.
    std::optional<Lawn> lawn() {
        auto& mem = rt_.mem();
        GuestAddr game = mem.read<std::uint64_t>(base_ + kGame);
        GuestAddr board = game ? mem.read<std::uint64_t>(game + kBoardOfGame) : 0;
        GuestAddr app = mem.read<std::uint64_t>(base_ + kApp);
        if (!board || !app || !board_vtable_ || mem.read<std::uint64_t>(board) != board_vtable_) return std::nullopt;
        // Not before the level starts, nor in the states where the lawn takes no touches (the
        // game's own test, in Board::TouchMoved).
        std::int32_t state = mem.read<std::int32_t>(board + kBoardState);
        if (state < 0 || state > 13 || state == 3 || ((1u << state) & 0x2f46u)) return std::nullopt;
        Lawn l;
        l.cols = mem.read<std::int32_t>(board + kGridCols);
        l.rows = mem.read<std::int32_t>(board + kGridCols + 4);
        if (l.cols < 1 || l.rows < 1 || l.cols > 32 || l.rows > 32) return std::nullopt;
        l.ox = static_cast<float>(mem.read<std::int32_t>(board + kGridOrigin));
        l.oy = static_cast<float>(mem.read<std::int32_t>(board + kGridOrigin + 4));
        l.tw = static_cast<float>(mem.read<std::int32_t>(board + kGridSize)) / l.cols;
        l.th = static_cast<float>(mem.read<std::int32_t>(board + kGridSize + 4)) / l.rows;
        l.zoom = mem.read<float>(board + kBoardZoom);
        l.cx = mem.read<float>(board + kBoardCentre);
        l.cy = mem.read<float>(board + kBoardCentre + 4);
        l.bx = static_cast<float>(mem.read<std::int32_t>(board + kBoardPos));
        l.by = static_cast<float>(mem.read<std::int32_t>(board + kBoardPos + 4));
        l.scale = mem.read<float>(app + kAppScale);
        l.lw = static_cast<float>(mem.read<std::int32_t>(app + kLogicalSize));
        l.lh = static_cast<float>(mem.read<std::int32_t>(app + kLogicalSize + 4));
        l.sw = static_cast<float>(mem.read<std::int32_t>(app + kSurfaceSize));
        l.sh = static_cast<float>(mem.read<std::int32_t>(app + kSurfaceSize + 4));
        if (!(l.tw > 0 && l.th > 0 && l.zoom > 0.1f && l.zoom < 10 && l.scale > 0.1f && l.lw > 0 && l.lh > 0 && l.sw > 0 && l.sh > 0)) return std::nullopt;
        col_ = std::clamp(col_, 0, l.cols - 1);
        row_ = std::clamp(row_, 0, l.rows - 1);
        // Nothing in front of the lawn (the pause menu, a dialog): the widget a touch in the
        // grid's middle would go to is the Board. (Board+0x72, the Board having the focus, isn't
        // enough: it can stay off after the window loses the focus, and come back only with a
        // new level.)
        float mx = l.ox + l.tw * l.cols / 2, my = l.oy + l.th * l.rows / 2;
        if (widget_at(static_cast<std::int32_t>(l.ui_x(mx)), static_cast<std::int32_t>(l.ui_y(my))) != board) return std::nullopt;
        return l;
    }

    // The widget a touch at (x, y), in the UI's units, would go to (the WidgetManager's own test).
    GuestAddr widget_at(std::int32_t x, std::int32_t y) {
        auto& mem = rt_.mem();
        GuestAddr app = mem.read<std::uint64_t>(base_ + kApp);
        GuestAddr wm = app ? mem.read<std::uint64_t>(app + kWidgetManager) : 0;
        if (!wm) return 0;
        std::uint32_t flags = mem.read<std::uint32_t>(wm + kFlagsBase);
        if (!mem.read<std::uint8_t>(wm + kFlagsMode)) flags = (mem.read<std::uint32_t>(wm + kFlagsAdd) | flags) & ~mem.read<std::uint32_t>(wm + kFlagsRemove);
        mem.write<std::uint8_t>(blocked_, 0);
        return rt_.call(base_ + kWidgetAt.offset, {wm, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), flags, blocked_, 0, 0});
    }

    // The UI's size and the surface's (all 0 before the app is up).
    struct Sizes {
        float lw = 0, lh = 0, sw = 0, sh = 0;
        bool ok() const { return lw > 0 && lh > 0 && sw > 0 && sh > 0; }
    };
    Sizes sizes() const {
        auto& mem = rt_.mem();
        GuestAddr app = mem.read<std::uint64_t>(base_ + kApp);
        if (!app) return {};
        return {static_cast<float>(mem.read<std::int32_t>(app + kLogicalSize)), static_cast<float>(mem.read<std::int32_t>(app + kLogicalSize + 4)),
                static_cast<float>(mem.read<std::int32_t>(app + kSurfaceSize)), static_cast<float>(mem.read<std::int32_t>(app + kSurfaceSize + 4))};
    }

    // What the pad does now.
    enum class Mode { Lawn, Menu, Cursor, Chooser, None };
    Mode mode() {
        if (lawn()) return Mode::Lawn;
        if (pause_dialog()) return Mode::Menu;
        // The seed chooser (and the look at the lawn from it) has controls of its own.
        auto& mem = rt_.mem();
        GuestAddr game = mem.read<std::uint64_t>(base_ + kGame);
        GuestAddr board = game ? mem.read<std::uint64_t>(game + kBoardOfGame) : 0;
        if (board && board_vtable_ && mem.read<std::uint64_t>(board) == board_vtable_ && mem.read<std::int32_t>(board + kBoardState) == 3) return Mode::Chooser;
        return sizes().ok() ? Mode::Cursor : Mode::None;
    }

    // The pause menu, when it's up: the dialog is what a touch in the screen's middle goes to, or
    // holds it.
    GuestAddr pause_dialog() {
        if (!pause_vtable_) return 0;
        Sizes z = sizes();
        if (!z.ok()) return 0;
        auto& mem = rt_.mem();
        GuestAddr w = widget_at(static_cast<std::int32_t>(z.lw / 2), static_cast<std::int32_t>(z.lh / 2));
        for (int i = 0; w && i < 8; ++i, w = mem.read<std::uint64_t>(w + kWidgetParent))
            if (mem.read<std::uint64_t>(w) == pause_vtable_) return w;
        return 0;
    }

    // The pause menu's sliders and buttons, top to bottom then left to right, in the UI's units.
    struct MenuItem {
        GuestAddr widget;
        Rect r;
        bool slider;
        int row;
    };
    std::vector<MenuItem> menu_items(GuestAddr dialog) {
        std::vector<MenuItem> items;
        auto& mem = rt_.mem();
        // Sexy::WidgetContainer: its children in a std::list, positions relative to the parent.
        auto collect = [&](auto& self, GuestAddr container, std::int32_t ax, std::int32_t ay, int depth) -> void {
            GuestAddr end = container + kWidgetList;
            GuestAddr node = mem.read<std::uint64_t>(end + 8);
            for (int n = 0; node && node != end && n < 64; ++n, node = mem.read<std::uint64_t>(node + 8)) {
                GuestAddr w = mem.read<std::uint64_t>(node + 16);
                if (!w || !mem.read<std::uint8_t>(w + kWidgetVisible)) continue;
                Rect r{ax + mem.read<std::int32_t>(w + kWidgetRect), ay + mem.read<std::int32_t>(w + kWidgetRect + 4),
                       mem.read<std::int32_t>(w + kWidgetRect + 8), mem.read<std::int32_t>(w + kWidgetRect + 12)};
                GuestAddr vt = mem.read<std::uint64_t>(w);
                if ((vt == slider_vtable_ || vt == button_vtable_) && r.w > 0 && r.h > 0) items.push_back({w, r, vt == slider_vtable_, 0});
                if (depth < 4) self(self, w, r.x, r.y, depth + 1);
            }
        };
        collect(collect, dialog, mem.read<std::int32_t>(dialog + kWidgetRect), mem.read<std::int32_t>(dialog + kWidgetRect + 4), 0);
        std::stable_sort(items.begin(), items.end(), [](const MenuItem& a, const MenuItem& b) {
            return a.r.y + a.r.h / 2 != b.r.y + b.r.h / 2 ? a.r.y + a.r.h / 2 < b.r.y + b.r.h / 2 : a.r.x < b.r.x;
        });
        // Rows: items whose middles are within half a height of the row's first.
        for (std::size_t i = 1, first = 0; i < items.size(); ++i) {
            const Rect& a = items[first].r;
            const Rect& b = items[i].r;
            bool same = std::abs((b.y + b.h / 2) - (a.y + a.h / 2)) < std::max(a.h, b.h) / 2;
            items[i].row = same ? items[first].row : items[first].row + 1;
            if (!same) first = i;
        }
        return items;
    }

    // The item the menu's box is on, as an index in items (the first when it's none of them).
    std::size_t menu_selected(const std::vector<MenuItem>& items) {
        for (std::size_t i = 0; i < items.size(); ++i)
            if (items[i].widget == menu_sel_) return i;
        menu_sel_ = items.empty() ? 0 : items[0].widget;
        return 0;
    }

    void menu_move(int dx, int dy) {
        GuestAddr dialog = pause_dialog();
        if (!dialog) return;
        auto items = menu_items(dialog);
        if (items.empty()) return;
        std::size_t at = menu_selected(items);
        const MenuItem& sel = items[at];
        if (dy) {
            // The nearest item, side to side, of the row above or below.
            int row = sel.row + dy;
            int mid = sel.r.x + sel.r.w / 2;
            std::optional<std::size_t> best;
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (items[i].row != row) continue;
                int d = std::abs(items[i].r.x + items[i].r.w / 2 - mid);
                if (!best || d < std::abs(items[*best].r.x + items[*best].r.w / 2 - mid)) best = i;
            }
            if (best) menu_sel_ = items[*best].widget;
        } else if (dx && sel.slider) {
            slide(sel, dx);
        } else if (dx) {
            std::size_t next = at + (dx > 0 ? 1 : -1);
            if (next < items.size() && items[next].row == sel.row) menu_sel_ = items[next].widget;
        }
    }

    // A slider a tenth along, the way a finger moves it: down on its thumb, across, up.
    void slide(const MenuItem& item, int dir) {
        double value = std::clamp(rt_.mem().read<double>(item.widget + kSliderValue), 0.0, 1.0);
        double to = std::clamp(value + 0.1 * dir, 0.0, 1.0);
        if (to == value) return;
        int thumb = item.r.h * 7 / 10;  // about the thumb's width
        auto x = [&](double v) { return static_cast<std::int32_t>(item.r.x + thumb / 2 + v * (item.r.w - thumb)); };
        std::int32_t y = item.r.y + item.r.h / 2;
        steps_.push_back({1, 0, x(value), y});
        steps_.push_back({1, 1, x((value + to) / 2), y});
        steps_.push_back({1, 1, x(to), y});
        steps_.push_back({1, 3, x(to), y});
    }

    bool menu_pad(const platform::PadInput& p) {
        using platform::PadButton;
        if (p.button == PadButton::B || p.button == PadButton::Start) return true;  // back: the game resumes
        if (p.button != PadButton::A) return false;
        GuestAddr dialog = pause_dialog();
        if (!dialog) return false;
        auto items = menu_items(dialog);
        if (items.empty()) return false;
        const MenuItem& sel = items[menu_selected(items)];
        if (!sel.slider) tap(sel.r.x + sel.r.w / 2, sel.r.y + sel.r.h / 2);
        return false;
    }

    bool cursor_pad(const platform::PadInput& p) {
        using platform::PadButton;
        // Back, but not in a level, where B is the shovel and back would pause.
        if (p.button == PadButton::B) return !button_rect(platform::KeyBind::Action::Pause);
        // On the world map: shortcuts to its buttons.
        if (on_map()) {
            std::optional<std::pair<float, float>> at;
            if (p.button == PadButton::LT) at = kMapAlmanac;
            else if (p.button == PadButton::RT) at = kMapZenGarden;
            else if (p.button == PadButton::Start) at = kMapSettings;
            if (at) {
                tap_anchored(at->first, at->second, false);
                return false;
            }
            if (p.button == PadButton::Y) {
                tap_anchored(kMapShop.first, kMapShop.second, true);
                return false;
            }
        }
        // The level's buttons still work while the lawn isn't taking touches (a banner's up).
        if (p.button == PadButton::Start || p.button == PadButton::L3) {
            if (auto r = button_rect(p.button == PadButton::Start ? platform::KeyBind::Action::Pause : platform::KeyBind::Action::FastForward))
                tap(r->x + r->w / 2, r->y + r->h / 2);
            return false;
        }
        if (p.button == PadButton::A && !cursor_down_ && !scroll_) {
            cursor_place();
            steps_.push_back({1, 0, static_cast<std::int32_t>(cursor_x_), static_cast<std::int32_t>(cursor_y_)});
            cursor_down_ = true;
        }
        return false;
    }

    // The cursor starts in the middle of the screen.
    void cursor_place() {
        if (cursor_x_ >= 0) return;
        Sizes z = sizes();
        cursor_x_ = z.lw / 2;
        cursor_y_ = z.lh / 2;
    }

    // Once a frame: the right stick moves the cursor (dragging the finger A holds down), and the
    // d-pad or left stick held scrolls what's under it, a finger dragging the other way.
    void cursor_frame(std::int64_t now) {
        double dt = std::min(0.1, static_cast<double>(now - last_frame_) / ticks(std::chrono::seconds(1)));
        last_frame_ = now;
        float mag = std::hypot(stick_x_, stick_y_);
        int dx = 0, dy = 0;
        for (const auto& d : dirs_)
            if (d.held) {
                dx += d.dx;
                dy += d.dy;
            }
        if (mag <= kDeadZone && !dx && !dy && !scroll_) return;
        Sizes z = sizes();
        if (mode() != Mode::Cursor || !z.ok()) {
            stop_scroll();
            return;
        }
        cursor_place();
        if (mag > kDeadZone) {
            // Faster the further it's pushed: across the screen's height in about a second at full tilt.
            float speed = z.lh * 1.1f * std::pow((std::min(mag, 1.0f) - kDeadZone) / (1 - kDeadZone), 2.0f);
            cursor_x_ = std::clamp(cursor_x_ + static_cast<float>(stick_x_ / mag * speed * dt), 0.0f, z.lw - 1);
            cursor_y_ = std::clamp(cursor_y_ + static_cast<float>(stick_y_ / mag * speed * dt), 0.0f, z.lh - 1);
            if (cursor_down_) steps_.push_back({1, 1, static_cast<std::int32_t>(cursor_x_), static_cast<std::int32_t>(cursor_y_)});
        }
        if ((dx || dy) && !cursor_down_) {
            if (scroll_ && (scroll_dx_ != dx || scroll_dy_ != dy)) stop_scroll();
            if (!scroll_) {
                platform::KeyBind bind;
                bind.action = platform::KeyBind::Action::Scroll;
                bind.dir_x = dx;
                bind.dir_y = dy;
                scroll_.emplace(bind, static_cast<int>(z.lw), static_cast<int>(z.lh), cursor_x_, cursor_y_, kScrollStroke);
                scroll_dx_ = dx;
                scroll_dy_ = dy;
            }
            for (const auto& step : scroll_->frame()) scroll_step(step);
        } else {
            stop_scroll();
        }
    }

    void stop_scroll() {
        if (!scroll_) return;
        for (const auto& step : scroll_->release()) scroll_step(step);
        scroll_.reset();
    }

    void scroll_step(const platform::ScrollDrag::Step& step) {
        x_ = static_cast<std::int32_t>(step.x);
        y_ = static_cast<std::int32_t>(step.y);
        send(step.phase == platform::ScrollDrag::Step::Down ? 0 : step.phase == platform::ScrollDrag::Step::Move ? 1 : 3);
    }

    // The seed chooser: its packets to pick from (top to bottom, then left to right) and the ones
    // picked, in the slots down the left (top to bottom), in the UI's units. Both are SeedPackets
    // among the chooser's children; an empty slot's is at (0, 0).
    struct Chooser {
        std::vector<std::pair<GuestAddr, Rect>> grid, slots;
    };
    Chooser chooser_packets() {
        Chooser c;
        GuestAddr chooser = live(chooser_, chooser_vtable_);
        Sizes z = sizes();
        if (!chooser || !z.ok()) return c;
        auto& mem = rt_.mem();
        GuestAddr begin = mem.read<std::uint64_t>(chooser + kChildren), end = mem.read<std::uint64_t>(chooser + kChildren + 8);
        if (end < begin || end - begin > 1024 * 8) return c;
        for (GuestAddr h = begin; h < end; h += 8) {
            GuestAddr packet = resolve(h);
            if (!packet || mem.read<std::uint64_t>(packet) != packet_vtable_) continue;
            Rect r = screen_rect(packet);
            if (r.w <= 0 || r.h <= 0 || (r.x == 0 && r.y == 0)) continue;
            // Drawn a little up and left of the rect, as in the seed bank.
            r.x -= r.w * 6 / 100;
            r.y -= r.h * 4 / 100;
            (r.x + r.w / 2 < z.lw * 0.1f ? c.slots : c.grid).push_back({packet, r});
        }
        auto order = [](const auto& a, const auto& b) { return a.second.y != b.second.y ? a.second.y < b.second.y : a.second.x < b.second.x; };
        std::stable_sort(c.grid.begin(), c.grid.end(), order);
        std::stable_sort(c.slots.begin(), c.slots.end(), order);
        return c;
    }

    // Where the chooser's list shows packets, top and bottom, in the UI's units (what isn't in it is
    // scrolled to first).
    std::pair<float, float> chooser_view() const {
        Sizes z = sizes();
        return {z.lh * 0.445f, z.lh * 0.975f};
    }

    // The grid packet the box is on (the first when it's none of them).
    std::size_t chooser_selected(const Chooser& c) {
        for (std::size_t i = 0; i < c.grid.size(); ++i)
            if (c.grid[i].first == ch_sel_) return i;
        ch_sel_ = c.grid.empty() ? 0 : c.grid[0].first;
        return 0;
    }

    void chooser_move(int dx, int dy) {
        // Not while the list is still being dragged to the last one: the packets' rects are about
        // to change, and a held direction would run the box out of view.
        if (!steps_.empty()) return;
        auto c = chooser_packets();
        if (c.grid.empty()) return;
        const Rect sel = c.grid[chooser_selected(c)].second;
        int cx = sel.x + sel.w / 2, cy = sel.y + sel.h / 2;
        // The nearest packet that way: the next in the row, or in the next row the one nearest across.
        std::optional<std::size_t> best;
        long best_d = 0;
        for (std::size_t i = 0; i < c.grid.size(); ++i) {
            const Rect& r = c.grid[i].second;
            int px = r.x + r.w / 2, py = r.y + r.h / 2;
            bool same_row = std::abs(py - cy) < sel.h / 2;
            long d;
            if (dx && same_row && (px - cx) * dx > 0) d = std::abs(px - cx);
            else if (dy && !same_row && (py - cy) * dy > 0) d = std::abs(py - cy) * 4L + std::abs(px - cx);
            else continue;
            if (!best || d < best_d) {
                best = i;
                best_d = d;
            }
        }
        if (!best) return;
        ch_sel_ = c.grid[*best].first;
        chooser_scroll_to(c.grid[*best].second);
    }

    // The list dragged (slowly, and held still before letting go, so it doesn't fling) until the
    // packet is in view.
    void chooser_scroll_to(const Rect& r) {
        auto [top, bottom] = chooser_view();
        float margin = r.h * 0.15f;
        float dy = 0;
        if (r.y < top + margin) dy = top + margin - r.y;
        else if (r.y + r.h > bottom - margin) dy = bottom - margin - (r.y + r.h);
        if (dy == 0) return;
        std::int32_t x = r.x + r.w / 2;
        float y0 = dy > 0 ? top + (bottom - top) * 0.2f : bottom - (bottom - top) * 0.2f;
        // A drag needs to go a little way before the list takes it as a scroll.
        float slop = dy > 0 ? r.h * 0.2f : -r.h * 0.2f;
        steps_.push_back({1, 0, x, static_cast<std::int32_t>(y0)});
        const int kSteps = 6;
        for (int i = 1; i <= kSteps; ++i)
            steps_.push_back({1, 1, x, static_cast<std::int32_t>(y0 + slop + dy * i / kSteps)});
        steps_.push_back({4, 1, x, static_cast<std::int32_t>(y0 + slop + dy)});
        steps_.push_back({2, 3, x, static_cast<std::int32_t>(y0 + slop + dy)});
    }

    void chooser_pad(const platform::PadInput& p) {
        using platform::PadButton;
        switch (p.button) {
        case PadButton::A: {
            auto c = chooser_packets();
            if (c.grid.empty()) break;
            const Rect& r = c.grid[chooser_selected(c)].second;
            auto [top, bottom] = chooser_view();
            if (r.y >= top && r.y + r.h <= bottom) tap(r.x + r.w / 2, r.y + r.h / 2);
            else chooser_scroll_to(r);
            break;
        }
        case PadButton::LB:
        case PadButton::RB: {
            int n = static_cast<int>(chooser_packets().slots.size());
            if (n == 0) break;
            ch_slot_ = ch_slot_ < 1 || ch_slot_ > n ? 1 : (ch_slot_ - 1 + (p.button == PadButton::RB ? 1 : n - 1)) % n + 1;
            break;
        }
        case PadButton::B: ch_b_since_ = now_ticks(); break;
        case PadButton::X: ch_x_since_ = now_ticks(); break;
        case PadButton::Y:
            if (auto r = live_rect(view_, view_vtable_)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        case PadButton::LT:
            if (auto r = live_rect(reload_, reload_vtable_)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        case PadButton::RT: tap_anchored(kChooserAlmanac.first, kChooserAlmanac.second, false); break;
        case PadButton::Start:
            if (auto r = button_rect(platform::KeyBind::Action::Pause)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        default: break;
        }
    }

    // Buttons the game draws itself, not widgets or UI objects of their own, where they are on the
    // screen: (x, y) in 768ths of its height, from the left edge (or the right one).
    static constexpr std::pair<float, float> kMapAlmanac{134, 38}, kMapZenGarden{301, 38}, kMapSettings{382, 38}, kMapShop{75, 53},
        kChooserAlmanac{286, 37};
    void tap_anchored(float x, float y, bool from_right) {
        Sizes z = sizes();
        if (!z.ok()) return;
        float k = z.lh / 768;
        tap(static_cast<std::int32_t>(from_right ? z.lw - x * k : x * k), static_cast<std::int32_t>(y * k));
    }

    // The world map is up (what a touch in the screen's middle goes to).
    bool on_map() {
        Sizes z = sizes();
        if (!map_vtable_ || !z.ok()) return false;
        GuestAddr w = widget_at(static_cast<std::int32_t>(z.lw / 2), static_cast<std::int32_t>(z.lh / 2));
        return w && rt_.mem().read<std::uint64_t>(w) == map_vtable_;
    }

    // B let go before it cleared the slots: the slot's packet goes back.
    void chooser_release(const platform::PadInput& p) {
        using platform::PadButton;
        if (p.button == PadButton::B && ch_b_since_) {
            ch_b_since_ = 0;
            auto c = chooser_packets();
            if (ch_slot_ >= 1 && static_cast<std::size_t>(ch_slot_) <= c.slots.size()) {
                const Rect& r = c.slots[ch_slot_ - 1].second;
                tap(r.x + r.w / 2, r.y + r.h / 2);
                // The box stays on the slot below, or the last one.
                if (static_cast<std::size_t>(ch_slot_) == c.slots.size()) --ch_slot_;
            }
        }
        if (p.button == PadButton::X) ch_x_since_ = 0;
    }

    // B held two seconds empties every slot; X held a second presses "let's rock".
    void chooser_frame(std::int64_t now) {
        if (ch_b_since_ && now - ch_b_since_ >= ticks(kClearHold)) {
            ch_b_since_ = 0;
            auto c = chooser_packets();
            // From the bottom, so the ones above don't move up under the next tap.
            for (auto it = c.slots.rbegin(); it != c.slots.rend(); ++it) {
                const Rect& r = it->second;
                steps_.push_back({4, 0, r.x + r.w / 2, r.y + r.h / 2});
                steps_.push_back({2, 3, r.x + r.w / 2, r.y + r.h / 2});
            }
            ch_slot_ = 1;
        }
        if (ch_x_since_ && now - ch_x_since_ >= ticks(kRockHold)) {
            ch_x_since_ = 0;
            if (auto r = rock_rect()) tap(r->x + r->w / 2, r->y + r->h / 2);
        }
    }

    // "Let's rock": drawn by the chooser, not a widget or UI object of its own; it sits in the
    // screen's bottom right corner (117 and 47 of 768 UI units in from it, at every shape).
    std::optional<Rect> rock_rect() {
        Sizes z = sizes();
        if (!z.ok() || !live(chooser_, chooser_vtable_)) return std::nullopt;
        float k = z.lh / 768;
        return Rect{static_cast<std::int32_t>(z.lw - 227 * k), static_cast<std::int32_t>(z.lh - 82 * k), static_cast<std::int32_t>(220 * k),
                    static_cast<std::int32_t>(70 * k)};
    }

    // A watched button's rect, while it's drawn.
    std::optional<Rect> live_rect(const Seen& seen, GuestAddr vtable) {
        GuestAddr object = live(seen, vtable);
        if (!object) return std::nullopt;
        Rect r = screen_rect(object);
        if (r.w <= 0 || r.h <= 0) return std::nullopt;
        return r;
    }

    // The chooser's box on the grid packet, the gold one on the slot LB/RB picked, the dials for B
    // and X held, and the buttons' labels.
    void chooser_overlay(platform::HintOverlay& out, float fx, float fy) {
        auto c = chooser_packets();
        auto box = [&](const Rect& r, std::uint32_t rgb, bool corners) {
            out.boxes.push_back({static_cast<int>(r.x * fx), static_cast<int>(r.y * fy), static_cast<int>(r.w * fx), static_cast<int>(r.h * fy), rgb, corners});
        };
        if (!c.grid.empty()) {
            const Rect& r = c.grid[chooser_selected(c)].second;
            auto [top, bottom] = chooser_view();
            if (r.y + r.h > top && r.y < bottom) box(r, 0xFFFFFFu, true);
        }
        std::optional<Rect> slot;
        if (ch_slot_ >= 1 && static_cast<std::size_t>(ch_slot_) <= c.slots.size()) {
            slot = c.slots[ch_slot_ - 1].second;
            box(*slot, platform::kHintBorder, false);
        }
        auto now = now_ticks();
        auto start = rock_rect();
        auto view = live_rect(view_, view_vtable_);
        if (ch_b_since_ && slot && now - ch_b_since_ >= ticks(std::chrono::milliseconds(250))) {
            float held = static_cast<float>(now - ch_b_since_) / ticks(kClearHold);
            int radius = std::max(6, static_cast<int>(slot->h * fy) / 6);
            out.meters.push_back({static_cast<int>((slot->x + slot->w) * fx), static_cast<int>(slot->y * fy), radius, held, 0xFF4040u});
        }
        if (ch_x_since_ && start) {
            float held = static_cast<float>(now - ch_x_since_) / ticks(kRockHold);
            int radius = std::max(6, static_cast<int>(start->h * fy) / 5);
            out.meters.push_back({static_cast<int>((start->x + start->w) * fx), static_cast<int>(start->y * fy), radius, held, 0x60E040u});
        }
        if (!pad_hints_) return;
        auto label = [&](platform::PadButton action, const Rect& r) {
            std::size_t i = 0;
            while (i < kPadButtons && action_of_[i] != action) ++i;
            if (i == kPadButtons) return;
            PadLabel l = pad_label(static_cast<platform::PadButton>(i), pad_type_);
            out.labels.push_back({l.text, static_cast<int>((r.x + r.w) * fx), static_cast<int>((r.y + r.h) * fy), platform::HintLabel::Anchor::BottomRight, l.rgb});
        };
        if (start) label(platform::PadButton::X, *start);
        if (view) label(platform::PadButton::Y, *view);
        if (auto reload = live_rect(reload_, reload_vtable_)) label(platform::PadButton::LT, *reload);
    }

    // Off the lawn: the pause menu's box, or the cursor.
    void off_lawn_overlay(platform::HintOverlay& out) {
        Mode m = mode();
        Sizes z = sizes();
        if (!z.ok()) return;
        float fx = z.sw / z.lw, fy = z.sh / z.lh;
        if (m == Mode::Chooser) {
            chooser_overlay(out, fx, fy);
        } else if (m == Mode::Menu) {
            GuestAddr dialog = pause_dialog();
            auto items = dialog ? menu_items(dialog) : std::vector<MenuItem>{};
            if (items.empty()) return;
            const Rect& r = items[menu_selected(items)].r;
            int pad = static_cast<int>(r.h * fy) / 8;
            out.boxes.push_back({static_cast<int>(r.x * fx) - pad, static_cast<int>(r.y * fy) - pad, static_cast<int>(r.w * fx) + 2 * pad,
                                 static_cast<int>(r.h * fy) + 2 * pad, 0xFFFFFFu, true});
        } else if (m == Mode::Cursor && cursor_x_ >= 0) {
            int x = static_cast<int>(cursor_x_ * fx), y = static_cast<int>(cursor_y_ * fy);
            int radius = std::max(6, static_cast<int>(z.sh) / 64);
            out.meters.push_back({x, y, radius + std::max(2, radius / 3), 1.0f, 0x202020u});
            out.meters.push_back({x, y, radius, 1.0f, cursor_down_ ? 0xFFD040u : 0xFFFFFFu});
        }
    }

    void move(int dx, int dy) {
        auto lawn = this->lawn();
        if (!lawn) return;
        col_ = std::clamp(col_ + dx, 0, lawn->cols - 1);
        row_ = std::clamp(row_ + dy, 0, lawn->rows - 1);
        // While a plant aims, the finger held for it slides along, and its reticle with it.
        if (aim_down_) {
            aim_x_ = tile_x(*lawn, col_);
            aim_y_ = tile_y(*lawn, row_);
            steps_.push_back({1, 1, aim_x_, aim_y_});
        }
        // So does the finger a powerup's held with.
        if (power_down_) steps_.push_back({1, 1, tile_x(*lawn, col_), tile_y(*lawn, row_)});
    }

    // The powerups' buttons, left to right, and what each is (its class's name).
    struct PowerButton {
        Rect rect;
        std::string kind;
    };
    std::vector<PowerButton> power_buttons() {
        std::vector<PowerButton> out;
        GuestAddr holder = live(holder_, holder_vtable_);
        if (!holder) return out;
        auto& mem = rt_.mem();
        GuestAddr begin = mem.read<std::uint64_t>(holder + kChildren), end = mem.read<std::uint64_t>(holder + kChildren + 8);
        if (end < begin || end - begin > 16 * 8) return out;
        for (GuestAddr h = begin; h < end; h += 8) {
            GuestAddr button = resolve(h);
            if (!button || mem.read<std::uint64_t>(button) != power_button_vtable_) continue;
            Rect r = screen_rect(button);
            if (r.w <= 0 || r.h <= 0) continue;
            GuestAddr type = resolve(button + kButtonType);
            out.push_back({r, type ? guest_string(type + kTypeClass) : std::string()});
        }
        std::stable_sort(out.begin(), out.end(), [](const PowerButton& a, const PowerButton& b) { return a.rect.x < b.rect.x; });
        return out;
    }

    // A libc++ std::string in guest memory.
    std::string guest_string(GuestAddr at) {
        auto& mem = rt_.mem();
        std::uint8_t first = mem.read<std::uint8_t>(at);
        std::uint64_t size = (first & 1) ? mem.read<std::uint64_t>(at + 8) : first >> 1;
        GuestAddr data = (first & 1) ? mem.read<std::uint64_t>(at + 16) : at + 1;
        if (size > 256) return {};
        std::string out(size, '\0');
        for (std::uint64_t i = 0; i < size; ++i) out[i] = static_cast<char>(mem.read<std::uint8_t>(data + i));
        return out;
    }

    // The powerup in use, if one is: the manager's handle to it.
    GuestAddr active_powerup() {
        GuestAddr manager = live(manager_, manager_vtable_);
        if (!manager) return 0;
        if (!rt_.mem().read<std::uint64_t>(manager + kManagerActive)) return 0;
        return resolve(manager + kManagerActive);
    }

    // The right stick flicked left, up or right picks the first, second or third powerup (its
    // button's tapped, as a finger would); it has to come back to the middle before the next.
    void power_flick() {
        float mag = std::hypot(stick_x_, stick_y_);
        if (mag < kDeadZone) {
            flicked_ = false;
            return;
        }
        if (flicked_ || mag < kFlick || !lawn()) return;
        int index = -1;
        if (std::abs(stick_x_) > std::abs(stick_y_)) index = stick_x_ < 0 ? 0 : 2;
        else if (stick_y_ < 0) index = 1;
        if (index < 0) return;
        flicked_ = true;
        auto buttons = power_buttons();
        if (static_cast<std::size_t>(index) >= buttons.size()) return;
        const Rect& r = buttons[index].rect;
        tap(r.x + r.w / 2, r.y + r.h / 2);
    }

    // X while a powerup's in use, on the cursor's tile. The flick (Power Toss) wants a swipe, which
    // X pressed twice quickly gives: a finger across the tile, left to right, so its zombies fly
    // back. Every other one (snowballs, the flamethrower, Power Zap, a mod's own) is a finger held
    // down while X is, which follows the cursor.
    void power_x(bool down, const Lawn& lawn) {
        GuestAddr active = active_powerup();
        bool flick = active && flick_vtable_ && rt_.mem().read<std::uint64_t>(active) == flick_vtable_;
        std::int32_t x = tile_x(lawn, col_), y = tile_y(lawn, row_);
        if (!down) {
            if (power_down_) steps_.push_back({1, 3, x, y});
            power_down_ = false;
            return;
        }
        if (!flick) {
            steps_.push_back({1, 0, x, y});
            power_down_ = true;
            return;
        }
        std::int64_t now = now_ticks();
        if (x_tapped_ && now - x_tapped_ <= ticks(kDoublePress)) {
            x_tapped_ = 0;
            auto half = static_cast<std::int32_t>(lawn.tw * lawn.zoom * 0.5f);
            steps_.push_back({1, 0, x - half * 2, y});
            for (int i = 1; i <= 4; ++i) steps_.push_back({1, 1, x - half * 2 + half * 2 * i, y});
            steps_.push_back({1, 3, x + half * 6, y});
        } else {
            x_tapped_ = now;
        }
    }

    // A plant's aiming cursor is up.
    bool aiming() const {
        return std::any_of(aim_cursors_.begin(), aim_cursors_.end(), [&](const auto& a) { return live(a.second, a.first, std::chrono::milliseconds(200)) != 0; });
    }

    // The tile's middle, in the UI's units.
    std::int32_t tile_x(const Lawn& l, int col) const { return static_cast<std::int32_t>(l.ui_x(l.ox + l.tw * (col + 0.5f))); }
    std::int32_t tile_y(const Lawn& l, int row) const { return static_cast<std::int32_t>(l.ui_y(l.oy + l.th * (row + 0.5f))); }

    // A finger sliding over the lawn to (x, y), in the UI's units: the Board's own TouchMoved.
    void slide_to(const Lawn& l, std::int32_t x, std::int32_t y) {
        GuestAddr game = rt_.mem().read<std::uint64_t>(base_ + kGame);
        GuestAddr board = rt_.mem().read<std::uint64_t>(game + kBoardOfGame);
        fill_touch(1, static_cast<std::int32_t>(x - l.bx), static_cast<std::int32_t>(y - l.by));
        rt_.call(base_ + kBoardTouchMoved.offset, {board, touch_});
    }

    // A finger down and up at (x, y) in the UI's units, after the touches already queued.
    void tap(std::int32_t x, std::int32_t y) {
        steps_.push_back({1, 0, x, y});
        steps_.push_back({2, 3, x, y});
    }
    void tap_tile(const Lawn& l) { tap(tile_x(l, col_), tile_y(l, row_)); }

    // One line of the vacuum's sweep a frame, top to bottom over the whole screen: a finger sliding
    // over the lawn collects what it passes, and the lawn's own TouchMoved is told of one at each
    // point, the way hover to collect tells it of the mouse.
    void sweep() {
        auto lawn = this->lawn();
        float y = (sweep_ + 0.5f) * kSweepStep;
        if (!lawn || y >= lawn->lh) {
            sweep_ = -1;
            return;
        }
        ++sweep_;
        for (float x = kSweepStep / 2.0f; x < lawn->lw; x += kSweepStep) slide_to(*lawn, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    }

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
    GuestAddr live(const Seen& seen, GuestAddr vtable, std::chrono::steady_clock::duration stale = kStale) const {
        GuestAddr object = seen.object;
        auto age = std::chrono::steady_clock::duration(now_ticks() - seen.when);
        if (!object || age > stale || rt_.mem().read<std::uint64_t>(object) != vtable) return 0;
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

    // The plant food bar's "+" (buy one): not a widget of its own but drawn by the bar's right
    // end, which moves out as upgrades lengthen the bar. Its centre, as a 1x1 Rect.
    std::optional<Rect> buy_rect() {
        auto bar = button_rect(platform::KeyBind::Action::PlantFood);
        if (!bar) return std::nullopt;
        return Rect{bar->x + bar->w + bar->h * 35 / 100, bar->y + bar->h / 2, 1, 1};
    }

    // The seed bank's packets, top to bottom, then left to right.
    std::vector<Rect> packet_rects() {
        std::vector<Rect> packets;
        GuestAddr bank = live(bank_, bank_vtable_);
        if (!bank) return packets;
        auto& mem = rt_.mem();
        GuestAddr begin = mem.read<std::uint64_t>(bank + kChildren), end = mem.read<std::uint64_t>(bank + kChildren + 8);
        if (end < begin || end - begin > 64 * 8) return packets;
        for (GuestAddr h = begin; h < end; h += 8) {
            GuestAddr packet = resolve(h);
            if (!packet || mem.read<std::uint64_t>(packet) != packet_vtable_) continue;
            Rect r = screen_rect(packet);
            if (r.w <= 0 || r.h <= 0) continue;
            // The card is drawn a little up and left of the packet's rect (by about 7 and 3 of
            // the UI's units on a 119x76 packet, at every resolution), so the box goes there.
            r.x -= r.w * 6 / 100;
            r.y -= r.h * 4 / 100;
            packets.push_back(r);
        }
        std::stable_sort(packets.begin(), packets.end(), [](const Rect& a, const Rect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
        return packets;
    }

    // The slot-th packet (from 1).
    std::optional<Rect> packet_rect(int slot) {
        auto packets = packet_rects();
        if (slot < 1 || static_cast<std::size_t>(slot) > packets.size()) return std::nullopt;
        return packets[slot - 1];
    }

    // A touch at (x_, y_) in the given phase (0 down, 1 moved, 3 up), into LawnApp's handlers.
    void send(int phase) {
        GuestAddr app = rt_.mem().read<std::uint64_t>(base_ + kApp);
        if (!app) return;
        fill_touch(phase, x_, y_);
        rt_.call(base_ + (phase == 0 ? kAppTouchBegan : phase == 1 ? kAppTouchMoved : kAppTouchEnded).offset, {app, touch_});
    }

    void fill_touch(int phase, std::int32_t x, std::int32_t y) {
        auto& mem = rt_.mem();
        std::memset(mem.host(touch_), 0, 0x38);
        mem.write<std::int32_t>(touch_, kPointer);
        mem.write<std::int32_t>(touch_ + 0x10, x);
        mem.write<std::int32_t>(touch_ + 0x14, y);
        // Where it was the event before, and when (as Android's touches have them: a list's
        // scroll measures its fling from them, and without them snaps back when let go).
        if (phase == 0) {
            last_x_ = x;
            last_y_ = y;
        }
        mem.write<std::int32_t>(touch_ + 0x18, last_x_);
        mem.write<std::int32_t>(touch_ + 0x1c, last_y_);
        last_x_ = x;
        last_y_ = y;
        mem.write<std::int32_t>(touch_ + 0x20, 1);
        mem.write<std::int32_t>(touch_ + 0x24, 8);
        mem.write<double>(touch_ + 0x28, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        mem.write<std::int32_t>(touch_ + 0x30, phase);
        mem.write<std::int32_t>(touch_ + 0x34, 13);
    }

    Runtime& rt_;
    GuestAddr base_;
    GuestAddr bank_vtable_ = 0, packet_vtable_ = 0;
    std::array<GuestAddr, kButtonCount> button_vtables_{};
    GuestAddr scratch_ = 0, touch_ = 0, rect_ = 0, ref_ = 0, rect_stub_ = 0, ref_stub_ = 0, blocked_ = 0;
    Seen bank_;
    std::array<Seen, kButtonCount> buttons_;
    std::vector<HintBind> hints_;
    bool pressed_ = false;
    std::int32_t x_ = 0, y_ = 0;

    // The controller.
    struct Dir {
        platform::PadButton button;
        int dx, dy;
        bool held = false;
        std::int64_t next = 0;  // when a held direction moves again
    };
    struct Step {
        int wait;  // frames after the step before it
        int phase;
        std::int32_t x, y;
    };
    bool pad_hints_;
    static constexpr std::size_t kPadButtons = static_cast<std::size_t>(platform::PadButton::RRight) + 1;
    std::array<std::optional<platform::PadButton>, kPadButtons> action_of_;  // what each button does
    GuestAddr board_vtable_ = 0, cursor_vtable_ = 0;
    Seen plant_cursor_;
    std::deque<std::pair<GuestAddr, Seen>> aim_cursors_;  // the aiming cursors' vtables, and the last of each seen
    bool aim_down_ = false;                                // a finger held on the lawn for a plant that aims
    std::int32_t aim_x_ = 0, aim_y_ = 0;                   // where it is
    std::optional<std::pair<int, int>> aim_from_;          // the tile X last tapped: the plant that's aiming
    std::array<Dir, 4> dirs_{{{platform::PadButton::Up, 0, -1}, {platform::PadButton::Down, 0, 1},
                              {platform::PadButton::Left, -1, 0}, {platform::PadButton::Right, 1, 0}}};
    std::deque<Step> steps_;
    std::int32_t last_x_ = 0, last_y_ = 0;  // where the pad's finger was the event before
    // Off the lawn: the pause menu's classes and its item the box is on; the cursor (in the UI's
    // units, -1 until it's first used), the right stick, the finger A holds down there, and the
    // scroll the d-pad drags.
    GuestAddr pause_vtable_ = 0, slider_vtable_ = 0, button_vtable_ = 0;
    GuestAddr menu_sel_ = 0;
    // The seed chooser and its buttons; the grid packet the box is on, the slot LB/RB picked (from
    // 1), and when B or X went down there.
    GuestAddr chooser_vtable_ = 0, view_vtable_ = 0, reload_vtable_ = 0, map_vtable_ = 0;
    Seen chooser_, view_, reload_;
    GuestAddr ch_sel_ = 0;
    int ch_slot_ = 1;
    std::int64_t ch_b_since_ = 0, ch_x_since_ = 0;
    static constexpr auto kClearHold = std::chrono::seconds(2), kRockHold = std::chrono::seconds(1);
    float cursor_x_ = -1, cursor_y_ = -1;
    float stick_x_ = 0, stick_y_ = 0;
    bool cursor_down_ = false;
    std::optional<platform::ScrollDrag> scroll_;
    int scroll_dx_ = 0, scroll_dy_ = 0;
    std::int64_t last_frame_ = 0;
    static constexpr float kDeadZone = 0.2f;
    static constexpr float kScrollStroke = 0.1f;  // a scroll's finger lifts after a tenth of the screen's height
    bool pad_active_ = false;
    platform::PadType pad_type_ = platform::PadType::Xbox;
    int col_ = 0, row_ = 2, slot_ = 1;
    std::int64_t b_since_ = 0, y_since_ = 0, x_since_ = 0;  // when B, Y or X went down, while it's held and still to act
    int sweep_ = -1;
    bool mower_down_ = false;  // a finger held on a mower, while X is
    std::int32_t mower_x_ = 0, mower_y_ = 0;  // the vacuum's next line, while it sweeps
    GuestAddr holder_vtable_ = 0, power_button_vtable_ = 0, manager_vtable_ = 0, flick_vtable_ = 0;
    Seen holder_, manager_;
    bool flicked_ = false;        // the right stick's out from a flick, not back yet
    bool power_down_ = false;     // X holds a finger down for a powerup
    std::int64_t x_tapped_ = 0;   // when X was pressed for the flick, waiting for a second press
    static constexpr float kFlick = 0.7f;  // how far the right stick goes to pick a powerup
    static constexpr std::chrono::milliseconds kDoublePress{400};
    bool slow_ = false;                 // slowmode (R3) is on
    std::optional<float> game_scale_;   // the game's own time scale, while slowmode holds it
};

}  // namespace

std::vector<PadBind> parse_pad_binds(const std::string& text, std::string& error) {
    using platform::PadButton;
    static const std::pair<const char*, PadButton> kActions[] = {
        {"plant", PadButton::A},     {"dig", PadButton::B},     {"use", PadButton::X},
        {"plantfood", PadButton::Y}, {"prevseed", PadButton::LB}, {"nextseed", PadButton::RB},
        {"vacuum", PadButton::LT},   {"vacuum2", PadButton::RT}, {"fastforward", PadButton::L3},
        {"pause", PadButton::Start}, {"slowmode", PadButton::R3}};
    static const std::pair<const char*, PadButton> kButtonNames[] = {
        {"a", PadButton::A},   {"b", PadButton::B},       {"x", PadButton::X},       {"y", PadButton::Y},         {"lb", PadButton::LB},
        {"rb", PadButton::RB}, {"lt", PadButton::LT},     {"rt", PadButton::RT},     {"l3", PadButton::L3},       {"r3", PadButton::R3},
        {"start", PadButton::Start}, {"back", PadButton::Back}, {"up", PadButton::Up}, {"down", PadButton::Down}, {"left", PadButton::Left},
        {"right", PadButton::Right}};
    std::vector<PadBind> binds;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(',', at);
        if (end == std::string::npos) end = text.size();
        std::string item = text.substr(at, end - at);
        at = end + 1;
        for (auto& c : item) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
        if (item.empty()) continue;
        std::size_t eq = item.find('=');
        std::string name = item.substr(0, eq), button = eq == std::string::npos ? "" : item.substr(eq + 1);
        auto a = std::find_if(std::begin(kActions), std::end(kActions), [&](const auto& k) { return name == k.first; });
        auto b = std::find_if(std::begin(kButtonNames), std::end(kButtonNames), [&](const auto& k) { return button == k.first; });
        if (a == std::end(kActions) || b == std::end(kButtonNames)) {
            error = "pad binds: \"" + item + "\" isn't action=button";
            return {};
        }
        binds.push_back({a->second, b->second});
    }
    return binds;
}

GameKeyHandlers install_game_keys(Runtime& rt, const Module& pvz2, const GameKeyOptions& options) {
    auto keys = std::make_shared<GameKeys>(rt, pvz2, options.hint_binds, options.pad_hints, options.pad_binds);
    if (!keys->install(pvz2)) {
        log::warn("game keys (seed, shovel, plant food...): this libPVZ2.so is a build they don't know; left off");
        return {};
    }
    log::info("game keys (seed, shovel, plant food...): on{}", options.hint_binds.empty() ? "" : ", with hints");
    GameKeyHandlers handlers;
    handlers.key = [keys](const platform::GameKeyInput& k) {
        keys->keyboard();
        keys->key(k);
    };
    handlers.mouse = [keys] { keys->keyboard(); };
    bool pad = options.controller && keys->install_pad(pvz2);
    if (options.controller) log::info("controller on the lawn: {}", pad ? "on" : "this libPVZ2.so is a build it doesn't know; left off");
    if (pad) {
        handlers.pad = [keys](const platform::PadInput& p) { return keys->pad(p); };
        handlers.frame = [keys] { keys->frame(); };
    }
    if (pad || !options.hint_binds.empty()) handlers.overlay = [keys] { return keys->overlay(); };
    return handlers;
}

}  // namespace rfu
