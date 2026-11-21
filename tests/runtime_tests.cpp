// Runtime tests: host-side units, and guest code from tests/guest/testlib.c run under the JIT.
//
//   rfu_tests <dir containing libtest.so>

#include <cmath>
#include "core/file_patch.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/patches.h"
#include "core/vfs.h"
#include "game/game_keys.h"
#include "game/plant_behaviours.h"
#include "platform/key_hints.h"
#include "platform/keybinds.h"
#include "platform/png.h"
#include "runtime.h"
#include "shims/abi.h"

#include <algorithm>
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
    // Once the game writes a file (its CDN download), it reads its own: the overlay's copy is older.
    EXPECT(vfs.written("/cdn/levels/../levels/a.json") && !vfs.written("/cdn/levels/a.json"));
    EXPECT(read("/cdn/levels/a.json") == rfu::path_to_utf8(root / "game/levels/a.json"));
    EXPECT(!vfs.written("/cdn/levels/b.json") && read("/cdn/levels/b.json") == rfu::path_to_utf8(root / "game/levels/b.json"));
    EXPECT(read("/cdn/levels/c.json") == rfu::path_to_utf8(root / "mod2/levels/c.json"));
    // A file mounted on its own (the OBB) shows up in its folder's listing, if it's there.
    write(root / "elsewhere/main.obb", "obb");
    vfs.mount("/cdn/levels/main.obb", root / "elsewhere/main.obb");
    vfs.mount("/cdn/levels/gone.obb", root / "elsewhere/gone.obb");
    // Android's two names for shared storage are one folder to an overlay.
    write(root / "sd/files/x.json", "game x");
    write(root / "modsd/files/x.json", "mod x");
    vfs.mount("/storage/emulated/0", root / "sd");
    vfs.mount("/sdcard", root / "sd");
    vfs.overlay("/sdcard", root / "modsd");
    EXPECT(read("/storage/emulated/0/files/x.json") == rfu::path_to_utf8(root / "modsd/files/x.json"));
    EXPECT(read("/sdcard/files/x.json") == rfu::path_to_utf8(root / "modsd/files/x.json"));
    EXPECT(vfs.written("/sdcard/files/x.json") && !vfs.written("/storage/emulated/0/files/x.json"));
    EXPECT(read("/storage/emulated/0/files/x.json") == rfu::path_to_utf8(root / "sd/files/x.json"));
    auto kids = vfs.mounted_children("/cdn/levels");
    EXPECT(kids.size() == 1 && kids[0] == "main.obb");
    EXPECT(vfs.mounted_children("/cdn").empty());
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
    EXPECT(frames == 47);  // 490 down to 20, ten pixels a frame
    EXPECT(drag.frame()[0].phase == rfu::platform::ScrollDrag::Step::Down && drag.release().size() == 1 && drag.release().empty());
    // From a point with no room to the left, it moves right just far enough for a short stroke.
    rfu::platform::ScrollDrag edge(binds[0], 1000, 500, 50, 400);
    steps = edge.frame();
    EXPECT(std::abs(steps[0].x - 140) < 0.01f && steps[0].y == 400);
    // Scrolling up drags down, from the mouse even low on the screen (the almanac's zombie list).
    rfu::platform::ScrollDrag low(binds[1], 1000, 720, 300, 600);
    steps = low.frame();
    EXPECT(steps[0].x == 300 && steps[0].y == 600);
    EXPECT(low.frame()[0].y > steps[0].y);
    rfu::platform::ScrollDrag bottom(binds[1], 1000, 500, 300, 495);
    EXPECT(std::abs(bottom.frame()[0].y - 430) < 0.01f);
    EXPECT(low.frame()[0].y > steps[0].y);
}

void test_key_hints() {
    using rfu::platform::HintFill;
    using rfu::platform::HintLabel;
    EXPECT(rfu::platform::hint_text("f") == "F" && rfu::platform::hint_text("1") == "1");
    EXPECT(rfu::platform::hint_text("Space") == "SPC" && rfu::platform::hint_text("Keypad 1") == "KP1");
    EXPECT(rfu::platform::hint_text("Left_Shift") == "LSHF" && rfu::platform::hint_text("Up").size() == 1);
    EXPECT(rfu::platform::hint_text("F12") == "F12" && rfu::platform::hint_text("Application") == "APPL");
    // "1" at 720: 2-pixel font pixels, a border one of them thick, then the back, then the glyph's runs.
    auto fills = rfu::platform::layout_hint({"1", 10, 20, HintLabel::Anchor::TopLeft}, 1280, 720);
    EXPECT(fills.size() == 9 && fills[0].rgb == rfu::platform::kHintBorder && fills[1].rgb == rfu::platform::kHintBack);
    EXPECT(fills[0].x == 10 && fills[0].y == 20 && fills[0].w == 22 && fills[0].h == 26);
    EXPECT(fills[2].rgb == 0xFFFFFF && fills[2].x == 10 + 6 + 4 && fills[2].y == 26 && fills[2].w == 2);
    EXPECT(fills.back().w == 6);  // the foot: 01110
    // At a corner of the surface the badge stays on it.
    fills = rfu::platform::layout_hint({"D", 1300, 730, HintLabel::Anchor::BottomRight}, 1280, 720);
    EXPECT(fills[0].x == 1280 - 22 && fills[0].y == 720 - 26);
    fills = rfu::platform::layout_hint({"D", 300, 300, HintLabel::Anchor::BottomRight}, 1920, 1080);
    EXPECT(fills[0].x == 300 - 33 && fills[0].y == 300 - 39);
    EXPECT(rfu::platform::layout_hint({"", 0, 0, HintLabel::Anchor::TopLeft}, 1280, 720).empty());
    // A box's corners: eight arms, a dark edge's then the colour's.
    fills = rfu::platform::layout_box({100, 100, 80, 60, 0xFFFFFF, true}, 720);
    EXPECT(fills.size() == 16 && fills[8].x == 100 && fills[8].y == 100 && fills[8].w == 15 && fills[8].h == 3);
    // A meter: a dark row for each line of its disc, and none of the colour while empty...
    auto count = [](const std::vector<HintFill>& f, std::uint32_t rgb) { return std::count_if(f.begin(), f.end(), [&](const HintFill& x) { return x.rgb == rgb; }); };
    fills = rfu::platform::layout_meter({50, 50, 10, 0, 0xFF0000});
    EXPECT(count(fills, rfu::platform::kHintBack) == 22 && count(fills, 0xFF0000) == 0);
    // ... the right half at half way, and all of it when full.
    fills = rfu::platform::layout_meter({50, 50, 10, 0.5f, 0xFF0000});
    EXPECT(std::all_of(fills.begin(), fills.end(), [](const HintFill& f) { return f.rgb != 0xFF0000 || f.x >= 50; }));
    fills = rfu::platform::layout_meter({50, 50, 10, 1, 0xFF0000});
    EXPECT(count(fills, 0xFF0000) == 20);
}

void test_pad_binds() {
    using rfu::platform::PadButton;
    std::string error;
    auto binds = rfu::parse_pad_binds("plant=B, Dig=a,,vacuum=rt", error);
    EXPECT(error.empty() && binds.size() == 3);
    EXPECT(binds[0].action == PadButton::A && binds[0].button == PadButton::B);
    EXPECT(binds[1].action == PadButton::B && binds[1].button == PadButton::A);
    EXPECT(binds[2].action == PadButton::LT && binds[2].button == PadButton::RT);
    EXPECT(rfu::parse_pad_binds("plant=q", error).empty() && !error.empty());
    error.clear();
    EXPECT(rfu::parse_pad_binds("jump=a", error).empty() && !error.empty());
    // Each screen has its own actions; no screen is the lawn's.
    error.clear();
    binds = rfu::parse_pad_binds("Chooser.take=x,map.shop=lb,use=a,menu.press=y", error);
    EXPECT(error.empty() && binds.size() == 4);
    EXPECT(binds[0].screen == rfu::PadScreen::Chooser && binds[0].action == PadButton::A && binds[0].button == PadButton::X);
    EXPECT(binds[1].screen == rfu::PadScreen::Map && binds[1].action == PadButton::Y && binds[1].button == PadButton::LB);
    EXPECT(binds[2].screen == rfu::PadScreen::Lawn && binds[2].action == PadButton::X && binds[2].button == PadButton::A);
    EXPECT(binds[3].screen == rfu::PadScreen::Menu && binds[3].action == PadButton::A && binds[3].button == PadButton::Y);
    EXPECT(rfu::parse_pad_binds("map.plant=a", error).empty() && !error.empty());
    error.clear();
    EXPECT(rfu::parse_pad_binds("shop.touch=a", error).empty() && !error.empty());
}

void test_pad_layout() {
    using rfu::platform::PadButton;
    std::vector<std::string> errors;
    auto taps = rfu::parse_pad_layout("# the shop moved\nmap Y tap 95% 7.5%\r\n\n  any back tap 50% 0%  # middle top\n", errors);
    EXPECT(errors.empty() && taps.size() == 1 + rfu::kPadScreens);
    EXPECT(taps[0].screen == rfu::PadScreen::Map && taps[0].button == PadButton::Y);
    EXPECT(std::abs(taps[0].x - 0.95f) < 1e-5f && std::abs(taps[0].y - 0.075f) < 1e-5f);
    EXPECT(taps[1].screen == rfu::PadScreen::Lawn && taps.back().screen == rfu::PadScreen::Other && taps.back().button == PadButton::Back);
    taps = rfu::parse_pad_layout("shop a tap 1% 1%\nmap q tap 1% 1%\nmap a tap 1 1\nmap a tap 120% 5%\nmap a press\nlawn rb tap 10% 20%\n", errors);
    EXPECT(errors.size() == 5 && taps.size() == 1 && taps[0].button == PadButton::RB);
    EXPECT(errors[0].rfind("line 1:", 0) == 0 && errors[4].rfind("line 5:", 0) == 0);
}

void test_map_buttons() {
    std::string error;
    auto m = rfu::parse_map_buttons("Settings=137:38, shop=-75.5:53", error);
    EXPECT(error.empty() && m.settings && m.shop && !m.almanac && !m.zen_garden);
    EXPECT(m.settings->first == 137 && m.settings->second == 38 && m.shop->first == -75.5f);
    rfu::parse_map_buttons("arcade=1:2", error);
    EXPECT(!error.empty());
    error.clear();
    rfu::parse_map_buttons("almanac=12", error);
    EXPECT(!error.empty());
    error.clear();
    rfu::parse_map_buttons("almanac=1x:2", error);
    EXPECT(!error.empty());
}

void test_plant_options() {
    std::string error;
    rfu::PlantBehaviours::KillShot k;
    EXPECT(rfu::parse_kill_shot("PineconeDefault=PineconeKnockback", k, error) && k.normal == "PineconeDefault" && k.special == "PineconeKnockback");
    EXPECT(k.animation.empty());
    EXPECT(rfu::parse_kill_shot("PineconeDefault=PineconeKnockback:attack2", k, error) && k.special == "PineconeKnockback" && k.animation == "attack2");
    EXPECT(!rfu::parse_kill_shot("PineconeDefault", k, error) && !error.empty());
    for (const char* bad : {"=b", "a=", "a=:attack2", "a=b:", "a=b:averyveryverylonglabel1"})
        EXPECT(!rfu::parse_kill_shot(bad, k, error));
    rfu::PlantBehaviours::PlantId p;
    EXPECT(rfu::parse_plant_id("pinecone=1000", p, error) && p.plant == "pinecone" && p.id == 1000);
    for (const char* bad : {"pinecone", "=5", "pinecone=", "pinecone=0", "pinecone=x1", "pinecone=99999999999", "averyveryverylongplantname=400"})
        EXPECT(!rfu::parse_plant_id(bad, p, error));
}

void test_file_patch() {
    // "RFUPATCH", version 1, two writes, over a 10-byte file made 16 long
    auto le = [](std::string& s, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    std::string data = "RFUPATCH";
    le(data, 1, 4);
    le(data, 2, 4);
    le(data, 10, 8);
    le(data, 16, 8);
    le(data, 12, 8);  // past the end, out of order
    le(data, 4, 8);
    data += "WXYZ";
    le(data, 2, 8);
    le(data, 3, 8);
    data += "abc";
    std::string error;
    auto p = rfu::parse_file_patch(data, error);
    EXPECT(p && error.empty() && p->writes.size() == 2 && p->writes[0].offset == 2);
    if (!p) return;
    const std::string base = "0123456789";
    auto read_base = [&](std::uint64_t at, void* out, std::size_t n) {
        std::memcpy(out, base.data() + at, n);
        return n;
    };
    char buf[32] = {};
    EXPECT(p->read(0, buf, sizeof buf, read_base) == 16);
    EXPECT(std::string(buf, 16) == std::string("01abc56789\0\0WXYZ", 16));
    EXPECT(p->read(3, buf, 2, read_base) == 2 && std::string(buf, 2) == "bc");
    EXPECT(p->read(9, buf, 4, read_base) == 4 && std::string(buf, 4) == std::string("9\0\0W", 4));
    EXPECT(p->read(16, buf, 4, read_base) == 0);
    EXPECT(!rfu::parse_file_patch("RFUPATCX", error) && !error.empty());
    error.clear();
    EXPECT(!rfu::parse_file_patch(data.substr(0, data.size() - 1), error) && !error.empty());
    // overlapping writes are refused
    std::string bad = "RFUPATCH";
    le(bad, 1, 4);
    le(bad, 2, 4);
    le(bad, 4, 8);
    le(bad, 4, 8);
    le(bad, 0, 8);
    le(bad, 3, 8);
    bad += "abc";
    le(bad, 2, 8);
    le(bad, 1, 8);
    bad += "x";
    error.clear();
    EXPECT(!rfu::parse_file_patch(bad, error) && !error.empty());
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
        {"quad", test_quad}, {"utf8", test_utf8}, {"vfs", test_vfs}, {"vfs overlay", test_vfs_overlay}, {"keybinds", test_keybinds}, {"key hints", test_key_hints}, {"pad binds", test_pad_binds}, {"map buttons", test_map_buttons}, {"patches", test_patches}, {"png", test_png}, {"heap", test_heap}, {"plant options", test_plant_options}};
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
