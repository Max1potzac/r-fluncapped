#pragma once

// Self-contained MD5 and AES-256-CBC for PvZ2 China's cloud protocol (the ditwan "cloudpvz2android"
// servers). The game POSTs multipart/form-data with fields `req` (an endpoint code), `e` (the body),
// and `ev` ("3"). `e` is base64url(AES-256-CBC(JSON)); the AES key is the 32 lowercase hex chars of
// MD5("1geh6fvq4r20M02s" + req) taken as ASCII, and the IV is the first 16 hex chars of MD5(key).
// Responses use the same scheme. No external crypto library, so this stays dependency-free.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rfu::android::cloud {

// ---- MD5 (RFC 1321), returns the 16 raw bytes.
inline std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t len) {
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    std::vector<std::uint8_t> msg(data, data + len);
    std::uint64_t bits = static_cast<std::uint64_t>(len) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    auto rol = [](std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = msg[off + i * 4] | (msg[off + i * 4 + 1] << 8) | (msg[off + i * 4 + 2] << 16) |
                   (static_cast<std::uint32_t>(msg[off + i * 4 + 3]) << 24);
        std::uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t F;
            int g;
            if (i < 16) { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
            else { F = C ^ (B | ~D); g = (7 * i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + rol(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
    std::array<std::uint8_t, 16> out{};
    std::uint32_t h[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (8 * j));
    return out;
}

inline std::string md5_hex(const std::string& s) {
    auto d = md5(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    static const char* hx = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) { out[i * 2] = hx[d[i] >> 4]; out[i * 2 + 1] = hx[d[i] & 15]; }
    return out;
}

// ---- AES-256 (CBC). Compact table-free implementation.
namespace detail {
inline const std::uint8_t* sbox() {
    static std::uint8_t s[256], inited = 0;
    static std::uint8_t inv[256];
    if (!inited) {
        std::uint8_t p = 1, q = 1;
        do {
            p = p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0);
            q ^= q << 1; q ^= q << 2; q ^= q << 4; if (q & 0x80) q ^= 0x09;
            std::uint8_t x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)) ^ 0x63;
            s[p] = x;
        } while (p != 1);
        s[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[s[i]] = static_cast<std::uint8_t>(i);
        inited = 1;
        (void)inv;
    }
    return s;
}
inline std::uint8_t xtime(std::uint8_t x) { return static_cast<std::uint8_t>((x << 1) ^ ((x >> 7) * 0x1B)); }
inline std::uint8_t mul(std::uint8_t a, std::uint8_t b) {
    std::uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; }
    return r;
}
struct Aes256 {
    std::uint8_t rk[240];
    const std::uint8_t* S;
    std::uint8_t ISbox[256];
    explicit Aes256(const std::uint8_t key[32]) {
        S = sbox();
        for (int i = 0; i < 256; ++i) ISbox[S[i]] = static_cast<std::uint8_t>(i);
        std::memcpy(rk, key, 32);
        std::uint8_t rcon = 1;
        for (int i = 8; i < 60; ++i) {
            std::uint8_t t[4];
            std::memcpy(t, rk + (i - 1) * 4, 4);
            if (i % 8 == 0) {
                std::uint8_t tmp = t[0]; t[0] = t[1]; t[1] = t[2]; t[2] = t[3]; t[3] = tmp;
                for (int j = 0; j < 4; ++j) t[j] = S[t[j]];
                t[0] ^= rcon; rcon = xtime(rcon);
            } else if (i % 8 == 4) {
                for (int j = 0; j < 4; ++j) t[j] = S[t[j]];
            }
            for (int j = 0; j < 4; ++j) rk[i * 4 + j] = rk[(i - 8) * 4 + j] ^ t[j];
        }
    }
    void encrypt_block(std::uint8_t st[16]) const {
        auto addrk = [&](int r) { for (int i = 0; i < 16; ++i) st[i] ^= rk[r * 16 + i]; };
        addrk(0);
        for (int round = 1; round <= 14; ++round) {
            for (int i = 0; i < 16; ++i) st[i] = S[st[i]];
            std::uint8_t t;
            t = st[1]; st[1] = st[5]; st[5] = st[9]; st[9] = st[13]; st[13] = t;
            t = st[2]; st[2] = st[10]; st[10] = t; t = st[6]; st[6] = st[14]; st[14] = t;
            t = st[15]; st[15] = st[11]; st[11] = st[7]; st[7] = st[3]; st[3] = t;
            if (round != 14)
                for (int c = 0; c < 4; ++c) {
                    std::uint8_t* col = st + c * 4;
                    std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = static_cast<std::uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
                    col[1] = static_cast<std::uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
                    col[2] = static_cast<std::uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
                    col[3] = static_cast<std::uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
                }
            addrk(round);
        }
    }
    void decrypt_block(std::uint8_t st[16]) const {
        auto addrk = [&](int r) { for (int i = 0; i < 16; ++i) st[i] ^= rk[r * 16 + i]; };
        addrk(14);
        for (int round = 13; round >= 0; --round) {
            std::uint8_t t;
            t = st[13]; st[13] = st[9]; st[9] = st[5]; st[5] = st[1]; st[1] = t;
            t = st[2]; st[2] = st[10]; st[10] = t; t = st[6]; st[6] = st[14]; st[14] = t;
            t = st[3]; st[3] = st[7]; st[7] = st[11]; st[11] = st[15]; st[15] = t;
            for (int i = 0; i < 16; ++i) st[i] = ISbox[st[i]];
            addrk(round);
            if (round != 0)
                for (int c = 0; c < 4; ++c) {
                    std::uint8_t* col = st + c * 4;
                    std::uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = static_cast<std::uint8_t>(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                    col[1] = static_cast<std::uint8_t>(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                    col[2] = static_cast<std::uint8_t>(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                    col[3] = static_cast<std::uint8_t>(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
                }
        }
    }
};
}  // namespace detail

// ---- base64url (no padding), matching the game.
inline std::string b64url_encode(const std::uint8_t* d, std::size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        std::uint32_t v = d[i] << 16;
        if (i + 1 < n) v |= d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        out += t[v >> 18 & 63];
        out += t[v >> 12 & 63];
        if (i + 1 < n) out += t[v >> 6 & 63];
        if (i + 2 < n) out += t[v & 63];
    }
    return out;
}
inline std::vector<std::uint8_t> b64url_decode(std::string_view s) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::vector<std::uint8_t> out;
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : s) {
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<std::uint8_t>(buf >> bits)); }
    }
    return out;
}

// The AES key (32 ascii hex bytes) and IV (16 ascii hex bytes) for an endpoint code.
struct KeyIv {
    std::string key;  // 32 bytes
    std::string iv;   // 16 bytes
};
inline KeyIv key_iv(const std::string& req) {
    std::string key = md5_hex("1geh6fvq4r20M02s" + req);  // 32 hex chars
    std::string iv = md5_hex(key).substr(0, 16);
    return {key, iv};
}

// Decrypt the `e` field (base64url) for an endpoint. Returns the plaintext JSON (PKCS7 stripped).
inline std::string decrypt(const std::string& req, std::string_view e) {
    auto ki = key_iv(req);
    auto ct = b64url_decode(e);
    if (ct.empty() || ct.size() % 16 != 0) return {};
    detail::Aes256 aes(reinterpret_cast<const std::uint8_t*>(ki.key.data()));
    std::uint8_t prev[16];
    std::memcpy(prev, ki.iv.data(), 16);
    std::string out;
    for (std::size_t off = 0; off < ct.size(); off += 16) {
        std::uint8_t block[16];
        std::memcpy(block, ct.data() + off, 16);
        std::uint8_t cipher[16];
        std::memcpy(cipher, block, 16);
        aes.decrypt_block(block);
        for (int i = 0; i < 16; ++i) block[i] ^= prev[i];
        std::memcpy(prev, cipher, 16);
        out.append(reinterpret_cast<char*>(block), 16);
    }
    if (!out.empty()) {
        std::uint8_t pad = static_cast<std::uint8_t>(out.back());
        if (pad >= 1 && pad <= 16 && pad <= out.size()) out.resize(out.size() - pad);
    }
    return out;
}

// Encrypt a plaintext JSON response for an endpoint, returning the base64url `e` value.
inline std::string encrypt(const std::string& req, std::string_view plain) {
    auto ki = key_iv(req);
    detail::Aes256 aes(reinterpret_cast<const std::uint8_t*>(ki.key.data()));
    std::vector<std::uint8_t> buf(plain.begin(), plain.end());
    std::uint8_t pad = static_cast<std::uint8_t>(16 - (buf.size() % 16));
    buf.insert(buf.end(), pad, pad);
    std::uint8_t prev[16];
    std::memcpy(prev, ki.iv.data(), 16);
    for (std::size_t off = 0; off < buf.size(); off += 16) {
        for (int i = 0; i < 16; ++i) buf[off + i] ^= prev[i];
        aes.encrypt_block(buf.data() + off);
        std::memcpy(prev, buf.data() + off, 16);
    }
    return b64url_encode(buf.data(), buf.size());
}

}  // namespace rfu::android::cloud
