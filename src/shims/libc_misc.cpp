// Process, time, environment, logging, setjmp/longjmp, qsort, signals, syscall(), and stubs
// for the networking and process-control calls a single-process port never needs.

#include "core/log.h"
#include "shims/common.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <thread>

#ifdef _MSC_VER
#include <stdlib.h>
#endif

namespace rfu {

namespace {

// --- time ------------------------------------------------------------------------------------

std::tm read_tm(const GuestMemory& mem, GuestAddr a) {
    std::tm t{};
    int f[9];
    mem.read_bytes(a, f, sizeof f);
    t.tm_sec = f[0], t.tm_min = f[1], t.tm_hour = f[2], t.tm_mday = f[3], t.tm_mon = f[4], t.tm_year = f[5];
    t.tm_wday = f[6], t.tm_yday = f[7], t.tm_isdst = f[8];
    return t;
}

void write_tm(Runtime& rt, GuestAddr a, const std::tm& t, std::int64_t gmtoff, const char* zone) {
    int f[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    rt.mem().write_bytes(a, f, sizeof f);
    rt.mem().write<std::int64_t>(a + 40, gmtoff);
    rt.mem().write<GuestAddr>(a + 48, intern_string(rt, zone));
}

std::tm host_gmtime(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}

std::tm host_localtime(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::time_t host_timegm(std::tm* t) {
#ifdef _WIN32
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}

// Fills guest *tm with local or UTC time for `t`.
void broken_down(Runtime& rt, GuestAddr out, std::time_t t, bool local) {
    if (!local) return write_tm(rt, out, host_gmtime(t), 0, "UTC");
    std::tm tm = host_localtime(t);
    std::tm copy = tm;
    std::int64_t gmtoff = static_cast<std::int64_t>(host_timegm(&copy) - t);
    write_tm(rt, out, tm, gmtoff, "LOCAL");
}

std::int64_t now_ns(bool realtime) {
    auto d = realtime ? std::chrono::system_clock::now().time_since_epoch() : std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

// --- environment -------------------------------------------------------------------------------

struct Environment {
    std::mutex mutex;
    std::map<std::string, GuestAddr> values;  // name -> guest "value" string
    bool initialised = false;
};

Environment& environment(Runtime& rt) {
    auto& env = rt.state<Environment>();
    std::lock_guard lock(env.mutex);
    if (!env.initialised) {
        env.initialised = true;
        for (auto [k, v] : {std::pair{"ANDROID_ROOT", "/system"}, {"ANDROID_DATA", "/data"}, {"EXTERNAL_STORAGE", "/sdcard"},
                            {"PATH", "/system/bin"}, {"TMPDIR", "/data/local/tmp"}})
            env.values[k] = rt.heap().strdup(v);
    }
    return env;
}

// --- futex (for libc++'s std::atomic wait and friends) -----------------------------------------

struct Futexes {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<GuestAddr, std::uint64_t> wakes;
};

std::int64_t futex(CallContext& c) {
    constexpr int kWait = 0, kWake = 1, kWaitBitset = 9, kWakeBitset = 10;
    GuestAddr addr = c.x(1);
    int op = static_cast<int>(c.x(2)) & 0x7F;
    auto val = static_cast<std::uint32_t>(c.x(3));
    auto& f = c.rt().state<Futexes>();
    std::unique_lock lock(f.mutex);
    if (op == kWait || op == kWaitBitset) {
        if (c.mem().read<std::uint32_t>(addr) != val) return -android::kEAGAIN;
        std::uint64_t seen = f.wakes[addr];
        auto woken = [&] { return f.wakes[addr] != seen; };
        if (GuestAddr ts = c.x(4)) {
            auto timeout = std::chrono::seconds(c.mem().read<std::int64_t>(ts)) + std::chrono::nanoseconds(c.mem().read<std::int64_t>(ts + 8));
            if (op == kWaitBitset) {  // absolute CLOCK_MONOTONIC deadline
                timeout -= std::chrono::nanoseconds(now_ns(false));
            }
            if (!f.cv.wait_for(lock, timeout, woken)) return -android::kETIMEDOUT;
        } else {
            f.cv.wait(lock, woken);
        }
        return 0;
    }
    if (op == kWake || op == kWakeBitset) {
        ++f.wakes[addr];
        f.cv.notify_all();
        return static_cast<std::int64_t>(val);
    }
    log::warn("futex op {} isn't supported", op);
    return -android::kENOSYS;
}

// --- qsort -------------------------------------------------------------------------------------

// A merge sort over element indices: it never misbehaves with an inconsistent comparator,
// and elements stay put (the comparator sees them in place) until the final permutation.
void guest_qsort(CallContext& c) {
    GuestAddr base = c.x(0);
    std::uint64_t n = c.x(1), size = c.x(2);
    GuestAddr cmp = c.x(3);
    if (n < 2 || !size) return;
    std::vector<std::uint64_t> idx(n), tmp(n);
    for (std::uint64_t i = 0; i < n; ++i) idx[i] = i;
    Cpu& cpu = c.cpu();
    auto less_equal = [&](std::uint64_t a, std::uint64_t b) {
        return static_cast<std::int32_t>(cpu.call(cmp, {base + a * size, base + b * size})) <= 0;
    };
    for (std::uint64_t width = 1; width < n; width *= 2) {
        for (std::uint64_t lo = 0; lo < n; lo += 2 * width) {
            std::uint64_t mid = std::min(lo + width, n), hi = std::min(lo + 2 * width, n);
            std::uint64_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = less_equal(idx[i], idx[j]) ? idx[i++] : idx[j++];
            while (i < mid) tmp[k++] = idx[i++];
            while (j < hi) tmp[k++] = idx[j++];
        }
        idx.swap(tmp);
    }
    std::vector<std::uint8_t> sorted(n * size);
    for (std::uint64_t i = 0; i < n; ++i) std::memcpy(&sorted[i * size], c.mem().host(base + idx[i] * size), size);
    std::memcpy(c.mem().host(base), sorted.data(), sorted.size());
}

// --- setjmp/longjmp ----------------------------------------------------------------------------
// Our own jmp_buf layout (bionic's is 32 longs, which is plenty): a magic word, x19-x30, sp,
// d8-d15, and the host call depth, so a longjmp can't cross a shim's host frames.

constexpr std::uint64_t kJmpMagic = 0x52465553'4A4D5042;  // "RFUSJMPB"

void save_jmp(CallContext& c) {
    GuestAddr buf = c.x(0);
    auto& jit = c.cpu().jit();
    c.mem().write(buf, kJmpMagic);
    for (int i = 0; i < 12; ++i) c.mem().write(buf + 8 + 8ull * i, jit.GetRegister(static_cast<std::size_t>(19 + i)));
    c.mem().write(buf + 104, jit.GetSP());
    for (int i = 0; i < 8; ++i) c.mem().write(buf + 112 + 8ull * i, jit.GetVector(static_cast<std::size_t>(8 + i))[0]);
    c.mem().write<std::uint64_t>(buf + 176, c.cpu().depth());
    c.ret(0);
}

void restore_jmp(CallContext& c) {
    GuestAddr buf = c.x(0);
    auto value = static_cast<std::int32_t>(c.x(1));
    if (c.mem().read<std::uint64_t>(buf) != kJmpMagic) throw GuestFault("longjmp to a jmp_buf setjmp never filled");
    if (c.mem().read<std::uint64_t>(buf + 176) != c.cpu().depth()) throw GuestFault("longjmp across a host callback isn't supported");
    auto& jit = c.cpu().jit();
    for (int i = 0; i < 12; ++i) jit.SetRegister(static_cast<std::size_t>(19 + i), c.mem().read<std::uint64_t>(buf + 8 + 8ull * i));
    jit.SetSP(c.mem().read<std::uint64_t>(buf + 104));
    for (int i = 0; i < 8; ++i) jit.SetVector(static_cast<std::size_t>(8 + i), {c.mem().read<std::uint64_t>(buf + 112 + 8ull * i), 0});
    // The thunk's `ret` now returns from setjmp a second time.
    c.ret(static_cast<std::uint64_t>(value ? value : 1));
}

// --- misc ------------------------------------------------------------------------------------

struct AbortMessage {
    std::string text;
};

constexpr std::uint32_t kPid = 4242;

void android_log(int prio, const char* tag, std::string msg) {
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    tag = tag ? tag : "";
    if (prio >= 6) log::error("[{}] {}", tag, msg);
    else if (prio == 5) log::warn("[{}] {}", tag, msg);
    else if (prio == 4) log::info("[{}] {}", tag, msg);
    else log::debug("[{}] {}", tag, msg);
}

}  // namespace

void register_libc_misc(ShimRegistry& reg) {
#ifdef _MSC_VER
    // The CRT's default reaction to a bad argument (an unknown strftime conversion, say) is
    // to kill the process. Bad guest arguments should fail the call instead.
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {});
#endif

    reg.add("__errno", [](CallContext& c) { c.ret(c.thread().errno_addr); });

    // --- lifetime ------------------------------------------------------------------------
    reg.add("__cxa_atexit", [](CallContext& c) {
        c.rt().add_atexit(c.x(0), c.x(1), c.x(2));
        c.ret(0);
    });
    reg.add("__cxa_finalize", [](CallContext& c) { c.rt().run_atexit(c.x(0)); });
    reg.add("__cxa_thread_atexit_impl", [](CallContext& c) { c.ret(0); });  // thread_local dtors: not yet
    reg.add("__register_atfork", [](CallContext& c) { c.ret(0); });
    reg.add("exit", [](CallContext& c) { c.rt().exit(static_cast<int>(c.x(0))); });
    reg.add("_exit", [](CallContext& c) { throw GuestExit{static_cast<int>(c.x(0))}; });
    reg.add("abort", [](CallContext& c) {
        std::string msg = c.rt().state<AbortMessage>().text;
        throw GuestFault("abort() called from " + fmt::format("{:#x}", c.x(30)) + (msg.empty() ? "" : ": " + msg));
    });
    reg.add("android_set_abort_message", [](CallContext& c) { c.rt().state<AbortMessage>().text = c.x(0) ? c.str(0) : ""; });
    reg.add("__stack_chk_fail", [](CallContext& c) { throw GuestFault(fmt::format("stack corruption detected (called from {:#x})", c.x(30))); });

    // --- logging -------------------------------------------------------------------------
    reg.add("__android_log_print", [](CallContext& c) {
        RegisterArgs args(c, 3);
        android_log(static_cast<int>(c.x(0)), c.str(1), guest_format(c.mem(), c.str(2), args));
        c.ret(1);
    });
    reg.add("__android_log_vprint", [](CallContext& c) {
        VaListArgs args(c.mem(), c.x(3));
        android_log(static_cast<int>(c.x(0)), c.str(1), guest_format(c.mem(), c.str(2), args));
        c.ret(1);
    });
    reg.add("__android_log_write", [](CallContext& c) {
        android_log(static_cast<int>(c.x(0)), c.str(1), c.x(2) ? c.str(2) : "");
        c.ret(1);
    });
    reg.add("openlog", [](CallContext& c) {});
    reg.add("closelog", [](CallContext& c) {});
    reg.add("syslog", [](CallContext& c) {
        RegisterArgs args(c, 2);
        log::info("[syslog] {}", guest_format(c.mem(), c.str(1), args));
    });

    // --- time ----------------------------------------------------------------------------
    reg.add("time", [](CallContext& c) {
        auto t = static_cast<std::uint64_t>(now_ns(true) / 1'000'000'000);
        if (c.x(0)) c.mem().write(c.x(0), t);
        c.ret(t);
    });
    reg.add("clock_gettime", [](CallContext& c) {
        auto clk = static_cast<int>(c.x(0));
        std::int64_t ns = now_ns(clk == 0 || clk == 5 || clk == 8);  // REALTIME, REALTIME_COARSE, REALTIME_ALARM
        c.mem().write<std::int64_t>(c.x(1), ns / 1'000'000'000);
        c.mem().write<std::int64_t>(c.x(1) + 8, ns % 1'000'000'000);
        c.ret(0);
    });
    reg.add("gettimeofday", [](CallContext& c) {
        std::int64_t ns = now_ns(true);
        if (c.x(0)) {
            c.mem().write<std::int64_t>(c.x(0), ns / 1'000'000'000);
            c.mem().write<std::int64_t>(c.x(0) + 8, (ns % 1'000'000'000) / 1000);
        }
        if (c.x(1)) c.mem().write<std::uint64_t>(c.x(1), 0);
        c.ret(0);
    });
    reg.add("clock", [](CallContext& c) {
        static const std::int64_t start = now_ns(false);
        c.ret(static_cast<std::uint64_t>((now_ns(false) - start) / 1000));  // CLOCKS_PER_SEC = 1000000
    });
    reg.add("nanosleep", [](CallContext& c) {
        auto d = std::chrono::seconds(c.mem().read<std::int64_t>(c.x(0))) + std::chrono::nanoseconds(c.mem().read<std::int64_t>(c.x(0) + 8));
        std::this_thread::sleep_for(d);
        if (c.x(1)) std::memset(c.mem().host(c.x(1)), 0, 16);
        c.ret(0);
    });
    reg.add("usleep", [](CallContext& c) {
        std::this_thread::sleep_for(std::chrono::microseconds(c.x(0) & 0xFFFFFFFF));
        c.ret(0);
    });
    reg.add("gmtime_r", [](CallContext& c) {
        broken_down(c.rt(), c.x(1), static_cast<std::time_t>(c.mem().read<std::int64_t>(c.x(0))), false);
        c.ret(c.x(1));
    });
    reg.add("localtime_r", [](CallContext& c) {
        broken_down(c.rt(), c.x(1), static_cast<std::time_t>(c.mem().read<std::int64_t>(c.x(0))), true);
        c.ret(c.x(1));
    });
    struct StaticTm {
        GuestAddr tm = 0;
        GuestAddr ctime = 0;
    };
    reg.add("gmtime", [](CallContext& c) {
        auto& s = c.rt().state<StaticTm>();
        if (!s.tm) s.tm = c.rt().heap().alloc_zeroed(android::kSizeofTm);
        broken_down(c.rt(), s.tm, static_cast<std::time_t>(c.mem().read<std::int64_t>(c.x(0))), false);
        c.ret(s.tm);
    });
    reg.add("localtime", [](CallContext& c) {
        auto& s = c.rt().state<StaticTm>();
        if (!s.tm) s.tm = c.rt().heap().alloc_zeroed(android::kSizeofTm);
        broken_down(c.rt(), s.tm, static_cast<std::time_t>(c.mem().read<std::int64_t>(c.x(0))), true);
        c.ret(s.tm);
    });
    reg.add("mktime", [](CallContext& c) {
        std::tm t = read_tm(c.mem(), c.x(0));
        std::time_t r = std::mktime(&t);
        broken_down(c.rt(), c.x(0), r, true);
        c.ret(static_cast<std::uint64_t>(r));
    });
    reg.add("timegm", [](CallContext& c) {
        std::tm t = read_tm(c.mem(), c.x(0));
        std::time_t r = host_timegm(&t);
        broken_down(c.rt(), c.x(0), r, false);
        c.ret(static_cast<std::uint64_t>(r));
    });
    reg.add("ctime", [](CallContext& c) {
        auto& s = c.rt().state<StaticTm>();
        if (!s.ctime) s.ctime = c.rt().heap().alloc_zeroed(32);
        std::tm t = host_localtime(static_cast<std::time_t>(c.mem().read<std::int64_t>(c.x(0))));
        std::strftime(c.mem().host_as<char>(s.ctime), 32, "%a %b %d %H:%M:%S %Y\n", &t);
        c.ret(s.ctime);
    });
    auto strftime = [](CallContext& c) {
        std::tm t = read_tm(c.mem(), c.x(3));
        if (!c.x(1)) return c.ret(0);
        c.ret(std::strftime(c.str(0), c.x(1), c.str(2), &t));
    };
    reg.add("strftime", strftime);
    reg.add("strftime_l", strftime);
    reg.add_data("timezone", {8, [](Runtime&, GuestAddr) {}});

    // --- environment and identity --------------------------------------------------------
    reg.add("getenv", [](CallContext& c) {
        auto& env = environment(c.rt());
        std::lock_guard lock(env.mutex);
        auto it = env.values.find(c.str(0));
        c.ret(it == env.values.end() ? 0 : it->second);
    });
    reg.add("setenv", [](CallContext& c) {
        auto& env = environment(c.rt());
        std::lock_guard lock(env.mutex);
        auto [it, inserted] = env.values.emplace(c.str(0), 0);
        if (inserted || c.x(2)) it->second = c.rt().heap().strdup(c.str(1));  // old values may still be in use
        c.ret(0);
    });
    reg.add("unsetenv", [](CallContext& c) {
        auto& env = environment(c.rt());
        std::lock_guard lock(env.mutex);
        env.values.erase(c.str(0));
        c.ret(0);
    });
    reg.add("getpid", [](CallContext& c) { c.ret(kPid); });
    reg.add("getppid", [](CallContext& c) { c.ret(1); });
    reg.add("geteuid", [](CallContext& c) { c.ret(10123); });
    reg.add("gethostname", [](CallContext& c) {
        std::strncpy(c.str(0), "localhost", c.x(1));
        c.ret(0);
    });
    for (const char* name : {"getpwnam", "getpwuid", "getgrgid", "getgrnam", "gethostbyname"})
        reg.add(name, [](CallContext& c) { c.fail(android::kENOENT, 0); });
    reg.add("getpwuid_r", [](CallContext& c) {
        c.mem().write<GuestAddr>(c.x(4), 0);
        c.ret(android::kENOENT);
    });

    // --- system information --------------------------------------------------------------
    reg.add("sysconf", [](CallContext& c) {
        switch (c.x(0)) {
        case 0x06: return c.ret(100);                     // _SC_CLK_TCK
        case 0x27:
        case 0x28: return c.ret(GuestMemory::kPageSize);  // _SC_PAGESIZE, _SC_PAGE_SIZE
        case 0x60:
        case 0x61: return c.ret(std::max(1u, std::min(8u, std::thread::hardware_concurrency())));  // _SC_NPROCESSORS_*
        case 0x62: return c.ret(1ull << 20);              // _SC_PHYS_PAGES (4 GiB)
        case 0x63: return c.ret(1ull << 19);              // _SC_AVPHYS_PAGES
        default:
            log::debug("sysconf({:#x}) unknown", c.x(0));
            return c.fail(android::kEINVAL);
        }
    });
    reg.add("getauxval", [](CallContext& c) {
        switch (c.x(0)) {
        case 6: return c.ret(GuestMemory::kPageSize);  // AT_PAGESZ
        case 16: return c.ret(0x3);                     // AT_HWCAP: fp, asimd
        default: return c.ret(0);
        }
    });
    reg.add("getentropy", [](CallContext& c) {
        static std::mt19937_64 rng{std::random_device{}()};
        for (std::uint64_t i = 0; i < c.x(1); ++i) c.mem().host(c.x(0))[i] = static_cast<std::uint8_t>(rng());
        c.ret(0);
    });
    struct Rand {
        std::uint64_t state = 1;
    };
    reg.add("srand", [](CallContext& c) { c.rt().state<Rand>().state = c.x(0) & 0xFFFFFFFF; });
    reg.add("rand", [](CallContext& c) {
        auto& s = c.rt().state<Rand>().state;
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        c.ret((s >> 33) & 0x7FFFFFFF);
    });
    reg.add("srand48", [](CallContext& c) { c.rt().state<Rand>().state = c.x(0); });
    reg.add("lrand48", [](CallContext& c) {
        auto& s = c.rt().state<Rand>().state;
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        c.ret((s >> 33) & 0x7FFFFFFF);
    });
    reg.add("getpagesize", [](CallContext& c) { c.ret(GuestMemory::kPageSize); });
    // mincore(addr, length, vec): everything counts as resident.
    reg.add("mincore", [](CallContext& c) {
        std::uint64_t pages = (c.x(1) + GuestMemory::kPageSize - 1) / GuestMemory::kPageSize;
        std::memset(c.mem().host(c.x(2)), 1, pages);
        c.ret(0);
    });
    reg.add("sysinfo", [](CallContext& c) {
        // struct sysinfo { long uptime; ulong loads[3]; ulong totalram, freeram, sharedram, bufferram,
        //                  totalswap, freeswap; u16 procs; ...; ulong totalhigh, freehigh; u32 mem_unit; }
        GuestAddr info = c.x(0);
        std::memset(c.mem().host(info), 0, 112);
        c.mem().write<std::int64_t>(info, 3600);
        c.mem().write<std::uint64_t>(info + 32, 4ull << 30);
        c.mem().write<std::uint64_t>(info + 40, 2ull << 30);
        c.mem().write<std::uint16_t>(info + 80, 1);
        c.mem().write<std::uint32_t>(info + 104, 1);
        c.ret(0);
    });
    reg.add("syscall", [](CallContext& c) {
        switch (c.x(0)) {
        case 98: return c.ret(static_cast<std::uint64_t>(futex(c)));  // futex: errors are -errno
        case 172: return c.ret(kPid);                                 // getpid
        case 178: return c.ret(c.thread().id == 1 ? kPid : kPid + c.thread().id);  // gettid
        case 278: {                                                   // getrandom
            static std::mt19937_64 rng{std::random_device{}()};
            for (std::uint64_t i = 0; i < c.x(2); ++i) c.mem().host(c.x(1))[i] = static_cast<std::uint8_t>(rng());
            return c.ret(c.x(2));
        }
        default:
            log::warn("syscall({}) isn't supported", c.x(0));
            return c.fail(android::kENOSYS);
        }
    });
    reg.add("prctl", [](CallContext& c) { c.ret(0); });
    reg.add("ptrace", [](CallContext& c) { c.ret(0); });
    reg.add("sched_yield", [](CallContext& c) {
        std::this_thread::yield();
        c.ret(0);
    });
    reg.add("sched_get_priority_max", [](CallContext& c) { c.ret(0); });
    reg.add("sched_get_priority_min", [](CallContext& c) { c.ret(0); });

    // --- qsort, setjmp -------------------------------------------------------------------
    reg.add("qsort", guest_qsort);
    reg.add("setjmp", save_jmp);
    reg.add("sigsetjmp", save_jmp);
    reg.add("_setjmp", save_jmp);
    reg.add("longjmp", restore_jmp);
    reg.add("siglongjmp", restore_jmp);
    reg.add("_longjmp", restore_jmp);

    // --- signals: accepted and ignored; nothing delivers them --------------------------------
    reg.add("signal", [](CallContext& c) { c.ret(0); });
    reg.add("sigaction", [](CallContext& c) {
        if (c.x(2)) std::memset(c.mem().host(c.x(2)), 0, 32);
        c.ret(0);
    });
    reg.add("sigprocmask", [](CallContext& c) {
        if (c.x(2)) c.mem().write<std::uint64_t>(c.x(2), 0);
        c.ret(0);
    });
    reg.add("sigfillset", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0), ~0ull);
        c.ret(0);
    });
    reg.add("sigemptyset", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0), 0);
        c.ret(0);
    });
    reg.add("sigaddset", [](CallContext& c) {
        c.mem().write(c.x(0), c.mem().read<std::uint64_t>(c.x(0)) | (1ull << ((c.x(1) - 1) & 63)));
        c.ret(0);
    });
    reg.add("sigdelset", [](CallContext& c) {
        c.mem().write(c.x(0), c.mem().read<std::uint64_t>(c.x(0)) & ~(1ull << ((c.x(1) - 1) & 63)));
        c.ret(0);
    });
    reg.add("raise", [](CallContext& c) { throw GuestFault(fmt::format("raise({}) called from {:#x}", c.x(0), c.x(30))); });
    reg.add("kill", [](CallContext& c) {
        if (c.x(0) == kPid && c.x(1)) throw GuestFault(fmt::format("kill(self, {}) called from {:#x}", c.x(1), c.x(30)));
        c.ret(0);
    });

    // --- not available: child processes, terminals (networking is in libc_net.cpp) ---------
    for (const char* name : {"fork", "execl", "execv", "waitpid", "system"}) reg.add(name, [](CallContext& c) { c.fail(android::kENOSYS); });
    for (const char* name : {"tcgetattr", "tcsetattr"}) reg.add(name, [](CallContext& c) { c.fail(25 /* ENOTTY */); });
}

}  // namespace rfu
