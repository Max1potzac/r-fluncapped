// rfu: loads PvZ2's Android libraries and runs them as far as the runtime currently gets.
//
// Today that is: map and link libc++_shared, libNimble, libPVZ2 and libRFLib, run every
// static initializer under the JIT, and report what the game imported that the runtime
// doesn't implement yet.

#include "core/log.h"
#include "jni/jni.h"
#include "runtime.h"

#include <fmt/ranges.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>

namespace {

void usage() {
    std::cerr << "usage: rfu --lib-dir <dir with libPVZ2.so> [options]\n"
                 "  --mount <guest>=<host>   make a host directory visible to the game\n"
                 "  --no-rflib               don't load libRFLib.so (run vanilla code only)\n"
                 "  --offline                no network: the game skips its CDN update and plays offline\n"
                 "  --log <level>            trace, debug, info (default), warn, error\n"
                 "  --dump-patches <file>    write the libPVZ2.so offsets RFLib patched, one per line\n";
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
    std::vector<std::pair<std::string, std::string>> mounts;
    bool load_rflib = true;
    std::string dump_patches;
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
            } else if (arg == "--dump-patches") {
                dump_patches = value();
            } else if (arg == "--offline") {
                options.network = false;
            } else if (arg == "--no-rflib") {
                load_rflib = false;
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
    } catch (const std::exception& e) {
        std::cerr << "rfu: " << e.what() << "\n";
        usage();
        return 2;
    }

    try {
        rfu::Runtime rt(options);
        for (const auto& [guest, host] : mounts) rt.vfs().mount(guest, rfu::path_from_utf8(host));

        // What the game's Java side does: System.loadLibrary() on libPVZ2 (bringing in its
        // dependencies), JNI_OnLoad on each library that has one, then RFLib, which looks for
        // libPVZ2 in /proc/self/maps and patches it.
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
        if (load_rflib) {
            auto rflib = options.lib_dir / "libRFLib.so";
            if (std::filesystem::exists(rflib)) {
                rt.linker().load(rflib);
                rt.run_initializers();
            } else {
                rfu::log::warn("no libRFLib.so in {}; running vanilla code only", rfu::path_to_utf8(options.lib_dir));
            }
        }
        rfu::log::info("all static initializers finished");

        auto sites = snapshot.patched_sites(rt.mem());
        rfu::log::info("{} code sites in libPVZ2.so were patched during initialization", sites.size());
        if (!dump_patches.empty()) {
            std::ofstream out(rfu::path_from_utf8(dump_patches));
            for (auto a : sites) out << fmt::format("{:#x}\n", a - pvz2.base());
        }

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
        return 0;
    } catch (const rfu::GuestFault& e) {
        rfu::log::error("guest fault: {}", e.what());
        return 1;
    } catch (const rfu::GuestExit& e) {
        rfu::log::info("guest exited with code {}", e.code);
        return e.code;
    } catch (const std::exception& e) {
        rfu::log::error("{}", e.what());
        return 1;
    }
}
