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

Each thread records the import it is inside (with its first argument and the guest's `lr` and
`fp` at the call). If no frame finishes for 30 seconds, rfu logs every guest thread: the import it
is blocked in, where it was called from, and the guest call stack found by following frame
pointers (`libPVZ2.so+0x...`), which is usually enough to see two threads waiting on each other.

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
object model (classes by name, strings, arrays, instances with fields). Calls to Java methods are
logged once and answered with a neutral value (0, an empty string, or a new instance of the
declared return type) unless something implements them with `Jni::implement`, which receives the
call's Java arguments decoded from whichever of the three calling conventions native code used
(C varargs, `va_list` or a `jvalue` array).

`src/android` is the game's Java side, written from its decompiled classes
(`com.popcap.SexyAppFramework.*`, `com.popcap.PvZ2.PvZ2GameActivity`):

- **Lifecycle.** `run_app` makes the same native calls the activity does, in the same order:
  `Native_createNativeApplicationLifecycleObserver` (onCreate), `Native_GameAppInitialize` (which
  registers the remaining natives), `applicationWillFinishLaunching`/`DidFinishLaunching`,
  onResume, then the GL surface's `Native_onSurfaceCreated`/`Changed` and the focus logic of
  `AndroidGameApp.TryGainFocus`. Closing the window runs onPause and onDestroy, where the game
  saves.
- **Render loop.** `AndroidRenderer.onDrawFrame`: `PumpMessageQueue`, clear, the frame limiter set
  by `Graphics_SetFramerate`, `Native_onDrawFrame`, swap. Everything runs on the main thread, which
  is both Android's UI thread and its GL thread; the game's own threads are guest threads.
- **Input.** `AndroidUIEventManager` serialises touches, keys, text, the back button and gestures
  into a direct ByteBuffer the game passes to `UI_ProcessEvents`: the event count at offset 0,
  events from offset 16, each a type word followed by its fields (see `UiEvents` in
  `src/android/app.cpp`). As on Android, keys only go through the soft keyboard's input
  connection (Enter, Delete and digits as key events, other characters as committed text), and
  touches are dropped until the game removes the splash screen.
- **Storage.** `mount_storage` maps Android's paths onto the `--data` folder: `/data/data/<package>`
  to `data/`, `/sdcard/Android/data/<package>` to `external/`, the rest of `/sdcard` to `sdcard/`,
  and the OBB to `/sdcard/Android/obb/<package>/main.<version>.<package>.obb`.
  `Activity.getPreferences()` is `data/shared_prefs/PvZ2GameActivity.prefs`.
- **Web requests.** Besides its CDN client, the game makes some requests through Java
  (`AndroidHttpTransaction`, over `HttpURLConnection`). `src/android/java_http.cpp` runs them on
  four worker threads with the host's HTTP client (`src/platform/http.cpp`: WinHTTP on Windows,
  libcurl elsewhere) and reports back through the same natives Java calls.

## Graphics, sound and the window

`src/platform` is the only part of rfu that knows about SDL2.

- **OpenGL ES.** libPVZ2 imports 57 OpenGL ES 2.0 functions and nothing else (no buffer objects:
  every vertex array is a client-side pointer into guest memory, which the host GL reads in
  place). Each is a shim calling the host's ES implementation through `SDL_GL_GetProcAddress`, with
  guest pointers turned into host ones; `glTexImage2D`'s ninth argument comes from the guest
  stack. The context is an ES 2 context: on Windows from the driver's ES profile
  (`WGL_EXT_create_context_es2_profile`, which NVIDIA, AMD and Intel drivers have) or, with
  `--angle`, from ANGLE; on Linux from Mesa through EGL.
- **Textures.** Reflourished's PTX textures are RGBA8888, RGBA4444, RGB565, ETC1 with a separate
  alpha plane, and ETC2 RGBA8. When the driver doesn't list an ETC format as a compressed texture
  format, `glCompressedTexImage2D` decodes it to RGBA8 (`src/platform/etc.cpp`, the same decoder as
  pvz2conv) and uploads that instead; `glGetString(GL_EXTENSIONS)` and the compressed-format
  queries always report ETC1 and ETC2, so the game picks the same textures as on a phone.
- **Sound.** The game's sound engine (Wwise) plays PCM through an OpenSL ES buffer-queue player.
  `src/platform/opensles.cpp` implements the engine, output mix and player objects with their
  vtables of thunks; each player is an SDL audio device whose callback copies the queued guest
  buffers out and, when one is used up, calls the game's buffer-queue callback right there, on a
  guest thread bound to SDL's audio thread. Without an audio device the game runs silently.
- **Exit.** When the game stops, rfu ends the process without unwinding it, as Android does: the
  game's threads are still running and the runtime can't be torn down under them.

## Testing with scripts

`--script <file>` feeds input to the game at given frames, and takes screenshots, so a run can be
repeated without a person at the mouse (and on a machine without one). Each line is
`<frame> <action> [arguments]`, `#` starts a comment:

| Action | What |
| --- | --- |
| `tap <x> <y>` | press and release (three frames later) at surface pixel x, y |
| `drag <x1> <y1> <x2> <y2> [frames]` | press, move over `frames` frames (default 10), release |
| `text <characters>` | type, as on the soft keyboard (digits as key presses) |
| `key <android keycode>` | press and release a key: 66 Enter, 67 Delete, 7-16 digits |
| `back` | Android's back button |
| `screenshot <file.bmp>` | save the frame |
| `quit` | close the window |

For example, from a fresh `--data` folder on the 1.4.2 OBB at 1280x720, this reaches the intro:

```
800  tap 640 225     # the name box on the Welcome dialog
860  text 25
900  key 66          # Enter: accept
960  tap 476 486     # "Skip base game content?": No
1070 tap 640 656     # Play
1300 screenshot intro.bmp
1310 quit
```
