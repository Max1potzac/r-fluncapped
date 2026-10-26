#pragma once

// com.popcap.SexyAppFramework.AndroidHttpTransaction: the game's own HTTP requests (as
// opposed to the CDN client inside libPVZ2, which uses its built-in libcurl over rfu's
// sockets), made with the host's HTTP client on a small pool of worker threads.

namespace rfu {
class Runtime;
}

namespace rfu::android {

// online = false: every request fails at once, as with no connection.
void implement_http(Runtime& rt, bool online);

}  // namespace rfu::android
