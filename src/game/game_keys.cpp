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
#include <cmath>
#include <cstring>
#include <deque>
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
// LawnApp: the size the UI is laid out at (1365x768 on a 1280x720 surface), and the surface's.
constexpr std::uint64_t kLogicalSize = 0xf4, kSurfaceSize = 0x744;
// The lawn, libPVZ2's Board, hangs off the game singleton. Board: its position among the UI (ints),
// the grid's columns and rows, the grid's origin and size in world units (200, 160, 9 x 64, 5 x 76),
// and the zoom about a centre it's drawn with. It loses focus while a dialog is up (paused, the seed
// chooser...). LawnApp: world units to the UI's before the zoom.
constexpr std::uint64_t kGame = 0x2593760, kBoardOfGame = 0x9a8;
constexpr std::uint64_t kBoardPos = 0x44, kBoardFocus = 0x72, kGridCols = 0xd8, kGridOrigin = 0x440, kGridSize = 0x448, kBoardZoom = 0x460, kBoardCentre = 0x464;
constexpr std::uint64_t kAppScale = 0x980;
// Board's world Rect to UI one, whose maths Lawn::ui copies, and the world x to column it checks against.
constexpr Known kWorldToUi{0xab23d0, {0xFC1D0FE8, 0xF90007F5, 0xA9014FF4}};
constexpr Known kWorldToColumn{0x9b17d0, {0xD0008F08, 0xBD47D101, 0xD0008F08}};
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
    GameKeys(Runtime& rt, const Module& pvz2, const std::vector<platform::KeyBind>& hint_binds, bool pad_hints)
        : rt_(rt), base_(pvz2.base()), pad_hints_(pad_hints) {
        for (const auto& b : hint_binds) {
            if (!platform::is_game_action(b.action)) continue;
            // A second key for the same thing isn't shown.
            bool dup = std::any_of(hints_.begin(), hints_.end(), [&](const HintBind& h) { return h.action == b.action && h.slot == b.slot; });
            std::string text = platform::hint_text(b.key);
            if (!dup && !text.empty()) hints_.push_back({b.action, b.slot, text});
        }
    }

    bool install(const Module& pvz2) {
        for (const Known* k : {&kAppTouchBegan, &kAppTouchEnded, &kScreenRect, &kRegistry, &kHandleRef, &kRefIsNull, &kRefType, &kTypeTable, &kRefResolve, &kRefFree})
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
        write_stub(rect_stub_, 0xAA0103E8, base_ + kScreenRect.offset);  // mov x8, x1
        write_stub(ref_stub_, 0xAA0203E8, base_ + kHandleRef.offset);    // mov x8, x2

        watch(bank->functions[kUpdateSlot], bank_vtable_, bank_, "game keys: seed bank");
        for (std::size_t i = 0; i < kButtonCount; ++i)
            watch(buttons[i]->functions[kUpdateSlot], button_vtables_[i], buttons_[i], std::string("game keys: ") + kButtons[i].name);
        return true;
    }

    // The controller's part: the lawn's grid, and the plant cursor that's up while a packet is picked.
    bool install_pad(const Module& pvz2) {
        if (!known(kWorldToUi) || !known(kWorldToColumn)) return false;
        auto board = find_vtable(rt_.mem(), pvz2, "Board");
        auto cursor = find_vtable(rt_.mem(), pvz2, "PlantCursor");
        if (!board || !cursor || cursor->functions.empty()) return false;
        board_vtable_ = board->address;
        cursor_vtable_ = cursor->address;
        watch(cursor->functions[0], cursor_vtable_, plant_cursor_, "controller: plant cursor");  // its per-frame call
        return true;
    }

    // A pad button. The lawn's controls, in the manner of PvZ1's console version: the d-pad or left
    // stick moves the cursor a tile, LB/RB pick the packet, A plants it (or taps the tile), B held a
    // second digs, Y feeds plant food, Start pauses, L3 fast-forwards.
    void pad(const platform::PadInput& p) {
        using platform::PadButton;
        pad_type_ = p.type;
        pad_active_ = true;
        auto dir = std::find_if(dirs_.begin(), dirs_.end(), [&](const Dir& d) { return d.button == p.button; });
        if (dir != dirs_.end()) {
            dir->held = p.down;
            if (p.down) {
                move(dir->dx, dir->dy);
                dir->next = now_ticks() + ticks(kRepeatDelay);
            }
            return;
        }
        if (p.button == PadButton::B) {
            b_since_ = p.down ? now_ticks() : 0;
            return;
        }
        if (!p.down) return;
        auto lawn = this->lawn();
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
            // The picked packet first, unless the game's plant cursor says one is picked already.
            if (slot_ > 0 && !live(plant_cursor_, cursor_vtable_, std::chrono::milliseconds(200))) {
                if (auto r = packet_rect(slot_)) tap(r->x + r->w / 2, r->y + r->h / 2);
            }
            tap_tile(*lawn);
            break;
        case PadButton::Y:
            if (!lawn) break;
            if (auto r = button_rect(platform::KeyBind::Action::PlantFood)) tap(r->x + r->w / 2, r->y + r->h / 2);
            tap_tile(*lawn);
            break;
        case PadButton::Start:
            if (auto r = button_rect(platform::KeyBind::Action::Pause)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        case PadButton::L3:
            if (auto r = button_rect(platform::KeyBind::Action::FastForward)) tap(r->x + r->w / 2, r->y + r->h / 2);
            break;
        default: break;
        }
    }

    // Once a frame, between frames: held directions repeat, a held B digs after a second, and the
    // queued touches go out one step at a time.
    void frame() {
        auto now = now_ticks();
        for (auto& d : dirs_)
            if (d.held && now >= d.next) {
                move(d.dx, d.dy);
                d.next = now + ticks(kRepeatEvery);
            }
        if (b_since_ && now - b_since_ >= ticks(kDigHold)) {
            b_since_ = 0;
            if (auto lawn = this->lawn()) {
                if (auto r = button_rect(platform::KeyBind::Action::Shovel)) tap(r->x + r->w / 2, r->y + r->h / 2);
                tap_tile(*lawn);
            }
        }
        if (!steps_.empty() && --steps_.front().wait <= 0) {
            Step step = steps_.front();
            steps_.pop_front();
            x_ = step.x;
            y_ = step.y;
            send(step.phase);
        }
    }

    // A game key is pressed: the keyboard is in use again, so its hints come back.
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
        if (!lawn) return out;
        float fx = lawn->sw / lawn->lw, fy = lawn->sh / lawn->lh;
        // The tile, in the surface's pixels; red while B is held to dig.
        float x0 = lawn->ox + lawn->tw * col_, y0 = lawn->oy + lawn->th * row_;
        int left = static_cast<int>(lawn->ui_x(x0) * fx), top = static_cast<int>(lawn->ui_y(y0) * fy);
        int right = static_cast<int>(lawn->ui_x(x0 + lawn->tw) * fx), bottom = static_cast<int>(lawn->ui_y(y0 + lawn->th) * fy);
        out.boxes.push_back({left, top, right - left, bottom - top, b_since_ ? 0xFF4040u : 0xFFFFFFu});
        auto packets = packet_rects();
        if (slot_ >= 1 && static_cast<std::size_t>(slot_) <= packets.size()) {
            const Rect& r = packets[slot_ - 1];
            out.boxes.push_back({static_cast<int>(r.x * fx), static_cast<int>(r.y * fy), static_cast<int>(r.w * fx), static_cast<int>(r.h * fy), platform::kHintBorder});
        }
        if (!pad_hints_) return out;
        using platform::PadButton;
        auto label = [&](PadButton b, int x, int y, platform::HintLabel::Anchor anchor) {
            PadLabel l = pad_label(b, pad_type_);
            out.labels.push_back({l.text, x, y, anchor, l.rgb});
        };
        int n = static_cast<int>(packets.size());
        if (n > 1 && slot_ >= 1 && slot_ <= n) {
            const Rect& prev = packets[(slot_ + n - 2) % n];
            const Rect& next = packets[slot_ % n];
            label(PadButton::LB, static_cast<int>(prev.x * fx), static_cast<int>(prev.y * fy), platform::HintLabel::Anchor::TopLeft);
            label(PadButton::RB, static_cast<int>(next.x * fx), static_cast<int>(next.y * fy), platform::HintLabel::Anchor::TopLeft);
        }
        const std::pair<platform::KeyBind::Action, PadButton> kOnButtons[] = {
            {platform::KeyBind::Action::Shovel, PadButton::B}, {platform::KeyBind::Action::PlantFood, PadButton::Y},
            {platform::KeyBind::Action::FastForward, PadButton::L3}, {platform::KeyBind::Action::Pause, PadButton::Start}};
        for (auto [action, b] : kOnButtons)
            if (auto r = button_rect(action))
                label(b, static_cast<int>((r->x + r->w) * fx), static_cast<int>((r->y + r->h) * fy), platform::HintLabel::Anchor::BottomRight);
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
    static constexpr auto kDigHold = std::chrono::seconds(1);
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
        if (!mem.read<std::uint8_t>(board + kBoardFocus)) return std::nullopt;
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
        return l;
    }

    void move(int dx, int dy) {
        auto lawn = this->lawn();
        if (!lawn) return;
        col_ = std::clamp(col_ + dx, 0, lawn->cols - 1);
        row_ = std::clamp(row_ + dy, 0, lawn->rows - 1);
    }

    // A finger down and up at (x, y) in the UI's units, after the touches already queued.
    void tap(std::int32_t x, std::int32_t y) {
        steps_.push_back({1, 0, x, y});
        steps_.push_back({2, 3, x, y});
    }
    void tap_tile(const Lawn& l) {
        float wx = l.ox + l.tw * (col_ + 0.5f), wy = l.oy + l.th * (row_ + 0.5f);
        tap(static_cast<std::int32_t>(l.ui_x(wx)), static_cast<std::int32_t>(l.ui_y(wy)));
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
            if (r.w > 0 && r.h > 0) packets.push_back(r);
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
    GuestAddr board_vtable_ = 0, cursor_vtable_ = 0;
    Seen plant_cursor_;
    std::array<Dir, 4> dirs_{{{platform::PadButton::Up, 0, -1}, {platform::PadButton::Down, 0, 1},
                              {platform::PadButton::Left, -1, 0}, {platform::PadButton::Right, 1, 0}}};
    std::deque<Step> steps_;
    bool pad_active_ = false;
    platform::PadType pad_type_ = platform::PadType::Xbox;
    int col_ = 0, row_ = 2, slot_ = 1;
    std::int64_t b_since_ = 0;  // when B went down, while it's held
};

}  // namespace

GameKeyHandlers install_game_keys(Runtime& rt, const Module& pvz2, const GameKeyOptions& options) {
    auto keys = std::make_shared<GameKeys>(rt, pvz2, options.hint_binds, options.pad_hints);
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
    bool pad = options.controller && keys->install_pad(pvz2);
    if (options.controller) log::info("controller on the lawn: {}", pad ? "on" : "this libPVZ2.so is a build it doesn't know; left off");
    if (pad) {
        handlers.pad = [keys](const platform::PadInput& p) { keys->pad(p); };
        handlers.frame = [keys] { keys->frame(); };
    }
    if (pad || !options.hint_binds.empty()) handlers.overlay = [keys] { return keys->overlay(); };
    return handlers;
}

}  // namespace rfu
