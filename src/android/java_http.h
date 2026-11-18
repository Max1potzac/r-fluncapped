#pragma once

// com.popcap.SexyAppFramework.AndroidHttpTransaction: the game's own HTTP requests (as
// opposed to the CDN client inside libPVZ2, which uses its built-in libcurl over rfu's
// sockets), made with the host's HTTP client on a small pool of worker threads.

#include <cstdint>
#include <filesystem>
#include <string>

namespace rfu {
class Runtime;
}

namespace rfu::android {

// online = false: every request fails at once, as with no connection. PvZ2 China's class is
// com/popcap/pvz2cthddev/AndroidHttpTransaction; it works the same way. cache_dir: where images
// (the title screen's banners) are kept from their last good download, for when a later one fails.
void implement_http(Runtime& rt, bool online,
                    const std::string& transaction_class = "com/popcap/SexyAppFramework/AndroidHttpTransaction",
                    const std::filesystem::path& cache_dir = {},
                    const std::string& hold_ui = {});

// PvZ2 China cloud (RFU_CLOUD_STUB / RFU_CLOUD_RECORD): main.cpp hooks the game's
// request-encrypt function (libSrc.so+0x21b8870) and reports each call here. `manager` is
// the function's first argument, the cloud manager object whose libc++ std::string at +0x40
// is the crypto seed of the moment (empty before the V202 login, the per-login session seed
// after). This is how the stub keeps its key in sync with the game.
void cloud_stub_note_encrypt_entry(Runtime& rt, std::uint64_t manager);

}  // namespace rfu::android
