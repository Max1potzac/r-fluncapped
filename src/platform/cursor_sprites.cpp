#include "platform/cursor_sprites.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>

namespace rfu::platform {

namespace {

#include "platform/cursor_sprites.inc"

const Image& decoded(bool pressed) {
    static const Image images[2] = {
        [] {
            std::string error;
            auto image = decode_png(std::vector<std::uint8_t>(std::begin(kCursorPng), std::end(kCursorPng)), error);
            if (!image) throw std::runtime_error("cursor sprite: " + error);
            return *image;
        }(),
        [] {
            std::string error;
            auto image = decode_png(std::vector<std::uint8_t>(std::begin(kCursorDownPng), std::end(kCursorDownPng)), error);
            if (!image) throw std::runtime_error("cursor sprite: " + error);
            return *image;
        }()};
    return images[pressed ? 1 : 0];
}

}  // namespace

CursorSprite cursor_sprite(bool pressed) {
    // The fingertips: the plain glove points up and left, the pressing one down and left.
    return pressed ? CursorSprite{decoded(true), 1, 29} : CursorSprite{decoded(false), 3, 3};
}

Image scaled_cursor(bool pressed, int height, int& hot_x, int& hot_y) {
    CursorSprite s = cursor_sprite(pressed);
    const Image& src = s.image;
    double k = static_cast<double>(std::max(height, 4)) / cursor_sprite(false).image.height;
    Image out;
    out.width = std::max(1, static_cast<int>(src.width * k + 0.5));
    out.height = std::max(1, static_cast<int>(src.height * k + 0.5));
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4, 0);
    // Each pixel averages the source pixels it covers (colour weighted by alpha).
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            int x0 = static_cast<int>(x / k), x1 = std::max(x0 + 1, static_cast<int>((x + 1) / k));
            int y0 = static_cast<int>(y / k), y1 = std::max(y0 + 1, static_cast<int>((y + 1) / k));
            double r = 0, g = 0, b = 0, a = 0;
            int n = 0;
            for (int sy = y0; sy < std::min(y1, src.height); ++sy)
                for (int sx = x0; sx < std::min(x1, src.width); ++sx) {
                    const std::uint8_t* p = &src.rgba[(static_cast<std::size_t>(sy) * src.width + sx) * 4];
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    ++n;
                }
            std::uint8_t* q = &out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4];
            if (a > 0) {
                q[0] = static_cast<std::uint8_t>(r / a);
                q[1] = static_cast<std::uint8_t>(g / a);
                q[2] = static_cast<std::uint8_t>(b / a);
            }
            q[3] = static_cast<std::uint8_t>(n ? a / n : 0);
        }
    hot_x = static_cast<int>(s.hot_x * k + 0.5);
    hot_y = static_cast<int>(s.hot_y * k + 0.5);
    return out;
}

std::vector<HintFill> layout_cursor(const HintCursor& cursor) {
    // The runs, made once for each size (the cursor's drawn every frame).
    struct Runs {
        std::vector<HintFill> fills;  // relative to the fingertip
    };
    static std::mutex mutex;
    static std::map<std::pair<bool, int>, Runs> cache;
    std::lock_guard lock(mutex);
    auto key = std::pair{cursor.pressed, cursor.height};
    auto it = cache.find(key);
    if (it == cache.end()) {
        int hx = 0, hy = 0;
        Image image = scaled_cursor(cursor.pressed, cursor.height, hx, hy);
        Runs runs;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width;) {
                const std::uint8_t* p = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
                if (p[3] < 128) {
                    ++x;
                    continue;
                }
                // Colours a little apart are one run (fewer clears, the look's the same).
                auto quant = [](const std::uint8_t* c) { return (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | (c[2] >> 3); };
                int start = x, q = quant(p);
                while (x < image.width) {
                    const std::uint8_t* c = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
                    if (c[3] < 128 || quant(c) != q) break;
                    ++x;
                }
                std::uint32_t rgb = static_cast<std::uint32_t>(p[0]) << 16 | p[1] << 8 | p[2];
                runs.fills.push_back({start - hx, y - hy, x - start, 1, rgb});
            }
        it = cache.emplace(key, std::move(runs)).first;
    }
    std::vector<HintFill> out = it->second.fills;
    for (auto& f : out) {
        f.x += cursor.x;
        f.y += cursor.y;
    }
    return out;
}

}  // namespace rfu::platform
