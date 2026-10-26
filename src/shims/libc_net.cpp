// Sockets and name resolution: <sys/socket.h>, <netdb.h>, <arpa/inet.h>, poll() and select().
//
// A guest socket is a host socket wrapped in a GuestSocket and kept in the fd table, so the
// game's own libcurl (statically linked into libPVZ2) talks to its CDN directly. Constants
// and structs are bionic's; Windows numbers differ for most of them (AF_INET6, the poll bits,
// SOL_SOCKET, errno), so every value crossing the boundary is translated. With
// RuntimeOptions::network off, socket() and getaddrinfo() fail the way they would without a
// connection and the game plays offline.

#ifdef _WIN32
#define FD_SETSIZE 1024  // Winsock's default of 64 sockets per select() is too few
#endif

#include "core/log.h"
#include "shims/common.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rfu {

#ifdef _WIN32
using HostSocket = SOCKET;
constexpr HostSocket kNoSocket = INVALID_SOCKET;
#else
using HostSocket = int;
constexpr HostSocket kNoSocket = -1;
#endif

struct GuestSocket {
    HostSocket s = kNoSocket;
    bool nonblocking = false;

    ~GuestSocket() {
#ifdef _WIN32
        if (s != kNoSocket) closesocket(s);
#else
        if (s != kNoSocket) ::close(s);
#endif
    }
};

namespace {

namespace guest {
// bionic values
constexpr int kAF_UNSPEC = 0, kAF_INET = 2, kAF_INET6 = 10;
constexpr int kSOCK_STREAM = 1, kSOCK_DGRAM = 2, kSOCK_NONBLOCK = 04000, kSOCK_CLOEXEC = 02000000;
constexpr int kSOL_SOCKET = 1, kIPPROTO_TCP = 6, kIPPROTO_IPV6 = 41;
constexpr int kSO_REUSEADDR = 2, kSO_TYPE = 3, kSO_ERROR = 4, kSO_BROADCAST = 6, kSO_SNDBUF = 7, kSO_RCVBUF = 8,
              kSO_KEEPALIVE = 9, kSO_LINGER = 13, kSO_RCVTIMEO = 20, kSO_SNDTIMEO = 21;
constexpr int kTCP_NODELAY = 1, kIPV6_V6ONLY = 26;
constexpr int kMSG_OOB = 1, kMSG_PEEK = 2, kMSG_DONTWAIT = 0x40, kMSG_WAITALL = 0x100;
constexpr int kF_GETFL = 3, kF_SETFL = 4, kO_RDWR = 2, kO_NONBLOCK = 04000;
constexpr std::uint64_t kFIONREAD = 0x541B, kFIONBIO = 0x5421;
constexpr short kPOLLIN = 1, kPOLLPRI = 2, kPOLLOUT = 4, kPOLLERR = 8, kPOLLHUP = 0x10, kPOLLNVAL = 0x20, kPOLLRDNORM = 0x40,
                kPOLLWRNORM = 0x100;
constexpr int kAI_PASSIVE = 1, kAI_CANONNAME = 2, kAI_NUMERICHOST = 4, kAI_NUMERICSERV = 8, kAI_ADDRCONFIG = 0x400;
constexpr int kEAI_AGAIN = 2, kEAI_BADFLAGS = 3, kEAI_FAIL = 4, kEAI_FAMILY = 5, kEAI_MEMORY = 6, kEAI_NONAME = 8,
              kEAI_SERVICE = 9, kEAI_SOCKTYPE = 10, kEAI_SYSTEM = 11;
constexpr int kENOSPC = 28, kENOTSOCK = 88, kEINPROGRESS = 115;
#ifdef _WIN32
// only needed to translate Winsock errors; Linux hosts already use these numbers
constexpr int kEPIPE = 32, kEADDRINUSE = 98, kEADDRNOTAVAIL = 99, kENETUNREACH = 101, kECONNABORTED = 103,
              kECONNRESET = 104, kENOBUFS = 105, kEISCONN = 106, kENOTCONN = 107, kECONNREFUSED = 111,
              kEHOSTUNREACH = 113, kEALREADY = 114;
#endif
// struct addrinfo: flags, family, socktype, protocol, addrlen, canonname, addr, next
constexpr int kSizeofAddrinfo = 48, ai_canonname = 24, ai_addr = 32, ai_next = 40;
}  // namespace guest

// --- errors --------------------------------------------------------------------------------

int last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Host socket error -> bionic errno.
int to_guest_errno(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEWOULDBLOCK: return android::kEAGAIN;
    case WSAEINPROGRESS: return guest::kEINPROGRESS;
    case WSAEALREADY: return guest::kEALREADY;
    case WSAEINTR: return android::kEINTR;
    case WSAEBADF: return android::kEBADF;
    case WSAEACCES: return android::kEACCES;
    case WSAEINVAL: return android::kEINVAL;
    case WSAEMFILE: return android::kEMFILE;
    case WSAENOTSOCK: return guest::kENOTSOCK;
    case WSAEAFNOSUPPORT: return android::kEAFNOSUPPORT;
    case WSAEADDRINUSE: return guest::kEADDRINUSE;
    case WSAEADDRNOTAVAIL: return guest::kEADDRNOTAVAIL;
    case WSAENETDOWN:
    case WSAENETUNREACH: return guest::kENETUNREACH;
    case WSAECONNABORTED: return guest::kECONNABORTED;
    case WSAECONNRESET: return guest::kECONNRESET;
    case WSAENOBUFS: return guest::kENOBUFS;
    case WSAEISCONN: return guest::kEISCONN;
    case WSAENOTCONN: return guest::kENOTCONN;
    case WSAESHUTDOWN: return guest::kEPIPE;
    case WSAETIMEDOUT: return android::kETIMEDOUT;
    case WSAECONNREFUSED: return guest::kECONNREFUSED;
    case WSAEHOSTDOWN:
    case WSAEHOSTUNREACH: return guest::kEHOSTUNREACH;
    default: return android::kEIO;
    }
#else
    return e;  // Linux and bionic share the kernel's numbers
#endif
}

int fail_code() { return -to_guest_errno(last_error()); }

int to_guest_eai(int e) {
    switch (e) {
    case 0: return 0;
    case EAI_AGAIN: return guest::kEAI_AGAIN;
    case EAI_BADFLAGS: return guest::kEAI_BADFLAGS;
    case EAI_FAIL: return guest::kEAI_FAIL;
    case EAI_FAMILY: return guest::kEAI_FAMILY;
    case EAI_MEMORY: return guest::kEAI_MEMORY;
    case EAI_NONAME: return guest::kEAI_NONAME;
    case EAI_SERVICE: return guest::kEAI_SERVICE;
    case EAI_SOCKTYPE: return guest::kEAI_SOCKTYPE;
#ifndef _WIN32
    case EAI_SYSTEM: return guest::kEAI_SYSTEM;
#endif
    default: return guest::kEAI_FAIL;
    }
}

void ensure_started() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) log::error("WSAStartup failed: {}", WSAGetLastError());
    });
#endif
}

// --- addresses -----------------------------------------------------------------------------

int to_host_family(int f) {
    switch (f) {
    case guest::kAF_UNSPEC: return AF_UNSPEC;
    case guest::kAF_INET: return AF_INET;
    case guest::kAF_INET6: return AF_INET6;
    default: return -1;
    }
}

int to_guest_family(int f) { return f == AF_INET ? guest::kAF_INET : f == AF_INET6 ? guest::kAF_INET6 : guest::kAF_UNSPEC; }

// sockaddr_in and sockaddr_in6 have the same layout on every platform apart from the family
// value, so a copy with the family rewritten is enough. Returns the host length or -errno.
int to_host_addr(const std::uint8_t* src, std::uint32_t len, sockaddr_storage& out) {
    if (!src || len < 2) return -android::kEINVAL;
    std::uint16_t family;
    std::memcpy(&family, src, 2);
    std::memset(&out, 0, sizeof out);
    std::uint32_t need = family == guest::kAF_INET ? sizeof(sockaddr_in) : family == guest::kAF_INET6 ? sizeof(sockaddr_in6) : 0;
    if (!need) return -android::kEAFNOSUPPORT;
    if (len < need) return -android::kEINVAL;
    std::memcpy(&out, src, need);
    out.ss_family = static_cast<decltype(out.ss_family)>(to_host_family(family));
    return static_cast<int>(need);
}

// Writes a host address to a guest sockaddr of *len bytes, truncating like the kernel, and
// sets *len to the full size.
void to_guest_addr(const sockaddr_storage& a, std::uint32_t host_len, std::uint8_t* dst, std::uint32_t* len) {
    if (!dst || !len) return;
    std::uint8_t tmp[sizeof(sockaddr_storage)];
    std::memcpy(tmp, &a, host_len);
    auto family = static_cast<std::uint16_t>(to_guest_family(a.ss_family));
    std::memcpy(tmp, &family, 2);
    std::memcpy(dst, tmp, std::min(*len, host_len));
    *len = host_len;
}

// --- per-socket helpers --------------------------------------------------------------------

bool set_host_nonblocking(HostSocket s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK) == 0;
#endif
}

// What a host socket is ready for, from a zero-or-more-millisecond wait.
struct Ready {
    bool read = false, write = false, error = false, hup = false;
};

struct Want {
    HostSocket s;
    bool read, write;
};

// Waits until one of `wants` is ready or `timeout_ms` passes (-1 = forever). Returns the
// number ready, or -errno.
int host_wait(const std::vector<Want>& wants, int timeout_ms, std::vector<Ready>& out) {
    out.assign(wants.size(), {});
    if (wants.empty()) {
        if (timeout_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return 0;
    }
#ifdef _WIN32
    // select() rather than WSAPoll, which doesn't report a failed connect.
    fd_set r, w, e;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);
    for (const auto& x : wants) {
        if (x.read) FD_SET(x.s, &r);
        if (x.write) FD_SET(x.s, &w);
        FD_SET(x.s, &e);
    }
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int n = select(0, &r, &w, &e, timeout_ms < 0 ? nullptr : &tv);
    if (n == SOCKET_ERROR) return fail_code();
    int ready = 0;
    for (std::size_t i = 0; i < wants.size(); ++i) {
        out[i].read = FD_ISSET(wants[i].s, &r) != 0;
        out[i].write = FD_ISSET(wants[i].s, &w) != 0;
        out[i].error = FD_ISSET(wants[i].s, &e) != 0;
        if (out[i].read || out[i].write || out[i].error) ++ready;
    }
    return ready;
#else
    std::vector<pollfd> p(wants.size());
    for (std::size_t i = 0; i < wants.size(); ++i)
        p[i] = {wants[i].s, static_cast<short>((wants[i].read ? POLLIN : 0) | (wants[i].write ? POLLOUT : 0)), 0};
    int n = ::poll(p.data(), p.size(), timeout_ms);
    if (n < 0) return fail_code();
    for (std::size_t i = 0; i < wants.size(); ++i) {
        out[i].read = p[i].revents & POLLIN;
        out[i].write = p[i].revents & POLLOUT;
        out[i].error = p[i].revents & (POLLERR | POLLNVAL);
        out[i].hup = p[i].revents & POLLHUP;
    }
    return n;
#endif
}

int to_host_msg_flags(int f) {
    int h = 0;
    if (f & guest::kMSG_OOB) h |= MSG_OOB;
    if (f & guest::kMSG_PEEK) h |= MSG_PEEK;
    if (f & guest::kMSG_WAITALL) h |= MSG_WAITALL;
#ifndef _WIN32
    if (f & guest::kMSG_DONTWAIT) h |= MSG_DONTWAIT;
    h |= MSG_NOSIGNAL;  // a closed peer must not kill rfu with SIGPIPE
#endif
    return h;
}

#ifdef _WIN32
// Windows has no MSG_DONTWAIT: check readiness first instead.
bool would_block(const GuestSocket& s, int flags, bool for_write) {
    if (!(flags & guest::kMSG_DONTWAIT) || s.nonblocking) return false;
    std::vector<Ready> r;
    return host_wait({{s.s, !for_write, for_write}}, 0, r) == 0;
}
#else
bool would_block(const GuestSocket&, int, bool) { return false; }
#endif

std::int64_t do_recv(GuestSocket& s, void* buf, std::uint64_t n, int flags, sockaddr_storage* from = nullptr,
                     socklen_t* from_len = nullptr) {
    if (would_block(s, flags, false)) return -android::kEAGAIN;
    auto len = static_cast<int>(std::min<std::uint64_t>(n, 1u << 30));
    auto r = from ? ::recvfrom(s.s, static_cast<char*>(buf), len, to_host_msg_flags(flags), reinterpret_cast<sockaddr*>(from), from_len)
                  : ::recv(s.s, static_cast<char*>(buf), len, to_host_msg_flags(flags));
    return r < 0 ? fail_code() : static_cast<std::int64_t>(r);
}

std::int64_t do_send(GuestSocket& s, const void* buf, std::uint64_t n, int flags, const sockaddr_storage* to = nullptr,
                     int to_len = 0) {
    if (would_block(s, flags, true)) return -android::kEAGAIN;
    auto len = static_cast<int>(std::min<std::uint64_t>(n, 1u << 30));
    auto r = to ? ::sendto(s.s, static_cast<const char*>(buf), len, to_host_msg_flags(flags), reinterpret_cast<const sockaddr*>(to), to_len)
                : ::send(s.s, static_cast<const char*>(buf), len, to_host_msg_flags(flags));
    return r < 0 ? fail_code() : static_cast<std::int64_t>(r);
}

// --- shim helpers --------------------------------------------------------------------------

bool online(const CallContext& c) { return c.rt().options().network; }

void ret_or_fail(CallContext& c, std::int64_t r) {
    if (r < 0) return c.fail(static_cast<int>(-r));
    c.ret(static_cast<std::uint64_t>(r));
}

// The socket behind fd argument `i`, or nullptr after setting errno.
std::shared_ptr<GuestSocket> sock_arg(CallContext& c, int i) {
    int fd = static_cast<int>(c.x(i));
    auto s = socket_at(c.rt(), fd);
    if (!s) c.fail(fd_is_open(c.rt(), fd) ? guest::kENOTSOCK : android::kEBADF);
    return s;
}

int new_socket_fd(CallContext& c, HostSocket h, bool nonblocking) {
    auto s = std::make_shared<GuestSocket>();
    s->s = h;
    s->nonblocking = nonblocking;
    return add_socket_fd(c.rt(), std::move(s));
}

// An option value as the host wants it, for the options rfu translates. Returns false when
// the option has no host equivalent and should be accepted and ignored.
struct HostOpt {
    int level, name;
};
bool to_host_opt(int level, int name, HostOpt& out) {
    if (level == guest::kSOL_SOCKET) {
        out.level = SOL_SOCKET;
        switch (name) {
        case guest::kSO_REUSEADDR: out.name = SO_REUSEADDR; return true;
        case guest::kSO_TYPE: out.name = SO_TYPE; return true;
        case guest::kSO_ERROR: out.name = SO_ERROR; return true;
        case guest::kSO_BROADCAST: out.name = SO_BROADCAST; return true;
        case guest::kSO_SNDBUF: out.name = SO_SNDBUF; return true;
        case guest::kSO_RCVBUF: out.name = SO_RCVBUF; return true;
        case guest::kSO_KEEPALIVE: out.name = SO_KEEPALIVE; return true;
        case guest::kSO_LINGER: out.name = SO_LINGER; return true;
        case guest::kSO_RCVTIMEO: out.name = SO_RCVTIMEO; return true;
        case guest::kSO_SNDTIMEO: out.name = SO_SNDTIMEO; return true;
        default: return false;
        }
    }
    if (level == guest::kIPPROTO_TCP && name == guest::kTCP_NODELAY) {
        out = {IPPROTO_TCP, TCP_NODELAY};
        return true;
    }
    if (level == guest::kIPPROTO_IPV6 && name == guest::kIPV6_V6ONLY) {
        out = {IPPROTO_IPV6, IPV6_V6ONLY};
        return true;
    }
    return false;  // TCP keepalive timing and the like: harmless to drop
}

// --- name resolution -----------------------------------------------------------------------

int to_host_ai_flags(int f) {
    int h = 0;
    if (f & guest::kAI_PASSIVE) h |= AI_PASSIVE;
    if (f & guest::kAI_CANONNAME) h |= AI_CANONNAME;
    if (f & guest::kAI_NUMERICHOST) h |= AI_NUMERICHOST;
    if (f & guest::kAI_NUMERICSERV) h |= AI_NUMERICSERV;
    if (f & guest::kAI_ADDRCONFIG) h |= AI_ADDRCONFIG;
    return h;
}

GuestAddr guest_strdup(Runtime& rt, const char* s) {
    std::size_t n = std::strlen(s) + 1;
    GuestAddr a = rt.heap().alloc(n);
    std::memcpy(rt.mem().host(a), s, n);
    return a;
}

// Builds the guest addrinfo list for a host one. Every node, address and name is its own
// heap block, which freeaddrinfo releases.
GuestAddr to_guest_addrinfo(Runtime& rt, const addrinfo* list) {
    GuestAddr head = 0, prev = 0;
    for (const addrinfo* ai = list; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        GuestAddr node = rt.heap().alloc_zeroed(guest::kSizeofAddrinfo);
        auto& mem = rt.mem();
        mem.write<std::int32_t>(node + 0, 0);
        mem.write<std::int32_t>(node + 4, to_guest_family(ai->ai_family));
        mem.write<std::int32_t>(node + 8, ai->ai_socktype == SOCK_DGRAM ? guest::kSOCK_DGRAM : ai->ai_socktype == SOCK_STREAM ? guest::kSOCK_STREAM : 0);
        mem.write<std::int32_t>(node + 12, ai->ai_protocol);
        sockaddr_storage ss{};
        std::memcpy(&ss, ai->ai_addr, ai->ai_addrlen);
        std::uint32_t len = static_cast<std::uint32_t>(ai->ai_addrlen);
        GuestAddr addr = rt.heap().alloc_zeroed(len);
        to_guest_addr(ss, len, mem.host(addr), &len);
        mem.write<std::uint32_t>(node + 16, len);
        mem.write<std::uint64_t>(node + guest::ai_addr, addr);
        if (ai->ai_canonname) mem.write<std::uint64_t>(node + guest::ai_canonname, guest_strdup(rt, ai->ai_canonname));
        if (prev) mem.write<std::uint64_t>(prev + guest::ai_next, node);
        else head = node;
        prev = node;
    }
    return head;
}

// gethostbyname's result lives here until the next call, like bionic's per-thread buffer.
struct HostEnt {
    std::mutex mutex;
    GuestAddr block = 0;
};

const char* gai_text(int e) {
    switch (e) {
    case guest::kEAI_AGAIN: return "Temporary failure in name resolution";
    case guest::kEAI_BADFLAGS: return "Invalid flags";
    case guest::kEAI_FAIL: return "Non-recoverable failure in name resolution";
    case guest::kEAI_FAMILY: return "Address family not supported";
    case guest::kEAI_MEMORY: return "Memory allocation failure";
    case guest::kEAI_NONAME: return "No address associated with hostname";
    case guest::kEAI_SERVICE: return "Service not supported for socket type";
    case guest::kEAI_SOCKTYPE: return "Socket type not supported";
    case guest::kEAI_SYSTEM: return "System error";
    default: return "Unknown error";
    }
}

// --- poll / select -------------------------------------------------------------------------

// One fd being waited on: a socket goes to the host, anything else is always ready.
struct Waited {
    int fd;
    bool read, write;
    std::shared_ptr<GuestSocket> sock;
    bool valid;
    Ready ready;
};

// Fills in `ready` for each entry. Returns how many entries are ready, or -errno.
int wait_fds(Runtime& rt, std::vector<Waited>& fds, int timeout_ms) {
    int files_ready = 0;
    std::vector<Want> wants;
    std::vector<std::size_t> where;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        auto& f = fds[i];
        f.sock = socket_at(rt, f.fd);
        f.valid = f.sock || fd_is_open(rt, f.fd);
        if (!f.valid) continue;
        if (f.sock) {
            wants.push_back({f.sock->s, f.read, f.write});
            where.push_back(i);
        } else {
            f.ready.read = f.read;
            f.ready.write = f.write;
            if (f.read || f.write) ++files_ready;
        }
    }
    std::vector<Ready> got;
    int n = host_wait(wants, files_ready ? 0 : timeout_ms, got);
    if (n < 0) return n;
    for (std::size_t k = 0; k < where.size(); ++k) fds[where[k]].ready = got[k];
    return n + files_ready;
}

}  // namespace

// --- read/write/fcntl/ioctl on socket fds (called from libc_stdio.cpp) ----------------------

std::int64_t socket_read(GuestSocket& s, void* buf, std::uint64_t n) { return do_recv(s, buf, n, 0); }
std::int64_t socket_write(GuestSocket& s, const void* buf, std::uint64_t n) { return do_send(s, buf, n, 0); }

int socket_fcntl(GuestSocket& s, int cmd, std::uint64_t arg) {
    if (cmd == guest::kF_GETFL) return guest::kO_RDWR | (s.nonblocking ? guest::kO_NONBLOCK : 0);
    if (cmd == guest::kF_SETFL) {
        bool on = (arg & guest::kO_NONBLOCK) != 0;
        if (on != s.nonblocking && !set_host_nonblocking(s.s, on)) return fail_code();
        s.nonblocking = on;
    }
    return 0;  // F_GETFD/F_SETFD (close-on-exec) and the rest don't matter here
}

int socket_ioctl(CallContext& c, GuestSocket& s, std::uint64_t request, GuestAddr arg) {
    if (request == guest::kFIONBIO) {
        bool on = c.mem().read<std::int32_t>(arg) != 0;
        if (!set_host_nonblocking(s.s, on)) return fail_code();
        s.nonblocking = on;
        return 0;
    }
    if (request == guest::kFIONREAD) {
#ifdef _WIN32
        u_long n = 0;
        if (ioctlsocket(s.s, FIONREAD, &n) != 0) return fail_code();
#else
        int n = 0;
        if (::ioctl(s.s, FIONREAD, &n) != 0) return fail_code();
#endif
        c.mem().write<std::int32_t>(arg, static_cast<std::int32_t>(n));
        return 0;
    }
    return -android::kEINVAL;
}

void register_libc_net(ShimRegistry& reg) {
    reg.add("socket", [](CallContext& c) {
        if (!online(c)) return c.fail(android::kEAFNOSUPPORT);
        int family = to_host_family(static_cast<int>(c.x(0)));
        int type = static_cast<int>(c.x(1));
        int base = type & ~(guest::kSOCK_NONBLOCK | guest::kSOCK_CLOEXEC);
        if (family <= 0) return c.fail(android::kEAFNOSUPPORT);
        if (base != guest::kSOCK_STREAM && base != guest::kSOCK_DGRAM) return c.fail(android::kEINVAL);
        ensure_started();
        HostSocket h = ::socket(family, base == guest::kSOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, static_cast<int>(c.x(2)));
        if (h == kNoSocket) return c.fail(-fail_code());
        bool nonblocking = (type & guest::kSOCK_NONBLOCK) != 0;
        if (nonblocking) set_host_nonblocking(h, true);
        int fd = new_socket_fd(c, h, nonblocking);
        log::debug("socket({}, {}) = {}", c.x(0), type, fd);
        c.ret(static_cast<std::uint64_t>(fd));
    });
    reg.add("socketpair", [](CallContext& c) { c.fail(android::kEAFNOSUPPORT); });

    reg.add("connect", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        sockaddr_storage a;
        int len = to_host_addr(c.ptr(1), static_cast<std::uint32_t>(c.x(2)), a);
        if (len < 0) return c.fail(-len);
        if (::connect(s->s, reinterpret_cast<sockaddr*>(&a), len) == 0) return c.ret(0);
        int err = to_guest_errno(last_error());
        // Windows reports a non-blocking connect in progress as "would block".
        if (err == android::kEAGAIN && s->nonblocking) err = guest::kEINPROGRESS;
        c.fail(err);
    });
    reg.add("bind", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        sockaddr_storage a;
        int len = to_host_addr(c.ptr(1), static_cast<std::uint32_t>(c.x(2)), a);
        if (len < 0) return c.fail(-len);
        ::bind(s->s, reinterpret_cast<sockaddr*>(&a), len) == 0 ? c.ret(0) : c.fail(-fail_code());
    });
    reg.add("listen", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        ::listen(s->s, static_cast<int>(c.x(1))) == 0 ? c.ret(0) : c.fail(-fail_code());
    });
    reg.add("accept", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        sockaddr_storage a{};
        socklen_t len = sizeof a;
        HostSocket h = ::accept(s->s, reinterpret_cast<sockaddr*>(&a), &len);
        if (h == kNoSocket) return c.fail(-fail_code());
        to_guest_addr(a, static_cast<std::uint32_t>(len), c.ptr(1), c.ptr<std::uint32_t>(2));
        c.ret(static_cast<std::uint64_t>(new_socket_fd(c, h, false)));
    });
    reg.add("shutdown", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        ::shutdown(s->s, static_cast<int>(c.x(1))) == 0 ? c.ret(0) : c.fail(-fail_code());
    });

    reg.add("send", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (s) ret_or_fail(c, do_send(*s, c.ptr(1), c.x(2), static_cast<int>(c.x(3))));
    });
    reg.add("sendto", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        if (!c.x(4)) return ret_or_fail(c, do_send(*s, c.ptr(1), c.x(2), static_cast<int>(c.x(3))));
        sockaddr_storage a;
        int len = to_host_addr(c.ptr(4), static_cast<std::uint32_t>(c.x(5)), a);
        if (len < 0) return c.fail(-len);
        ret_or_fail(c, do_send(*s, c.ptr(1), c.x(2), static_cast<int>(c.x(3)), &a, len));
    });
    reg.add("recv", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (s) ret_or_fail(c, do_recv(*s, c.ptr(1), c.x(2), static_cast<int>(c.x(3))));
    });
    reg.add("recvfrom", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        sockaddr_storage a{};
        socklen_t len = sizeof a;
        std::int64_t r = do_recv(*s, c.ptr(1), c.x(2), static_cast<int>(c.x(3)), c.x(4) ? &a : nullptr, &len);
        if (r >= 0 && c.x(4)) to_guest_addr(a, static_cast<std::uint32_t>(len), c.ptr(4), c.ptr<std::uint32_t>(5));
        ret_or_fail(c, r);
    });

    static constexpr auto name_query = [](CallContext& c, bool peer) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        sockaddr_storage a{};
        socklen_t len = sizeof a;
        int r = peer ? ::getpeername(s->s, reinterpret_cast<sockaddr*>(&a), &len) : ::getsockname(s->s, reinterpret_cast<sockaddr*>(&a), &len);
        if (r != 0) return c.fail(-fail_code());
        to_guest_addr(a, static_cast<std::uint32_t>(len), c.ptr(1), c.ptr<std::uint32_t>(2));
        c.ret(0);
    };
    reg.add("getsockname", [](CallContext& c) { name_query(c, false); });
    reg.add("getpeername", [](CallContext& c) { name_query(c, true); });

    reg.add("setsockopt", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        int level = static_cast<int>(c.x(1)), name = static_cast<int>(c.x(2));
        HostOpt o;
        if (!to_host_opt(level, name, o)) return c.ret(0);
        const std::uint8_t* val = c.ptr(3);
        auto len = static_cast<std::uint32_t>(c.x(4));
        if (!val) return c.fail(android::kEINVAL);
        int r;
        if (o.level == SOL_SOCKET && (o.name == SO_RCVTIMEO || o.name == SO_SNDTIMEO)) {
            // struct timeval { long tv_sec; long tv_usec; }
            std::int64_t tv[2] = {0, 0};
            std::memcpy(tv, val, std::min<std::uint32_t>(len, sizeof tv));
#ifdef _WIN32
            DWORD ms = static_cast<DWORD>(tv[0] * 1000 + tv[1] / 1000);
            r = ::setsockopt(s->s, o.level, o.name, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
            timeval htv{static_cast<time_t>(tv[0]), static_cast<suseconds_t>(tv[1])};
            r = ::setsockopt(s->s, o.level, o.name, &htv, sizeof htv);
#endif
        } else if (o.level == SOL_SOCKET && o.name == SO_LINGER) {
            linger l{};
            std::int32_t gl[2] = {0, 0};
            std::memcpy(gl, val, std::min<std::uint32_t>(len, sizeof gl));
            l.l_onoff = static_cast<decltype(l.l_onoff)>(gl[0]);
            l.l_linger = static_cast<decltype(l.l_linger)>(gl[1]);
            r = ::setsockopt(s->s, o.level, o.name, reinterpret_cast<const char*>(&l), sizeof l);
        } else {
            std::int32_t v = 0;
            std::memcpy(&v, val, std::min<std::uint32_t>(len, 4));
            r = ::setsockopt(s->s, o.level, o.name, reinterpret_cast<const char*>(&v), sizeof v);
        }
        r == 0 ? c.ret(0) : c.fail(-fail_code());
    });
    reg.add("getsockopt", [](CallContext& c) {
        auto s = sock_arg(c, 0);
        if (!s) return;
        int level = static_cast<int>(c.x(1)), name = static_cast<int>(c.x(2));
        auto* val = c.ptr(3);
        auto* len = c.ptr<std::uint32_t>(4);
        if (!val || !len || *len < 4) return c.fail(android::kEINVAL);
        HostOpt o;
        std::int32_t v = 0;
        if (to_host_opt(level, name, o) && !(o.level == SOL_SOCKET && (o.name == SO_RCVTIMEO || o.name == SO_SNDTIMEO || o.name == SO_LINGER))) {
            socklen_t hl = sizeof v;
            if (::getsockopt(s->s, o.level, o.name, reinterpret_cast<char*>(&v), &hl) != 0) return c.fail(-fail_code());
            if (o.level == SOL_SOCKET && o.name == SO_ERROR) v = v ? to_guest_errno(v) : 0;
            if (o.level == SOL_SOCKET && o.name == SO_TYPE) v = v == SOCK_DGRAM ? guest::kSOCK_DGRAM : guest::kSOCK_STREAM;
        }
        std::memcpy(val, &v, 4);
        *len = 4;
        c.ret(0);
    });

    // --- names -----------------------------------------------------------------------------
    reg.add("getaddrinfo", [](CallContext& c) {
        if (!online(c)) return c.ret(guest::kEAI_NONAME);
        ensure_started();
        addrinfo hints{};
        if (GuestAddr h = c.x(2)) {
            auto& mem = c.mem();
            hints.ai_flags = to_host_ai_flags(mem.read<std::int32_t>(h));
            int family = to_host_family(mem.read<std::int32_t>(h + 4));
            if (family < 0) return c.ret(guest::kEAI_FAMILY);
            hints.ai_family = family;
            int type = mem.read<std::int32_t>(h + 8);
            hints.ai_socktype = type == guest::kSOCK_STREAM ? SOCK_STREAM : type == guest::kSOCK_DGRAM ? SOCK_DGRAM : 0;
            hints.ai_protocol = mem.read<std::int32_t>(h + 12);
        }
        addrinfo* res = nullptr;
        int r = ::getaddrinfo(c.str(0), c.str(1), c.x(2) ? &hints : nullptr, &res);
        log::debug("getaddrinfo({}, {}) = {}", c.x(0) ? c.str(0) : "(null)", c.x(1) ? c.str(1) : "(null)", r);
        if (r != 0) return c.ret(static_cast<std::uint64_t>(to_guest_eai(r)));
        GuestAddr list = to_guest_addrinfo(c.rt(), res);
        ::freeaddrinfo(res);
        if (!list) return c.ret(guest::kEAI_NONAME);
        c.mem().write<std::uint64_t>(c.x(3), list);
        c.ret(0);
    });
    reg.add("freeaddrinfo", [](CallContext& c) {
        auto& rt = c.rt();
        for (GuestAddr node = c.x(0); node;) {
            GuestAddr next = rt.mem().read<std::uint64_t>(node + guest::ai_next);
            if (GuestAddr a = rt.mem().read<std::uint64_t>(node + guest::ai_addr)) rt.heap().free(a);
            if (GuestAddr n = rt.mem().read<std::uint64_t>(node + guest::ai_canonname)) rt.heap().free(n);
            rt.heap().free(node);
            node = next;
        }
    });
    reg.add("gai_strerror", [](CallContext& c) { c.ret(intern_string(c.rt(), gai_text(static_cast<int>(c.x(0))))); });
    reg.add("getnameinfo", [](CallContext& c) {
        sockaddr_storage a;
        int len = to_host_addr(c.ptr(0), static_cast<std::uint32_t>(c.x(1)), a);
        if (len < 0) return c.ret(guest::kEAI_FAMILY);
        ensure_started();
        // NI_* flags have the same values on bionic, Linux and Windows.
        int flags = static_cast<int>(c.x(6));
        if (!online(c)) flags |= NI_NUMERICHOST | NI_NUMERICSERV;
        int r = ::getnameinfo(reinterpret_cast<sockaddr*>(&a), len, c.str(2), static_cast<socklen_t>(c.x(3)), c.str(4),
                              static_cast<socklen_t>(c.x(5)), flags);
        c.ret(static_cast<std::uint64_t>(to_guest_eai(r)));
    });
    reg.add("gethostbyname", [](CallContext& c) {
        if (!online(c)) return c.ret(0);
        ensure_started();
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (::getaddrinfo(c.str(0), nullptr, &hints, &res) != 0 || !res) return c.ret(0);
        // struct hostent { char* h_name; char** h_aliases; int h_addrtype; int h_length; char** h_addr_list; }
        // followed here by an empty alias list, a two-entry address list, the address and the name.
        auto& rt = c.rt();
        auto& he = rt.state<HostEnt>();
        std::lock_guard lock(he.mutex);
        if (!he.block) he.block = rt.heap().alloc_zeroed(32 + 8 + 16 + 4 + 256);
        GuestAddr b = he.block, aliases = b + 32, addrs = b + 40, addr = b + 56, name = b + 60;
        std::memset(rt.mem().host(b), 0, 32 + 8 + 16 + 4 + 256);
        std::size_t n = std::min<std::size_t>(std::strlen(c.str(0)), 255);
        std::memcpy(rt.mem().host(name), c.str(0), n);
        std::memcpy(rt.mem().host(addr), &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, 4);
        ::freeaddrinfo(res);
        rt.mem().write<std::uint64_t>(b, name);
        rt.mem().write<std::uint64_t>(b + 8, aliases);
        rt.mem().write<std::int32_t>(b + 16, guest::kAF_INET);
        rt.mem().write<std::int32_t>(b + 20, 4);
        rt.mem().write<std::uint64_t>(b + 24, addrs);
        rt.mem().write<std::uint64_t>(addrs, addr);
        c.ret(b);
    });
    reg.add("inet_pton", [](CallContext& c) {
        int family = static_cast<int>(c.x(0));
        if (family != guest::kAF_INET && family != guest::kAF_INET6) return c.fail(android::kEAFNOSUPPORT);
        c.ret(static_cast<std::uint64_t>(::inet_pton(to_host_family(family), c.str(1), c.ptr(2)) == 1 ? 1 : 0));
    });
    reg.add("inet_ntop", [](CallContext& c) {
        int family = static_cast<int>(c.x(0));
        if (family != guest::kAF_INET && family != guest::kAF_INET6) return c.fail(android::kEAFNOSUPPORT, 0);
        char buf[64];
        if (!::inet_ntop(to_host_family(family), c.ptr(1), buf, sizeof buf)) return c.fail(android::kEINVAL, 0);
        std::size_t n = std::strlen(buf) + 1;
        if (n > c.x(3)) return c.fail(guest::kENOSPC, 0);
        std::memcpy(c.ptr(2), buf, n);
        c.ret(c.x(2));
    });
    reg.add("if_nametoindex", [](CallContext& c) { c.ret(0); });

    // --- waiting ---------------------------------------------------------------------------
    reg.add("poll", [](CallContext& c) {
        // struct pollfd { int fd; short events; short revents; }
        GuestAddr p = c.x(0);
        auto n = static_cast<std::size_t>(c.x(1));
        int timeout = static_cast<int>(c.x(2));
        auto& mem = c.mem();
        std::vector<Waited> fds;
        for (std::size_t i = 0; i < n; ++i) {
            int fd = mem.read<std::int32_t>(p + 8 * i);
            short ev = mem.read<std::int16_t>(p + 8 * i + 4);
            fds.push_back({fd, (ev & (guest::kPOLLIN | guest::kPOLLRDNORM | guest::kPOLLPRI)) != 0,
                           (ev & (guest::kPOLLOUT | guest::kPOLLWRNORM)) != 0, nullptr, false, {}});
        }
        int r = wait_fds(c.rt(), fds, timeout);
        if (r < 0) return c.fail(-r);
        int count = 0;
        for (std::size_t i = 0; i < n; ++i) {
            short ev = mem.read<std::int16_t>(p + 8 * i + 4), rev = 0;
            const auto& f = fds[i];
            if (f.fd >= 0) {
                if (!f.valid) rev = guest::kPOLLNVAL;
                else {
                    if (f.ready.read) rev |= ev & (guest::kPOLLIN | guest::kPOLLRDNORM | guest::kPOLLPRI);
                    if (f.ready.write) rev |= ev & (guest::kPOLLOUT | guest::kPOLLWRNORM);
                    if (f.ready.error) rev |= guest::kPOLLERR;
                    if (f.ready.hup) rev |= guest::kPOLLHUP;
                }
            }
            mem.write<std::int16_t>(p + 8 * i + 6, rev);
            if (rev) ++count;
        }
        c.ret(static_cast<std::uint64_t>(count));
    });

    // fd_set is 1024 bits, as an array of 64-bit words.
    reg.add("__FD_SET_chk", [](CallContext& c) {
        auto fd = static_cast<std::uint64_t>(c.x(0));
        c.mem().host(c.x(1))[fd / 8] |= static_cast<std::uint8_t>(1u << (fd % 8));
    });
    reg.add("__FD_CLR_chk", [](CallContext& c) {
        auto fd = static_cast<std::uint64_t>(c.x(0));
        c.mem().host(c.x(1))[fd / 8] &= static_cast<std::uint8_t>(~(1u << (fd % 8)));
    });
    reg.add("__FD_ISSET_chk", [](CallContext& c) {
        auto fd = static_cast<std::uint64_t>(c.x(0));
        c.ret((c.mem().host(c.x(1))[fd / 8] >> (fd % 8)) & 1);
    });
    reg.add("select", [](CallContext& c) {
        int nfds = std::clamp(static_cast<int>(c.x(0)), 0, 1024);
        std::uint8_t* sets[3] = {c.ptr(1), c.ptr(2), c.ptr(3)};
        auto is_set = [&](int k, int fd) { return sets[k] && ((sets[k][fd / 8] >> (fd % 8)) & 1); };
        std::vector<Waited> fds;
        std::vector<bool> except;
        for (int fd = 0; fd < nfds; ++fd)
            if (is_set(0, fd) || is_set(1, fd) || is_set(2, fd)) {
                fds.push_back({fd, is_set(0, fd), is_set(1, fd), nullptr, false, {}});
                except.push_back(is_set(2, fd));
            }
        int timeout = -1;
        if (GuestAddr tv = c.x(4))  // struct timeval { long tv_sec; long tv_usec; }
            timeout = static_cast<int>(std::min<std::int64_t>(c.mem().read<std::int64_t>(tv) * 1000 + c.mem().read<std::int64_t>(tv + 8) / 1000, INT32_MAX));
        for (const auto& f : fds)
            if (!socket_at(c.rt(), f.fd) && !fd_is_open(c.rt(), f.fd)) return c.fail(android::kEBADF);
        int r = wait_fds(c.rt(), fds, timeout);
        if (r < 0) return c.fail(-r);
        for (auto* s : sets)
            if (s) std::memset(s, 0, static_cast<std::size_t>((nfds + 7) / 8));
        int count = 0;
        auto mark = [&](int k, int fd) {
            if (!sets[k]) return;
            sets[k][fd / 8] |= static_cast<std::uint8_t>(1u << (fd % 8));
            ++count;
        };
        for (std::size_t i = 0; i < fds.size(); ++i) {
            const auto& f = fds[i];
            // A failed connect shows up as readable and writable, as on Linux (Windows only
            // flags it as an exception).
            if (f.read && (f.ready.read || f.ready.hup || f.ready.error)) mark(0, f.fd);
            if (f.write && (f.ready.write || f.ready.error)) mark(1, f.fd);
            if (except[i] && f.ready.error) mark(2, f.fd);
        }
        c.ret(static_cast<std::uint64_t>(count));
    });
}

}  // namespace rfu
