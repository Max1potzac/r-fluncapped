#include "android/java_http.h"

#include "android/cloud_crypto.h"
#include "core/log.h"
#include "jni/jni.h"
#include "platform/http.h"
#include "runtime.h"

#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>

namespace rfu::android {

namespace {

constexpr int kWorkers = 4;

struct Transaction {
    GuestAddr self = 0;
    std::atomic<std::uint64_t> native{0};  // the C++ transaction; 0 once released
    platform::HttpRequest request;
    platform::HttpResponse response;
    std::mutex mutex;  // guards response
    std::atomic<bool> cancel{false};
    GuestAddr buffer = 0;  // the byte[16384] handed to HttpReceivedData, reused like Java does
};

struct HttpState {
    Runtime* rt = nullptr;
    std::string cls;  // the game's AndroidHttpTransaction class
    bool online = true;
    std::mutex mutex;
    std::condition_variable cv;
    std::map<GuestAddr, std::shared_ptr<Transaction>> transactions;
    std::deque<std::shared_ptr<Transaction>> queue;
    std::vector<std::thread> workers;
};

std::shared_ptr<Transaction> find(HttpState& s, GuestAddr self) {
    std::lock_guard lock(s.mutex);
    auto it = s.transactions.find(self);
    return it == s.transactions.end() ? nullptr : it->second;
}

// PvZ2 China's cloud (the ditwan "cloudpvz2android"/"uuidpvz2android" hosts) is reachable only from
// inside China and needs a Chinese account; offline, rfu answers these requests itself so the game's
// own local profile path runs. The request is multipart/form-data with fields `req` (an endpoint
// code), `e` (base64url(AES-256-CBC(JSON))) and `ev`. See cloud_crypto.h for the scheme.
namespace cloud_stub {

// The game's own cloud hosts. The talkyun/log hosts are telemetry (plain JSON, no reply needed).
bool is_cloud_host(const std::string& url) {
    static const char* hosts[] = {"cloudpvz2android.ditwan.cn", "cloudpvz2androidbeta.ditwan.cn",
                                  "uuidpvz2android.ditwan.cn", "profileintelligent.ditwan.cn",
                                  "pvz2androidtga.talkyun.com.cn", "iosandroidlog.ditwan.cn",
                                  "pvz2logandroid.intelligent.popcap.com.cn"};
    for (const char* h : hosts)
        if (url.find(h) != std::string::npos) return true;
    return false;
}

// Pull a named field's value out of a multipart/form-data body (boundary _{{}}_).
std::string field(const std::string& body, const std::string& name) {
    std::string key = "name=\"" + name + "\"";
    auto p = body.find(key);
    if (p == std::string::npos) return {};
    auto nl = body.find("\r\n\r\n", p);
    std::size_t skip = 4;
    if (nl == std::string::npos) { nl = body.find("\n\n", p); skip = 2; }
    if (nl == std::string::npos) return {};
    auto start = nl + skip;
    auto end = body.find("\r\n--", start);
    if (end == std::string::npos) end = body.find("\n--", start);
    if (end == std::string::npos) end = body.size();
    std::string v = body.substr(start, end - start);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == '-' || v.back() == ' ')) v.pop_back();
    return v;
}

// The plaintext JSON reply for an endpoint, or empty to answer with an empty body. Driven by files
// under RFU_CLOUD_DIR (<req>.json) while the protocol is still being worked out, so responses can be
// tuned without rebuilding; .raw next to it is sent verbatim (to try different response framings).
std::optional<std::string> response_plain(const std::string& req, const std::string& req_json, bool& raw) {
    raw = false;
    if (const char* dir = std::getenv("RFU_CLOUD_DIR")) {
        std::filesystem::path d(dir);
        std::error_code ec;
        if (std::filesystem::exists(d / (req + ".raw"), ec)) {
            std::ifstream in(d / (req + ".raw"), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            raw = true;
            return ss.str();
        }
        if (std::filesystem::exists(d / (req + ".json"), ec)) {
            std::ifstream in(d / (req + ".json"), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            return ss.str();
        }
    }
    (void)req_json;
    return std::nullopt;
}

}  // namespace cloud_stub

// AndroidHttpTransaction.run(), on a worker thread.
void run(HttpState& s, Transaction& t) {
    auto& jni = jni::get(*s.rt);
    auto fire = [&](const char* name, std::vector<std::uint64_t> extra = {}) {
        std::uint64_t native = t.native.load();
        if (!native) return;
        std::vector<std::uint64_t> args{native};
        args.insert(args.end(), extra.begin(), extra.end());
        jni.call_native(s.cls, name, t.self, args);
    };
    // For testing against a local copy of a server: RFU_URL_REWRITE=<prefix>=<replacement>.
    if (const char* rw = std::getenv("RFU_URL_REWRITE")) {
        std::string_view v(rw);
        auto eq = v.find('=');
        if (eq != std::string_view::npos && t.request.url.starts_with(v.substr(0, eq)))
            t.request.url = std::string(v.substr(eq + 1)) + t.request.url.substr(eq);
    }
    log::debug("http: {} {}", t.request.method, t.request.url);
    // RFU_HTTP_LOG=1: the headers and body too, to study a server's protocol.
    static const bool log_bodies = std::getenv("RFU_HTTP_LOG") != nullptr;
    if (log_bodies) {
        for (const auto& [k, v] : t.request.headers) log::info("http:   {}: {}", k, v);
        if (t.request.has_body) log::info("http:   body ({} bytes): {}", t.request.body.size(), std::string(t.request.body.begin(), t.request.body.end()));
    }
    // RFU_CLOUD_STUB: answer PvZ2 China's cloud hosts locally instead of over the network, so the
    // game's offline profile path runs. Decrypts the request, builds an (encrypted) reply, and hands
    // it back through the same callbacks a real response would use.
    static const bool cloud_stub = std::getenv("RFU_CLOUD_STUB") != nullptr;
    if (cloud_stub && cloud_stub::is_cloud_host(t.request.url)) {
        std::string body(t.request.body.begin(), t.request.body.end());
        std::string req = cloud_stub::field(body, "req");
        std::string e = cloud_stub::field(body, "e");
        std::string req_json;
        if (!req.empty() && !e.empty()) req_json = cloud::decrypt(req, e);
        log::info("cloud stub: {} req={} -> {}", t.request.url, req, req_json.empty() ? "(no payload)" : req_json);

        bool raw = false;
        std::optional<std::string> plain = cloud_stub::response_plain(req, req_json, raw);
        std::string reply;  // the response body bytes to deliver
        if (plain) {
            reply = raw ? *plain : (req.empty() ? *plain : cloud::encrypt(req, *plain));
            log::info("cloud stub: reply to {} ({} bytes{}): {}", req, reply.size(), raw ? ", raw" : "",
                      raw ? std::string("<raw>") : *plain);
        }
        {
            std::lock_guard lock(t.mutex);
            t.response = platform::HttpResponse{};
            t.response.status = 200;
            t.response.status_line = "OK";
            t.response.content_length = static_cast<std::int64_t>(reply.size());
        }
        fire("HttpReceivedResponse");
        if (!reply.empty() && t.native.load()) {
            std::size_t sent = 0;
            while (sent < reply.size() && t.native.load()) {
                std::size_t chunk = std::min<std::size_t>(reply.size() - sent, 16384);
                s.rt->mem().write_bytes(jni.array_data(t.buffer), reply.data() + sent, chunk);
                fire("HttpReceivedData", {t.buffer, chunk});
                sent += chunk;
            }
        }
        fire("HttpTransactionComplete");
        return;
    }
    if (!s.online) {
        fire("HttpTransactionError");
        return;
    }
    platform::HttpHandler handler;
    handler.on_response = [&](const platform::HttpResponse& r) {
        {
            std::lock_guard lock(t.mutex);
            t.response = r;
        }
        log::debug("http: {} -> {} {}", t.request.url, r.status, r.status_line);
        fire("HttpReceivedResponse");
    };
    handler.on_data = [&](const std::uint8_t* data, std::size_t n) {
        while (n && t.native.load()) {
            std::size_t chunk = std::min<std::size_t>(n, 16384);
            s.rt->mem().write_bytes(jni.array_data(t.buffer), data, chunk);
            fire("HttpReceivedData", {t.buffer, chunk});
            data += chunk;
            n -= chunk;
        }
    };
    std::string error;
    if (platform::http_perform(t.request, handler, t.cancel, error)) {
        if (!t.cancel.load()) fire("HttpTransactionComplete");
    } else {
        if (!t.cancel.load()) log::info("http: {} failed: {}", t.request.url, error);
        fire("HttpTransactionError");
    }
}

void worker(HttpState& s) {
    for (;;) {
        std::shared_ptr<Transaction> t;
        {
            std::unique_lock lock(s.mutex);
            s.cv.wait(lock, [&] { return !s.queue.empty(); });
            t = std::move(s.queue.front());
            s.queue.pop_front();
        }
        try {
            run(s, *t);
        } catch (const std::exception& e) {
            log::error("http: {} crashed the game's handler: {}", t->request.url, e.what());
        }
    }
}

}  // namespace

void implement_http(Runtime& rt, bool online, const std::string& transaction_class) {
    auto& s = rt.state<HttpState>();
    s.rt = &rt;
    s.cls = transaction_class;
    s.online = online && platform::http_available();
    if (online && !platform::http_available()) log::warn("rfu has no HTTP client on this platform: the game's own web requests will fail");
    auto& jni = jni::get(rt);
    HttpState* sp = &s;
    auto impl = [&](const char* name, jni::MethodImpl f) { jni.implement(sp->cls, name, std::move(f)); };

    impl("<init>", [sp](jni::JavaCall& j) -> std::uint64_t {
        // (long nativeTransaction, String method, String url)
        auto t = std::make_shared<Transaction>();
        t->self = j.jni.new_instance(sp->cls);
        t->native = j.args.at(0);
        t->request.method = j.string_arg(1);
        t->request.url = j.string_arg(2);
        t->buffer = j.jni.new_array('B', 16384);
        std::lock_guard lock(sp->mutex);
        sp->transactions[t->self] = t;
        return t->self;
    });
    impl("SetRequestHeader", [sp](jni::JavaCall& j) -> std::uint64_t {
        if (auto t = find(*sp, j.self)) t->request.headers.emplace_back(j.string_arg(0), j.string_arg(1));
        return 0;
    });
    impl("SetRequestBody", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        auto body = j.jni.object(j.args.at(0));
        if (!t) return 0;
        t->request.has_body = true;
        t->request.body.clear();
        if (body && body->data) {
            const std::uint8_t* p = sp->rt->mem().host(body->data);
            t->request.body.assign(p, p + body->length);
        }
        return 0;
    });
    impl("GetRequestBody", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t || !t->request.has_body) return 0;
        GuestAddr a = j.jni.new_array('B', t->request.body.size());
        sp->rt->mem().write_bytes(j.jni.array_data(a), t->request.body.data(), t->request.body.size());
        return a;
    });
    impl("SetBasicAuth", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0;
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string in = j.string_arg(0) + ":" + j.string_arg(1), out;
        for (std::size_t i = 0; i < in.size(); i += 3) {
            std::uint32_t v = static_cast<std::uint8_t>(in[i]) << 16;
            if (i + 1 < in.size()) v |= static_cast<std::uint8_t>(in[i + 1]) << 8;
            if (i + 2 < in.size()) v |= static_cast<std::uint8_t>(in[i + 2]);
            out += b64[v >> 18 & 63];
            out += b64[v >> 12 & 63];
            out += i + 1 < in.size() ? b64[v >> 6 & 63] : '=';
            out += i + 2 < in.size() ? b64[v & 63] : '=';
        }
        t->request.headers.emplace_back("Authorization", "Basic " + out);
        return 0;
    });
    impl("GetHumanReadableUrl", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        return j.jni.new_string(t ? t->request.url : "");
    });
    impl("SetTimeout", [sp](jni::JavaCall& j) -> std::uint64_t {
        if (auto t = find(*sp, j.self)) t->request.timeout_seconds = std::max(1, static_cast<std::int32_t>(j.args.at(0)));
        return 0;
    });
    impl("Start", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0;
        std::lock_guard lock(sp->mutex);
        if (sp->workers.empty())
            for (int i = 0; i < kWorkers; ++i) {
                sp->workers.emplace_back(worker, std::ref(*sp));
                sp->workers.back().detach();
            }
        sp->queue.push_back(t);
        sp->cv.notify_one();
        return 0;
    });
    impl("Release", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0;
        std::uint64_t native = t->native.exchange(0);
        t->cancel = true;
        if (native) j.jni.call_native(sp->cls, "HttpTransactionCleanup", j.self, {native});
        std::lock_guard lock(sp->mutex);
        sp->transactions.erase(j.self);
        return 0;
    });
    impl("GetStatusCode", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0;
        std::lock_guard lock(t->mutex);
        return static_cast<std::uint32_t>(t->response.status);
    });
    impl("GetStatusLine", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0;
        std::lock_guard lock(t->mutex);
        return t->response.status ? j.jni.new_string(t->response.status_line) : 0;
    });
    impl("GetResponseLength", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0xFFFFFFFF;
        std::lock_guard lock(t->mutex);
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(t->response.content_length));
    });
    impl("GetResponseHeader", [sp](jni::JavaCall& j) -> std::uint64_t {
        auto t = find(*sp, j.self);
        if (!t) return 0;
        std::lock_guard lock(t->mutex);
        std::string v = t->response.header(j.string_arg(0));
        return v.empty() ? 0 : j.jni.new_string(v);
    });
}

}  // namespace rfu::android
