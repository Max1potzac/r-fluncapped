// <string.h>, <ctype.h>, <wchar.h>, <wctype.h>, <stdlib.h> number parsing, <locale.h>.
//
// Narrow strings are used in place: a guest pointer is a host pointer into guest memory, so
// host libc does the work. Wide strings are 32-bit on Android and 16-bit on Windows, so the
// wide functions are written out here.

#include "shims/common.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <unordered_map>

namespace rfu {

namespace {

using wchar = std::uint32_t;  // guest wchar_t

wchar* wstr(const CallContext& c, int i) { return c.ptr<wchar>(i); }

std::size_t wlen(const wchar* s) {
    std::size_t n = 0;
    while (s[n]) ++n;
    return n;
}

int cmp(std::int64_t a, std::int64_t b) { return a < b ? -1 : a > b ? 1 : 0; }

bool is_wspace(wchar c) { return c == ' ' || (c >= '\t' && c <= '\r') || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 || c == 0x3000; }

wint_t host_wc(wchar c) { return static_cast<wint_t>(c <= 0xFFFF ? c : 0); }

// strtol-style: parses with `parse`, stores the end pointer through the endptr argument.
template <typename F>
void parse_number(CallContext& c, int endptr_arg, F parse) {
    const char* s = c.str(0);
    char* end = nullptr;
    errno = 0;
    auto v = parse(s, &end);
    if (errno == ERANGE) c.rt().set_errno(android::kERANGE);
    if (GuestAddr ep = c.x(endptr_arg)) c.mem().write<GuestAddr>(ep, c.x(0) + static_cast<std::uint64_t>(end - s));
    if constexpr (std::is_floating_point_v<decltype(v)>) {
        if constexpr (std::is_same_v<decltype(v), float>) c.ret_f(v);
        else c.ret_d(v);
    } else {
        c.ret(static_cast<std::uint64_t>(v));
    }
}

// The wide versions: copy the leading ASCII part out, parse that, map the end back.
template <typename F>
void parse_wide_number(CallContext& c, F parse) {
    const wchar* w = wstr(c, 0);
    std::string narrow;
    for (std::size_t i = 0; w[i] && w[i] < 0x80 && narrow.size() < 512; ++i) narrow.push_back(static_cast<char>(w[i]));
    char* end = nullptr;
    auto v = parse(narrow.c_str(), &end);
    if (GuestAddr ep = c.x(1)) c.mem().write<GuestAddr>(ep, c.x(0) + 4ull * static_cast<std::uint64_t>(end - narrow.c_str()));
    if constexpr (std::is_floating_point_v<decltype(v)>) {
        if constexpr (std::is_same_v<decltype(v), float>) c.ret_f(v);
        else c.ret_d(v);
    } else {
        c.ret(static_cast<std::uint64_t>(v));
    }
}

void ret_long_double(CallContext& c, double v) {
    auto q = double_to_quad(v);
    c.ret_q(q[0], q[1]);
}

}  // namespace

const char* android_strerror(int err) {
    switch (err) {
    case 0: return "Success";
    case android::kEPERM: return "Operation not permitted";
    case android::kENOENT: return "No such file or directory";
    case android::kEINTR: return "Interrupted system call";
    case android::kEIO: return "I/O error";
    case android::kEBADF: return "Bad file descriptor";
    case android::kEAGAIN: return "Try again";
    case android::kENOMEM: return "Out of memory";
    case android::kEACCES: return "Permission denied";
    case android::kEBUSY: return "Device or resource busy";
    case android::kEEXIST: return "File exists";
    case android::kENOTDIR: return "Not a directory";
    case android::kEISDIR: return "Is a directory";
    case android::kEINVAL: return "Invalid argument";
    case android::kEMFILE: return "Too many open files";
    case android::kENOSPC: return "No space left on device";
    case android::kESPIPE: return "Illegal seek";
    case android::kERANGE: return "Math result not representable";
    case android::kEDEADLK: return "Resource deadlock would occur";
    case android::kENOSYS: return "Function not implemented";
    case android::kENOTEMPTY: return "Directory not empty";
    case android::kETIMEDOUT: return "Connection timed out";
    default: return "Unknown error";
    }
}

GuestAddr intern_string(Runtime& rt, const std::string& s) {
    struct Interned {
        std::mutex mutex;
        std::unordered_map<std::string, GuestAddr> strings;
    };
    auto& table = rt.state<Interned>();
    std::lock_guard lock(table.mutex);
    auto [it, inserted] = table.strings.emplace(s, 0);
    if (inserted) it->second = rt.heap().strdup(s);
    return it->second;
}

void register_libc_string(ShimRegistry& reg) {
    // --- memory -----------------------------------------------------------------------
    reg.add("memcpy", [](CallContext& c) {
        if (c.x(2)) std::memcpy(c.ptr(0), c.ptr(1), c.x(2));
        c.ret(c.x(0));
    });
    reg.add("memmove", [](CallContext& c) {
        if (c.x(2)) std::memmove(c.ptr(0), c.ptr(1), c.x(2));
        c.ret(c.x(0));
    });
    reg.add("__memmove_chk", [](CallContext& c) {
        if (c.x(2)) std::memmove(c.ptr(0), c.ptr(1), c.x(2));
        c.ret(c.x(0));
    });
    reg.add("memset", [](CallContext& c) {
        if (c.x(2)) std::memset(c.ptr(0), static_cast<int>(c.x(1)), c.x(2));
        c.ret(c.x(0));
    });
    reg.add("memcmp", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(c.x(2) ? std::memcmp(c.ptr(0), c.ptr(1), c.x(2)) : 0)); });
    reg.add("memchr", [](CallContext& c) {
        c.ret(c.x(2) ? to_guest(c, std::memchr(c.ptr(0), static_cast<int>(c.x(1) & 0xFF), c.x(2))) : 0);
    });
    reg.add("memrchr", [](CallContext& c) {
        auto* p = c.ptr(0);
        auto ch = static_cast<std::uint8_t>(c.x(1));
        for (std::uint64_t i = c.x(2); i-- > 0;)
            if (p[i] == ch) return c.ret(c.x(0) + i);
        c.ret(0);
    });

    // --- strings ----------------------------------------------------------------------
    reg.add("strlen", [](CallContext& c) { c.ret(std::strlen(c.str(0))); });
    reg.add("__strlen_chk", [](CallContext& c) { c.ret(std::strlen(c.str(0))); });
    reg.add("strcmp", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::strcmp(c.str(0), c.str(1)))); });
    reg.add("strncmp", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::strncmp(c.str(0), c.str(1), c.x(2)))); });
    reg.add("strcasecmp", [](CallContext& c) {
        const char *a = c.str(0), *b = c.str(1);
        int r = 0;
        for (;; ++a, ++b) {
            r = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
            if (r || !*a) break;
        }
        c.ret(static_cast<std::uint64_t>(r));
    });
    reg.add("strncasecmp", [](CallContext& c) {
        const char *a = c.str(0), *b = c.str(1);
        int r = 0;
        for (std::uint64_t n = c.x(2); n; --n, ++a, ++b) {
            r = std::tolower(static_cast<unsigned char>(*a)) - std::tolower(static_cast<unsigned char>(*b));
            if (r || !*a) break;
        }
        c.ret(static_cast<std::uint64_t>(r));
    });
    reg.add("strcoll_l", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::strcmp(c.str(0), c.str(1)))); });
    reg.add("strxfrm_l", [](CallContext& c) {
        std::size_t len = std::strlen(c.str(1));
        if (c.x(2) > len) std::memcpy(c.str(0), c.str(1), len + 1);
        c.ret(len);
    });
    reg.add("strcpy", [](CallContext& c) {
        std::strcpy(c.str(0), c.str(1));
        c.ret(c.x(0));
    });
    reg.add("strncpy", [](CallContext& c) {
        std::strncpy(c.str(0), c.str(1), c.x(2));
        c.ret(c.x(0));
    });
    reg.add("strlcpy", [](CallContext& c) {
        std::size_t len = std::strlen(c.str(1));
        if (std::uint64_t size = c.x(2)) {
            std::size_t n = std::min<std::size_t>(len, size - 1);
            std::memcpy(c.str(0), c.str(1), n);
            c.str(0)[n] = 0;
        }
        c.ret(len);
    });
    reg.add("strcat", [](CallContext& c) {
        std::strcat(c.str(0), c.str(1));
        c.ret(c.x(0));
    });
    reg.add("strncat", [](CallContext& c) {
        std::strncat(c.str(0), c.str(1), c.x(2));
        c.ret(c.x(0));
    });
    reg.add("strdup", [](CallContext& c) { c.ret(c.rt().heap().strdup(c.str(0))); });
    reg.add("strchr", [](CallContext& c) { c.ret(to_guest(c, std::strchr(c.str(0), static_cast<int>(c.x(1) & 0xFF)))); });
    reg.add("strrchr", [](CallContext& c) { c.ret(to_guest(c, std::strrchr(c.str(0), static_cast<int>(c.x(1) & 0xFF)))); });
    reg.add("strstr", [](CallContext& c) { c.ret(to_guest(c, std::strstr(c.str(0), c.str(1)))); });
    reg.add("strpbrk", [](CallContext& c) { c.ret(to_guest(c, std::strpbrk(c.str(0), c.str(1)))); });
    reg.add("strspn", [](CallContext& c) { c.ret(std::strspn(c.str(0), c.str(1))); });
    reg.add("strcspn", [](CallContext& c) { c.ret(std::strcspn(c.str(0), c.str(1))); });
    reg.add("strtok_r", [](CallContext& c) {
        GuestAddr s = c.x(0) ? c.x(0) : c.mem().read<GuestAddr>(c.x(2));
        if (!s) return c.ret(0);
        char* p = c.mem().host_as<char>(s);
        const char* delim = c.str(1);
        p += std::strspn(p, delim);
        if (!*p) {
            c.mem().write<GuestAddr>(c.x(2), 0);
            return c.ret(0);
        }
        char* end = p + std::strcspn(p, delim);
        if (*end) *end++ = 0;
        c.mem().write<GuestAddr>(c.x(2), *end || end[-1] == 0 ? c.mem().guest(end) : 0);
        c.ret(c.mem().guest(p));
    });
    reg.add("strtok", [](CallContext& c) {
        static GuestAddr saved = 0;  // strtok isn't thread safe on Android either
        GuestAddr s = c.x(0) ? c.x(0) : saved;
        if (!s) return c.ret(0);
        char* p = c.mem().host_as<char>(s);
        const char* delim = c.str(1);
        p += std::strspn(p, delim);
        if (!*p) {
            saved = 0;
            return c.ret(0);
        }
        char* end = p + std::strcspn(p, delim);
        saved = *end ? c.mem().guest(end + 1) : 0;
        if (*end) *end = 0;
        c.ret(c.mem().guest(p));
    });
    reg.add("strsep", [](CallContext& c) {
        GuestAddr s = c.mem().read<GuestAddr>(c.x(0));
        if (!s) return c.ret(0);
        char* p = c.mem().host_as<char>(s);
        char* end = p + std::strcspn(p, c.str(1));
        if (*end) {
            *end = 0;
            c.mem().write<GuestAddr>(c.x(0), c.mem().guest(end + 1));
        } else {
            c.mem().write<GuestAddr>(c.x(0), 0);
        }
        c.ret(s);
    });
    reg.add("strerror", [](CallContext& c) { c.ret(intern_string(c.rt(), android_strerror(static_cast<int>(c.x(0))))); });
    reg.add("strerror_r", [](CallContext& c) {
        // bionic exports the POSIX version: int strerror_r(int, char*, size_t).
        const char* msg = android_strerror(static_cast<int>(c.x(0)));
        if (std::uint64_t n = c.x(2)) {
            std::size_t len = std::min<std::size_t>(std::strlen(msg), n - 1);
            std::memcpy(c.str(1), msg, len);
            c.str(1)[len] = 0;
        }
        c.ret(0);
    });
    reg.add("basename", [](CallContext& c) {
        std::string s = c.x(0) ? c.str(0) : "";
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        auto slash = s.rfind('/');
        std::string out = s.empty() ? "." : slash == std::string::npos || s == "/" ? s : s.substr(slash + 1);
        c.ret(intern_string(c.rt(), out));
    });
    reg.add("dirname", [](CallContext& c) {
        std::string s = c.x(0) ? c.str(0) : "";
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        auto slash = s.rfind('/');
        std::string out = slash == std::string::npos ? "." : slash == 0 ? "/" : s.substr(0, slash);
        c.ret(intern_string(c.rt(), out));
    });

    // --- <ctype.h>: the "C" locale, whatever locale_t is passed --------------------------
#define RFU_CTYPE(name, expr)                                                       \
    reg.add(#name, [](CallContext& c) {                                             \
        int ch = static_cast<int>(c.x(0));                                          \
        c.ret(static_cast<std::uint64_t>((ch >= 0 && ch < 256) ? (expr) : 0)); \
    });                                                                             \
    reg.add(#name "_l", [](CallContext& c) {                                        \
        int ch = static_cast<int>(c.x(0));                                          \
        c.ret(static_cast<std::uint64_t>((ch >= 0 && ch < 256) ? (expr) : 0)); \
    });
    RFU_CTYPE(isalnum, std::isalnum(ch) != 0)
    RFU_CTYPE(isalpha, std::isalpha(ch) != 0)
    RFU_CTYPE(iscntrl, std::iscntrl(ch) != 0)
    RFU_CTYPE(isdigit, std::isdigit(ch) != 0)
    RFU_CTYPE(isgraph, std::isgraph(ch) != 0)
    RFU_CTYPE(islower, std::islower(ch) != 0)
    RFU_CTYPE(isprint, std::isprint(ch) != 0)
    RFU_CTYPE(ispunct, std::ispunct(ch) != 0)
    RFU_CTYPE(isspace, std::isspace(ch) != 0)
    RFU_CTYPE(isupper, std::isupper(ch) != 0)
    RFU_CTYPE(isxdigit, std::isxdigit(ch) != 0)
#undef RFU_CTYPE
    reg.add("tolower", [](CallContext& c) { int ch = static_cast<int>(c.x(0)); c.ret(static_cast<std::uint64_t>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)); });
    reg.add("toupper", [](CallContext& c) { int ch = static_cast<int>(c.x(0)); c.ret(static_cast<std::uint64_t>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)); });
    reg.add("tolower_l", [](CallContext& c) { int ch = static_cast<int>(c.x(0)); c.ret(static_cast<std::uint64_t>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch)); });
    reg.add("toupper_l", [](CallContext& c) { int ch = static_cast<int>(c.x(0)); c.ret(static_cast<std::uint64_t>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch)); });

    // --- <wctype.h> ----------------------------------------------------------------------
#define RFU_WCTYPE(name, expr)                                                   \
    reg.add(#name, [](CallContext& c) {                                          \
        auto wc = static_cast<wchar>(c.x(0));                                    \
        c.ret(static_cast<std::uint64_t>((expr) ? 1 : 0));                       \
    });                                                                          \
    reg.add(#name "_l", [](CallContext& c) {                                     \
        auto wc = static_cast<wchar>(c.x(0));                                    \
        c.ret(static_cast<std::uint64_t>((expr) ? 1 : 0));                       \
    });
    RFU_WCTYPE(iswalnum, std::iswalnum(host_wc(wc)))
    RFU_WCTYPE(iswalpha, std::iswalpha(host_wc(wc)))
    RFU_WCTYPE(iswblank, wc == ' ' || wc == '\t')
    RFU_WCTYPE(iswcntrl, std::iswcntrl(host_wc(wc)))
    RFU_WCTYPE(iswdigit, wc >= '0' && wc <= '9')
    RFU_WCTYPE(iswlower, std::iswlower(host_wc(wc)))
    RFU_WCTYPE(iswprint, std::iswprint(host_wc(wc)))
    RFU_WCTYPE(iswpunct, std::iswpunct(host_wc(wc)))
    RFU_WCTYPE(iswspace, is_wspace(wc))
    RFU_WCTYPE(iswupper, std::iswupper(host_wc(wc)))
    RFU_WCTYPE(iswxdigit, (wc >= '0' && wc <= '9') || (wc >= 'a' && wc <= 'f') || (wc >= 'A' && wc <= 'F'))
#undef RFU_WCTYPE
    auto towlower = [](CallContext& c) {
        auto wc = static_cast<wchar>(c.x(0));
        c.ret(wc <= 0xFFFF ? static_cast<wchar>(std::towlower(host_wc(wc))) : wc);
    };
    auto towupper = [](CallContext& c) {
        auto wc = static_cast<wchar>(c.x(0));
        c.ret(wc <= 0xFFFF ? static_cast<wchar>(std::towupper(host_wc(wc))) : wc);
    };
    reg.add("towlower", towlower);
    reg.add("towlower_l", towlower);
    reg.add("towupper", towupper);
    reg.add("towupper_l", towupper);

    // --- number parsing --------------------------------------------------------------------
    reg.add("atoi", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(static_cast<std::int32_t>(std::strtoll(c.str(0), nullptr, 10)))); });
    reg.add("atol", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::strtoll(c.str(0), nullptr, 10))); });
    reg.add("atof", [](CallContext& c) { c.ret_d(std::strtod(c.str(0), nullptr)); });
    reg.add("strtol", [](CallContext& c) { parse_number(c, 1, [&](const char* s, char** e) { return std::strtoll(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("strtoll", [](CallContext& c) { parse_number(c, 1, [&](const char* s, char** e) { return std::strtoll(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("strtoll_l", [](CallContext& c) { parse_number(c, 1, [&](const char* s, char** e) { return std::strtoll(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("strtoul", [](CallContext& c) { parse_number(c, 1, [&](const char* s, char** e) { return std::strtoull(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("strtoull", [](CallContext& c) { parse_number(c, 1, [&](const char* s, char** e) { return std::strtoull(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("strtoull_l", [](CallContext& c) { parse_number(c, 1, [&](const char* s, char** e) { return std::strtoull(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("strtod", [](CallContext& c) { parse_number(c, 1, [](const char* s, char** e) { return std::strtod(s, e); }); });
    reg.add("strtof", [](CallContext& c) { parse_number(c, 1, [](const char* s, char** e) { return std::strtof(s, e); }); });
    auto strtold = [](CallContext& c) {
        const char* s = c.str(0);
        char* end;
        double v = std::strtod(s, &end);
        if (GuestAddr ep = c.x(1)) c.mem().write<GuestAddr>(ep, c.x(0) + static_cast<std::uint64_t>(end - s));
        ret_long_double(c, v);
    };
    reg.add("strtold", strtold);
    reg.add("strtold_l", strtold);
    reg.add("wcstol", [](CallContext& c) { parse_wide_number(c, [&](const char* s, char** e) { return std::strtoll(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("wcstoll", [](CallContext& c) { parse_wide_number(c, [&](const char* s, char** e) { return std::strtoll(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("wcstoul", [](CallContext& c) { parse_wide_number(c, [&](const char* s, char** e) { return std::strtoull(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("wcstoull", [](CallContext& c) { parse_wide_number(c, [&](const char* s, char** e) { return std::strtoull(s, e, static_cast<int>(c.x(2))); }); });
    reg.add("wcstod", [](CallContext& c) { parse_wide_number(c, [](const char* s, char** e) { return std::strtod(s, e); }); });
    reg.add("wcstof", [](CallContext& c) { parse_wide_number(c, [](const char* s, char** e) { return std::strtof(s, e); }); });
    reg.add("wcstold", [](CallContext& c) {
        double v = 0;
        parse_wide_number(c, [&](const char* s, char** e) { return v = std::strtod(s, e); });
        ret_long_double(c, v);
    });

    // --- wide strings ----------------------------------------------------------------------
    reg.add("wcslen", [](CallContext& c) { c.ret(wlen(wstr(c, 0))); });
    reg.add("wcscmp", [](CallContext& c) {
        const wchar *a = wstr(c, 0), *b = wstr(c, 1);
        while (*a && *a == *b) ++a, ++b;
        c.ret(static_cast<std::uint64_t>(cmp(*a, *b)));
    });
    reg.add("wcsncmp", [](CallContext& c) {
        const wchar *a = wstr(c, 0), *b = wstr(c, 1);
        std::uint64_t n = c.x(2);
        int r = 0;
        for (; n && !(r = cmp(*a, *b)) && *a; --n) ++a, ++b;
        c.ret(static_cast<std::uint64_t>(n ? r : 0));
    });
    reg.add("wcscoll_l", [](CallContext& c) {
        const wchar *a = wstr(c, 0), *b = wstr(c, 1);
        while (*a && *a == *b) ++a, ++b;
        c.ret(static_cast<std::uint64_t>(cmp(*a, *b)));
    });
    reg.add("wcsxfrm_l", [](CallContext& c) {
        std::size_t len = wlen(wstr(c, 1));
        if (c.x(2) > len) std::memcpy(wstr(c, 0), wstr(c, 1), 4 * (len + 1));
        c.ret(len);
    });
    reg.add("wcscpy", [](CallContext& c) {
        std::memcpy(wstr(c, 0), wstr(c, 1), 4 * (wlen(wstr(c, 1)) + 1));
        c.ret(c.x(0));
    });
    reg.add("wcsncpy", [](CallContext& c) {
        wchar *d = wstr(c, 0), *s = wstr(c, 1);
        std::uint64_t n = c.x(2), i = 0;
        for (; i < n && s[i]; ++i) d[i] = s[i];
        for (; i < n; ++i) d[i] = 0;
        c.ret(c.x(0));
    });
    reg.add("wcschr", [](CallContext& c) {
        const wchar* s = wstr(c, 0);
        auto ch = static_cast<wchar>(c.x(1));
        for (;; ++s) {
            if (*s == ch) return c.ret(to_guest(c, s));
            if (!*s) return c.ret(0);
        }
    });
    reg.add("wcsspn", [](CallContext& c) {
        const wchar *s = wstr(c, 0), *accept = wstr(c, 1);
        std::size_t n = 0, alen = wlen(accept);
        while (s[n] && std::find(accept, accept + alen, s[n]) != accept + alen) ++n;
        c.ret(n);
    });
    reg.add("wmemchr", [](CallContext& c) {
        const wchar* s = wstr(c, 0);
        for (std::uint64_t i = 0; i < c.x(2); ++i)
            if (s[i] == static_cast<wchar>(c.x(1))) return c.ret(c.x(0) + 4 * i);
        c.ret(0);
    });
    reg.add("wmemcmp", [](CallContext& c) {
        const wchar *a = wstr(c, 0), *b = wstr(c, 1);
        for (std::uint64_t i = 0; i < c.x(2); ++i)
            if (a[i] != b[i]) return c.ret(static_cast<std::uint64_t>(cmp(a[i], b[i])));
        c.ret(0);
    });
    reg.add("wmemcpy", [](CallContext& c) {
        if (c.x(2)) std::memcpy(c.ptr(0), c.ptr(1), 4 * c.x(2));
        c.ret(c.x(0));
    });
    reg.add("wmemmove", [](CallContext& c) {
        if (c.x(2)) std::memmove(c.ptr(0), c.ptr(1), 4 * c.x(2));
        c.ret(c.x(0));
    });
    reg.add("wmemset", [](CallContext& c) {
        wchar* d = wstr(c, 0);
        for (std::uint64_t i = 0; i < c.x(2); ++i) d[i] = static_cast<wchar>(c.x(1));
        c.ret(c.x(0));
    });

    // --- multibyte <-> wide: UTF-8, as on Android ----------------------------------------
    constexpr std::uint64_t kInvalid = ~0ull, kIncomplete = ~0ull - 1;
    constexpr int kEILSEQ = 84;
    reg.add("__ctype_get_mb_cur_max", [](CallContext& c) { c.ret(4); });
    reg.add("btowc", [](CallContext& c) {
        auto ch = static_cast<std::int32_t>(c.x(0));
        c.ret(ch >= 0 && ch < 0x80 ? static_cast<std::uint64_t>(ch) : 0xFFFFFFFFu);
    });
    reg.add("wctob", [](CallContext& c) {
        auto wc = static_cast<wchar>(c.x(0));
        c.ret(wc < 0x80 ? wc : static_cast<std::uint64_t>(-1));
    });
    reg.add("mbrtowc", [](CallContext& c) {
        if (!c.x(1)) return c.ret(0);
        char32_t wc;
        int n = decode_utf8(c.str(1), c.x(2), &wc);
        if (n == -1) return c.fail(kEILSEQ, kInvalid);
        if (n == -2) return c.ret(kIncomplete);
        if (c.x(0)) c.mem().write<wchar>(c.x(0), wc);
        c.ret(static_cast<std::uint64_t>(n));
    });
    reg.add("mbtowc", [](CallContext& c) {
        if (!c.x(1)) return c.ret(0);
        char32_t wc;
        int n = decode_utf8(c.str(1), c.x(2), &wc);
        if (n < 0) return c.fail(kEILSEQ, kInvalid);
        if (c.x(0)) c.mem().write<wchar>(c.x(0), wc);
        c.ret(static_cast<std::uint64_t>(n));
    });
    reg.add("mbrlen", [](CallContext& c) {
        char32_t wc;
        int n = decode_utf8(c.str(0), c.x(1), &wc);
        if (n == -1) return c.fail(kEILSEQ, kInvalid);
        c.ret(n == -2 ? kIncomplete : static_cast<std::uint64_t>(n));
    });
    reg.add("wcrtomb", [](CallContext& c) {
        if (!c.x(0)) return c.ret(1);
        c.ret(encode_utf8(static_cast<char32_t>(c.x(1)), c.str(0)));
    });
    // mbsnrtowcs(dst, const char** src, nms, len, ps); mbsrtowcs(dst, src, len, ps)
    reg.add("mbsnrtowcs", [](CallContext& c) {
        std::uint64_t nms = c.x(2), len = c.x(3);
        GuestAddr src = c.mem().read<GuestAddr>(c.x(1));
        wchar* dst = c.x(0) ? wstr(c, 0) : nullptr;
        const char* s = c.mem().host_as<char>(src);
        std::uint64_t used = 0, count = 0;
        while (!dst || count < len) {
            char32_t wc;
            int n = decode_utf8(s + used, nms - used, &wc);
            if (n == -1) {
                if (dst) c.mem().write<GuestAddr>(c.x(1), src + used);
                return c.fail(kEILSEQ, kInvalid);
            }
            if (n == -2) break;
            if (n == 0) {
                if (dst) {
                    dst[count] = 0;
                    c.mem().write<GuestAddr>(c.x(1), 0);
                }
                return c.ret(count);
            }
            if (dst) dst[count] = wc;
            used += static_cast<std::uint64_t>(n);
            ++count;
        }
        if (dst) c.mem().write<GuestAddr>(c.x(1), src + used);
        c.ret(count);
    });
    reg.add("mbsrtowcs", [](CallContext& c) {
        std::uint64_t len = c.x(2);
        GuestAddr src = c.mem().read<GuestAddr>(c.x(1));
        wchar* dst = c.x(0) ? wstr(c, 0) : nullptr;
        const char* s = c.mem().host_as<char>(src);
        std::uint64_t used = 0, count = 0;
        while (!dst || count < len) {
            char32_t wc;
            int n = decode_utf8(s + used, 4, &wc);
            if (n < 0) {
                if (dst) c.mem().write<GuestAddr>(c.x(1), src + used);
                return c.fail(kEILSEQ, kInvalid);
            }
            if (n == 0) {
                if (dst) {
                    dst[count] = 0;
                    c.mem().write<GuestAddr>(c.x(1), 0);
                }
                return c.ret(count);
            }
            if (dst) dst[count] = wc;
            used += static_cast<std::uint64_t>(n);
            ++count;
        }
        if (dst) c.mem().write<GuestAddr>(c.x(1), src + used);
        c.ret(count);
    });
    // wcsnrtombs(dst, const wchar_t** src, nwc, len, ps)
    reg.add("wcsnrtombs", [](CallContext& c) {
        GuestAddr src = c.mem().read<GuestAddr>(c.x(1));
        const wchar* s = c.mem().host_as<wchar>(src);
        char* dst = c.x(0) ? c.str(0) : nullptr;
        std::uint64_t nwc = c.x(2), len = c.x(3), out = 0, i = 0;
        for (; i < nwc; ++i) {
            if (!s[i]) {
                if (dst && out < len) dst[out] = 0;
                if (dst) c.mem().write<GuestAddr>(c.x(1), 0);
                return c.ret(out);
            }
            char buf[4];
            std::size_t n = encode_utf8(s[i], buf);
            if (dst) {
                if (out + n > len) break;
                std::memcpy(dst + out, buf, n);
            }
            out += n;
        }
        if (dst) c.mem().write<GuestAddr>(c.x(1), src + 4 * i);
        c.ret(out);
    });
    reg.add("wcstombs", [](CallContext& c) {
        std::string s = u32_to_utf8(std::u32string_view(reinterpret_cast<const char32_t*>(wstr(c, 1)), wlen(wstr(c, 1))));
        if (!c.x(0)) return c.ret(s.size());
        std::size_t n = std::min<std::size_t>(s.size(), c.x(2));
        std::memcpy(c.str(0), s.data(), n);
        if (n < c.x(2)) c.str(0)[n] = 0;
        c.ret(n);
    });

    // --- <locale.h>: only the "C"/UTF-8 locale exists --------------------------------------
    reg.add("setlocale", [](CallContext& c) { c.ret(intern_string(c.rt(), "C")); });
    reg.add("newlocale", [](CallContext& c) { c.ret(c.rt().heap().alloc_zeroed(16)); });
    reg.add("freelocale", [](CallContext& c) {
        if (c.x(0) && c.x(0) != ~0ull) c.rt().heap().free(c.x(0));
    });
    reg.add("uselocale", [](CallContext& c) { c.ret(~0ull); });  // LC_GLOBAL_LOCALE
    reg.add("localeconv", [](CallContext& c) {
        struct Lconv {
            GuestAddr addr = 0;
        };
        GuestAddr& lconv = c.rt().state<Lconv>().addr;
        if (!lconv) {
            // Ten char* fields, then fourteen char fields (CHAR_MAX = unspecified).
            GuestAddr l = c.rt().heap().alloc_zeroed(10 * 8 + 14);
            GuestAddr empty = intern_string(c.rt(), "");
            for (int i = 0; i < 10; ++i) c.mem().write<GuestAddr>(l + 8ull * i, empty);
            c.mem().write<GuestAddr>(l, intern_string(c.rt(), "."));
            std::memset(c.mem().host(l + 80), 127, 14);
            lconv = l;
        }
        c.ret(lconv);
    });
}

}  // namespace rfu
