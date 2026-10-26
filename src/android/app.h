#pragma once

// The game's Android shell, in place of its Java code: the activity lifecycle, the GL
// surface's render loop, the input queue, and the Java methods native code calls
// (preferences, device information, paths, the keyboard...).
//
// What runs here follows the game's own Java classes (com.popcap.SexyAppFramework.*,
// com.popcap.PvZ2.PvZ2GameActivity) step for step, so native code sees the same calls in
// the same order as on a phone.

#include "platform/window.h"

#include <filesystem>
#include <string>

namespace rfu {
class Runtime;
}

namespace rfu::android {

struct AppOptions {
    std::filesystem::path obb;       // main.<version>.<package>.obb
    std::filesystem::path data_dir;  // where the game's internal and external storage live
    int version_code = 675;
    std::string version_name = "1.4.0";
    platform::WindowOptions window;
    int max_frames = 0;              // stop after this many frames (0: run until the window closes)
    std::filesystem::path screenshot;  // with max_frames: save the last frame here as a .bmp
    std::filesystem::path script;      // scripted input for testing: "<frame> <action> <args>" lines
};

// Guest paths of the game's storage, as Android would give them.
struct AndroidPaths {
    std::string internal;   // Context.getFilesDir()'s parent: /data/data/<package>
    std::string external;   // Context.getExternalFilesDir(null)'s parent
    std::string obb_dir;    // Context.getObbDir()
    std::string obb_file;
};
AndroidPaths android_paths(const std::string& package, int version_code);

// Mounts the game's storage and OBB in the VFS (call before the libraries load).
void mount_storage(Runtime& rt, const AppOptions& options);

// Runs the game after its libraries are loaded and initialised (JNI_OnLoad included): the
// activity's onCreate, launchGame, onStart and onResume, then the render loop until the
// window is closed. Returns the process exit code.
int run_app(Runtime& rt, const AppOptions& options);

}  // namespace rfu::android
