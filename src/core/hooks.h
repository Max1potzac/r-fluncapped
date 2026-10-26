#pragma once

// Hooks: host code that runs when the guest reaches an address, the way rfu's own fixes
// reach into the game (hover to collect sun, keybinds...). The instruction at the address
// becomes an `svc` to the hook; the callback sees the guest's registers through a
// CallContext, then the guest carries on with the instruction it replaced, unless the
// callback returned from the guest function itself (CallContext::ret plus return_now).

#include "core/guest_memory.h"

#include <functional>
#include <string>

namespace rfu {

class CallContext;
class Runtime;

using HookFn = std::function<void(CallContext&)>;

// Hooks `addr`, normally a function's first instruction (x16 and x17 are free there, which
// the jump back uses). Throws when the instruction there can't be moved (cbz, ldr literal...).
void hook(Runtime& rt, GuestAddr addr, std::string name, HookFn fn);

// From inside a hook: return to the guest function's caller with `value` in x0, skipping it.
void return_now(CallContext& c, std::uint64_t value);

// The instruction at `pc` rewritten to run from elsewhere, followed by a jump back to pc + 4,
// or an empty vector when it can't be. Exposed for the tests.
std::vector<std::uint32_t> relocate_instruction(std::uint32_t insn, GuestAddr pc);

}  // namespace rfu
