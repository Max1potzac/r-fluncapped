// <math.h>. Doubles and floats arrive in v registers; the host's libm does the work.

#include "shims/common.h"

#include <cmath>

namespace rfu {

void register_libc_math(ShimRegistry& reg) {
#define RFU_MATH1(name) \
    reg.add(#name, [](CallContext& c) { c.ret_d(std::name(c.d(0))); }); \
    reg.add(#name "f", [](CallContext& c) { c.ret_f(std::name(c.s(0))); });
#define RFU_MATH2(name) \
    reg.add(#name, [](CallContext& c) { c.ret_d(std::name(c.d(0), c.d(1))); }); \
    reg.add(#name "f", [](CallContext& c) { c.ret_f(std::name(c.s(0), c.s(1))); });
    RFU_MATH1(sin)
    RFU_MATH1(cos)
    RFU_MATH1(tan)
    RFU_MATH1(asin)
    RFU_MATH1(acos)
    RFU_MATH1(atan)
    RFU_MATH1(sinh)
    RFU_MATH1(cosh)
    RFU_MATH1(tanh)
    RFU_MATH1(exp)
    RFU_MATH1(exp2)
    RFU_MATH1(log)
    RFU_MATH1(log2)
    RFU_MATH1(log10)
    RFU_MATH1(sqrt)
    RFU_MATH1(cbrt)
    RFU_MATH1(floor)
    RFU_MATH1(ceil)
    RFU_MATH1(round)
    RFU_MATH1(trunc)
    RFU_MATH1(fabs)
    RFU_MATH2(atan2)
    RFU_MATH2(pow)
    RFU_MATH2(fmod)
    RFU_MATH2(hypot)
    RFU_MATH2(fmin)
    RFU_MATH2(fmax)
#undef RFU_MATH1
#undef RFU_MATH2

    reg.add("lround", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::llround(c.d(0)))); });
    reg.add("lroundf", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::llround(c.s(0)))); });
    reg.add("lrint", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::llrint(c.d(0)))); });
    reg.add("lrintf", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(std::llrint(c.s(0)))); });
    reg.add("modf", [](CallContext& c) {
        double ip;
        double frac = std::modf(c.d(0), &ip);
        c.mem().write(c.x(0), ip);
        c.ret_d(frac);
    });
    reg.add("modff", [](CallContext& c) {
        float ip;
        float frac = std::modf(c.s(0), &ip);
        c.mem().write(c.x(0), ip);
        c.ret_f(frac);
    });
    reg.add("sincosf", [](CallContext& c) {
        float x = c.s(0);
        c.mem().write(c.x(0), std::sin(x));
        c.mem().write(c.x(1), std::cos(x));
    });
    reg.add("sincos", [](CallContext& c) {
        double x = c.d(0);
        c.mem().write(c.x(0), std::sin(x));
        c.mem().write(c.x(1), std::cos(x));
    });
    reg.add("ldexp", [](CallContext& c) { c.ret_d(std::ldexp(c.d(0), static_cast<int>(c.x(0)))); });
    reg.add("ldexpf", [](CallContext& c) { c.ret_f(std::ldexp(c.s(0), static_cast<int>(c.x(0)))); });
    reg.add("frexp", [](CallContext& c) {
        int e;
        double m = std::frexp(c.d(0), &e);
        c.mem().write<std::int32_t>(c.x(0), e);
        c.ret_d(m);
    });
}

}  // namespace rfu
