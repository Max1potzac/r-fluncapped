#pragma once

// A blocking HTTP(S) client for the requests the game makes through Java
// (java.net.HttpURLConnection on Android): WinHTTP on Windows, libcurl elsewhere.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rfu::platform {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
    bool has_body = false;
    int timeout_seconds = 30;
};

struct HttpResponse {
    int status = 0;
    std::string status_line;  // the reason phrase: "OK", "Not Found"
    std::vector<std::pair<std::string, std::string>> headers;
    std::int64_t content_length = -1;

    // Value of a response header, matched without regard to case ("" when absent).
    std::string header(const std::string& name) const;
};

struct HttpHandler {
    std::function<void(const HttpResponse&)> on_response;  // once, before any data
    std::function<void(const std::uint8_t*, std::size_t)> on_data;
};

// Performs the request. Returns false (with `error` set) when no response arrived: no
// connection, TLS failure, timeout, or `cancel` was set.
bool http_perform(const HttpRequest& request, HttpHandler& handler, const std::atomic<bool>& cancel, std::string& error);

bool http_available();

}  // namespace rfu::platform
