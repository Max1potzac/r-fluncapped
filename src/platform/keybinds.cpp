#include "platform/keybinds.h"

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
        } else {
            fail("unknown action " + action + " (tap or back)");
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
