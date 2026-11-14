#include "android/app.h"

#include "android/apk.h"
#include "android/java_http.h"
#include "core/log.h"
#include "core/trace.h"
#include "jni/jni.h"
#include "runtime.h"
#include "shims/common.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <bit>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <thread>

namespace rfu::android {

namespace {

namespace fs = std::filesystem;

constexpr const char* kGameApp = "com/popcap/SexyAppFramework/AndroidGameApp";
constexpr const char* kSurfaceView = "com/popcap/SexyAppFramework/AndroidSurfaceView";
constexpr const char* kFrameworkActivity = "com/popcap/SexyAppFramework/SexyAppFrameworkActivity";
constexpr const char* kGameActivity = "com/popcap/PvZ2/PvZ2GameActivity";
// PvZ2 China's fork of the framework.
constexpr const char* kCnPackage = "com/popcap/pvz2cthddev/";
constexpr std::uint32_t kDeadBeef = 0xDEADBEEF;

// Where Android installs an APK; PvZ2 China opens its own to read its packages.
std::string apk_path(const std::string& package) { return "/data/app/" + package + "-1/base.apk"; }

std::string random_uuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uint64_t a = rng(), b = rng();
    a = (a & ~0xF000ull) | 0x4000;                       // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // variant 1
    return fmt::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", a >> 32, (a >> 16) & 0xFFFF, a & 0xFFFF, b >> 48, b & 0xFFFFFFFFFFFFull);
}

// Activity.getPreferences(): a typed key/value store, saved after every write.
class Preferences {
public:
    explicit Preferences(fs::path file) : file_(std::move(file)) {
        std::ifstream in(file_, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            auto t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
            if (t1 == std::string::npos || t2 == std::string::npos) continue;
            values_[unescape(line.substr(t1 + 1, t2 - t1 - 1))] = {line[0], unescape(line.substr(t2 + 1))};
        }
    }

    bool contains(const std::string& key) const { return values_.count(key) != 0; }
    void erase(const std::string& key) {
        values_.erase(key);
        save();
    }
    std::optional<std::string> get(const std::string& key, char type) const {
        auto it = values_.find(key);
        if (it == values_.end() || it->second.first != type) return std::nullopt;  // ClassCastException
        return it->second.second;
    }
    void put(const std::string& key, char type, std::string value) {
        values_[key] = {type, std::move(value)};
        save();
    }

private:
    static std::string escape(const std::string& s) {
        std::string out;
        for (char ch : s) {
            if (ch == '\\') out += "\\\\";
            else if (ch == '\n') out += "\\n";
            else if (ch == '\t') out += "\\t";
            else if (ch == '\r') out += "\\r";
            else out += ch;
        }
        return out;
    }
    static std::string unescape(const std::string& s) {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\' || i + 1 == s.size()) {
                out += s[i];
                continue;
            }
            char n = s[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
        }
        return out;
    }
    void save() const {
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
        fs::path tmp = file_;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            for (const auto& [key, v] : values_) out << v.first << '\t' << escape(key) << '\t' << escape(v.second) << '\n';
        }
        fs::rename(tmp, file_, ec);
        if (ec) log::warn("couldn't save preferences to {}: {}", path_to_utf8(file_), ec.message());
    }

    fs::path file_;
    std::map<std::string, std::pair<char, std::string>> values_;  // type: s(tring), i(nt), b(oolean)
};

// AndroidUIEventManager: input waiting for the game's next UI_ProcessEvents, serialised the
// way the Java side writes it into the game's direct ByteBuffer.
class UiEvents {
public:
    void touch(const platform::TouchInput& t) {
        if (tracing() && t.phase != platform::TouchInput::Moved)
            log::info("trace: touch {} at {:.0f}, {:.0f}", t.phase == platform::TouchInput::Began ? "down" : "up", t.x, t.y);
        auto& prev = touches_[t.id];
        if (t.phase == platform::TouchInput::Began) prev = {t.x, t.y};
        Record r;
        r.i32(0).i32(t.id).i32(static_cast<int>(t.x)).i32(static_cast<int>(t.y));
        r.i32(static_cast<int>(prev.first)).i32(static_cast<int>(prev.second)).i32(1).f64(timestamp());
        int phase = t.phase;
        if (phase == platform::TouchInput::Moved && prev.first == t.x && prev.second == t.y) phase = platform::TouchInput::Stationary;
        r.i32(phase).u32(kDeadBeef).u32(kDeadBeef);
        prev = {t.x, t.y};
        if (t.phase == platform::TouchInput::Ended || t.phase == platform::TouchInput::Cancelled) touches_.erase(t.id);
        push(std::move(r));
    }
    void key(const platform::KeyInput& k) {
        Record r;
        r.i32(1).i32(k.android_keycode).i32(static_cast<int>(k.unicode)).i32(k.down ? 0 : 1).f64(timestamp()).i32(k.repeat).u32(kDeadBeef);
        push(std::move(r));
    }
    void text(const platform::TextInput& t) {
        Record r;
        r.i32(6).i32(t.action).i32(static_cast<int>(t.utf8.size()));
        r.bytes.insert(r.bytes.end(), t.utf8.begin(), t.utf8.end());
        while (r.bytes.size() % 4) r.bytes.push_back(0);
        push(std::move(r));
    }
    void back() {
        Record r;
        r.i32(5).u32(kDeadBeef).u32(kDeadBeef).u32(kDeadBeef);
        push(std::move(r));
    }
    // HandleLostFocus: every touch in progress is cancelled.
    void cancel_touches() {
        auto touches = touches_;
        for (const auto& [id, pos] : touches) touch({platform::TouchInput::Cancelled, id, pos.first, pos.second});
    }

    // ProcessEvents(ByteBuffer): the event count at offset 0, events from offset 16. Returns
    // true when events are left over for the next call.
    bool write(GuestMemory& mem, GuestAddr buffer, std::uint64_t capacity) {
        std::lock_guard lock(mutex_);
        std::uint64_t pos = 16;
        std::int32_t count = 0;
        while (!queue_.empty() && pos + queue_.front().bytes.size() <= capacity) {
            const auto& b = queue_.front().bytes;
            mem.write_bytes(buffer + pos, b.data(), b.size());
            pos += b.size();
            ++count;
            queue_.pop_front();
        }
        if (capacity >= 4) mem.write<std::int32_t>(buffer, count);
        if (count) log::debug("UI_ProcessEvents: {} event(s) into a {}-byte buffer", count, capacity);
        return !queue_.empty();
    }

private:
    struct Record {
        std::vector<std::uint8_t> bytes;
        Record& u32(std::uint32_t v) {
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            return *this;
        }
        Record& i32(std::int32_t v) { return u32(static_cast<std::uint32_t>(v)); }
        Record& f64(double d) {
            auto v = std::bit_cast<std::uint64_t>(d);
            for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            return *this;
        }
    };
    static double timestamp() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    void push(Record r) {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(r));
    }

    std::mutex mutex_;
    std::deque<Record> queue_;
    std::map<int, std::pair<float, float>> touches_;
};

struct App {
    Runtime& rt;
    jni::Jni& jni;
    const AppOptions& options;
    AndroidPaths paths;
    std::unique_ptr<platform::Window> window;
    Preferences prefs;
    UiEvents events;

    // The Java objects native code holds on to.
    GuestAddr activity = 0, game_app = 0, gl_view = 0;

    // AndroidGameApp / AndroidSurfaceView state.
    bool splash_showing = true;  // touches are dropped until the game calls UI_DidRecieveFocus
    bool app_has_focus = false;  // AndroidUIEventManager.mbAppHasFocus
    bool focus_state = false;    // AndroidSurfaceView.mbFocusState: draw frames
    bool app_focused = false;    // AndroidGameApp.mbAppIsFocusedState
    bool got_focus_event = false, got_resumed_event = false, surface_exists = false;
    bool keyboard_showing = false;
    float view_scale = 0.5f;
    int framerate = 1000;
    bool quit = false;
    std::string device_id;

    // The game's Java classes: SexyAppFramework's, or PvZ2 China's fork.
    const bool china;
    const std::string cls_game_app, cls_surface;
    std::unique_ptr<ApkIndex> apk;  // PvZ2 China: its assets, read out of the APK
    std::vector<std::uint8_t> certificate;  // and the certificate it was signed with
    GuestAddr tw_purchase = 0;              // TWPurchase.sPurchase
    GuestAddr ui_manager = 0, audio_proxy = 0;
    // Activity.runOnUiThread: run on the main thread before the next frame.
    std::mutex ui_mutex;
    std::deque<std::function<void()>> ui_tasks;
    void run_on_ui(std::function<void()> f) {
        std::lock_guard lock(ui_mutex);
        ui_tasks.push_back(std::move(f));
    }
    void run_ui_tasks() {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lock(ui_mutex);
            tasks.swap(ui_tasks);
        }
        for (auto& t : tasks) t();
    }

    App(Runtime& r, const AppOptions& o)
        : rt(r), jni(jni::get(r)), options(o), paths(android_paths(r.options().package, o.version_code)),
          prefs(o.data_dir / "data" / "shared_prefs" / (o.game == Game::Pvz2c ? "SexyAppActivity.prefs" : "PvZ2GameActivity.prefs")),
          china(o.game == Game::Pvz2c),
          cls_game_app(china ? std::string(kCnPackage) + "AndroidGameApp" : kGameApp),
          cls_surface(china ? std::string(kCnPackage) + "AndroidSurfaceView" : kSurfaceView) {}

    std::uint64_t call(const std::string& cls, const char* name, GuestAddr self, std::vector<std::uint64_t> args = {}) {
        log::debug("java -> native: {}.{}", cls, name);
        return jni.call_native(cls, name, self, args);
    }
    GuestAddr str(const std::string& s) { return jni.new_string(s); }
    GuestAddr nullable_str(const std::optional<std::string>& s) { return s ? jni.new_string(*s) : 0; }

    void write_longs(GuestAddr array, std::initializer_list<std::int64_t> values) {
        auto a = jni.object(array);
        if (!a || !a->data) return;
        std::uint64_t i = 0;
        for (std::int64_t v : values)
            if (i < a->length) rt.mem().write<std::int64_t>(a->data + 8 * i++, v);
    }
    void write_ints(GuestAddr array, std::initializer_list<std::int32_t> values) {
        auto a = jni.object(array);
        if (!a || !a->data) return;
        std::uint64_t i = 0;
        for (std::int32_t v : values)
            if (i < a->length) rt.mem().write<std::int32_t>(a->data + 4 * i++, v);
    }

    // AndroidGameApp.TryGainFocus / TryLoseFocus.
    void try_gain_focus() {
        if (app_focused || !(got_focus_event || got_resumed_event) || !surface_exists) return;
        app_has_focus = true;  // mUIEventManager.HandleGotFocus()
        call(cls_game_app, "Native_applicationDidBecomeActive", game_app);
        app_focused = true;
        focus_state = true;
    }
    void try_lose_focus() {
        if (!app_focused || (got_focus_event && got_resumed_event && surface_exists)) return;
        app_focused = false;
        keyboard_showing = false;
        focus_state = false;
        app_has_focus = false;
        events.cancel_touches();
        call(cls_game_app, "Native_applicationWillResignActive", game_app);
    }

    // --script: inputs and actions keyed by frame number.
    struct Scripted {
        std::optional<platform::Input> input;
        std::string action;  // "screenshot <file>", "quit"
    };
    std::multimap<std::uint64_t, Scripted> script;
    std::streamoff script_read = 0;  // how far into the file: lines added later run too
    void load_script(const fs::path& file, std::uint64_t now);
    void save_screenshot(const fs::path& file);
    // "peek <address> <bytes>": logs guest memory as words and floats, to study game objects.
    void peek(const std::string& args) {
        std::istringstream ls(args);
        std::uint64_t addr = 0, size = 64;
        ls >> std::hex >> addr >> std::dec >> size;
        for (std::uint64_t off = 0; off < size; off += 16) {
            if (!rt.mem().is_committed(addr + off, 16)) break;
            std::string words, floats;
            for (int i = 0; i < 4; ++i) {
                auto w = rt.mem().read<std::uint32_t>(addr + off + 4 * i);
                float f;
                std::memcpy(&f, &w, 4);
                words += fmt::format(" {:08x}", w);
                floats += fmt::format(" {:10.3f}", f);
            }
            log::info("peek {:#x} +{:#05x}:{}  |{}", addr, off, words, floats);
        }
    }

    void implement_java();
    void implement_java_china();
    void handle(const platform::Input& input);
    int run();
    void start_china();
};

// Reads the script's lines from where the last call stopped. A frame written "+N" is N frames
// after `now`, so lines appended to the file while the game runs drive it live.
void App::load_script(const fs::path& file, std::uint64_t now) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("can't read the script " + path_to_utf8(file));
    in.seekg(script_read);
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        if (in.eof()) break;  // a line still being written: read it next time
        script_read = in.tellg();
        ++line_no;
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string when, verb;
        if (!(ls >> when >> verb)) continue;
        std::uint64_t frame = when[0] == '+' ? now + std::stoull(when.substr(1)) : std::stoull(when);
        using platform::TouchInput;
        auto touch = [&](std::uint64_t f, TouchInput::Phase phase, float x, float y) {
            script.emplace(f, Scripted{platform::Input{TouchInput{phase, 1, x, y}}, {}});
        };
        if (verb == "tap") {
            float x, y;
            ls >> x >> y;
            touch(frame, TouchInput::Began, x, y);
            touch(frame + 3, TouchInput::Ended, x, y);
        } else if (verb == "drag") {
            float x1, y1, x2, y2;
            int steps = 10;
            ls >> x1 >> y1 >> x2 >> y2 >> steps;
            steps = std::max(steps, 1);
            touch(frame, TouchInput::Began, x1, y1);
            for (int i = 1; i <= steps; ++i)
                touch(frame + i, TouchInput::Moved, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
            touch(frame + steps + 1, TouchInput::Ended, x2, y2);
        } else if (verb == "hover") {
            // The mouse moving with no button down, from x1, y1 to x2, y2 over `frames` frames.
            float x1, y1, x2, y2;
            int steps = 10;
            ls >> x1 >> y1 >> x2 >> y2 >> steps;
            steps = std::max(steps, 1);
            for (int i = 0; i <= steps; ++i)
                script.emplace(frame + i, Scripted{platform::Input{TouchInput{TouchInput::Moved, platform::kHoverPointer, x1 + (x2 - x1) * i / steps,
                                                                              y1 + (y2 - y1) * i / steps}}, {}});
        } else if (verb == "back") {
            script.emplace(frame, Scripted{platform::Input{platform::BackInput{}}, {}});
        } else if (verb == "focus") {
            // The window losing or getting back the focus (alt-tab): "focus off", "focus on".
            std::string state;
            ls >> state;
            if (state != "on" && state != "off") throw std::runtime_error(fmt::format("{}:{}: focus on or focus off", path_to_utf8(file), line_no));
            script.emplace(frame, Scripted{platform::Input{platform::FocusInput{state == "on"}}, {}});
        } else if (verb == "text") {
            // Typed as on a keyboard: digits as key presses, everything else as text.
            std::string text;
            std::getline(ls >> std::ws, text);
            std::uint64_t f = frame;
            for (char ch : text) {
                if (ch >= '0' && ch <= '9') {
                    int code = 7 + (ch - '0');
                    script.emplace(f, Scripted{platform::Input{platform::KeyInput{code, static_cast<std::uint32_t>(ch), true, 0}}, {}});
                    script.emplace(f + 1, Scripted{platform::Input{platform::KeyInput{code, static_cast<std::uint32_t>(ch), false, 0}}, {}});
                } else {
                    script.emplace(f, Scripted{platform::Input{platform::TextInput{platform::TextInput::Commit, std::string(1, ch)}}, {}});
                }
                f += 2;
            }
        } else if (verb == "key") {
            int code = 0;
            ls >> code;
            script.emplace(frame, Scripted{platform::Input{platform::KeyInput{code, code == 66 ? 10u : 0u, true, 0}}, {}});
            script.emplace(frame + 1, Scripted{platform::Input{platform::KeyInput{code, code == 66 ? 10u : 0u, false, 0}}, {}});
        } else if (verb == "scroll") {
            // A scroll keybind held for `frames` frames with the mouse at (x, y), the middle by default:
            // "scroll <left|right|up|down> <frames> [speed] [x y]".
            std::string dir, speed = "2%";
            int frames = 30;
            float x = window->width() / 2.0f, y = window->height() / 2.0f;
            ls >> dir >> frames >> speed >> x >> y;
            std::vector<std::string> errors;
            auto binds = platform::parse_keybinds("K scroll " + dir + " " + speed, errors);
            if (binds.empty()) throw std::runtime_error(fmt::format("{}:{}: {}", path_to_utf8(file), line_no, errors.at(0)));
            platform::ScrollDrag drag(binds[0], window->width(), window->height(), x, y);
            auto add = [&](std::uint64_t f, const std::vector<platform::ScrollDrag::Step>& steps) {
                for (const auto& step : steps)
                    touch(f, step.phase == platform::ScrollDrag::Step::Down ? TouchInput::Began : step.phase == platform::ScrollDrag::Step::Move ? TouchInput::Moved : TouchInput::Ended,
                          step.x, step.y);
            };
            for (int i = 0; i < frames; ++i) add(frame + i, drag.frame());
            add(frame + frames, drag.release());
        } else if (verb == "seed" || verb == "shovel" || verb == "plantfood" || verb == "fastforward" || verb == "pause") {
            // A game keybind pressed for three frames: "seed <n>", "shovel", "plantfood"...
            std::string rest;
            std::getline(ls, rest);
            std::vector<std::string> errors;
            auto binds = platform::parse_keybinds("K " + verb + rest, errors);
            if (binds.empty()) throw std::runtime_error(fmt::format("{}:{}: {}", path_to_utf8(file), line_no, errors.at(0)));
            int slot = binds[0].slot;
            auto action = binds[0].action;
            script.emplace(frame, Scripted{platform::Input{platform::GameKeyInput{action, slot, true}}, {}});
            script.emplace(frame + 3, Scripted{platform::Input{platform::GameKeyInput{action, slot, false}}, {}});
        } else if (verb == "pad") {
            // A controller button held for some frames: "pad <button> [frames] [xbox|ps|switch]".
            static const std::pair<const char*, platform::PadButton> kButtons[] = {
                {"a", platform::PadButton::A}, {"b", platform::PadButton::B}, {"x", platform::PadButton::X}, {"y", platform::PadButton::Y},
                {"lb", platform::PadButton::LB}, {"rb", platform::PadButton::RB}, {"lt", platform::PadButton::LT}, {"rt", platform::PadButton::RT},
                {"l3", platform::PadButton::L3}, {"r3", platform::PadButton::R3}, {"start", platform::PadButton::Start}, {"back", platform::PadButton::Back},
                {"up", platform::PadButton::Up}, {"down", platform::PadButton::Down}, {"left", platform::PadButton::Left}, {"right", platform::PadButton::Right},
                {"rup", platform::PadButton::RUp}, {"rdown", platform::PadButton::RDown}, {"rleft", platform::PadButton::RLeft}, {"rright", platform::PadButton::RRight}};
            std::string name, frames_text, type_name;
            ls >> name >> frames_text >> type_name;
            int frames = frames_text.empty() ? 3 : std::stoi(frames_text);
            auto b = std::find_if(std::begin(kButtons), std::end(kButtons), [&](const auto& k) { return name == k.first; });
            if (b == std::end(kButtons)) throw std::runtime_error(fmt::format("{}:{}: no pad button {}", path_to_utf8(file), line_no, name));
            auto type = type_name == "ps" ? platform::PadType::PlayStation : type_name == "switch" ? platform::PadType::Switch : platform::PadType::Xbox;
            script.emplace(frame, Scripted{platform::Input{platform::PadInput{b->second, true, type}}, {}});
            script.emplace(frame + std::max(frames, 1), Scripted{platform::Input{platform::PadInput{b->second, false, type}}, {}});
        } else if (verb == "stick") {
            // A controller stick pushed for some frames, then let go: "stick <l|r> <x> <y> <frames>".
            std::string side;
            float x = 0, y = 0;
            int frames = 30;
            ls >> side >> x >> y >> frames;
            auto b = side == "l" ? platform::PadButton::LStick : platform::PadButton::RStick;
            script.emplace(frame, Scripted{platform::Input{platform::PadInput{b, true, platform::PadType::Xbox, x, y}}, {}});
            script.emplace(frame + std::max(frames, 1), Scripted{platform::Input{platform::PadInput{b, true, platform::PadType::Xbox, 0, 0}}, {}});
        } else if (verb == "screenshot" || verb == "quit" || verb == "peek") {
            std::string rest;
            std::getline(ls >> std::ws, rest);
            script.emplace(frame, Scripted{std::nullopt, verb + " " + rest});
        } else {
            throw std::runtime_error(fmt::format("{}:{}: unknown script action {}", path_to_utf8(file), line_no, verb));
        }
    }
}

void App::save_screenshot(const fs::path& file) {
    using ReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
    static auto read_pixels = reinterpret_cast<ReadPixels>(SDL_GL_GetProcAddress("glReadPixels"));
    if (!read_pixels) return;
    int w = window->width(), h = window->height();
    window->bind_surface();
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4), flipped(rgba.size());
    read_pixels(0, 0, w, h, 0x1908, 0x1401, rgba.data());  // GL_RGBA, GL_UNSIGNED_BYTE
    std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y) std::memcpy(&flipped[y * row], &rgba[(h - 1 - y) * row], row);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(flipped.data(), w, h, 32, static_cast<int>(row), SDL_PIXELFORMAT_ABGR8888);
    if (s && SDL_SaveBMP(s, path_to_utf8(file).c_str()) == 0) log::info("saved {}", path_to_utf8(file));
    else log::warn("couldn't save {}: {}", path_to_utf8(file), SDL_GetError());
    if (s) SDL_FreeSurface(s);
}

void App::implement_java() {
    auto impl = [&](const std::string& cls, const char* name, jni::MethodImpl f) { jni.implement(cls, name, std::move(f)); };
    auto ret_str = [this](const std::string& s) { return str(s); };

    // --- AndroidGameApp: preferences ---------------------------------------------------
    impl(cls_game_app, "Config_ConfigKeyExists", [this](jni::JavaCall& j) -> std::uint64_t { return prefs.contains(j.string_arg(0)); });
    impl(cls_game_app, "Config_ConfigEraseKey", [this](jni::JavaCall& j) -> std::uint64_t {
        prefs.erase(j.string_arg(0));
        return 0;
    });
    impl(cls_game_app, "Config_ConfigReadString", [this](jni::JavaCall& j) { return nullable_str(prefs.get(j.string_arg(0), 's')); });
    impl(cls_game_app, "Config_ConfigReadInteger", [this](jni::JavaCall& j) -> std::uint64_t {
        auto v = prefs.get(j.string_arg(0), 'i');
        return static_cast<std::uint32_t>(v ? std::stoi(*v) : 0);
    });
    impl(cls_game_app, "Config_ConfigReadBoolean", [this](jni::JavaCall& j) -> std::uint64_t {
        auto v = prefs.get(j.string_arg(0), 'b');
        return v && *v == "1";
    });
    impl(cls_game_app, "Config_ConfigWriteString", [this](jni::JavaCall& j) -> std::uint64_t {
        prefs.put(j.string_arg(0), 's', j.string_arg(1));
        return 1;
    });
    impl(cls_game_app, "Config_ConfigWriteInteger", [this](jni::JavaCall& j) -> std::uint64_t {
        prefs.put(j.string_arg(0), 'i', std::to_string(static_cast<std::int32_t>(j.args.at(1))));
        return 1;
    });
    impl(cls_game_app, "Config_ConfigWriteBoolean", [this](jni::JavaCall& j) -> std::uint64_t {
        prefs.put(j.string_arg(0), 'b', (j.args.at(1) & 0xFF) ? "1" : "0");
        return 1;
    });

    // --- AndroidGameApp: input and the UI ----------------------------------------------
    impl(cls_game_app, "UI_ProcessEvents", [this](jni::JavaCall& j) -> std::uint64_t {
        auto buffer = jni.object(j.args.at(0));
        if (!buffer || !buffer->data) return 0;
        return events.write(rt.mem(), buffer->data, buffer->length);
    });
    impl(cls_game_app, "UI_DidRecieveFocus", [this](jni::JavaCall&) -> std::uint64_t {
        if (splash_showing) log::info("the game is up (it removed the splash screen)");
        splash_showing = false;
        return 0;
    });
    impl(cls_game_app, "UI_UpdateSelection", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Device_IsKeyboardShowing", [this](jni::JavaCall&) -> std::uint64_t { return keyboard_showing; });
    for (const char* name : {"Device_ShowKeyboard", "Device_ShowNumericKeyboard"})
        impl(cls_game_app, name, [this](jni::JavaCall&) -> std::uint64_t {
            keyboard_showing = true;
            window->set_text_input(true);
            return 0;
        });
    impl(cls_game_app, "Device_HideKeyboard", [this](jni::JavaCall&) -> std::uint64_t {
        keyboard_showing = false;
        window->set_text_input(false);
        return 0;
    });
    impl(cls_game_app, "Device_ForceRotateToOrientation", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    for (const char* name : {"Device_ExitToHome", "Device_KillApplication"})
        impl(cls_game_app, name, [this](jni::JavaCall& j) -> std::uint64_t {
            log::info("the game asked to close ({})", j.member.name);
            quit = true;
            return 0;
        });
    impl(cls_game_app, "Web_SysOpenURL", [](jni::JavaCall& j) -> std::uint64_t {
        std::string url = j.string_arg(0);
        log::info("opening {} in the browser", url);
#if SDL_VERSION_ATLEAST(2, 0, 14)
        SDL_OpenURL(url.c_str());
#endif
        return 1;
    });
    impl(cls_game_app, "isValidUrl", [](jni::JavaCall& j) -> std::uint64_t {
        std::string url = j.string_arg(0);
        return url.starts_with("http://") || url.starts_with("https://");
    });
    impl(cls_game_app, "DEBUG_PauseInJava", [](jni::JavaCall& j) -> std::uint64_t {
        log::warn("[PauseInJava] {}({})", j.string_arg(0), static_cast<std::int32_t>(j.args.at(1)));
        return 0;
    });

    // --- AndroidGameApp: device and app information ------------------------------------
    const std::string package = rt.options().package;
    impl(cls_game_app, "Info_SysGetProductVersion", [this](jni::JavaCall&) -> std::uint64_t { return static_cast<std::uint32_t>(options.version_code); });
    impl(cls_game_app, "Info_SysGetProductVersionString", [this, ret_str](jni::JavaCall&) { return ret_str(options.version_name); });
    impl(cls_game_app, "GetBuildURL", [ret_str](jni::JavaCall&) { return ret_str(""); });
    impl(cls_game_app, "Info_SysGetPackageName", [ret_str, package](jni::JavaCall&) { return ret_str(package); });
    impl(cls_game_app, "Info_SysGetActivityName", [ret_str](jni::JavaCall&) { return ret_str("com.popcap.PvZ2.PvZ2GameActivity"); });
    impl(cls_game_app, "Info_SysGetIntentExtraDataString", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Info_SysGetUserCurrencySymbol", [ret_str](jni::JavaCall&) { return ret_str("$"); });
    impl(cls_game_app, "Info_SysGetUserCurrencyCode", [ret_str](jni::JavaCall&) { return ret_str("USD"); });
    impl(cls_game_app, "Info_SysGetUserLocale", [ret_str](jni::JavaCall&) { return ret_str("en_US"); });
    impl(cls_game_app, "Info_SysGetCountryCodeString", [ret_str](jni::JavaCall&) { return ret_str("US"); });
    impl(cls_game_app, "Device_GetDeviceName", [ret_str](jni::JavaCall&) { return ret_str("rfu PC"); });
    impl(cls_game_app, "Device_IsTablet", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Device_GetCurrentUIOrientation", [](jni::JavaCall&) -> std::uint64_t { return 4; });  // landscape left
    impl(cls_game_app, "Util_GetUUIDString", [ret_str](jni::JavaCall&) { return ret_str(random_uuid()); });
    impl(cls_game_app, "Diag_GetDeviceID", [this, ret_str](jni::JavaCall&) { return ret_str(device_id); });
    impl(cls_game_app, "Diag_GetIDForAdvertisers", [ret_str](jni::JavaCall&) { return ret_str("00000000-0000-0000-0000-000000000000"); });
    impl(cls_game_app, "Diag_GetOSVersion", [ret_str](jni::JavaCall&) { return ret_str("11"); });
    impl(cls_game_app, "Diag_GetHardwareModel", [ret_str](jni::JavaCall&) { return ret_str("rfu PC"); });
    impl(cls_game_app, "Diag_GetMemoryAvailable", [](jni::JavaCall&) -> std::uint64_t { return 4ull << 30; });
    impl(cls_game_app, "Diag_GetMemoryUsed", [this](jni::JavaCall&) -> std::uint64_t { return rt.heap().bytes_in_use(); });
    impl(cls_game_app, "Diag_GetMemoryTotal", [](jni::JavaCall&) -> std::uint64_t { return 8ull << 30; });

    // --- AndroidGameApp: storage ---------------------------------------------------------
    impl(cls_game_app, "Device_GetCachesDir", [this, ret_str](jni::JavaCall&) { return ret_str(paths.internal + "/cache"); });
    impl(cls_game_app, "Resources_GetResourceFolder", [this, ret_str](jni::JavaCall&) { return ret_str(paths.internal + "/files"); });
    for (const char* name : {"Resources_GetUserDataFolder", "Resources_GetCacheDataFolder", "Resources_GetAppSupportDataFolder"})
        impl(cls_game_app, name, [this, ret_str](jni::JavaCall&) { return ret_str(paths.external + "/files"); });
    impl(cls_game_app, "Resources_GetExternalStorageDirectory", [ret_str](jni::JavaCall&) { return ret_str("/storage/emulated/0"); });
    impl(cls_game_app, "Resources_GetAssetFileInfo", [](jni::JavaCall&) -> std::uint64_t { return 0; });  // the APK has no game assets
    impl(cls_game_app, "Resources_GetAssetFileSize", [](jni::JavaCall&) -> std::uint64_t { return ~0ull; });
    impl(cls_game_app, "Resources_GetFileSystemBlockCount", [](jni::JavaCall&) -> std::uint64_t { return 64ull << 20; });
    impl(cls_game_app, "Resources_GetFileSystemBlocksFree", [](jni::JavaCall&) -> std::uint64_t { return 32ull << 20; });
    impl(cls_game_app, "Resources_GetFileSystemBlockSize", [](jni::JavaCall&) -> std::uint64_t { return 4096; });

    // --- AndroidSurfaceView ---------------------------------------------------------------
    impl(cls_surface, "Graphics_SetFramerate", [this](jni::JavaCall& j) -> std::uint64_t {
        framerate = std::max(1, static_cast<std::int32_t>(j.args.at(0)));
        return 0;
    });
    impl(cls_surface, "EglContextIsValid", [](jni::JavaCall&) -> std::uint64_t { return 1; });
    impl(cls_surface, "Graphics_GetGLViewSysFBO", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_surface, "Graphics_CanSetGLViewScaleFactor", [](jni::JavaCall&) -> std::uint64_t { return 1; });
    impl(cls_surface, "Graphics_GetGLViewScaleFactor", [this](jni::JavaCall&) -> std::uint64_t { return std::bit_cast<std::uint32_t>(view_scale); });
    impl(cls_surface, "Graphics_SetGLViewScaleFactor", [this](jni::JavaCall& j) -> std::uint64_t {
        view_scale = j.float_arg(0);
        return 0;
    });
    for (const char* name : {"Graphics_GetScreenSizeInPixels", "Graphics_GetScreenSizeInPoints"})
        impl(cls_surface, name, [this](jni::JavaCall& j) -> std::uint64_t {
            write_ints(j.args.at(0), {window->width(), window->height()});
            return 0;
        });
    impl(cls_surface, "Graphics_GetSafeRect", [this](jni::JavaCall& j) -> std::uint64_t {
        write_ints(j.args.at(0), {0, 0, 0, 0});
        return 0;
    });
    impl(cls_surface, "Graphics_GetPointSizeInPixels", [](jni::JavaCall&) -> std::uint64_t { return std::bit_cast<std::uint32_t>(1.0f); });

    // --- AndroidHttpProxy -----------------------------------------------------------------
    impl("com/popcap/SexyAppFramework/AndroidHttpProxy", "GetNetworkStatus", [this](jni::JavaCall&) -> std::uint64_t {
        return rt.options().network ? 2 : 0;  // NET_REACHABLE_WIFI / NET_NOT_REACHABLE
    });

    // --- Nimble's Network component: Network.Status is an enum (UNKNOWN, NONE, DEAD, OK), which
    // the game reads with ordinal(). -------------------------------------------------------------
    impl("com/ea/nimble/INetwork", "getStatus", [this](jni::JavaCall& j) -> std::uint64_t {
        GuestAddr status = j.jni.new_instance("com/ea/nimble/Network$Status");
        j.jni.set_field(status, "ordinal", rt.options().network ? 3 : 1);
        return status;
    });
    impl("com/ea/nimble/INetwork", "isNetworkWifi", [this](jni::JavaCall&) -> std::uint64_t { return rt.options().network; });
    impl("java/lang/Enum", "ordinal", [](jni::JavaCall& j) -> std::uint64_t { return j.jni.field(j.self, "ordinal").value_or(0); });

    // --- AndroidNotification ---------------------------------------------------------------
    // There are no local notifications on the desktop, so nothing is ever scheduled; the game
    // parses the list as a JSON array.
    impl("com/popcap/SexyAppFramework/AndroidNotification", "getScheduledLocalNotificationsData",
         [](jni::JavaCall& call) -> std::uint64_t { return call.jni.new_string("[]"); });

    // --- SexyAppFrameworkActivity (statics) ---------------------------------------------
    for (const char* cls : {kFrameworkActivity, kGameActivity}) {
        impl(cls, "instance", [this](jni::JavaCall&) -> std::uint64_t { return activity; });
        impl(cls, "getAndroidPackageName", [ret_str, package](jni::JavaCall&) { return ret_str(package); });
        impl(cls, "getHasCrashReport", [](jni::JavaCall&) -> std::uint64_t { return 0; });
        impl(cls, "FrameworkInfo_SysGetMainExpansionFilePath", [this, ret_str](jni::JavaCall&) { return ret_str(paths.obb_file); });
        impl(cls, "RemoveSplashScreen", [this](jni::JavaCall&) -> std::uint64_t {
            splash_showing = false;
            return 0;
        });
        impl(cls, "SplashScreenShowing", [this](jni::JavaCall&) -> std::uint64_t { return splash_showing; });
        impl(cls, "KillApplication", [this](jni::JavaCall&) -> std::uint64_t {
            quit = true;
            return 0;
        });
        impl(cls, "getPackageName", [ret_str, package](jni::JavaCall&) { return ret_str(package); });
    }
}

// PvZ2 China's Java side (com.popcap.pvz2cthddev.*), where it differs from SexyAppFramework's:
// implement_java() has answered the methods both share, under this game's class names.
void App::implement_java_china() {
    auto impl = [&](const std::string& cls, const char* name, jni::MethodImpl f) { jni.implement(cls, name, std::move(f)); };
    auto ret_str = [this](const std::string& s) { return str(s); };
    const std::string pkg = kCnPackage;
    const std::string package = rt.options().package;
    const std::string files = paths.internal + "/files", external_files = paths.external + "/files";

    // --- AndroidGameApp ----------------------------------------------------------------------
    // The version code comes back as a string here.
    impl(cls_game_app, "Info_SysGetProductVersion", [this, ret_str](jni::JavaCall&) { return ret_str(std::to_string(options.version_code)); });
    impl(cls_game_app, "Info_SysGetUserCurrencySymbol", [ret_str](jni::JavaCall&) { return ret_str("\xC2\xA5"); });  // ¥
    impl(cls_game_app, "Util_GetPackageName", [ret_str, package](jni::JavaCall&) { return ret_str(package); });
    impl(cls_game_app, "Util_GetElapsedRealtime", [](jni::JavaCall&) -> std::uint64_t {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    });
    // The user data, cache and app support folders are all Context.getFilesDir() here, and only the
    // "external files" folder is on the SD card.
    for (const char* name : {"Resources_GetUserDataFolder", "GetUserDataFolder", "Resources_GetCacheDataFolder", "Resources_GetAppSupportDataFolder"})
        impl(cls_game_app, name, [ret_str, files](jni::JavaCall&) { return ret_str(files); });
    for (const char* name : {"Resources_GetExternalFilesDirectory", "GetExternalFilesDirectory"})
        impl(cls_game_app, name, [ret_str, external_files](jni::JavaCall&) { return ret_str(external_files); });
    // Resources_GetAssetFileInfo(name, long[2] {offset, length}): a file that exists is itself, at
    // offset 0; anything else is an asset, "<6 characters>/<path>" -> "assets/<path>.smf" in the
    // APK (AndroidAssetUtils.GetAssetNameFromFileName), and the answer is the APK's path.
    auto asset_name = [](std::string name) {
        if (name.size() <= 6) return std::string();
        name = name.substr(6);
        std::replace(name.begin(), name.end(), '\\', '/');
        return name + ".smf";
    };
    impl(cls_game_app, "Resources_GetAssetFileInfo", [this, asset_name](jni::JavaCall& j) -> std::uint64_t {
        std::string name = j.string_arg(0);
        if (name.empty()) return 0;
        std::error_code ec;
        if (auto host = rt.vfs().to_host_read(name); host && fs::is_regular_file(*host, ec)) {
            write_longs(j.args.at(1), {0, static_cast<std::int64_t>(fs::file_size(*host, ec))});
            return str(name);
        }
        auto entry = apk ? apk->find("assets/" + asset_name(name)) : std::nullopt;
        if (!entry || !entry->stored) {
            log::debug("asset {} ({}) isn't in the APK", name, asset_name(name));
            return 0;
        }
        write_longs(j.args.at(1), {static_cast<std::int64_t>(entry->offset), static_cast<std::int64_t>(entry->size)});
        return str(apk_path(rt.options().package));
    });
    impl(cls_game_app, "Resources_GetAssetFileSize", [this, asset_name](jni::JavaCall& j) -> std::uint64_t {
        auto entry = apk ? apk->find("assets/" + asset_name(j.string_arg(0))) : std::nullopt;
        return entry && entry->stored ? entry->size : ~0ull;
    });
    impl(cls_game_app, "Resources_InstallApk", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    for (const char* name : {"Device_ShowKeyboard", "Device_HideKeyboard"})
        impl(cls_game_app, name, [this, name](jni::JavaCall&) -> std::uint64_t {
            keyboard_showing = name[7] == 'S';
            window->set_text_input(keyboard_showing);
            return 1;
        });
    impl(cls_game_app, "Device_IsSupportedUIOrientation", [](jni::JavaCall& j) -> std::uint64_t { return j.args.at(0) == 3 || j.args.at(0) == 4; });
    for (const char* name : {"Device_ExitApp", "UI_ShowExitGame"})
        impl(cls_game_app, name, [this](jni::JavaCall& j) -> std::uint64_t {
            log::info("the game asked to close ({})", j.member.name);
            quit = true;
            return 0;
        });
    impl(cls_game_app, "UI_ShowAlertDialog", [](jni::JavaCall& j) -> std::uint64_t {
        log::warn("the game shows a dialog: {}: {} [{}]", j.string_arg(0), j.string_arg(1), j.string_arg(2));
        return 0;
    });
    impl(cls_game_app, "UI_Dip2Pixels", [](jni::JavaCall& j) -> std::uint64_t { return j.args.at(0); });
    impl(cls_game_app, "SetFPS", [this](jni::JavaCall& j) -> std::uint64_t {
        framerate = (j.args.at(0) & 0xFF) ? 60 : 30;
        return 0;
    });
    impl(cls_game_app, "IsHighFPS", [this](jni::JavaCall&) -> std::uint64_t { return framerate > 30; });
    impl(cls_game_app, "IsEmulator", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Util_IsAutosmoking", [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Util_CallNativeCrashed", [](jni::JavaCall& j) -> std::uint64_t {
        log::error("the game reports a crash: {}", j.string_arg(0));
        return 0;
    });
    for (const char* name : {"LogBugly", "LogBuglyUserId", "UploadTracking", "Util_NotifyDecompressStage", "Util_StartPNS", "UI_StartShowProgressbar",
                             "UI_FinishShowProgressBar", "Device_Vibrate", "Device_VibrateWithPattern", "Device_CancelVibrate"})
        impl(cls_game_app, name, [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Device_CopyToClipBoard", [](jni::JavaCall& j) -> std::uint64_t {
        SDL_SetClipboardText(j.string_arg(0).c_str());
        return 0;
    });
    impl(cls_game_app, "Config_ConfigReadStringEx", [this](jni::JavaCall& j) { return nullable_str(prefs.get(j.string_arg(0), 's')); });
    // Device details. No phone, no SIM card, no notch.
    for (const char* name : {"Diag_GetDeviceIMEI", "Diag_GetDeviceIMSI"})
        impl(cls_game_app, name, [ret_str](jni::JavaCall&) { return ret_str(""); });
    impl(cls_game_app, "Diag_GetUniqueCharacterID", [this, ret_str](jni::JavaCall&) { return ret_str(device_id); });
    impl(cls_game_app, "Diag_GetPrimaryMACAddress", [ret_str](jni::JavaCall&) { return ret_str("02:00:00:00:00:00"); });
    impl(cls_game_app, "Diag_GetDeviceBRAND", [ret_str](jni::JavaCall&) { return ret_str("rfu"); });
    impl(cls_game_app, "Diag_GetDeviceMODEL", [ret_str](jni::JavaCall&) { return ret_str("rfu PC"); });
    impl(cls_game_app, "Diag_GetNetworkType", [this, ret_str](jni::JavaCall&) { return ret_str(rt.options().network ? "WIFI" : ""); });
    for (const char* name : {"Diag_GetMobileCardType", "Diag_GetSimProvince", "Diag_GetNotchHeight", "Diag_GetNotchWidth", "Diag_HasNotchInScreen", "Diag_IsBigRCorner"})
        impl(cls_game_app, name, [](jni::JavaCall&) -> std::uint64_t { return 0; });
    impl(cls_game_app, "Diag_GetAvailMemory", [](jni::JavaCall&) -> std::uint64_t { return 4ull << 30; });
    impl(cls_game_app, "Diag_GetUsedMemory", [this](jni::JavaCall&) -> std::uint64_t { return rt.heap().bytes_in_use(); });
    impl(cls_game_app, "getStatusBarHeight", [](jni::JavaCall&) -> std::uint64_t { return 0; });

    // --- AndroidSurfaceView --------------------------------------------------------------------
    impl(cls_surface, "Graphics_IsOpenGLES20", [](jni::JavaCall&) -> std::uint64_t { return 1; });
    impl(cls_surface, "Graphics_GetOpenGLESVersion", [](jni::JavaCall&) -> std::uint64_t { return 2; });
    impl(cls_surface, "IsInitailized", [](jni::JavaCall&) -> std::uint64_t { return 1; });
    impl(cls_surface, "HasDrawnFirstFrame", [this](jni::JavaCall&) -> std::uint64_t { return !splash_showing; });
    impl(cls_surface, "IsHighFPS", [this](jni::JavaCall&) -> std::uint64_t { return framerate > 30; });
    impl(cls_surface, "SetFPS", [this](jni::JavaCall& j) -> std::uint64_t {
        framerate = (j.args.at(0) & 0xFF) ? 60 : 30;
        return 0;
    });

    // --- PackageManager.getPackageInfo: the game checks the APK's signing certificate
    // (signatures[0].hashCode()), so it gets the real one, read from the APK. --------------------
    impl("android/content/pm/PackageManager", "getPackageInfo", [this](jni::JavaCall& j) -> std::uint64_t {
        GuestAddr info = jni.new_instance("android/content/pm/PackageInfo");
        jni.set_field(info, "versionCode", static_cast<std::uint32_t>(options.version_code));
        jni.set_field(info, "versionName", str(options.version_name));
        jni.set_field(info, "packageName", str(rt.options().package));
        if (j.args.size() > 1 && (j.args[1] & 0x40)) {  // GET_SIGNATURES
            GuestAddr sig = jni.new_instance("android/content/pm/Signature");
            GuestAddr list = jni.new_array('L', 1, "android/content/pm/Signature");
            jni.set_element(list, 0, sig);
            jni.set_field(info, "signatures", list);
            if (certificate.empty()) log::warn("the game checks the APK's signature, and there's no certificate in the APK");
        }
        return info;
    });
    impl("android/content/pm/Signature", "hashCode", [this](jni::JavaCall&) -> std::uint64_t {
        std::int32_t h = 1;  // Arrays.hashCode(byte[])
        for (std::uint8_t b : certificate) h = static_cast<std::int32_t>(31u * static_cast<std::uint32_t>(h) + static_cast<std::uint32_t>(static_cast<std::int8_t>(b)));
        return static_cast<std::uint32_t>(h);
    });
    impl("android/content/pm/Signature", "toByteArray", [this](jni::JavaCall&) -> std::uint64_t {
        GuestAddr a = jni.new_array('B', certificate.size());
        rt.mem().write_bytes(jni.array_data(a), certificate.data(), certificate.size());
        return a;
    });
    impl("android/content/pm/Signature", "toCharsString", [this](jni::JavaCall&) -> std::uint64_t {
        std::string hex;
        for (std::uint8_t b : certificate) hex += fmt::format("{:02x}", b);
        return str(hex);
    });

    // --- TWPurchase: Talkweb's offline SDK, which the game starts (InitializeSDK) and logs into
    // before it finishes loading. There's no SDK here: it starts at once and logs in as a guest
    // (its code 3006), as TWPurchase$1$1 and $1$1$1 answer the game, on the UI thread. ------------
    const std::string tw = pkg + "TWPurchase";
    impl(tw, "<init>", [this, tw](jni::JavaCall& j) -> std::uint64_t {
        GuestAddr self = jni.new_instance(tw);
        jni.set_field(self, "mNativeDriverPtr", j.args.at(0));
        tw_purchase = self;
        return self;
    });
    impl(tw, "instance", [this](jni::JavaCall&) -> std::uint64_t { return tw_purchase; });
    impl(tw, "InitializeSDK", [this, tw](jni::JavaCall& j) -> std::uint64_t {
        GuestAddr self = j.self;
        log::info("Talkweb SDK: starting (app id {}); logging in as a guest, offline", j.string_arg(0));
        std::uint64_t login_code = 0;
        if (const char* c = std::getenv("RFU_SDK_CODE")) login_code = std::strtoull(c, nullptr, 0);
        int relogin = 0;
        if (const char* r = std::getenv("RFU_SDK_RELOGIN")) relogin = std::atoi(r);
        run_on_ui([this, tw, self, login_code] {
            std::uint64_t driver = jni.field(self, "mNativeDriverPtr").value_or(0);
            call(tw, "Native_SDKInitStart", self, {driver});
            call(tw, "Native_SDKInitEnd", self, {driver, 0});
            call(tw, "Native_SDKLoginStart", self, {driver});
            call(tw, "Native_SDKLoginEnd", self, {driver, login_code, str("rfu-offline-" + device_id)});
        });
        // Experiment: re-fire the login-complete callback on later frames, in case the welcome
        // dialog subscribes to it only after our first (early) fire. Self-reschedules `relogin` times.
        if (relogin > 0) {
            auto refire = std::make_shared<std::function<void(int)>>();
            *refire = [this, tw, self, login_code, refire](int n) {
                std::uint64_t driver = jni.field(self, "mNativeDriverPtr").value_or(0);
                call(tw, "Native_SDKLoginStart", self, {driver});
                call(tw, "Native_SDKLoginEnd", self, {driver, login_code, str("rfu-offline-" + device_id)});
                if (n > 1) run_on_ui([refire, n] { (*refire)(n - 1); });
            };
            run_on_ui([refire, relogin] { (*refire)(relogin); });
        }
        return 0;
    });
    impl(tw, "GetDeviceID", [this, ret_str](jni::JavaCall&) { return ret_str(device_id); });
    impl(tw, "GetUniqueID", [this, ret_str](jni::JavaCall&) { return ret_str(device_id); });
    impl(tw, "GetUniqueCharacterID", [ret_str](jni::JavaCall&) {
        std::string id = random_uuid();
        id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
        return ret_str(id);
    });
    for (const char* name : {"GetSignature", "GetChannelID", "GetTWRequestHead", "GetDeclare", "GetGameCenterUrl", "GetChannelUpdateUrl"})
        impl(tw, name, [ret_str](jni::JavaCall&) { return ret_str(""); });

    // --- AndroidUIEventManager: the native engine drains the input queue through this, not
    // AndroidGameApp.UI_ProcessEvents; the byte layout is the one UiEvents::write already writes. ---
    impl(pkg + "AndroidUIEventManager", "ProcessEvents", [this](jni::JavaCall& j) -> std::uint64_t {
        auto buffer = jni.object(j.args.at(0));
        if (!buffer || !buffer->data) return 0;
        return events.write(rt.mem(), buffer->data, buffer->length);
    });

    // --- AndroidHttpProxy --------------------------------------------------------------------
    impl(pkg + "AndroidHttpProxy", "GetNetworkStatus", [this](jni::JavaCall&) -> std::uint64_t { return rt.options().network ? 2 : 0; });
    impl(pkg + "AndroidHttpProxy", "Startup", [](jni::JavaCall&) -> std::uint64_t { return 1; });

    // --- SexyAppFrameworkActivity (statics and the instance's methods) --------------------------
    for (const std::string cls : {pkg + "SexyAppFrameworkActivity", pkg + "SexyAppActivity"}) {
        impl(cls, "instance", [this](jni::JavaCall&) -> std::uint64_t { return activity; });
        impl(cls, "getGameApp", [this](jni::JavaCall&) -> std::uint64_t { return game_app; });
        impl(cls, "getSurfaceView", [this](jni::JavaCall&) -> std::uint64_t { return gl_view; });
        impl(cls, "getUIEventMgr", [this](jni::JavaCall&) -> std::uint64_t { return ui_manager; });
        impl(cls, "SplashScreenShowing", [this](jni::JavaCall&) -> std::uint64_t { return splash_showing; });
        impl(cls, "RemoveSplashScreen", [this](jni::JavaCall&) -> std::uint64_t {
            splash_showing = false;
            return 0;
        });
        impl(cls, "IsEnablePNS", [](jni::JavaCall&) -> std::uint64_t { return 0; });
        impl(cls, "IsFoldableDevice", [](jni::JavaCall&) -> std::uint64_t { return 0; });
        impl(cls, "ShowExitGame", [this](jni::JavaCall&) -> std::uint64_t {
            quit = true;
            return 0;
        });
    }
}

// SexyAppActivity.onCreate (SexyAppFrameworkActivity's), onStart and onResume, then the GL thread
// and the first window focus, in PvZ2 China's order.
void App::start_china() {
    const std::string pkg = kCnPackage;
    if (!options.apk.empty()) {
        apk = std::make_unique<ApkIndex>(options.apk);
        certificate = apk->signing_certificate();
        log::info("APK: {} entries, signed with a {}-byte certificate", apk->size(), certificate.size());
    } else {
        log::warn("no --apk: the game's packages in its assets folder can't be found");
    }
    activity = jni.new_instance(pkg + "SexyAppActivity");
    ui_manager = jni.new_instance(pkg + "AndroidUIEventManager");
    gl_view = jni.new_instance(cls_surface);
    game_app = jni.new_instance(cls_game_app);
    GuestAddr http_proxy = jni.new_instance(pkg + "AndroidHttpProxy");
    GuestAddr package_manager = jni.new_instance("android/content/pm/PackageManager");
    framerate = 30;  // AndroidRenderer: 33 ms a frame unless the game asks for high FPS

    // onCreate: the GL view, then AndroidGameApp's constructor.
    window = std::make_unique<platform::Window>(options.window);
    if (!call(cls_game_app, "Native_GameAppInitialize", game_app, {gl_view, http_proxy, package_manager}))
        log::warn("Native_GameAppInitialize returned false");
    call(cls_game_app, "Native_applicationWillFinishLaunching", game_app, {0});  // onActivityCreate (no command line)
    call(cls_game_app, "Native_applicationDidFinishLaunching", game_app);        // onStart -> onActivityStart

    // onResume -> onActivityResume: TryGainFocus, then the two notifications.
    got_resumed_event = true;
    focus_state = app_has_focus = app_focused = true;
    call(cls_game_app, "Native_applicationWillBecomeForeground", game_app);
    call(cls_game_app, "Native_applicationDidBecomeActive", game_app);

    // The GL thread: onSurfaceCreated, onSurfaceChanged (once only, in this renderer).
    call(cls_surface, "Native_onSurfaceCreated", gl_view);
    call(cls_surface, "Native_onSurfaceChanged", gl_view,
         {static_cast<std::uint32_t>(window->width()), static_cast<std::uint32_t>(window->height())});
    surface_exists = true;
    got_focus_event = true;  // onWindowFocusChanged(true): already focused
}

void App::handle(const platform::Input& input) {
    std::visit(
        [this](const auto& in) {
            using T = std::decay_t<decltype(in)>;
            if constexpr (std::is_same_v<T, platform::QuitInput>) {
                quit = true;
            } else if constexpr (std::is_same_v<T, platform::ResizeInput>) {
                call(cls_surface, "Native_onSurfaceChanged", gl_view, {static_cast<std::uint32_t>(in.width), static_cast<std::uint32_t>(in.height)});
            } else if constexpr (std::is_same_v<T, platform::FocusInput>) {
                // Losing window focus on a PC is not the same as the app going to the
                // background, so only the focus notification is passed on.
                if (!china) {
                    call(cls_game_app, "Native_NotifyFocusChange", game_app, {in.focused ? 1u : 0u});
                } else if (in.focused) {
                    call(cls_game_app, "Native_applicationGainFocus", game_app);  // onWindowFocusChanged(true), focus lost only
                } else {
                    call(cls_game_app, "Native_applicationLoseFocus", game_app);  // the activity is still resumed
                    events.cancel_touches();
                }
            } else if constexpr (std::is_same_v<T, platform::BackInput>) {
                events.back();
            } else if constexpr (std::is_same_v<T, platform::TouchInput>) {
                if (app_has_focus && !splash_showing) events.touch(in);
                if (options.game_mouse) options.game_mouse();
            } else if constexpr (std::is_same_v<T, platform::GameKeyInput>) {
                if (options.game_keys && app_has_focus && !splash_showing) options.game_keys(in);
            } else if constexpr (std::is_same_v<T, platform::PadInput>) {
                if (options.game_pad && app_has_focus && !splash_showing && options.game_pad(in)) events.back();
            } else if constexpr (std::is_same_v<T, platform::KeyInput>) {
                // Keys only reach the game through the soft keyboard's InputConnection
                // (EditInputConnection.sendKeyEvent), which passes on Enter, Delete and the
                // digits and nothing else.
                bool passed = in.android_keycode == 66 || in.android_keycode == 67 || (in.android_keycode >= 7 && in.android_keycode <= 16);
                if (app_has_focus && keyboard_showing && passed) events.key(in);
            } else if constexpr (std::is_same_v<T, platform::TextInput>) {
                // commitText: a lone digit arrives as a key event instead.
                bool digit = in.utf8.size() == 1 && in.utf8[0] >= '0' && in.utf8[0] <= '9';
                if (keyboard_showing && !digit && !in.utf8.empty()) events.text(in);
            }
        },
        input);
}

int App::run() {
    device_id = prefs.get("rfu.device_id", 's').value_or("");
    if (device_id.empty()) {
        device_id = random_uuid();
        prefs.put("rfu.device_id", 's', device_id);
    }
    implement_java();
    if (china) implement_java_china();
    implement_http(rt, rt.options().network, china ? std::string(kCnPackage) + "AndroidHttpTransaction" : "com/popcap/SexyAppFramework/AndroidHttpTransaction");

    if (china) {
        start_china();
    } else {
    // The objects the game's Java side creates.
    activity = jni.new_instance(kGameActivity);
    gl_view = jni.new_instance(kSurfaceView);
    game_app = jni.new_instance(kGameApp);
    GuestAddr http_proxy = jni.new_instance("com/popcap/SexyAppFramework/AndroidHttpProxy");
    GuestAddr cloud = jni.new_instance("com/popcap/SexyAppFramework/cloud/Cloud");
    GuestAddr play_connect = jni.new_instance("com/popcap/SexyAppFramework/GooglePlay/GooglePlayConnect");
    GuestAddr achievements = jni.new_instance("com/popcap/SexyAppFramework/GooglePlay/GooglePlayAchievements");
    GuestAddr leaderboard = jni.new_instance("com/popcap/SexyAppFramework/GooglePlay/GooglePlayLeaderboard");
    GuestAddr notification = jni.new_instance("com/popcap/SexyAppFramework/AndroidNotification");

    // SexyAppFrameworkActivity.onCreate
    call(kFrameworkActivity, "Native_createNativeApplicationLifecycleObserver", activity);

    // PvZ2GameActivity.onCreate -> launchGame(): the GL view, then AndroidGameApp's constructor.
    window = std::make_unique<platform::Window>(options.window);
    if (!call(cls_game_app, "Native_GameAppInitialize", game_app,
              {gl_view, http_proxy, cloud, play_connect, achievements, leaderboard, notification}))
        log::warn("Native_GameAppInitialize returned false");
    call(cls_game_app, "Native_applicationWillFinishLaunching", game_app, {0});  // onActivityCreate (no command line)
    call(cls_game_app, "Native_applicationDidFinishLaunching", game_app);        // onActivityStart

    // onResume
    call(cls_game_app, "Native_NotifyAudioGainedFocus", game_app);
    got_resumed_event = true;
    call(cls_game_app, "Native_NotifyAppRunning", game_app, {1});
    call(cls_game_app, "Native_applicationWillBecomeForeground", game_app);
    try_gain_focus();

    // The GL thread starts: onSurfaceCreated, onSurfaceChanged; then SurfaceHolder's
    // surfaceCreated reaches AndroidGameApp.onSurfaceCreated.
    call(cls_surface, "Native_onSurfaceCreated", gl_view);
    call(cls_surface, "Native_onSurfaceChanged", gl_view,
         {static_cast<std::uint32_t>(window->width()), static_cast<std::uint32_t>(window->height())});
    surface_exists = true;
    call(cls_game_app, "Native_NotifySurfaceChange", game_app, {1});
    try_gain_focus();
    // onWindowFocusChanged(true)
    call(cls_game_app, "Native_NotifyFocusChange", game_app, {1});
    got_focus_event = true;
    try_gain_focus();
    }

    // AndroidRenderer.onDrawFrame, forever.
    if (!options.script.empty()) load_script(options.script, 0);
    auto* clear = reinterpret_cast<void (*)(unsigned)>(SDL_GL_GetProcAddress("glClear"));
    using clock = std::chrono::steady_clock;
    auto last_frame = clock::now();
    auto last_report = clock::now();
    std::uint64_t frames = 0, frames_since_report = 0;
    // If a frame never finishes, say where every guest thread is waiting.
    auto progress = std::make_shared<std::atomic<std::uint64_t>>(0);
    std::thread([&rt = rt, progress] {
        std::uint64_t seen = progress->load();
        int stalled = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            std::uint64_t now = progress->load();
            if (now != seen) {
                seen = now;
                stalled = 0;
            } else if (++stalled == 6) {
                log::warn("no frame for 30 s; the guest threads are:");
                for (const auto& line : rt.thread_report()) log::warn("  {}", line);
            }
        }
    }).detach();
    while (!quit) {
        for (const auto& input : window->poll()) handle(input);
        if (!options.script.empty() && frames % 10 == 0) load_script(options.script, frames);
        std::vector<std::string> actions;
        for (auto [it, end] = script.equal_range(frames); it != end; ++it) {
            if (it->second.input) handle(*it->second.input);
            else actions.push_back(it->second.action);
        }
        if (quit) break;
        if (!china) call(cls_surface, "PumpMessageQueue", gl_view);
        run_ui_tasks();
        if (options.game_frame && !splash_showing) options.game_frame();
        clear(0x4000);  // GL_COLOR_BUFFER_BIT, as the Java renderer does before every frame
        if (focus_state) {
            auto frame_time = std::chrono::milliseconds(1000 / framerate);
            auto elapsed = clock::now() - last_frame;
            if (elapsed < frame_time) std::this_thread::sleep_for(frame_time - elapsed);
            last_frame = clock::now();
            call(cls_surface, "Native_onDrawFrame", gl_view);
            // PvZ2 China's renderer takes its splash screen down after the tenth frame.
            if (china && splash_showing && frames >= 10) {
                splash_showing = false;
                log::info("the game is up (the splash screen is gone)");
            }
            if (options.overlay && !splash_showing) window->draw_hints(options.overlay());
        }
        ++frames;
        ++frames_since_report;
        progress->store(frames);
        for (const auto& a : actions) {
            if (a.starts_with("screenshot ")) save_screenshot(path_from_utf8(a.substr(11)));
            else if (a.starts_with("quit")) quit = true;
            else if (a.starts_with("peek ")) peek(a.substr(5));
        }
        if (options.max_frames && frames >= static_cast<std::uint64_t>(options.max_frames)) {
            if (!options.screenshot.empty()) save_screenshot(options.screenshot);
            quit = true;
        }
        window->swap();
        if (clock::now() - last_report >= std::chrono::seconds(10)) {
            log::info("{} frames, {:.1f} fps, guest heap {:.1f} MiB", frames, frames_since_report / std::chrono::duration<double>(clock::now() - last_report).count(),
                      rt.heap().bytes_in_use() / 1048576.0);
            last_report = clock::now();
            frames_since_report = 0;
        }
    }

    // Closing the window: onPause, then onDestroy. The game saves on the way.
    if (china) {
        // AndroidGameApp.onActivityPause, then onActivityStop and onActivityDestroy.
        got_resumed_event = false;
        call(cls_game_app, "Native_applicationWillResignActive", game_app);
        call(cls_game_app, "Native_applicationDidEnterBackground", game_app);
        focus_state = app_has_focus = app_focused = false;
        call(cls_game_app, "Native_applicationWillTerminate", game_app);
        return 0;
    }
    got_resumed_event = false;
    call(cls_game_app, "Native_NotifyAppRunning", game_app, {0});
    try_lose_focus();
    call(cls_game_app, "Native_applicationDidEnterBackground", game_app);
    call(cls_surface, "PumpMessageQueue", gl_view);
    call(cls_game_app, "Native_applicationWillTerminate", game_app);
    return 0;
}

}  // namespace

AndroidPaths android_paths(const std::string& package, int version_code) {
    AndroidPaths p;
    p.internal = "/data/data/" + package;
    p.external = "/storage/emulated/0/Android/data/" + package;
    p.obb_dir = "/storage/emulated/0/Android/obb/" + package;
    p.obb_file = fmt::format("{}/main.{}.{}.obb", p.obb_dir, version_code, package);
    return p;
}

void mount_storage(Runtime& rt, const AppOptions& options) {
    const std::string& package = rt.options().package;
    auto paths = android_paths(package, options.version_code);
    fs::path data = options.data_dir / "data", external = options.data_dir / "external", sdcard = options.data_dir / "sdcard";
    for (const auto& dir : {data / "files", data / "cache", external / "files", sdcard / "Android" / "obb" / package})
        fs::create_directories(dir);
    auto& vfs = rt.vfs();
    vfs.mount(paths.internal, data);
    vfs.mount("/data/user/0/" + package, data);
    vfs.mount("/storage/emulated/0", sdcard);
    vfs.mount("/sdcard", sdcard);
    vfs.mount(paths.external, external);
    vfs.mount("/sdcard/Android/data/" + package, external);
    if (!options.apk.empty()) vfs.mount(apk_path(package), options.apk);
    if (!options.obb.empty()) {
        // The OBB keeps its own name and folder on the PC; the game sees it where Android
        // would have put it.
        vfs.mount(paths.obb_file, options.obb);
        vfs.mount("/sdcard/Android/obb/" + package + "/" + fs::path(paths.obb_file).filename().string(), options.obb);
    }
}

int run_app(Runtime& rt, const AppOptions& options) {
    App app(rt, options);
    return app.run();
}

}  // namespace rfu::android
