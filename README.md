# r-fluncapped

A native Windows runtime for Plants vs. Zombies 2 Reflourished.

The plan: load the game's own Android libraries (`libPVZ2.so`, `libRFLib.so`) on the PC through an
ARM64 recompiler and a shim layer, so the game is fully playable early, then replace its code with
native C++ one function at a time.

This repository contains only original code. It ships no game code or assets: the runtime loads
them from your own copy of the game.

## Status

`rfu` runs the game. It loads Reflourished's own Android libraries, opens a window and plays the
part of the game's Java side, so the game boots, loads its OBB, shows the EA welcome dialog, the
main menu and its intro, and takes mouse, touch and keyboard input. In detail, `rfu`:

- maps `libPVZ2.so`, `libRFLib.so`, `libc++_shared.so` and `libNimble.so` into one guest address
  space, links them the way Android's linker would, and runs them on an x86-64 PC through
  [dynarmic](https://github.com/azahar-emu/dynarmic), with bionic's libc, libm, libdl and zlib
  provided by host code (`src/shims`);
- gives native code a JavaVM/JNIEnv backed by a small fake Java world (`src/jni`), and implements
  the Java methods the game calls (`src/android`): preferences, device information, storage
  paths, the input queue, the soft keyboard, its web requests;
- draws through OpenGL ES 2 in an SDL2 window (`src/platform`), using the GPU driver's ES
  profile or ANGLE, and decodes ETC1/ETC2 textures itself where the driver can't;
- plays the game's sound (OpenSL ES buffer queues) through SDL audio;
- gives the game real sockets and DNS, so its own CDN client (libcurl and OpenSSL inside libPVZ2)
  can fetch updates and live events from the Reflourished server.

## Building

You need CMake 3.24+, a C++20 compiler, Boost headers (icl, variant), zlib and SDL2. dynarmic and
its bundled dependencies are fetched by CMake.

**Windows**: Visual Studio 2022 (the "Desktop development with C++" workload) and a clone of
vcpkg, which installs Boost, zlib and SDL2 from `vcpkg.json`. Once:

```
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
```

Then, in a "Developer PowerShell for VS 2022" in this folder:

```
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

Name your clone's toolchain file, not `$env:VCPKG_ROOT`'s: in a Developer prompt that points at
the vcpkg bundled with Visual Studio, which refuses a manifest without a baseline. `rfu.exe` and
its DLLs end up in `build\Release`; copy them next to the launcher.

**Linux**:

```
sudo apt install cmake ninja-build clang lld libboost-dev zlib1g-dev libsdl2-dev libcurl4-openssl-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The tests (`ctest --test-dir build`) include an arm64 guest library; building it needs `clang` and
`ld.lld` on the PATH (any recent LLVM, no Android NDK). Without them only the host tests run.

## Running

You need the game's APK and its OBB (`main.675.com.ea.game.pvz2_rfl.obb` for 1.4.2). Drop the APK
onto `tools/pvz2conv` to take its `lib/arm64-v8a/` out (it checks the libraries rfu needs are there),
then:

```
rfu --lib-dir path/to/arm64-v8a --obb path/to/main.675.com.ea.game.pvz2_rfl.obb
```

Saves, settings and downloaded content go to `rfu-data` in the current folder (`--data <dir>` to
put them elsewhere). The game's own folders are inside it: `external/files` is what Android calls
`/sdcard/Android/data/com.ea.game.pvz2_rfl/files`, so custom levels go in
`external/files/No_Backup/CDN.9.6/levels`.

Or use the launcher (`tools/launcher`): a menu for each game you have (Reflourished, AltverZ...)
that takes the libraries out of the APK, checks for a new version the way the game does, and
starts `rfu` with your settings, keybinds, code patches and custom content. See its README.txt.

Controls: the left mouse button is your finger, Escape (or the mouse's back button) is Android's
back button, F11 toggles full screen. When the game shows its keyboard, type.

Options:

| Option | What |
| --- | --- |
| `--data <dir>` | where the game keeps saves and downloads (default `rfu-data`) |
| `--window <w>x<h>`, `--fullscreen` | window size (default 1280x720) or full screen |
| `--log-file <file>` | write the log, and the game's own output, to a file |
| `--assets high\|low` | the game's 1536 (default) or 768 asset set; low is lighter on slow GPUs |
| `--resolution <w>x<h>` | the size the game draws at, scaled to fit the window (default: the desktop's shape, 720 px tall for low assets, else 1080 to 1440) |
| `--angle` | draw through ANGLE: put `libEGL.dll` and `libGLESv2.dll` (from vcpkg's `angle` port, or a Chrome / Edge install) next to `rfu.exe`. Use it if your driver has no OpenGL ES profile |
| `--no-vsync` | don't wait for the display between frames |
| `--offline` | no network: the game skips its CDN update and plays offline |
| `--no-mod-libs` | don't load the mod's own libraries (vanilla code only). Without it, rfu loads every library in `--lib-dir` but the game's (`libPVZ2`, `libc++_shared`, `libNimble`) and Crashlytics', after libPVZ2: Reflourished's `libRFLib.so`, AltverZ's `libSnowie.so`. Files named like libraries that aren't code are skipped; the game sees `--lib-dir` at `/data/app/<package>-1/lib/arm64`, so mods can read them there |
| `--package <name>`, `--version-code <n>` | for other builds of the game (default `com.ea.game.pvz2_rfl`, 675) |
| `--log trace\|debug\|info\|warn\|error` | how much to log (default info) |
| `--mount <guest path>=<host dir>` | make a folder visible to the game |
| `--overlay <guest path>=<host dir>` | lay a folder over one of the game's, read-only: custom levels, strings and other content without changing the game's files (later ones win) |
| `--hover` | collect sun, coins and plant food by moving the mouse over them, no click needed |
| `--keybinds <file>` | keys for seed packets and the shovel, a finger on a spot of the screen, or the back button (see `src/platform/keybinds.h`) |
| `--patch <file>` | code patches to apply after the libraries load, checked against the bytes they expect (see `src/core/patches.h`) |
| `--icon <file.png>`, `--title <text>` | the window's icon (pvz2conv takes the game's out of the APK) and title |
| `--version-name <x.y.z>` | the APK's version name, which the game reports (default 1.4.0) |
| `--init-only` | stop after loading and initialising the libraries, and list unimplemented imports |
| `--script <file>`, `--frames <n>`, `--screenshot <file.bmp>` | scripted input and screenshots, for testing (see [docs/runtime.md](docs/runtime.md)) |
| `--dump-patches <file>` | write the libPVZ2.so offsets RFLib patched |
| `--trace-class <name>` | log calls to a libPVZ2 class's virtual functions, to study the game's code (see [docs/runtime.md](docs/runtime.md)) |

## Layout

| Path | What |
| --- | --- |
| `src/core` | Guest memory, heap, ELF loading and linking, the virtual file system |
| `src/cpu` | The dynarmic JIT wrapper: one CPU per guest thread, calls in and out of guest code |
| `src/shims` | Host implementations of the Android system libraries the game imports |
| `src/jni` | JavaVM and JNIEnv for native code, with logged default answers for Java calls |
| `src/android` | The game's Java side: activity lifecycle, render loop, input queue, preferences, paths, web requests |
| `src/platform` | SDL2 window and input, OpenGL ES and OpenSL ES for the guest, ETC texture decoding, the host HTTP client |
| `tests` | Host unit tests and `tests/guest/testlib.c`, an arm64 library run under the JIT |
| `tools/pvz2conv` | C# tool that takes the libraries and icon out of the game's .apk, unpacks its .obb, converts PTX to PNG and RTON, PAM, POPFX to JSON, and JSON back to RTON (see its README.txt) |
| `tools/launcher` | C# launcher: a menu per game, the update check, custom content (mods), keybinds and code patches (see its README.txt) |

See [docs/runtime.md](docs/runtime.md) for how the pieces fit together.
