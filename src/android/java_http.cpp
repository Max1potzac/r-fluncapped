#include "android/java_http.h"

#include "core/log.h"
#include "jni/jni.h"
#include "platform/http.h"
#include "runtime.h"

#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <thread>

namespace rfu::android {

namespace {

constexpr const char* kTransaction = "com/popcap/SexyAppFramework/AndroidHttpTransaction";
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

// AndroidHttpTransaction.run(), on a worker thread.
void run(HttpState& s, Transaction& t) {
    auto& jni = jni::get(*s.rt);
    auto fire = [&](const char* name, std::vector<std::uint64_t> extra = {}) {
        std::uint64_t native = t.native.load();
        if (!native) return;
        std::vector<std::uint64_t> args{native};
        args.insert(args.end(), extra.begin(), extra.end());
        jni.call_native(kTransaction, name, t.self, args);
    };
    // For testing against a local copy of a server: RFU_URL_REWRITE=<prefix>=<replacement>.
    if (const char* rw = std::getenv("RFU_URL_REWRITE")) {
        std::string_view v(rw);
        auto eq = v.find('=');
        if (eq != std::string_view::npos && t.request.url.starts_with(v.substr(0, eq)))
            t.request.url = std::string(v.substr(eq + 1)) + t.request.url.substr(eq);
    }
    log::debug("http: {} {}", t.request.method, t.request.url);
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

void implement_http(Runtime& rt, bool online) {
    auto& s = rt.state<HttpState>();
    s.rt = &rt;
    s.online = online && platform::http_available();
    if (online && !platform::http_available()) log::warn("rfu has no HTTP client on this platform: the game's own web requests will fail");
    auto& jni = jni::get(rt);
    auto impl = [&](const char* name, jni::MethodImpl f) { jni.implement(kTransaction, name, std::move(f)); };
    HttpState* sp = &s;

    impl("<init>", [sp](jni::JavaCall& j) -> std::uint64_t {
        // (long nativeTransaction, String method, String url)
        auto t = std::make_shared<Transaction>();
        t->self = j.jni.new_instance(kTransaction);
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
        if (native) j.jni.call_native(kTransaction, "HttpTransactionCleanup", j.self, {native});
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
