#pragma once

// com.popcap.SexyAppFramework.AndroidHttpTransaction: the game's own HTTP requests (as
// opposed to the CDN client inside libPVZ2, which uses its built-in libcurl over rfu's
// sockets), made with the host's HTTP client on a small pool of worker threads.

#include <string>

namespace rfu {
class Runtime;
}

namespace rfu::android {

// online = false: every request fails at once, as with no connection. PvZ2 China's class is
// com/popcap/pvz2cthddev/AndroidHttpTransaction; it works the same way.
void implement_http(Runtime& rt, bool online,
                    const std::string& transaction_class = "com/popcap/SexyAppFramework/AndroidHttpTransaction");

}  // namespace rfu::android
