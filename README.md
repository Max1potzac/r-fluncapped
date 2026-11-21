# r-fluncapped

A native Windows runtime for Plants vs. Zombies 2 and its many versions.

The plan: load the game's own Android libraries (`libPVZ2.so`) on the PC through an
ARM64 recompiler and a shim layer, so the game is fully playable early, then replace its code with
native C++ one function at a time.

This repository contains only original code. It ships no game code or assets: the runtime loads
them from your own copy of the game.

## Status

`rfu` runs the game. It loads PVZ2's own Android libraries, opens a window and plays the
part of the game's Java side, so the game boots, loads its OBB, shows the EA welcome dialog, the
main menu and its intro, and takes mouse, touch and keyboard input. In detail, `rfu`:

- maps `libPVZ2.so`, `libc++_shared.so` and `libNimble.so` into one guest address
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
- answers Reflourished 1.5.0's `FilePickerHelper`: Settings > Import/Export Files (custom levels,
  saves, quest progress) opens the desktop's own Open and Save As windows (zenity on Linux), and
  the OBB patches the game applies at start-up (`<obb>.patchN`, downloaded by the launcher) find
  the OBB in the game's OBB folder. When the game asks to be restarted, rfu exits with code 3.

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

**PvZ2 China** (`com.popcap.pvz2cthdlx`, work in progress): take `lib/arm64-v8a/` out of the APK
(the whole game is `libSrc.so`) and point rfu at the APK itself, where the game's packages live:

```
rfu --lib-dir path/to/arm64-v8a --apk path/to/pvz2c.apk
```

rfu sees `libSrc.so` and plays PvZ2 China's own Java side (`com.popcap.pvz2cthddev.*`): it reads the
packages out of the APK as Android's `openFd` would, answers the game's APK signature check with the
APK's real certificate, and answers the Talkweb SDK with an offline guest login. The game unpacks
its packages and loads; it then waits on its cloud save sync, which is next.

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
| `--custom-cursor` | the glove cursor (`assets/cursor.png`) for the mouse, in place of the system's arrow; it presses while the button's down |
| `--fix-animations` | unfreeze plants stuck on an animation while the level isn't running (Citron before the first wave or after the last zombie) |
| `--keybinds <file>` | keys for seed packets, the shovel, plant food, fast-forward, pause, scrolling, a finger on a spot of the screen, or the back button (see `src/platform/keybinds.h`) |
| `--key-hints` | with `--keybinds` or `--controller`: show each key (or the controller's buttons, labelled for the pad plugged in) in a small badge on the seed packet or button it presses |
| `--controller` | play the lawn with a game controller, PvZ1 console style: the d-pad or left stick moves a tile cursor, LB/RB pick the seed packet, A plants it, B held a second digs, Y feeds plant food (held two seconds, buys one), X taps the tile (plant abilities; an aiming plant's reticle then follows the cursor, X fires, A calls it off; held in the first column, the mower), LT/RT collect everything on the lawn, Start pauses, L3 fast-forwards, R3 turns slowmode (half speed) on and off. The right stick flicked left, up or right picks the level's first, second or third powerup; while it's in use X held uses it on the tile (snowballs, the flamethrower, Power Zap, a mod's own), or X pressed twice swipes the tile for the flick. While a character talks ("tap to continue") any button moves it on. On the pause menu and the settings the d-pad picks a slider, button or checkbox (left/right move a slider; the settings' list scrolls along), A presses it, B resumes; on the seed chooser the d-pad moves a box over the packets (scrolling the list), A picks one, LB/RB pick a slot, B takes its packet out (held two seconds, all of them), Y looks at the lawn, LT picks the last level's packets again, RT opens the almanac and X held a second is Let's Rock; elsewhere the right stick moves a glove cursor, A touches there, the d-pad or left stick scrolls (world by world on the worlds) and B goes back (on the world map LT is the almanac, RT the Zen Garden, Start the settings and Y the shop). Select held two seconds in a level (a dial fills at the top) swaps the tile cursor for the free glove cursor, and back: the sticks and d-pad move it, A touches there (taking the LB/RB packet first), the powerups are off. The game's other dialogs (a level's objectives, confirmations) and the title screen work like the pause menu; on the title screen LB/RB swipe the banners. Using the mouse hides the pad's cursor and labels until the pad is used again |
| `--pad-binds <binds>` | with `--controller`: the button for each action, each screen on its own. The lawn's: `plant=a,dig=b,use=x,plantfood=y,prevseed=lb,nextseed=rb,vacuum=lt,vacuum2=rt,fastforward=l3,pause=start,slowmode=r3,freecursor=back`; the others' with their screen, e.g. `chooser.take=a`, `menu.press=a`, `map.shop=y`, `other.touch=a` (all in `src/game/game_keys.h`). The launcher's controller settings write it |
| `--pad-layout <file>` | with `--controller`: buttons that tap a spot on one screen in place of what they do there, as `map y tap 95% 7%` lines (screens: `lawn`, `chooser`, `menu`, `map`, `other`, `any`). Several add up, later ones winning. The launcher's keybind layout editor writes them |
| `--map-buttons <spots>` | with `--controller`: where the world map's buttons are, for a map layout rfu doesn't know yet, as `settings=137:38,almanac=222:38,zengarden=395:38,shop=-75:53` (pixels of the screen scaled to 768 high, from the top left; a negative x counts from the right). Buttons left out keep the built-in positions. Add it under the launcher's extra arguments |
| `--pad-mappings <file>` | with `--controller`: SDL mappings (gamecontrollerdb.txt lines) for controllers SDL doesn't know, such as the launcher's controllers.txt |
| `--obb-patch <file>` | read the OBB with some of its files replaced (mods' fonts, `PACKAGES/FONTS.rton`), from a patch the launcher makes; the OBB itself isn't changed (see `src/core/file_patch.h`) |
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
| `tools/builder` | C# modding toolkit (PvZ2 Builder): opens any international PvZ2 game (EA's, Reflourished, fan games), lists what that game has, edits its property sheets with typed fields, previews its animations, and saves the edits as a launcher mod (see its README.txt) |

See [docs/runtime.md](docs/runtime.md) for how the pieces fit together.
