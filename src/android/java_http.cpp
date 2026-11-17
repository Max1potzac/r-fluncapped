#include "android/java_http.h"

#include "android/cloud_crypto.h"
#include "core/log.h"
#include "jni/jni.h"
#include "platform/http.h"
#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
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

// Portable byte-substring search: memmem is a GNU/BSD extension absent on MSVC.
inline const std::uint8_t* find_bytes(const std::uint8_t* hay, std::size_t hlen, const void* needle, std::size_t nlen) {
    if (nlen == 0) return hay;
    if (hlen < nlen) return nullptr;
    const auto* n = static_cast<const std::uint8_t*>(needle);
    for (const std::uint8_t* p = hay; p <= hay + (hlen - nlen); ++p)
        if (*p == *n && std::memcmp(p, n, nlen) == 0) return p;
    return nullptr;
}

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
    std::filesystem::path cache_dir;  // images kept from their last good download; empty: none
};

// Images the game fetches (the title screen's banners): kept from their last good download and
// handed back when a later one fails (a slow server, a blocked connection, offline). A good
// download replaces the copy, so a changed banner shows once it gets through.
bool cacheable(const platform::HttpRequest& r) {
    if (r.method != "GET" || r.has_body) return false;
    std::string path = r.url.substr(0, r.url.find_first_of("?#"));
    for (auto& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* ext : {".png", ".jpg", ".jpeg", ".webp"})
        if (path.ends_with(ext)) return true;
    return false;
}

std::filesystem::path cache_path(const HttpState& s, const std::string& url) {
    std::string name;
    std::string_view rest(url);
    if (auto scheme = rest.find("://"); scheme != std::string_view::npos) rest.remove_prefix(scheme + 3);
    for (char c : rest.substr(0, rest.find_first_of("?#"))) name += std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' ? c : '_';
    if (name.size() > 180) name = name.substr(name.size() - 180);
    return s.cache_dir / name;
}

std::optional<std::string> read_cache(const HttpState& s, const std::string& url) {
    std::ifstream in(cache_path(s, url), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_cache(const HttpState& s, const std::string& url, const std::vector<std::uint8_t>& body) {
    std::error_code ec;
    std::filesystem::create_directories(s.cache_dir, ec);
    auto path = cache_path(s, url);
    auto tmp = path;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        if (!out) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

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
        if (!e.empty() && std::getenv("RFU_CLOUD_RAWE")) log::info("cloud stub: rawe req={} e={}", req, e);
        // RFU_SCAN_NEEDLE + RFU_SCAN_REQ: when this request code matches RFU_SCAN_REQ, scan committed
        // guest heap+module memory for the ASCII needle and log every hit address. Used to locate the
        // native "current cloud ui" field at a known moment in the cloud flow.
        // RFU_HOLD_UI=<value>: record every heap address holding an SSO std::string equal to <value>
        // (seen while populated, e.g. at V1270/V202), and on EVERY cloud request rewrite that SSO
        // std::string back into each recorded address. This survives the game's post-V202 "reset
        // session" clear so the later V437 welcome-sync request reads the real ui instead of empty.
        if (const char* holdv = std::getenv("RFU_HOLD_UI")) {
            static std::vector<rfu::GuestAddr> ui_fields;  // object-start of each SSO std::string
            std::string val(holdv);
            if (val.size() <= 22) {
                auto& mem = s.rt->mem();
                std::uint8_t* base = mem.base();
                std::uint8_t sso0 = (std::uint8_t)(val.size() << 1);  // libc++ short: size<<1, low bit 0
                // discover: scan heap for sso0 followed by val followed by 0, record object start
                auto scan_discover = [&](rfu::GuestAddr lo, rfu::GuestAddr hi) {
                    const std::uint64_t PG = 0x1000;
                    for (rfu::GuestAddr a = lo; a + val.size() + 2 < hi; ) {
                        if (!mem.is_committed(a, 1)) { a += PG; continue; }
                        rfu::GuestAddr runEnd = a; while (runEnd < hi && mem.is_committed(runEnd, 1)) runEnd += PG; if (runEnd > hi) runEnd = hi;
                        const std::uint8_t* hp = base + a; std::size_t len = (std::size_t)(runEnd - a);
                        const std::uint8_t* f = hp; const std::uint8_t* end = hp + len;
                        while ((f = (const std::uint8_t*)find_bytes(f, (std::size_t)(end - f), val.data(), val.size())) != nullptr) {
                            rfu::GuestAddr strAddr = a + (rfu::GuestAddr)(f - hp);
                            // require preceding byte == sso0 (so this is an SSO std::string, not a payload substring)
                            if (strAddr >= 1 && mem.is_committed(strAddr - 1, 1) && base[strAddr - 1] == sso0
                                && mem.is_committed(strAddr + val.size(), 1) && base[strAddr + val.size()] == 0) {
                                rfu::GuestAddr obj = strAddr - 1;
                                if (std::find(ui_fields.begin(), ui_fields.end(), obj) == ui_fields.end()) {
                                    ui_fields.push_back(obj);
                                    log::info("hold-ui: recorded field {:#x} (req={})", obj, req);
                                }
                            }
                            f += 1;
                        }
                        a = runEnd;
                    }
                };
                scan_discover(0x4'0000'0000ull, 0x4'3000'0000ull);
                // restore: ONLY into slots that are now an empty libc++ SSO string (byte0==0, data[0]==0)
                // — the exact signature of "this was our ui std::string, since cleared". Never touch a
                // slot whose byte0 differs (it was freed and reused: writing there would corrupt it).
                int restored = 0;
                for (rfu::GuestAddr obj : ui_fields) {
                    if (!mem.is_committed(obj, val.size() + 2)) continue;
                    if (base[obj] == sso0 && std::memcmp(base + obj + 1, val.data(), val.size()) == 0) continue; // already ok
                    // Restore ONLY a slot bearing the exact "cleared-in-place" signature of OUR ui string.
                    // libc++ clear() on a short string zeroes byte0 (size) and byte1 (data[0]/terminator)
                    // but leaves the remaining original chars, so a cleared "123456789" reads
                    // {0x00, 0x00, '2','3','4','5','6','7','8','9', ...} == {0,0}+val[1:]. A freed+reused
                    // long-string header won't match this tail, so we never corrupt one.
                    if (val.size() < 2) continue;
                    if (base[obj] != 0x00 || base[obj + 1] != 0x00) continue;
                    if (std::memcmp(base + obj + 2, val.data() + 1, val.size() - 1) != 0) continue;
                    base[obj] = sso0;
                    std::memcpy(base + obj + 1, val.data(), val.size());
                    base[obj + 1 + val.size()] = 0;
                    restored++;
                    log::info("hold-ui: restored field {:#x} at req={}", obj, req);
                }
                if (restored) log::info("hold-ui: restored {} field(s) at req={}", restored, req);
            }
        }
        if (const char* needle = std::getenv("RFU_SCAN_NEEDLE")) {
            const char* scan_req = std::getenv("RFU_SCAN_REQ");
            if (!scan_req || req == scan_req) {
                std::string n(needle);
                auto& mem = s.rt->mem();
                std::uint8_t* base = mem.base();
                std::uint64_t hits = 0;
                // Scan modules [0x10000000 .. 0x100000000) and heap [0x400000000 .. +committed), page by page.
                auto scan_range = [&](rfu::GuestAddr lo, rfu::GuestAddr hi) {
                    const std::uint64_t PG = 0x1000;
                    for (rfu::GuestAddr a = lo; a + n.size() < hi; ) {
                        if (!mem.is_committed(a, 1)) { a += PG; continue; }
                        // find extent of committed run (cheap: probe forward in pages)
                        rfu::GuestAddr runEnd = a;
                        while (runEnd < hi && mem.is_committed(runEnd, 1)) runEnd += PG;
                        if (runEnd > hi) runEnd = hi;
                        const std::uint8_t* hp = base + a;
                        std::size_t len = (std::size_t)(runEnd - a);
                        const std::uint8_t* found = hp;
                        const std::uint8_t* end = hp + len;
                        while ((found = (const std::uint8_t*)find_bytes(found, (std::size_t)(end - found), n.data(), n.size())) != nullptr) {
                            rfu::GuestAddr ga = a + (rfu::GuestAddr)(found - hp);
                            if (hits < 40) {
                                log::info("scan hit [{}] req={}: {:#x}", n, req, ga);
                                if (std::getenv("RFU_SCAN_DUMP")) {
                                    // dump 32 bytes before and 32 after as hex, to reveal the std::string
                                    // header and the containing struct (adjacent fields / vtable).
                                    rfu::GuestAddr lo2 = ga >= 32 ? ga - 32 : 0;
                                    char buf[96]; std::string hex;
                                    if (mem.is_committed(lo2, 96)) {
                                        mem.read_bytes(lo2, buf, 96);
                                        for (int i = 0; i < 96; i++) { char hx[4]; std::snprintf(hx, 4, "%02x", (unsigned char)buf[i]); hex += hx; hex += (i==31||i==63)?'|':' '; }
                                        log::info("   ctx @{:#x} (hit at +32): {}", lo2, hex);
                                    }
                                }
                            }
                            hits++;
                            found += 1;
                        }
                        a = runEnd;
                    }
                };
                scan_range(0x1000'0000ull, 0x1'0000'0000ull);
                scan_range(0x4'0000'0000ull, 0x4'3000'0000ull);
                log::info("scan [{}] req={}: {} total hits", n, req, hits);
            }
        }

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
    // RFU_CDN_DIR: serve non-cloud GETs (the offline sync's resource/version checks — res_shipping,
    // level_shipping hotupdate md5, version files) from local files so the sync can complete without
    // real egress. The served path is the URL path under the dir; a missing file falls through to the
    // normal offline error unless RFU_CDN_EMPTY is set, in which case an empty 200 is returned.
    static const char* cdn_dir = std::getenv("RFU_CDN_DIR");
    if (cdn_dir && std::getenv("RFU_CLOUD_STUB") != nullptr && !cloud_stub::is_cloud_host(t.request.url)) {
        std::string path;
        auto scheme = t.request.url.find("://");
        if (scheme != std::string::npos) {
            auto slash = t.request.url.find('/', scheme + 3);
            if (slash != std::string::npos) path = t.request.url.substr(slash + 1);
        }
        auto q = path.find('?');
        if (q != std::string::npos) path = path.substr(0, q);
        std::string reply;
        bool have = false;
        if (!path.empty()) {
            std::filesystem::path f = std::filesystem::path(cdn_dir) / path;
            std::error_code ec;
            if (std::filesystem::is_regular_file(f, ec)) {
                std::ifstream in(f, std::ios::binary);
                std::ostringstream ss; ss << in.rdbuf();
                reply = ss.str();
                have = true;
            }
        }
        if (have || std::getenv("RFU_CDN_EMPTY")) {
            log::info("cdn stub: {} -> {} ({} bytes)", t.request.url, have ? "file" : "empty200", reply.size());
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
    }
    // A whole response at once, through the same callbacks a streamed one uses.
    auto deliver = [&](const platform::HttpResponse& r, const std::uint8_t* data, std::size_t size) {
        {
            std::lock_guard lock(t.mutex);
            t.response = r;
        }
        fire("HttpReceivedResponse");
        for (std::size_t sent = 0; sent < size && t.native.load();) {
            std::size_t chunk = std::min<std::size_t>(size - sent, 16384);
            s.rt->mem().write_bytes(jni.array_data(t.buffer), data + sent, chunk);
            fire("HttpReceivedData", {t.buffer, chunk});
            sent += chunk;
        }
        fire("HttpTransactionComplete");
    };
    auto from_cache = [&]() {
        auto body = read_cache(s, t.request.url);
        if (!body) return false;
        platform::HttpResponse r;
        r.status = 200;
        r.status_line = "OK";
        r.content_length = static_cast<std::int64_t>(body->size());
        r.headers.emplace_back("Content-Length", std::to_string(body->size()));
        deliver(r, reinterpret_cast<const std::uint8_t*>(body->data()), body->size());
        return true;
    };
    bool cache = !s.cache_dir.empty() && cacheable(t.request);
    if (!s.online) {
        if (cache && from_cache()) return;
        fire("HttpTransactionError");
        return;
    }
    if (cache && from_cache()) {
        // The kept copy at once (the game gives up on a slow server before the download does),
        // then the download in the background, for the next launch.
        std::thread([&s, request = t.request] {
            platform::HttpResponse response;
            std::vector<std::uint8_t> body;
            platform::HttpHandler buffered;
            buffered.on_response = [&](const platform::HttpResponse& r) { response = r; };
            buffered.on_data = [&](const std::uint8_t* data, std::size_t n) { body.insert(body.end(), data, data + n); };
            std::atomic<bool> cancel{false};
            std::string error;
            if (platform::http_perform(request, buffered, cancel, error) && response.status == 200 &&
                (response.content_length < 0 || static_cast<std::size_t>(response.content_length) == body.size()))
                write_cache(s, request.url, body);
            else
                log::debug("http: {}: no new copy ({})", request.url, error.empty() ? "HTTP " + std::to_string(response.status) : error);
        }).detach();
        return;
    }
    if (cache) {
        // Downloaded whole before the game sees any of it, so a failure part way can still be
        // answered with the kept copy.
        platform::HttpResponse response;
        std::vector<std::uint8_t> body;
        platform::HttpHandler buffered;
        buffered.on_response = [&](const platform::HttpResponse& r) { response = r; };
        buffered.on_data = [&](const std::uint8_t* data, std::size_t n) { body.insert(body.end(), data, data + n); };
        std::string error;
        bool ok = platform::http_perform(t.request, buffered, t.cancel, error);
        if (t.cancel.load()) return;
        if (ok && response.status == 200 && (response.content_length < 0 || static_cast<std::size_t>(response.content_length) == body.size())) {
            write_cache(s, t.request.url, body);
            deliver(response, body.data(), body.size());
            return;
        }
        if (from_cache()) {
            log::info("http: {} {}; using the copy from its last download", t.request.url, ok ? "answered " + std::to_string(response.status) : "failed: " + error);
            return;
        }
        if (ok) {
            deliver(response, body.data(), body.size());
        } else {
            log::info("http: {} failed: {}", t.request.url, error);
            fire("HttpTransactionError");
        }
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

void implement_http(Runtime& rt, bool online, const std::string& transaction_class, const std::filesystem::path& cache_dir) {
    auto& s = rt.state<HttpState>();
    s.cache_dir = cache_dir;
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
