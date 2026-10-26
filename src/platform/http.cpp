#include "platform/http.h"

#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#elif defined(RFU_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace rfu::platform {

namespace {

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return s.substr(i);
}

}  // namespace

std::string HttpResponse::header(const std::string& name) const {
    for (const auto& [k, v] : headers)
        if (iequals(k, name)) return v;
    return {};
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w, std::size_t len) {
    if (!len) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
    return s;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string last_error(const char* what) { return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")"; }

}  // namespace

bool http_available() { return true; }

bool http_perform(const HttpRequest& request, HttpHandler& handler, const std::atomic<bool>& cancel, std::string& error) {
    std::wstring url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    parts.dwSchemeLength = 1;
    parts.dwExtraInfoLength = 1;  // the query string stays in lpszUrlPath when this is set to non-zero and lpszExtraInfo is null
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = last_error("WinHttpCrackUrl");
        return false;
    }
    std::wstring object(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (object.empty()) object = L"/";

    Handle session{WinHttpOpen(L"Dalvik/2.1.0 (Linux; U; Android 11)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) session.h = WinHttpOpen(L"Dalvik/2.1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        error = last_error("WinHttpOpen");
        return false;
    }
    int ms = request.timeout_seconds * 1000;
    WinHttpSetTimeouts(session.h, ms, ms, ms, ms);
    Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connection.h) {
        error = last_error("WinHttpConnect");
        return false;
    }
    bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;
    Handle req{WinHttpOpenRequest(connection.h, widen(request.method).c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)};
    if (!req.h) {
        error = last_error("WinHttpOpenRequest");
        return false;
    }
    std::wstring headers;
    for (const auto& [k, v] : request.headers) headers += widen(k + ": " + v + "\r\n");
    LPVOID body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<std::uint8_t*>(request.body.data());
    auto body_size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1L), body,
                            body_size, body_size, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        error = last_error("sending the request");
        return false;
    }

    HttpResponse response;
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    auto query_text = [&](DWORD what) {
        DWORD bytes = 0;
        WinHttpQueryHeaders(req.h, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
        if (!bytes) return std::string();
        std::wstring buf(bytes / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(req.h, what, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return std::string();
        return narrow(buf.c_str(), bytes / sizeof(wchar_t));
    };
    response.status_line = query_text(WINHTTP_QUERY_STATUS_TEXT);
    std::string raw = query_text(WINHTTP_QUERY_RAW_HEADERS_CRLF);
    std::size_t pos = raw.find("\r\n");  // skip the status line
    while (pos != std::string::npos && pos + 2 < raw.size()) {
        std::size_t end = raw.find("\r\n", pos + 2);
        std::string line = raw.substr(pos + 2, end == std::string::npos ? std::string::npos : end - pos - 2);
        auto colon = line.find(':');
        if (colon != std::string::npos) response.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
        pos = end;
    }
    std::string length = response.header("Content-Length");
    if (!length.empty()) response.content_length = std::stoll(length);
    if (handler.on_response) handler.on_response(response);

    std::vector<std::uint8_t> buf(16384);
    for (;;) {
        if (cancel.load()) {
            error = "cancelled";
            return false;
        }
        DWORD read = 0;
        if (!WinHttpReadData(req.h, buf.data(), static_cast<DWORD>(buf.size()), &read)) {
            error = last_error("WinHttpReadData");
            return false;
        }
        if (!read) break;
        if (handler.on_data) handler.on_data(buf.data(), read);
    }
    return true;
}

#elif defined(RFU_HAVE_CURL)

namespace {

struct CurlContext {
    HttpHandler* handler;
    const std::atomic<bool>* cancel;
    HttpResponse response;
    bool response_sent = false;
    CURL* curl;

    void send_response() {
        if (response_sent) return;
        response_sent = true;
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        response.status = static_cast<int>(code);
        curl_off_t length = -1;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
        response.content_length = length;
        if (handler->on_response) handler->on_response(response);
    }
};

std::size_t on_header(char* data, std::size_t size, std::size_t n, void* user) {
    auto* ctx = static_cast<CurlContext*>(user);
    std::string line(data, size * n);
    if (line.rfind("HTTP/", 0) == 0) {
        // A new status line (after a redirect or a 100 Continue): start over.
        ctx->response.headers.clear();
        auto sp1 = line.find(' '), sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        ctx->response.status_line = sp2 == std::string::npos ? "" : trim(line.substr(sp2 + 1));
    } else if (auto colon = line.find(':'); colon != std::string::npos) {
        ctx->response.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    return size * n;
}

std::size_t on_body(char* data, std::size_t size, std::size_t n, void* user) {
    auto* ctx = static_cast<CurlContext*>(user);
    if (ctx->cancel->load()) return 0;
    ctx->send_response();
    if (ctx->handler->on_data) ctx->handler->on_data(reinterpret_cast<const std::uint8_t*>(data), size * n);
    return size * n;
}

int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<CurlContext*>(user)->cancel->load() ? 1 : 0;
}

}  // namespace

bool http_available() { return true; }

bool http_perform(const HttpRequest& request, HttpHandler& handler, const std::atomic<bool>& cancel, std::string& error) {
    static const bool initialised = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    (void)initialised;
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    CurlContext ctx{&handler, &cancel, {}, false, curl};
    curl_slist* headers = nullptr;
    for (const auto& [k, v] : request.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    if (request.method == "HEAD") curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    if (request.has_body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Dalvik/2.1.0 (Linux; U; Android 11)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // HttpURLConnection follows redirects
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(request.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(request.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) ctx.send_response();  // an empty body never reached on_body
    else error = curl_easy_strerror(rc);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return rc == CURLE_OK;
}

#else

bool http_available() { return false; }

bool http_perform(const HttpRequest&, HttpHandler&, const std::atomic<bool>&, std::string& error) {
    error = "rfu was built without an HTTP client";
    return false;
}

#endif

}  // namespace rfu::platform
