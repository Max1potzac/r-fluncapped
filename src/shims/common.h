#pragma once

// Helpers shared by the shim files.

#include "runtime.h"
#include "shims/abi.h"

#include <string>
#include <string_view>

namespace rfu {

// A NUL-terminated copy of `s` in guest memory that lives as long as the runtime; equal
// strings share one copy. For functions that return static strings (strerror, setlocale).
GuestAddr intern_string(Runtime& rt, const std::string& s);

// Guest address of a host pointer into guest memory; null stays 0.
inline GuestAddr to_guest(const CallContext& c, const void* host) { return host ? c.mem().guest(host) : 0; }

// Text of a bionic errno value.
const char* android_strerror(int err);

// File descriptors (libc_stdio.cpp).
std::int64_t guest_pread(Runtime& rt, int fd, void* buf, std::uint64_t n, std::int64_t offset);

}  // namespace rfu
