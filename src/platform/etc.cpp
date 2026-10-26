#include "platform/etc.h"

#include <array>

namespace rfu::platform {

namespace {

// ETC1 / ETC2 / EAC block decoding as defined in the Khronos data format spec (the same
// code as tools/pvz2conv/Ptx.cs). A block is a 64-bit big-endian word; pixel (x, y) uses
// index bit x*4 + y.

constexpr int kEtc1Mod[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
constexpr int kDistance[8] = {3, 6, 11, 16, 23, 32, 41, 64};
constexpr int kEacMod[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10}, {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},  {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8},
};

using Block = std::array<std::uint8_t, 64>;  // 16 RGBA pixels, row by row

int bits(std::uint64_t v, int high, int count) { return static_cast<int>((v >> (high - count + 1)) & ((1ull << count) - 1)); }
std::uint8_t clamp(int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
int x4(int v) { return v * 17; }
int x5(int v) { return (v << 3) | (v >> 2); }
int x6(int v) { return (v << 2) | (v >> 4); }
int x7(int v) { return (v << 1) | (v >> 6); }
int signed3(int v) { return v >= 4 ? v - 8 : v; }

int pixel_index(std::uint64_t b, int x, int y) {
    int i = x * 4 + y;
    return static_cast<int>((((b >> (16 + i)) & 1) << 1) | ((b >> i) & 1));
}

void set(Block& block, int x, int y, int r, int g, int bl) {
    int o = (y * 4 + x) * 4;
    block[o] = clamp(r);
    block[o + 1] = clamp(g);
    block[o + 2] = clamp(bl);
    block[o + 3] = 255;
}

void paint(std::uint64_t b, Block& block, const std::array<int, 12>& colors) {
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int p = pixel_index(b, x, y) * 3;
            set(block, x, y, colors[p], colors[p + 1], colors[p + 2]);
        }
}

void decode_t(std::uint64_t b, Block& block) {
    int r1 = x4(bits(b, 60, 2) << 2 | bits(b, 57, 2)), g1 = x4(bits(b, 55, 4)), b1 = x4(bits(b, 51, 4));
    int r2 = x4(bits(b, 47, 4)), g2 = x4(bits(b, 43, 4)), b2 = x4(bits(b, 39, 4));
    int d = kDistance[bits(b, 35, 2) << 1 | bits(b, 32, 1)];
    paint(b, block, {r1, g1, b1, r2 + d, g2 + d, b2 + d, r2, g2, b2, r2 - d, g2 - d, b2 - d});
}

void decode_h(std::uint64_t b, Block& block) {
    int r1 = bits(b, 62, 4), g1 = bits(b, 58, 3) << 1 | bits(b, 52, 1), b1 = bits(b, 51, 1) << 3 | bits(b, 49, 3);
    int r2 = bits(b, 46, 4), g2 = bits(b, 42, 4), b2 = bits(b, 38, 4);
    int di = bits(b, 34, 1) << 2 | bits(b, 32, 1) << 1 | ((r1 << 8 | g1 << 4 | b1) >= (r2 << 8 | g2 << 4 | b2) ? 1 : 0);
    int d = kDistance[di];
    r1 = x4(r1), g1 = x4(g1), b1 = x4(b1), r2 = x4(r2), g2 = x4(g2), b2 = x4(b2);
    paint(b, block, {r1 + d, g1 + d, b1 + d, r1 - d, g1 - d, b1 - d, r2 + d, g2 + d, b2 + d, r2 - d, g2 - d, b2 - d});
}

void decode_planar(std::uint64_t b, Block& block) {
    int ro = x6(bits(b, 62, 6)), go = x7(bits(b, 56, 1) << 6 | bits(b, 54, 6));
    int bo = x6(bits(b, 48, 1) << 5 | bits(b, 44, 2) << 3 | bits(b, 41, 3));
    int rh = x6(bits(b, 38, 5) << 1 | bits(b, 32, 1)), gh = x7(bits(b, 31, 7)), bh = x6(bits(b, 24, 6));
    int rv = x6(bits(b, 18, 6)), gv = x7(bits(b, 12, 7)), bv = x6(bits(b, 5, 6));
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            set(block, x, y, (x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2, (x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2,
                (x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2);
}

void decode_color(std::uint64_t b, Block& block, bool allow_etc2) {
    bool diff = (b >> 33 & 1) != 0;
    int r1, g1, b1, r2, g2, b2;
    if (!diff) {
        r1 = x4(bits(b, 63, 4)), r2 = x4(bits(b, 59, 4));
        g1 = x4(bits(b, 55, 4)), g2 = x4(bits(b, 51, 4));
        b1 = x4(bits(b, 47, 4)), b2 = x4(bits(b, 43, 4));
    } else {
        int r = bits(b, 63, 5), g = bits(b, 55, 5), bb = bits(b, 47, 5);
        int rd = r + signed3(bits(b, 58, 3)), gd = g + signed3(bits(b, 50, 3)), bd = bb + signed3(bits(b, 42, 3));
        if (allow_etc2 && (rd < 0 || rd > 31)) return decode_t(b, block);
        if (allow_etc2 && (gd < 0 || gd > 31)) return decode_h(b, block);
        if (allow_etc2 && (bd < 0 || bd > 31)) return decode_planar(b, block);
        r1 = x5(r), g1 = x5(g), b1 = x5(bb);
        r2 = x5(rd & 31), g2 = x5(gd & 31), b2 = x5(bd & 31);
    }
    bool flip = (b >> 32 & 1) != 0;
    int t1 = bits(b, 39, 3), t2 = bits(b, 36, 3);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            bool second = flip ? y >= 2 : x >= 2;
            int idx = pixel_index(b, x, y);
            int m = kEtc1Mod[second ? t2 : t1][idx & 1];
            if (idx & 2) m = -m;
            if (second) set(block, x, y, r2 + m, g2 + m, b2 + m);
            else set(block, x, y, r1 + m, g1 + m, b1 + m);
        }
}

void decode_alpha(std::uint64_t b, Block& block) {
    int base = static_cast<int>(b >> 56), mult = static_cast<int>(b >> 52 & 15), table = static_cast<int>(b >> 48 & 15);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            int i = x * 4 + y;
            int idx = static_cast<int>(b >> (45 - 3 * i) & 7);
            block[(y * 4 + x) * 4 + 3] = clamp(base + kEacMod[table][idx] * mult);
        }
}

std::uint64_t read_be64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}

}  // namespace

bool is_etc_format(std::uint32_t f) { return f == kGL_ETC1_RGB8_OES || f == kGL_COMPRESSED_RGB8_ETC2 || f == kGL_COMPRESSED_RGBA8_ETC2_EAC; }

std::vector<std::uint8_t> decode_etc(std::uint32_t gl_format, std::span<const std::uint8_t> data, int width, int height) {
    if (width <= 0 || height <= 0) return {};
    bool rgba = gl_format == kGL_COMPRESSED_RGBA8_ETC2_EAC;
    int bx_count = (width + 3) / 4, by_count = (height + 3) / 4;
    std::size_t block_bytes = rgba ? 16 : 8;
    if (data.size() < block_bytes * bx_count * by_count) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
    Block block;
    for (int by = 0; by < by_count; ++by)
        for (int bx = 0; bx < bx_count; ++bx) {
            const std::uint8_t* p = data.data() + block_bytes * (static_cast<std::size_t>(by) * bx_count + bx);
            if (rgba) {
                decode_color(read_be64(p + 8), block, true);
                decode_alpha(read_be64(p), block);
            } else {
                decode_color(read_be64(p), block, gl_format != kGL_ETC1_RGB8_OES);
            }
            for (int y = 0; y < 4 && by * 4 + y < height; ++y)
                for (int x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    std::size_t d = (static_cast<std::size_t>(by * 4 + y) * width + bx * 4 + x) * 4;
                    for (int k = 0; k < 4; ++k) out[d + k] = block[(y * 4 + x) * 4 + k];
                }
        }
    return out;
}

}  // namespace rfu::platform
