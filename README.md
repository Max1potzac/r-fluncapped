# r-fluncapped

A native Windows runtime for Plants vs. Zombies 2 Reflourished.

The plan: load the game's own Android libraries (`libPVZ2.so`, `libRFLib.so`) on the PC through an
ARM64 recompiler and a shim layer, so the game is fully playable early, then replace its code with
native C++ one function at a time.

This repository contains only original code. It ships no game code or assets: the runtime loads
them from your own copy of the game.

## Status

Milestone 2 (the loader) is in progress. Today `rfu` can:

- map `libPVZ2.so`, `libRFLib.so`, `libc++_shared.so` and `libNimble.so` into one guest address
  space and link them the way Android's linker would;
- run them on an x86-64 PC through [dynarmic](https://github.com/azahar-emu/dynarmic), with
  bionic's libc, libm, libdl and zlib provided by host code (`src/shims`);
- give native code a JavaVM/JNIEnv backed by a small fake Java world (`src/jni`);
- run every static initializer and both `JNI_OnLoad`s, after which RFLib has installed all of its
  hooks into libPVZ2.

There is no window, rendering, audio or input yet: OpenGL ES, OpenSL ES and the game's Java side
are the next milestones. `rfu` reports which imports are still unimplemented.

## Building

You need CMake 3.24+, a C++20 compiler, Boost headers (icl, variant) and zlib. dynarmic and its
bundled dependencies are fetched by CMake.

**Windows** (Visual Studio 2022 with vcpkg, which installs Boost and zlib from `vcpkg.json`):

```
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**Linux**:

```
sudo apt install cmake ninja-build clang lld libboost-dev zlib1g-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The tests (`ctest --test-dir build`) include an arm64 guest library; building it needs `clang` and
`ld.lld` on the PATH (any recent LLVM, no Android NDK). Without them only the host tests run.

## Running

Extract `lib/arm64-v8a/` from the game's APK, then:

```
rfu --lib-dir path/to/arm64-v8a
```

Options: `--log trace|debug|info|warn|error`, `--no-rflib` (vanilla code only),
`--mount <guest path>=<host dir>` (make a folder visible to the game), `--dump-patches <file>`.

## Layout

| Path | What |
| --- | --- |
| `src/core` | Guest memory, heap, ELF loading and linking, the virtual file system |
| `src/cpu` | The dynarmic JIT wrapper: one CPU per guest thread, calls in and out of guest code |
| `src/shims` | Host implementations of the Android system libraries the game imports |
| `src/jni` | JavaVM and JNIEnv for native code, with logged default answers for Java calls |
| `tests` | Host unit tests and `tests/guest/testlib.c`, an arm64 library run under the JIT |
| `tools/pvz2conv` | C# tool that unpacks the game's .obb and converts PTX to PNG and RTON, PAM, POPFX to JSON (see its README.txt) |

See [docs/runtime.md](docs/runtime.md) for how the pieces fit together.
