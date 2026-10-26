# How the runtime works

## Guest memory

The guest (the game's arm64 code) sees a 64 GiB address space that is a single host reservation,
so a guest address is an offset from `GuestMemory::base()`. dynarmic uses the same reservation as
its fastmem arena: guest loads and stores compile to plain host loads and stores, and only
accesses to unmapped memory reach the slow path, where they become a `GuestFault`.

| Guest address | Contents |
| --- | --- |
| `0` - `0x1000'0000` | never mapped, so null pointer dereferences fault |
| `0x1000'0000` | loaded libraries, one after another |
| `0x4'0000'0000` | the guest heap (8 GiB), used by `malloc`, `mmap` and runtime objects |
| `0x8'0000'0000` | thread stacks, each with an unmapped guard page below it |
| `0xF'0000'0000` | import thunks |

## Calls between guest and host

Every imported function the libraries don't provide to each other (`malloc`, `fopen`,
`pthread_create`, JNI functions...) is bound to a *thunk*: two instructions, `svc #n; ret`. When
the guest calls one, dynarmic stops at the `svc`, and `Cpu::call` runs shim number `n` outside the
JIT. The shim reads its arguments from the guest registers through a `CallContext` (AAPCS64: x0-x7,
v0-v7, then the stack), writes the result to x0 or v0, and the guest resumes at `ret`.

Host code calls guest code with `Runtime::call` (or `Cpu::call` from inside a shim, for callbacks
such as `qsort` comparators): it sets up the arguments, points the link register at a special
return thunk, and runs the JIT until that thunk is reached. The interrupted guest state is saved
and restored around nested calls.

Imports nothing implements are bound to a stub that logs the first call and returns 0, so the game
runs as far as it can and the log says what to implement next.

## Threads

Each guest thread is a host thread with its own guest stack, bionic TLS block (`TPIDR_EL0`,
including the stack-protector cookie at offset 0x28) and its own dynarmic JIT. Guest mutexes,
condition variables and semaphores map to host primitives by address, created on first use, so
statically initialised ones work.

## Android ABI details that matter

- `long` and pointers are 64-bit; `wchar_t` is 32-bit; `long double` is IEEE binary128.
- `va_list` is a 32-byte struct passed by reference; `VaListArgs` walks it.
- `struct stat`, `struct tm`, `FILE`, `pthread_attr_t` and friends use bionic's layouts, which the
  shims fill in field by field.
- RFLib finds libPVZ2 by reading `/proc/self/maps`, which the VFS generates from the loaded modules,
  then patches libPVZ2's code. Patching code is followed by an instruction cache flush, which drops
  the JIT's translations of that range.

## Network

The game downloads its updates and live events itself: libPVZ2 links libcurl 7.61 and OpenSSL
1.1.1 statically, and RFLib points the CDN client at `https://reflourished.ducnguyen.org/cdn`. So
rfu only provides the BSD socket layer (`src/shims/libc_net.cpp`): `socket`, `connect`, `send`,
`recv`, `getaddrinfo`, `poll`, `select` and friends map onto host sockets (Winsock on Windows).
A socket is an fd in the same table as files, so `read`, `write`, `close` and `fcntl(O_NONBLOCK)`
work on it. bionic's constants and structs differ from Windows' (`AF_INET6`, the poll bits,
`SOL_SOCKET`, errno values, `struct addrinfo` field order), so every value is translated at the
boundary. `--offline` makes `socket()` and `getaddrinfo()` fail as they would without a
connection, and the game takes its offline path.

Not known yet: where the game's OpenSSL looks for trusted root certificates. Its compiled-in
default is a path from the build machine; if the game neither sets one nor turns verification
off, rfu will need to mount a CA bundle there.

## Java

There is no JVM. `src/jni` gives native code a JavaVM and JNIEnv whose functions work on a small
object model (classes by name, strings, arrays, instances). Calls to Java methods are logged once
and answered with a neutral value: 0, an empty string, or a new instance of the declared return
type. `Jni::implement` replaces that answer for a specific method; this is where the game's Java
side (assets, device info, the activity lifecycle) will be written.
