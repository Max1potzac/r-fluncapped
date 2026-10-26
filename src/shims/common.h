#pragma once

// Helpers shared by the shim files.

#include "runtime.h"
#include "shims/abi.h"

#include <memory>
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

// Sockets (libc_net.cpp) are fds in the same table as files, so read/write/close/fcntl
// work on them too. The socket functions return -errno on failure.
struct GuestSocket;
int add_socket_fd(Runtime& rt, std::shared_ptr<GuestSocket> s);
std::shared_ptr<GuestSocket> socket_at(Runtime& rt, int fd);
bool fd_is_open(Runtime& rt, int fd);
std::int64_t socket_read(GuestSocket& s, void* buf, std::uint64_t n);
std::int64_t socket_write(GuestSocket& s, const void* buf, std::uint64_t n);
int socket_fcntl(GuestSocket& s, int cmd, std::uint64_t arg);
int socket_ioctl(CallContext& c, GuestSocket& s, std::uint64_t request, GuestAddr arg);

}  // namespace rfu
