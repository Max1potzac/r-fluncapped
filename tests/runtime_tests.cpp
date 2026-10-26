// Runtime tests: host-side units, and guest code from tests/guest/testlib.c run under the JIT.
//
//   rfu_tests <dir containing libtest.so>

#include "core/log.h"
#include "core/vfs.h"
#include "runtime.h"
#include "shims/abi.h"

#include <cstring>
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
                             "test_setjmp", "test_threads", "test_files", "test_stack_guard"}) {
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

}  // namespace

int main(int argc, char** argv) {
    rfu::log::set_level(rfu::log::Level::Warn);
    std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"quad", test_quad}, {"utf8", test_utf8}, {"vfs", test_vfs}, {"heap", test_heap}};
    if (argc > 1) tests.emplace_back("guest", [&] { test_guest(argv[1]); });
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
