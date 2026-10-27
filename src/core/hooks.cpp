#include "core/hooks.h"

#include "runtime.h"

#include <fmt/format.h>

#include <stdexcept>

namespace rfu {

namespace {

constexpr std::uint32_t kLdrX16Literal8 = 0x58000050;  // ldr x16, #8
constexpr std::uint32_t kBrX16 = 0xD61F0200;           // br x16

// ldr x<rd>, #8; b #12; .quad value
void load_constant(std::vector<std::uint32_t>& out, std::uint32_t rd, std::uint64_t value) {
    out.push_back(0x58000040 | rd);
    out.push_back(0x14000003);
    out.push_back(static_cast<std::uint32_t>(value));
    out.push_back(static_cast<std::uint32_t>(value >> 32));
}

void jump_to(std::vector<std::uint32_t>& out, std::uint64_t target) {
    out.push_back(kLdrX16Literal8);
    out.push_back(kBrX16);
    out.push_back(static_cast<std::uint32_t>(target));
    out.push_back(static_cast<std::uint32_t>(target >> 32));
}

std::int64_t sign_extend(std::uint64_t v, int bits) { return static_cast<std::int64_t>(v << (64 - bits)) >> (64 - bits); }

}  // namespace

std::vector<std::uint32_t> relocate_instruction(std::uint32_t insn, GuestAddr pc) {
    std::vector<std::uint32_t> out;
    if ((insn & 0x9F000000) == 0x90000000) {  // adrp
        std::uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
        load_constant(out, insn & 0x1F, (pc & ~0xFFFull) + (sign_extend(imm, 21) << 12));
    } else if ((insn & 0x9F000000) == 0x10000000) {  // adr
        std::uint64_t imm = ((insn >> 29) & 3) | (((insn >> 5) & 0x7FFFF) << 2);
        load_constant(out, insn & 0x1F, pc + sign_extend(imm, 21));
    } else if ((insn & 0x7C000000) == 0x14000000) {  // b, bl
        GuestAddr target = pc + (sign_extend(insn & 0x3FFFFFF, 26) << 2);
        if (insn & 0x80000000) load_constant(out, 30, pc + 4);  // bl: the call returns to pc + 4
        jump_to(out, target);
        return out;
    } else if ((insn & 0x7E000000) == 0x34000000 || (insn & 0x7E000000) == 0x36000000 ||
               (insn & 0xFF000010) == 0x54000000 || (insn & 0x3B000000) == 0x18000000) {
        return {};  // cbz/cbnz, tbz/tbnz, b.cond, ldr literal: not needed at function starts yet
    } else {
        out.push_back(insn);
    }
    jump_to(out, pc + 4);
    return out;
}

void hook(Runtime& rt, GuestAddr addr, std::string name, HookFn fn) {
    std::uint32_t insn = rt.mem().read<std::uint32_t>(addr);
    auto moved = relocate_instruction(insn, addr);
    if (moved.empty()) throw std::runtime_error(fmt::format("can't hook {}: instruction {:08x} can't be moved", rt.describe(addr), insn));
    GuestAddr trampoline = rt.heap().alloc_aligned(16, moved.size() * 4);
    for (std::size_t i = 0; i < moved.size(); ++i) rt.mem().write<std::uint32_t>(trampoline + 4 * i, moved[i]);
    GuestAddr after = addr + 4;
    GuestAddr thunk = rt.make_hook_thunk(std::move(name), [fn = std::move(fn), trampoline, after](CallContext& c) {
        fn(c);
        // Unless the hook returned for the guest function, carry on with what it replaced.
        if (c.cpu().jit().GetPC() == after) c.cpu().jit().SetPC(trampoline);
    });
    // The svc at addr names the thunk's slot; the thunk itself is never jumped to.
    std::uint32_t svc = rt.mem().read<std::uint32_t>(thunk);
    rt.mem().write<std::uint32_t>(addr, svc);
    rt.invalidate_code(addr, 4);
}

void return_now(CallContext& c, std::uint64_t value) {
    c.ret(value);
    c.cpu().jit().SetPC(c.x(30));
}

}  // namespace rfu
