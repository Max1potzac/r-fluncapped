#include "platform/keybinds.h"

#include <algorithm>
#include <sstream>

namespace rfu::platform {

namespace {

// "640" -> 640 pixels, "50%" -> 0.5 of the surface.
bool parse_coord(const std::string& s, float& value, bool& fraction) {
    if (s.empty()) return false;
    fraction = s.back() == '%';
    std::string number = fraction ? s.substr(0, s.size() - 1) : s;
    try {
        std::size_t used = 0;
        value = std::stof(number, &used);
        if (used != number.size() || value < 0) return false;
    } catch (const std::exception&) {
        return false;
    }
    if (fraction) value /= 100;
    return true;
}

}  // namespace

bool is_game_action(KeyBind::Action action) {
    using A = KeyBind::Action;
    return action == A::Seed || action == A::Shovel || action == A::PlantFood || action == A::FastForward || action == A::Pause;
}

const char* action_name(KeyBind::Action action) {
    switch (action) {
    case KeyBind::Action::Tap: return "tap";
    case KeyBind::Action::Back: return "back";
    case KeyBind::Action::Seed: return "seed";
    case KeyBind::Action::Shovel: return "shovel";
    case KeyBind::Action::PlantFood: return "plantfood";
    case KeyBind::Action::FastForward: return "fastforward";
    case KeyBind::Action::Pause: return "pause";
    case KeyBind::Action::Scroll: return "scroll";
    }
    return "?";
}

namespace {
// The finger lifts this close to the edge, and needs this much room before it to start.
constexpr float kEdge = 0.02f;
constexpr float kRoom = 0.12f;
}  // namespace

ScrollDrag::ScrollDrag(const KeyBind& bind, int width, int height, float x, float y)
    : w_(width), h_(height), start_x_(std::clamp(x, w_ * 0.05f, w_ * 0.95f)), start_y_(std::clamp(y, h_ * 0.05f, h_ * 0.95f)) {
    float speed = bind.speed_fraction ? bind.speed * height : bind.speed;
    dx_ = -bind.dir_x * speed;
    dy_ = -bind.dir_y * speed;
    // Start at the mouse, so the drag lands in the panel under it (a page can hold several
    // scrolling panels, like the almanac's zombie page, whose list is a strip at the bottom).
    // Only a point right at the edge moves back, just far enough for a short stroke.
    if (dx_ < 0) start_x_ = std::max(start_x_, w_ * (kEdge + kRoom));
    if (dx_ > 0) start_x_ = std::min(start_x_, w_ * (1 - kEdge - kRoom));
    if (dy_ < 0) start_y_ = std::max(start_y_, h_ * (kEdge + kRoom));
    if (dy_ > 0) start_y_ = std::min(start_y_, h_ * (1 - kEdge - kRoom));
}

std::vector<ScrollDrag::Step> ScrollDrag::frame() {
    if (!down_) {
        down_ = true;
        x_ = start_x_;
        y_ = start_y_;
        return {{Step::Down, x_, y_}};
    }
    x_ += dx_;
    y_ += dy_;
    // At the edge: lift here; the next frame starts over.
    if (x_ < w_ * kEdge || x_ > w_ * (1 - kEdge) || y_ < h_ * kEdge || y_ > h_ * (1 - kEdge)) {
        down_ = false;
        return {{Step::Move, x_, y_}, {Step::Up, x_, y_}};
    }
    return {{Step::Move, x_, y_}};
}

std::vector<ScrollDrag::Step> ScrollDrag::release() {
    if (!down_) return {};
    down_ = false;
    return {{Step::Up, x_, y_}};
}

std::vector<KeyBind> parse_keybinds(const std::string& text, std::vector<std::string>& errors) {
    std::vector<KeyBind> binds;
    std::istringstream lines(text);
    std::string line;
    for (int n = 1; std::getline(lines, line); ++n) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream words(line);
        std::string key, action;
        if (!(words >> key)) continue;
        auto fail = [&](const std::string& why) { errors.push_back("line " + std::to_string(n) + ": " + why); };
        if (!(words >> action)) {
            fail("no action for " + key);
            continue;
        }
        for (char& ch : key)
            if (ch == '_') ch = ' ';
        KeyBind bind;
        bind.key = key;
        if (action == "tap") {
            std::string x, y;
            if (!(words >> x >> y) || !parse_coord(x, bind.x, bind.x_fraction) || !parse_coord(y, bind.y, bind.y_fraction)) {
                fail("tap wants <x> <y> (pixels, or a percentage like 50%)");
                continue;
            }
        } else if (action == "back") {
            bind.action = KeyBind::Action::Back;
        } else if (action == "seed") {
            bind.action = KeyBind::Action::Seed;
            if (!(words >> bind.slot) || bind.slot < 1) {
                fail("seed wants the packet's number, from 1");
                continue;
            }
        } else if (action == "shovel") {
            bind.action = KeyBind::Action::Shovel;
        } else if (action == "plantfood") {
            bind.action = KeyBind::Action::PlantFood;
        } else if (action == "fastforward") {
            bind.action = KeyBind::Action::FastForward;
        } else if (action == "pause") {
            bind.action = KeyBind::Action::Pause;
        } else if (action == "scroll") {
            bind.action = KeyBind::Action::Scroll;
            std::string dir, speed;
            words >> dir;
            if (dir == "left") bind.dir_x = -1;
            else if (dir == "right") bind.dir_x = 1;
            else if (dir == "up") bind.dir_y = -1;
            else if (dir == "down") bind.dir_y = 1;
            else {
                fail("scroll wants left, right, up or down");
                continue;
            }
            if (words >> speed && (!parse_coord(speed, bind.speed, bind.speed_fraction) || bind.speed <= 0)) {
                fail("bad scroll speed " + speed + " (pixels a frame, or a percentage like 2%)");
                continue;
            }
        } else {
            fail("unknown action " + action + " (tap, back, seed, shovel, plantfood, fastforward, pause or scroll)");
            continue;
        }
        std::string extra;
        if (words >> extra) {
            fail("unexpected " + extra);
            continue;
        }
        binds.push_back(std::move(bind));
    }
    return binds;
}

}  // namespace rfu::platform
