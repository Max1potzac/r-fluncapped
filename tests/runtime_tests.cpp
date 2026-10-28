// Runtime tests: host-side units, and guest code from tests/guest/testlib.c run under the JIT.
//
//   rfu_tests <dir containing libtest.so>

#include <cmath>
#include "core/hooks.h"
#include "core/log.h"
#include "core/patches.h"
#include "core/vfs.h"
#include "platform/keybinds.h"
#include "platform/png.h"
#include "runtime.h"
#include "shims/abi.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>

namespace {

int failures = 0;

#define EXPECT(cond)                                                                    \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::cerr << "  FAILED: " #cond " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

void test_quad() {
    for (double d : {0.0, 1.0, -2.5, 1e300, 3.14159, 1e-310}) EXPECT(rfu::quad_to_double(rfu::double_to_quad(d)) == d);
    // 1.0L in binary128: exponent 0x3FFF, zero mantissa.
    auto one = rfu::double_to_quad(1.0);
    EXPECT(one[1] == 0x3FFF'0000'0000'0000ull && one[0] == 0);
}

void test_utf8() {
    std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";  // a é € 😀
    auto w = rfu::utf8_to_u32(s);
    EXPECT(w == std::u32string({U'a', 0xE9, 0x20AC, 0x1F600}));
    EXPECT(rfu::u32_to_utf8(w) == s);
}

void test_vfs() {
    rfu::Vfs vfs;
    vfs.mount("/sdcard/data", "/host/data");
    EXPECT(vfs.absolute("/a/./b/../c//d") == "/a/c/d");
    EXPECT(rfu::path_to_utf8(*vfs.to_host("/sdcard/data/x/y.txt")) == "/host/data/x/y.txt");
    EXPECT(!vfs.to_host("/sdcard/database"));
    EXPECT(vfs.is_mount_parent("/") && vfs.is_mount_parent("//sdcard") && vfs.is_mount_parent("sdcard"));
    EXPECT(!vfs.is_mount_parent("/sdcard/data") && !vfs.is_mount_parent("/sdcard/dat") && !vfs.is_mount_parent("/storage"));
}

void test_vfs_overlay() {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "rfu_overlay_test";
    fs::remove_all(root);
    auto write = [](const fs::path& p, const std::string& text) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text;
    };
    write(root / "game/levels/a.json", "game a");
    write(root / "game/levels/b.json", "game b");
    write(root / "mod1/levels/a.json", "mod1 a");
    write(root / "mod1/levels/c.json", "mod1 c");
    write(root / "mod2/levels/c.json", "mod2 c");
    write(root / "mod2/new/d.json", "mod2 d");
    rfu::Vfs vfs;
    vfs.mount("/cdn", root / "game");
    vfs.overlay("/cdn", root / "mod1");
    vfs.overlay("/cdn", root / "mod2");
    auto read = [&](const std::string& guest) { return rfu::path_to_utf8(*vfs.to_host_read(guest)); };
    EXPECT(read("/cdn/levels/a.json") == rfu::path_to_utf8(root / "mod1/levels/a.json"));
    EXPECT(read("/cdn/levels/b.json") == rfu::path_to_utf8(root / "game/levels/b.json"));
    EXPECT(read("/cdn/levels/c.json") == rfu::path_to_utf8(root / "mod2/levels/c.json"));  // the later mod wins
    EXPECT(read("/cdn/levels") == rfu::path_to_utf8(root / "game/levels"));  // the game's directory stays its own
    EXPECT(read("/cdn/new") == rfu::path_to_utf8(root / "mod2/new"));  // one only a mod has
    EXPECT(rfu::path_to_utf8(*vfs.to_host("/cdn/levels/a.json")) == rfu::path_to_utf8(root / "game/levels/a.json"));  // writes
    EXPECT(vfs.layers("/cdn/levels").size() == 3 && vfs.layers("/cdn/new").size() == 1 && vfs.layers("/cdn/none").empty());
    fs::remove_all(root);
}

void test_keybinds() {
    std::vector<std::string> errors;
    auto binds = rfu::platform::parse_keybinds("# seed packets\n1 tap 5% 20%\nSpace tap 1180 650  # shovel\nLeft_Shift back\n"
                                               "Q jump\nW tap 10\n",
                                               errors);
    EXPECT(binds.size() == 3 && errors.size() == 2);
    EXPECT(binds[0].key == "1" && binds[0].x_fraction && binds[0].surface_x(1000) == 50 && binds[0].surface_y(1000) == 200);
    EXPECT(binds[1].key == "Space" && !binds[1].x_fraction && binds[1].surface_x(1920) == 1180);
    EXPECT(binds[2].key == "Left Shift" && binds[2].action == rfu::platform::KeyBind::Action::Back);
    EXPECT(!errors.empty() && errors[0].rfind("line 5:", 0) == 0);

    errors.clear();
    binds = rfu::platform::parse_keybinds("3 seed 3\nD shovel\n4 seed\n5 seed 0\n", errors);
    EXPECT(binds.size() == 2 && errors.size() == 2);
    EXPECT(binds[0].action == rfu::platform::KeyBind::Action::Seed && binds[0].slot == 3);
    EXPECT(binds[1].key == "D" && binds[1].action == rfu::platform::KeyBind::Action::Shovel);

    errors.clear();
    binds = rfu::platform::parse_keybinds("F plantfood\nV fastforward\nP pause\n", errors);
    EXPECT(binds.size() == 3 && errors.empty());
    EXPECT(binds[0].action == rfu::platform::KeyBind::Action::PlantFood && binds[1].action == rfu::platform::KeyBind::Action::FastForward &&
           binds[2].action == rfu::platform::KeyBind::Action::Pause);
    EXPECT(rfu::platform::is_game_action(binds[2].action) && !rfu::platform::is_game_action(rfu::platform::KeyBind::Action::Scroll));
    EXPECT(std::string(rfu::platform::action_name(binds[0].action)) == "plantfood");

    errors.clear();
    binds = rfu::platform::parse_keybinds("Right scroll right\nUp scroll up 10\nX scroll sideways\n", errors);
    EXPECT(binds.size() == 2 && errors.size() == 1);
    EXPECT(binds[0].dir_x == 1 && binds[0].speed_fraction && binds[1].dir_y == -1 && !binds[1].speed_fraction);
    // Scrolling right drags the finger left from the point, and lifts it near the edge.
    rfu::platform::ScrollDrag drag(binds[0], 1000, 500, 500, 250);
    auto steps = drag.frame();
    EXPECT(steps.size() == 1 && steps[0].phase == rfu::platform::ScrollDrag::Step::Down && steps[0].x == 500 && steps[0].y == 250);
    steps = drag.frame();
    EXPECT(steps.size() == 1 && steps[0].phase == rfu::platform::ScrollDrag::Step::Move && steps[0].x == 490);
    int frames = 0;
    while (frames < 100 && drag.frame().back().phase != rfu::platform::ScrollDrag::Step::Up) ++frames;
    EXPECT(frames == 39);  // 490 down to 100, ten pixels a frame
    EXPECT(drag.frame()[0].phase == rfu::platform::ScrollDrag::Step::Down && drag.release().size() == 1 && drag.release().empty());
    // From a point with no room to the left, it moves right only until a third of the width is free.
    rfu::platform::ScrollDrag edge(binds[0], 1000, 500, 150, 400);
    steps = edge.frame();
    EXPECT(std::abs(steps[0].x - (100 + 1000 / 3.0f)) < 0.01f && steps[0].y == 400);
    // Scrolling up drags down; from low on the screen it moves up only as far as it needs.
    rfu::platform::ScrollDrag low(binds[1], 1000, 500, 300, 480);
    steps = low.frame();
    EXPECT(steps[0].x == 300 && std::abs(steps[0].y - (450 - 500 / 3.0f)) < 0.01f);
    EXPECT(low.frame()[0].y > steps[0].y);
}

void test_patches() {
    std::vector<std::string> errors;
    auto p = rfu::parse_patches("# name: test\nlibPVZ2.so+0x1a2b3c 1F 20 03 D5 was 00 00 80 52\nlibRFLib.so+10 C0035FD6\n"
                                "nolib 00\nlibPVZ2.so+0x10 1F 20 was 00\n",
                                errors);
    EXPECT(p.size() == 2 && errors.size() == 2);
    EXPECT(p[0].module == "libPVZ2.so" && p[0].offset == 0x1a2b3c && p[0].bytes == std::vector<std::uint8_t>({0x1F, 0x20, 0x03, 0xD5}));
    EXPECT(p[0].expected == std::vector<std::uint8_t>({0x00, 0x00, 0x80, 0x52}));
    EXPECT(p[1].offset == 0x10 && p[1].bytes.size() == 4 && p[1].expected.empty());
}

void test_png() {
    // A 2x2 RGBA PNG: red, green / blue, half-transparent white.
    const std::vector<std::uint8_t> png{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D, 0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x0C, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78,
        0x28, 0xA0, 0xDB, 0x77, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    std::string error;
    auto img = rfu::platform::decode_png(png, error);
    EXPECT(img && img->width == 2 && img->height == 2);
    if (img) {
        EXPECT((std::vector<std::uint8_t>(img->rgba.begin(), img->rgba.begin() + 8) == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255}));
        EXPECT((std::vector<std::uint8_t>(img->rgba.begin() + 8, img->rgba.end()) == std::vector<std::uint8_t>{0, 0, 255, 255, 255, 255, 255, 128}));
        auto small = rfu::platform::shrink(*img, 1);
        EXPECT(small.width == 1 && small.height == 1 && small.rgba[3] == (255 * 3 + 128) / 4);
    }
    EXPECT(!rfu::platform::decode_png({1, 2, 3}, error));
}

void test_heap() {
    rfu::GuestMemory mem;
    rfu::GuestHeap heap(mem);
    auto a = heap.alloc(24), b = heap.alloc_aligned(256, 100);
    EXPECT(a && b % 256 == 0);
    heap.free(a);
    EXPECT(heap.alloc(20) == a);  // same size class: reused
    heap.free(b);
    auto big = heap.alloc(10 << 20);
    std::memset(mem.host(big), 1, 10 << 20);
    heap.free(big);
}

void test_guest(const std::filesystem::path& dir) {
    rfu::RuntimeOptions options;
    options.lib_dir = dir;
    rfu::Runtime rt(options);
    auto& lib = rt.linker().load(dir / "libtest.so");
    rt.run_initializers();
    EXPECT(rt.linker().missing().empty());
    for (const auto& name : rt.linker().missing()) std::cerr << "  missing import: " << name << "\n";

    for (const char* test : {"test_constructor", "test_printf", "test_scanf", "test_strto", "test_heap", "test_qsort",
                             "test_setjmp", "test_threads", "test_files", "test_stack_guard", "test_sockets"}) {
        auto fn = lib.find_export(test);
        EXPECT(fn.has_value());
        if (!fn) continue;
        std::uint64_t r = 1;
        try {
            r = rt.call(*fn) & 0xFFFFFFFF;
        } catch (const rfu::GuestFault& e) {
            std::cerr << "  " << test << ": guest fault: " << e.what() << "\n";
        }
        if (r) std::cerr << "  " << test << " failed check " << r << "\n";
        EXPECT(r == 0);
    }
    rt.run_atexit(0);
    EXPECT(rt.call(*lib.find_export("destroyed_value")) == 7);
}

void test_hooks(const std::filesystem::path& dir) {
    // b, bl and adrp move with their targets; cbz can't move yet.
    auto b = rfu::relocate_instruction(0x14000010, 0x1000);  // b #0x40
    EXPECT(b.size() == 4 && b[2] == 0x1040);
    auto bl = rfu::relocate_instruction(0x94000010, 0x1000);  // bl #0x40: lr = 0x1004, then jump
    EXPECT(bl.size() == 8 && bl[2] == 0x1004 && bl[6] == 0x1040);
    EXPECT(rfu::relocate_instruction(0xB4000040, 0x1000).empty());  // cbz x0
    auto stp = rfu::relocate_instruction(0xA9BF7BFD, 0x1000);  // stp x29, x30, [sp, #-16]!
    EXPECT(stp.size() == 5 && stp[0] == 0xA9BF7BFD && stp[3] == 0x1004);

    rfu::RuntimeOptions options;
    options.lib_dir = dir;
    rfu::Runtime rt(options);
    auto& lib = rt.linker().load(dir / "libtest.so");
    rt.run_initializers();
    auto target = lib.find_export("hook_target"), caller = lib.find_export("call_hook_target");
    EXPECT(target && caller);
    if (!target || !caller) return;
    EXPECT(rt.call(*caller, {5}) == 1016);
    std::uint64_t seen = 0;
    bool skip = false;
    rfu::hook(rt, *target, "hook_target", [&](rfu::CallContext& c) {
        seen = c.x(0);
        if (skip) rfu::return_now(c, 7);
    });
    EXPECT(rt.call(*caller, {5}) == 1016 && seen == 5);  // watched, then ran as before
    skip = true;
    EXPECT(rt.call(*caller, {6}) == 1007 && seen == 6);  // replaced
}

void test_guest_offline(const std::filesystem::path& dir) {
    rfu::RuntimeOptions options;
    options.lib_dir = dir;
    options.network = false;
    rfu::Runtime rt(options);
    auto& lib = rt.linker().load(dir / "libtest.so");
    rt.run_initializers();
    auto fn = lib.find_export("test_offline");
    EXPECT(fn.has_value());
    if (fn) {
        auto r = rt.call(*fn) & 0xFFFFFFFF;
        if (r) std::cerr << "  test_offline failed check " << r << "\n";
        EXPECT(r == 0);
    }
}

}  // namespace

int main(int argc, char** argv) {
    rfu::log::set_level(rfu::log::Level::Warn);
    std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"quad", test_quad}, {"utf8", test_utf8}, {"vfs", test_vfs}, {"vfs overlay", test_vfs_overlay}, {"keybinds", test_keybinds}, {"patches", test_patches}, {"png", test_png}, {"heap", test_heap}};
    if (argc > 1) {
        tests.emplace_back("guest", [&] { test_guest(argv[1]); });
        tests.emplace_back("hooks", [&] { test_hooks(argv[1]); });
        tests.emplace_back("guest offline", [&] { test_guest_offline(argv[1]); });
    }
    else std::cerr << "no guest library directory given; skipping guest tests\n";
    for (auto& [name, fn] : tests) {
        int before = failures;
        try {
            fn();
        } catch (const std::exception& e) {
            std::cerr << "  exception: " << e.what() << "\n";
            ++failures;
        }
        std::cout << (failures == before ? "[ ok ] " : "[FAIL] ") << name << "\n";
    }
    return failures ? 1 : 0;
}
