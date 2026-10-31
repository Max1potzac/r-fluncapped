// rfu: loads PvZ2's Android libraries and runs the game.
//
// Maps and links libc++_shared, libNimble, libPVZ2 and libRFLib, runs every static
// initializer and JNI_OnLoad under the JIT, then plays the part of the game's Java side
// (src/android): a window with an OpenGL ES context, audio, input and the activity
// lifecycle. --init-only stops after initialisation and reports what the game imported that
// the runtime doesn't implement.

#include "android/app.h"
#include "core/patches.h"
#include "core/trace.h"
#include "game/anim_fix.h"
#include "game/game_keys.h"
#include "game/hover.h"
#include "core/log.h"
#include "jni/jni.h"
#include "platform/gles.h"
#include "platform/opensles.h"
#include "runtime.h"

#include <fmt/ranges.h>

#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>

namespace {

void usage() {
    std::cerr << "usage: rfu --lib-dir <dir with libPVZ2.so> --obb <main.675.com.ea.game.pvz2_rfl.obb> [options]\n"
                 "  --data <dir>             where the game keeps its saves and downloads (default: rfu-data)\n"
                 "  --package <name>         Android package name (default: com.ea.game.pvz2_rfl)\n"
                 "  --version-code <n>       the APK's version code, which names the OBB (default: 675)\n"
                 "  --window <w>x<h>         window size in pixels (default: 1280x720)\n"
                 "  --fullscreen             start full screen (F11 switches)\n"
                 "  --assets high|low        the game's 1536 (default) or 768 asset set; low is lighter on slow GPUs\n"
                 "  --resolution <w>x<h>     the size the game draws at, whatever the window's (default: the\n"
                 "                           desktop's shape, 720 pixels tall for low assets, else 1080 to 1440)\n"
                 "  --angle                  draw through ANGLE (libEGL.dll and libGLESv2.dll next to rfu)\n"
                 "  --no-vsync               don't wait for the display between frames\n"
                 "  --mount <guest>=<host>   make a host directory visible to the game\n"
                 "  --overlay <guest>=<host> lay a host directory over a guest one, read-only (mods); later ones win\n"
                 "  --keybinds <file>        keys for seed packets, the shovel, plant food, fast-forward, pause, scrolling or spots of the screen (see src/platform/keybinds.h)\n"
                 "  --key-hints              show each keybind's key on the seed packet or button it presses\n"
                 "  --patch <file>           apply a code patch file after the libraries load (see src/core/patches.h)\n"
                 "  --hover                  collect sun, coins and plant food by moving the mouse over them\n"
                 "  --fix-animations         unfreeze plants stuck on an animation while the level isn't running\n"
                 "  --icon <file.png>        the window's icon\n"
                 "  --title <text>           the window's title\n"
                 "  --version-name <x.y.z>   the APK's version name, which the game reports (default: 1.4.0)\n"
                 "  --no-mod-libs            don't load the mod's own libraries (libRFLib, libSnowie...): vanilla code\n"
                 "  --offline                no network: the game skips its CDN update and plays offline\n"
                 "  --log <level>            trace, debug, info (default), warn, error\n"
                 "  --log-file <file>        write the log (and the game's own output) to a file\n"
                 "  --init-only              stop after initialisation and list unimplemented imports\n"
                 "  --frames <n>             quit after n frames (for testing)\n"
                 "  --screenshot <file.bmp>  with --frames: save the last frame\n"
                 "  --script <file>          scripted input for testing (see docs/runtime.md)\n"
                 "  --trace-class <name>     log calls to a libPVZ2 class's virtual functions (repeatable; see src/core/trace.h)\n"
                 "  --dump-patches <file>    write the libPVZ2.so offsets RFLib patched, one per line\n";
}

// The mod's own libraries in the APK's lib folder: every .so but the game's (libPVZ2 and what it
// links) and Crashlytics', which the game dlopens itself. Reflourished has libRFLib.so, AltverZ
// libSnowie.so.
std::vector<std::filesystem::path> mod_libraries(const std::filesystem::path& lib_dir) {
    static const std::set<std::string> game_libs{"libPVZ2.so", "libc++_shared.so", "libNimble.so"};
    std::vector<std::filesystem::path> libs;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(lib_dir, ec)) {
        std::string name = rfu::path_to_utf8(e.path().filename());
        if (e.path().extension() != ".so" || game_libs.count(name) || name.rfind("libcrashlytics", 0) == 0) continue;
        // Some mods keep data next to their code under a library's name (libSnowie.config.so).
        char magic[4] = {};
        std::ifstream(e.path(), std::ios::binary).read(magic, 4);
        if (std::memcmp(magic, "\x7f" "ELF", 4) != 0) continue;
        libs.push_back(e.path());
    }
    std::sort(libs.begin(), libs.end());
    return libs;
}

rfu::log::Level parse_level(const std::string& s) {
    using rfu::log::Level;
    static const std::map<std::string, Level> levels{
        {"trace", Level::Trace}, {"debug", Level::Debug}, {"info", Level::Info}, {"warn", Level::Warn}, {"error", Level::Error}};
    auto it = levels.find(s);
    if (it == levels.end()) throw std::invalid_argument("unknown log level " + s);
    return it->second;
}

// Snapshot of a module's executable segments, to see what the initializers patched.
struct CodeSnapshot {
    const rfu::Module* module = nullptr;
    std::vector<std::pair<rfu::GuestAddr, std::vector<std::uint8_t>>> segments;

    void take(rfu::GuestMemory& mem, const rfu::Module& m) {
        module = &m;
        for (const auto& seg : m.segments()) {
            if (!(seg.flags & 1)) continue;
            segments.push_back({seg.start, std::vector<std::uint8_t>(mem.host(seg.start), mem.host(seg.start) + seg.size)});
        }
    }

    // Distinct patched regions (runs of changed bytes less than 16 bytes apart count as one).
    std::vector<rfu::GuestAddr> patched_sites(rfu::GuestMemory& mem) const {
        std::vector<rfu::GuestAddr> sites;
        for (const auto& [start, before] : segments) {
            const std::uint8_t* now = mem.host(start);
            std::uint64_t last = 0;
            bool any = false;
            for (std::uint64_t i = 0; i < before.size(); i += 4) {
                if (std::memcmp(&before[i], now + i, 4) == 0) continue;
                if (!any || i - last > 16) sites.push_back(start + i);
                any = true;
                last = i;
            }
        }
        return sites;
    }
};

}  // namespace

int main(int argc, char** argv) {
    rfu::RuntimeOptions options;
    std::vector<std::pair<std::string, std::string>> mounts, overlays;
    std::vector<std::string> patch_files;
    bool load_mod_libs = true;
    bool fix_animations = false;
    bool key_hints = false;
    std::string dump_patches;
    std::vector<std::string> trace_classes;
    bool init_only = false;
    std::string log_file;
    rfu::android::AppOptions app;
    app.data_dir = "rfu-data";
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&] {
                if (i + 1 >= argc) throw std::invalid_argument(arg + " needs a value");
                return std::string(argv[++i]);
            };
            if (arg == "--lib-dir") {
                options.lib_dir = rfu::path_from_utf8(value());
            } else if (arg == "--mount") {
                std::string m = value();
                auto eq = m.find('=');
                if (eq == std::string::npos) throw std::invalid_argument("--mount wants <guest>=<host>");
                mounts.emplace_back(m.substr(0, eq), m.substr(eq + 1));
            } else if (arg == "--overlay") {
                std::string m = value();
                auto eq = m.find('=');
                if (eq == std::string::npos) throw std::invalid_argument("--overlay wants <guest>=<host>");
                overlays.emplace_back(m.substr(0, eq), m.substr(eq + 1));
            } else if (arg == "--patch") {
                patch_files.push_back(value());
            } else if (arg == "--keybinds") {
                std::string file = value();
                std::ifstream in(rfu::path_from_utf8(file));
                if (!in) throw std::invalid_argument("can't read " + file);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::vector<std::string> errors;
                app.window.keybinds = rfu::platform::parse_keybinds(text, errors);
                for (const auto& e : errors) std::cerr << "rfu: " << file << ": " << e << "\n";
            } else if (arg == "--icon") {
                app.window.icon = rfu::path_from_utf8(value());
            } else if (arg == "--title") {
                app.window.title = value();
            } else if (arg == "--version-name") {
                app.version_name = value();
            } else if (arg == "--hover") {
                app.window.hover = true;
            } else if (arg == "--fix-animations") {
                fix_animations = true;
            } else if (arg == "--key-hints") {
                key_hints = true;
            } else if (arg == "--trace-class") {
                trace_classes.push_back(value());
            } else if (arg == "--dump-patches") {
                dump_patches = value();
            } else if (arg == "--offline") {
                options.network = false;
            } else if (arg == "--obb") {
                app.obb = rfu::path_from_utf8(value());
            } else if (arg == "--data") {
                app.data_dir = rfu::path_from_utf8(value());
            } else if (arg == "--package") {
                options.package = value();
            } else if (arg == "--version-code") {
                app.version_code = std::stoi(value());
            } else if (arg == "--window") {
                std::string v = value();
                auto x = v.find('x');
                if (x == std::string::npos) throw std::invalid_argument("--window wants <width>x<height>");
                app.window.width = std::stoi(v.substr(0, x));
                app.window.height = std::stoi(v.substr(x + 1));
            } else if (arg == "--resolution") {
                std::string v = value();
                auto x = v.find('x');
                if (x == std::string::npos) throw std::invalid_argument("--resolution wants <width>x<height>");
                app.window.game_width = std::stoi(v.substr(0, x));
                app.window.game_height = std::stoi(v.substr(x + 1));
            } else if (arg == "--assets") {
                std::string v = value();
                if (v != "high" && v != "low") throw std::invalid_argument("--assets wants high or low");
                app.window.low_assets = v == "low";
            } else if (arg == "--fullscreen") {
                app.window.fullscreen = true;
            } else if (arg == "--angle") {
                app.window.angle = true;
            } else if (arg == "--no-vsync") {
                app.window.vsync = false;
            } else if (arg == "--init-only") {
                init_only = true;
            } else if (arg == "--frames") {
                app.max_frames = std::stoi(value());
            } else if (arg == "--script") {
                app.script = rfu::path_from_utf8(value());
            } else if (arg == "--screenshot") {
                app.screenshot = rfu::path_from_utf8(value());
            } else if (arg == "--no-mod-libs" || arg == "--no-rflib") {
                load_mod_libs = false;
            } else if (arg == "--log-file") {
                log_file = value();
            } else if (arg == "--log") {
                rfu::log::set_level(parse_level(value()));
            } else if (arg == "--help" || arg == "-h") {
                usage();
                return 0;
            } else {
                throw std::invalid_argument("unknown option " + arg);
            }
        }
        if (options.lib_dir.empty()) throw std::invalid_argument("--lib-dir is required");
        if (!init_only && app.obb.empty()) throw std::invalid_argument("--obb is required (or --init-only)");
        if (!app.obb.empty() && !std::filesystem::exists(app.obb))
            throw std::invalid_argument("no OBB at " + rfu::path_to_utf8(app.obb));
    } catch (const std::exception& e) {
        std::cerr << "rfu: " << e.what() << "\n";
        usage();
        return 2;
    }

    if (!log_file.empty()) {
        // Both streams into one file: rfu logs to stderr, the game prints to stdout.
#ifdef _WIN32
        FILE* f = _wfreopen(rfu::path_from_utf8(log_file).c_str(), L"w", stderr);
        if (f) _dup2(_fileno(stderr), _fileno(stdout));
#else
        FILE* f = std::freopen(log_file.c_str(), "w", stderr);
        if (f) dup2(fileno(stderr), fileno(stdout));
#endif
        if (!f) {
            std::cerr << "rfu: can't write " << log_file << "\n";
            return 2;
        }
        // Line by line, so a crash loses nothing (MSVC has no line buffering: unbuffered there).
#ifdef _WIN32
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
        std::setvbuf(stderr, nullptr, _IOLBF, 4096);
        std::setvbuf(stdout, nullptr, _IOLBF, 4096);
#endif
    }

    options.extra_shims = [](rfu::ShimRegistry& reg) {
        rfu::platform::register_gles(reg);
        rfu::platform::register_opensles(reg);
    };
    // Android ends an app's process without unwinding it, and the game's threads are still
    // running when it stops, so rfu ends the same way, whatever stopped it.
    auto finish = [](int code) {
        std::fflush(nullptr);
        std::_Exit(code);
    };
    try {
        auto& rt = *new rfu::Runtime(options);
        rfu::android::mount_storage(rt, app);
        for (const auto& [guest, host] : mounts) rt.vfs().mount(guest, rfu::path_from_utf8(host));
        for (const auto& [guest, host] : overlays) {
            rt.vfs().overlay(guest, rfu::path_from_utf8(host));
            rfu::log::info("overlay: {} over {}", host, guest);
        }

        // What the game's Java side does: System.loadLibrary() on libPVZ2 (bringing in its
        // dependencies), JNI_OnLoad on each library that has one, then the mod's own library
        // (Reflourished's libRFLib, AltverZ's libSnowie), which looks for libPVZ2 in
        // /proc/self/maps and patches it.
        auto& pvz2 = rt.linker().load(options.lib_dir / "libPVZ2.so");
        CodeSnapshot snapshot;
        snapshot.take(rt.mem(), pvz2);
        rt.run_initializers();
        rfu::GuestAddr vm = rfu::jni::get(rt).vm();
        for (const char* name : {"libNimble.so", "libPVZ2.so"}) {
            auto* m = rt.linker().find_module(name);
            auto onload = m ? m->find_export("JNI_OnLoad") : std::nullopt;
            if (!onload) continue;
            auto version = rt.call(*onload, {vm, 0});
            rfu::log::info("{}: JNI_OnLoad returned {:#x}", name, version & 0xFFFFFFFF);
        }
        if (load_mod_libs) {
            auto mod_libs = mod_libraries(options.lib_dir);
            if (mod_libs.empty()) rfu::log::info("no mod libraries in {}; running the game's own code only", rfu::path_to_utf8(options.lib_dir));
            for (const auto& path : mod_libs) {
                auto& m = rt.linker().load(path);
                rt.run_initializers();
                if (auto onload = m.find_export("JNI_OnLoad")) {
                    auto version = rt.call(*onload, {vm, 0});
                    rfu::log::info("{}: JNI_OnLoad returned {:#x}", rfu::path_to_utf8(path.filename()), version & 0xFFFFFFFF);
                }
            }
        }
        rfu::log::info("all static initializers finished");

        auto sites = snapshot.patched_sites(rt.mem());
        rfu::log::info("{} code sites in libPVZ2.so were patched during initialization", sites.size());
        if (!dump_patches.empty()) {
            std::ofstream out(rfu::path_from_utf8(dump_patches));
            for (auto a : sites) out << fmt::format("{:#x}\n", a - pvz2.base());
        }

        // Code patches go on after RFLib's own, so they can check (and build on) what it did, and
        // aren't counted as its.
        for (const auto& file : patch_files) {
            std::ifstream in(rfu::path_from_utf8(file));
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::vector<std::string> errors;
            auto patches = rfu::parse_patches(text, errors);
            std::string why = !in ? "can't read it" : !errors.empty() ? errors.front() : rfu::apply_patches(rt, patches);
            if (why.empty()) rfu::log::info("patch {}: {} changes applied", file, patches.size());
            else rfu::log::warn("patch {} left out: {}", file, why);
        }

        if (app.window.hover) app.window.hover = rfu::install_hover_collect(rt, pvz2);
        if (fix_animations) rfu::install_animation_fix(rt, pvz2);
        bool game_keybinds = !app.script.empty() || std::any_of(app.window.keybinds.begin(), app.window.keybinds.end(), [](const auto& b) {
            return rfu::platform::is_game_action(b.action);
        });
        if (game_keybinds) {
            auto keys = rfu::install_game_keys(rt, pvz2, key_hints ? app.window.keybinds : std::vector<rfu::platform::KeyBind>{});
            app.game_keys = keys.key;
            app.key_hints = keys.hints;
        }
        for (const auto& name : trace_classes) rfu::trace_class(rt, pvz2, name);

        const auto& missing = rt.linker().missing();
        rfu::log::info("{} imports have no implementation yet", missing.size());
        std::vector<std::string> called;
        for (std::uint32_t i = 0; i < rt.thunk_count(); ++i) {
            const rfu::Thunk* t = rt.thunk(i);
            if (t->calls && missing.count(t->name)) called.push_back(fmt::format("{} ({}x)", t->name, t->calls.load()));
        }
        if (!called.empty()) rfu::log::warn("unimplemented imports that were called: {}", fmt::join(called, ", "));
        rfu::log::debug("unimplemented imports: {}", fmt::join(missing, " "));
        rfu::log::info("guest heap in use: {:.1f} MiB", rt.heap().bytes_in_use() / 1048576.0);
        if (init_only) finish(0);
        finish(rfu::android::run_app(rt, app));
    } catch (const rfu::GuestFault& e) {
        rfu::log::error("guest fault: {}", e.what());
        finish(1);
    } catch (const rfu::GuestExit& e) {
        rfu::log::info("guest exited with code {}", e.code);
        finish(e.code);
    } catch (const std::exception& e) {
        rfu::log::error("{}", e.what());
        finish(1);
    }
}
