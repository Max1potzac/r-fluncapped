// printf/scanf for guest code, plus the AAPCS64 variadic argument readers they consume.

#include "cpu/cpu.h"
#include "shims/abi.h"

#include <fmt/format.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rfu {

// --- argument readers ----------------------------------------------------------------------

RegisterArgs::RegisterArgs(const CallContext& c, int first_int, int first_fp)
    : c_(c), ngrn_(first_int), nsrn_(first_fp), stack_(c.sp()) {
    // Named arguments beyond the eighth register were passed on the stack too.
    if (first_int > 8) stack_ += 8ull * static_cast<unsigned>(first_int - 8);
}

std::uint64_t RegisterArgs::next_stack(std::uint64_t align) {
    stack_ = (stack_ + align - 1) & ~(align - 1);
    GuestAddr a = stack_;
    stack_ += align < 8 ? 8 : align;
    return a;
}

std::uint64_t RegisterArgs::next_int() {
    if (ngrn_ < 8) return c_.x(ngrn_++);
    return c_.mem().read<std::uint64_t>(next_stack(8));
}

double RegisterArgs::next_double() {
    if (nsrn_ < 8) return c_.d(nsrn_++);
    return c_.mem().read<double>(next_stack(8));
}

std::array<std::uint64_t, 2> RegisterArgs::next_quad() {
    if (nsrn_ < 8) {
        auto v = c_.cpu().jit().GetVector(static_cast<std::size_t>(nsrn_++));
        return {v[0], v[1]};
    }
    return c_.mem().read<std::array<std::uint64_t, 2>>(next_stack(16));
}

VaListArgs::VaListArgs(GuestMemory& mem, GuestAddr va_list)
    : mem_(mem),
      stack_(mem.read<GuestAddr>(va_list)),
      gr_top_(mem.read<GuestAddr>(va_list + 8)),
      vr_top_(mem.read<GuestAddr>(va_list + 16)),
      gr_offs_(mem.read<std::int32_t>(va_list + 24)),
      vr_offs_(mem.read<std::int32_t>(va_list + 28)) {}

GuestAddr VaListArgs::next_stack(std::uint64_t size, std::uint64_t align) {
    stack_ = (stack_ + align - 1) & ~(align - 1);
    GuestAddr a = stack_;
    stack_ += (size + 7) & ~7ull;
    return a;
}

GuestAddr VaListArgs::next_gr() {
    std::int32_t offs = gr_offs_;
    if (offs >= 0) return next_stack(8, 8);
    gr_offs_ = offs + 8;
    if (gr_offs_ > 0) return next_stack(8, 8);
    return gr_top_ + static_cast<std::int64_t>(offs);
}

GuestAddr VaListArgs::next_vr() {
    std::int32_t offs = vr_offs_;
    if (offs >= 0) return 0;
    vr_offs_ = offs + 16;
    if (vr_offs_ > 0) return 0;
    return vr_top_ + static_cast<std::int64_t>(offs);
}

std::uint64_t VaListArgs::next_int() { return mem_.read<std::uint64_t>(next_gr()); }

double VaListArgs::next_double() {
    GuestAddr a = next_vr();
    if (!a) a = next_stack(8, 8);
    return mem_.read<double>(a);
}

std::array<std::uint64_t, 2> VaListArgs::next_quad() {
    GuestAddr a = next_vr();
    if (!a) a = next_stack(16, 16);
    return mem_.read<std::array<std::uint64_t, 2>>(a);
}

// --- long double -----------------------------------------------------------------------------

double quad_to_double(std::array<std::uint64_t, 2> q) {
    std::uint64_t lo = q[0], hi = q[1];
    bool negative = hi >> 63;
    int exp = static_cast<int>((hi >> 48) & 0x7FFF);
    std::uint64_t mant = ((hi & 0xFFFF'FFFF'FFFFull) << 4) | (lo >> 60);  // top 52 bits
    double v;
    if (exp == 0x7FFF)
        v = mant ? std::nan("") : HUGE_VAL;
    else if (exp == 0)
        v = 0.0;
    else
        v = std::ldexp(static_cast<double>((1ull << 52) | mant), exp - 16383 - 52);
    return negative ? -v : v;
}

std::array<std::uint64_t, 2> double_to_quad(double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, 8);
    std::uint64_t sign = bits >> 63;
    int exp = static_cast<int>((bits >> 52) & 0x7FF);
    std::uint64_t mant = bits & ((1ull << 52) - 1);
    std::uint64_t qexp;
    if (exp == 0x7FF) {
        qexp = 0x7FFF;
    } else if (exp == 0 && mant == 0) {
        qexp = 0;
    } else {
        if (exp == 0) {  // subnormal double: normalise
            exp = 1;
            while (!(mant & (1ull << 52))) {
                mant <<= 1;
                --exp;
            }
            mant &= (1ull << 52) - 1;
        }
        qexp = static_cast<std::uint64_t>(exp - 1023 + 16383);
    }
    return {(mant & 0xF) << 60, (sign << 63) | (qexp << 48) | (mant >> 4)};
}

// --- UTF-8 -----------------------------------------------------------------------------------

std::size_t encode_utf8(char32_t c, char* out) {
    if (c < 0x80) {
        out[0] = static_cast<char>(c);
        return 1;
    }
    if (c < 0x800) {
        out[0] = static_cast<char>(0xC0 | (c >> 6));
        out[1] = static_cast<char>(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (c >> 12));
        out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | ((c >> 18) & 0x07));
    out[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (c & 0x3F));
    return 4;
}

int decode_utf8(const char* s, std::size_t n, char32_t* out) {
    if (n == 0) return -2;
    auto b = static_cast<unsigned char>(s[0]);
    if (b == 0) {
        *out = 0;
        return 0;
    }
    int len;
    char32_t c;
    if (b < 0x80) {
        *out = b;
        return 1;
    } else if ((b & 0xE0) == 0xC0) {
        len = 2, c = b & 0x1F;
    } else if ((b & 0xF0) == 0xE0) {
        len = 3, c = b & 0x0F;
    } else if ((b & 0xF8) == 0xF0) {
        len = 4, c = b & 0x07;
    } else {
        return -1;
    }
    for (int i = 1; i < len; ++i) {
        if (static_cast<std::size_t>(i) >= n) return -2;
        auto cb = static_cast<unsigned char>(s[i]);
        if ((cb & 0xC0) != 0x80) return -1;
        c = (c << 6) | (cb & 0x3F);
    }
    *out = c;
    return len;
}

std::u32string utf8_to_u32(std::string_view s) {
    std::u32string out;
    std::size_t i = 0;
    while (i < s.size()) {
        char32_t c;
        int n = decode_utf8(s.data() + i, s.size() - i, &c);
        if (n <= 0) {
            if (n == 0) {
                out.push_back(0);
                ++i;
                continue;
            }
            c = 0xFFFD;
            n = 1;
        }
        out.push_back(c);
        i += static_cast<std::size_t>(n);
    }
    return out;
}

std::string u32_to_utf8(std::u32string_view s) {
    std::string out;
    char buf[4];
    for (char32_t c : s) out.append(buf, encode_utf8(c, buf));
    return out;
}

// --- printf ----------------------------------------------------------------------------------

namespace {

enum class Length { None, hh, h, l, ll, j, z, t, L };

template <typename... T>
std::string host_format(const std::string& spec, T... v) {
    int n = std::snprintf(nullptr, 0, spec.c_str(), v...);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n) + 1, '\0');
    std::snprintf(out.data(), out.size(), spec.c_str(), v...);
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::int64_t sign_extend(std::uint64_t v, Length len) {
    switch (len) {
    case Length::hh: return static_cast<std::int8_t>(v);
    case Length::h: return static_cast<std::int16_t>(v);
    case Length::None: return static_cast<std::int32_t>(v);
    default: return static_cast<std::int64_t>(v);
    }
}

std::uint64_t zero_extend(std::uint64_t v, Length len) {
    switch (len) {
    case Length::hh: return static_cast<std::uint8_t>(v);
    case Length::h: return static_cast<std::uint16_t>(v);
    case Length::None: return static_cast<std::uint32_t>(v);
    default: return v;
    }
}

}  // namespace

std::string guest_format(GuestMemory& mem, const char* fmt, VarArgs& args) {
    std::string out;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out.push_back(*p++);
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out.push_back('%');
            ++p;
            continue;
        }

        std::string flags;
        while (*p && std::strchr("-+ #0'", *p)) {
            if (*p != '\'') flags.push_back(*p);
            ++p;
        }
        int width = -1, precision = -1;
        if (*p == '*') {
            width = static_cast<std::int32_t>(args.next_int());
            if (width < 0) {
                flags.push_back('-');
                width = -width;
            }
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') width = (width < 0 ? 0 : width * 10) + (*p++ - '0');
        }
        if (*p == '.') {
            ++p;
            precision = 0;
            if (*p == '*') {
                precision = static_cast<std::int32_t>(args.next_int());
                ++p;
            } else {
                while (*p >= '0' && *p <= '9') precision = precision * 10 + (*p++ - '0');
            }
        }
        Length len = Length::None;
        if (p[0] == 'h' && p[1] == 'h') len = Length::hh, p += 2;
        else if (p[0] == 'l' && p[1] == 'l') len = Length::ll, p += 2;
        else if (*p == 'h') len = Length::h, ++p;
        else if (*p == 'l') len = Length::l, ++p;
        else if (*p == 'q') len = Length::ll, ++p;
        else if (*p == 'L') len = Length::L, ++p;
        else if (*p == 'j') len = Length::j, ++p;
        else if (*p == 'z') len = Length::z, ++p;
        else if (*p == 't') len = Length::t, ++p;

        char conv = *p;
        if (!conv) {
            out.append(start);
            break;
        }
        ++p;

        std::string spec = "%" + flags;
        if (width >= 0) spec += std::to_string(width);
        if (precision >= 0) spec += "." + std::to_string(precision);

        switch (conv) {
        case 'd':
        case 'i':
            out += host_format(spec + "lld", static_cast<long long>(sign_extend(args.next_int(), len)));
            break;
        case 'u':
        case 'x':
        case 'X':
        case 'o':
            out += host_format(spec + "ll" + conv, static_cast<unsigned long long>(zero_extend(args.next_int(), len)));
            break;
        case 'c': {
            auto v = args.next_int();
            std::string s;
            if (len == Length::l) {
                char buf[4];
                s.assign(buf, encode_utf8(static_cast<char32_t>(v), buf));
            } else {
                s.assign(1, static_cast<char>(v));
            }
            out += host_format(spec + "s", s.c_str());
            break;
        }
        case 's': {
            GuestAddr a = args.next_int();
            std::string s;
            if (!a) {
                s = "(null)";
            } else if (len == Length::l) {
                std::u32string w = mem.read_wstr(a, precision >= 0 ? static_cast<std::size_t>(precision) : 1 << 20);
                s = u32_to_utf8(w);
            } else {
                s = mem.read_cstr(a, precision >= 0 ? static_cast<std::size_t>(precision) : 1 << 20);
            }
            out += host_format(spec + "s", s.c_str());
            break;
        }
        case 'p': {
            auto v = args.next_int();
            out += host_format("%" + flags + (width >= 0 ? std::to_string(width) : "") + "s",
                               fmt::format("{:#x}", v).c_str());
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            double v = len == Length::L ? quad_to_double(args.next_quad()) : args.next_double();
            out += host_format(spec + conv, v);
            break;
        }
        case 'n': {
            GuestAddr a = args.next_int();
            auto n = static_cast<std::int64_t>(out.size());
            if (!a) break;
            switch (len) {
            case Length::hh: mem.write<std::int8_t>(a, static_cast<std::int8_t>(n)); break;
            case Length::h: mem.write<std::int16_t>(a, static_cast<std::int16_t>(n)); break;
            case Length::None: mem.write<std::int32_t>(a, static_cast<std::int32_t>(n)); break;
            default: mem.write<std::int64_t>(a, n); break;
            }
            break;
        }
        default:
            out.append(start, p);  // unknown conversion: print it verbatim
            break;
        }
    }
    return out;
}

// --- scanf -----------------------------------------------------------------------------------

namespace {

bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

// Parses a scanset like "[^]a-z]" starting after '['. Returns the position after ']'.
const char* parse_scanset(const char* p, bool set[256]) {
    bool negate = false;
    if (*p == '^') negate = true, ++p;
    std::memset(set, 0, 256);
    if (*p == ']') set[static_cast<unsigned char>(']')] = true, ++p;
    while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            for (int c = static_cast<unsigned char>(p[0]); c <= static_cast<unsigned char>(p[2]); ++c) set[c] = true;
            p += 3;
        } else {
            set[static_cast<unsigned char>(*p++)] = true;
        }
    }
    if (*p == ']') ++p;
    if (negate)
        for (int i = 0; i < 256; ++i) set[i] = !set[i];
    return p;
}

}  // namespace

int guest_scan(GuestMemory& mem, const char* input, const char* fmt, VarArgs& args, std::size_t* consumed) {
    const char* in = input;
    auto finish = [&](int result) {
        if (consumed) *consumed = static_cast<std::size_t>(in - input);
        return result;
    };
    int assigned = 0;
    bool any_conversion = false;
    for (const char* p = fmt; *p;) {
        if (is_space(*p)) {
            while (is_space(*p)) ++p;
            while (is_space(*in)) ++in;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') ++p;
            if (*in != *p) break;
            ++in, ++p;
            continue;
        }
        ++p;
        bool suppress = false;
        if (*p == '*') suppress = true, ++p;
        std::size_t width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + static_cast<std::size_t>(*p++ - '0');
        Length len = Length::None;
        if (p[0] == 'h' && p[1] == 'h') len = Length::hh, p += 2;
        else if (p[0] == 'l' && p[1] == 'l') len = Length::ll, p += 2;
        else if (*p == 'h') len = Length::h, ++p;
        else if (*p == 'l') len = Length::l, ++p;
        else if (*p == 'L' || *p == 'q') len = Length::L, ++p;
        else if (*p == 'j') len = Length::j, ++p;
        else if (*p == 'z') len = Length::z, ++p;
        else if (*p == 't') len = Length::t, ++p;
        char conv = *p++;
        if (!conv) break;

        if (conv == 'n') {
            if (!suppress) {
                GuestAddr a = args.next_int();
                auto n = static_cast<std::int64_t>(in - input);
                if (len == Length::None) mem.write<std::int32_t>(a, static_cast<std::int32_t>(n));
                else if (len == Length::h) mem.write<std::int16_t>(a, static_cast<std::int16_t>(n));
                else if (len == Length::hh) mem.write<std::int8_t>(a, static_cast<std::int8_t>(n));
                else mem.write<std::int64_t>(a, n);
            }
            continue;
        }
        if (conv != 'c' && conv != '[')
            while (is_space(*in)) ++in;
        if (!*in) {
            if (!any_conversion) return finish(-1);
            break;
        }
        any_conversion = true;

        // Copy at most `width` characters of the input so host strto* can't overrun it.
        std::size_t limit = width ? width : std::strlen(in);
        std::string field(in, std::min(limit, std::strlen(in)));

        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'i' ? 0 : conv == 'o' ? 8 : 16;
            char* end;
            std::uint64_t v = conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o'
                                  ? std::strtoull(field.c_str(), &end, base)
                                  : static_cast<std::uint64_t>(std::strtoll(field.c_str(), &end, base));
            if (end == field.c_str()) return finish(assigned);
            in += end - field.c_str();
            if (!suppress) {
                GuestAddr a = args.next_int();
                switch (len) {
                case Length::hh: mem.write<std::uint8_t>(a, static_cast<std::uint8_t>(v)); break;
                case Length::h: mem.write<std::uint16_t>(a, static_cast<std::uint16_t>(v)); break;
                case Length::None: mem.write<std::uint32_t>(a, static_cast<std::uint32_t>(v)); break;
                default: mem.write<std::uint64_t>(a, v); break;
                }
                ++assigned;
            }
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            char* end;
            double v = std::strtod(field.c_str(), &end);
            if (end == field.c_str()) return finish(assigned);
            in += end - field.c_str();
            if (!suppress) {
                GuestAddr a = args.next_int();
                if (len == Length::L) mem.write(a, double_to_quad(v));
                else if (len == Length::l) mem.write(a, v);
                else mem.write(a, static_cast<float>(v));
                ++assigned;
            }
            break;
        }
        case 's':
        case 'c':
        case '[': {
            bool set[256];
            if (conv == '[') p = parse_scanset(p, set);
            std::size_t n = 0;
            if (conv == 'c') {
                n = std::min<std::size_t>(width ? width : 1, field.size());
            } else {
                while (n < field.size() && (conv == 's' ? !is_space(field[n]) : set[static_cast<unsigned char>(field[n])])) ++n;
            }
            if (n == 0) return finish(assigned);
            if (!suppress) {
                GuestAddr a = args.next_int();
                if (len == Length::l) {
                    auto w = utf8_to_u32(std::string_view(in, n));
                    mem.write_bytes(a, w.data(), w.size() * 4);
                    if (conv != 'c') mem.write<std::uint32_t>(a + w.size() * 4, 0);
                } else {
                    mem.write_bytes(a, in, n);
                    if (conv != 'c') mem.write<std::uint8_t>(a + n, 0);
                }
                ++assigned;
            }
            in += n;
            break;
        }
        case 'p': {
            char* end;
            std::uint64_t v = std::strtoull(field.c_str(), &end, 16);
            if (end == field.c_str()) return finish(assigned);
            in += end - field.c_str();
            if (!suppress) {
                mem.write<std::uint64_t>(args.next_int(), v);
                ++assigned;
            }
            break;
        }
        default:
            return finish(assigned);
        }
    }
    return finish(assigned);
}

}  // namespace rfu
