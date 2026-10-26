#include "platform/png.h"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace rfu::platform {

namespace {

std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }

int paeth(int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

std::optional<Image> decode_png(const std::vector<std::uint8_t>& file, std::string& error) {
    static const std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (file.size() < 8 || std::memcmp(file.data(), kSignature, 8) != 0) {
        error = "not a PNG file";
        return std::nullopt;
    }
    std::uint32_t width = 0, height = 0;
    int depth = 0, color = -1, interlace = 0;
    std::vector<std::uint8_t> idat, palette, trns;
    for (std::size_t pos = 8; pos + 12 <= file.size();) {
        std::uint32_t len = be32(&file[pos]);
        if (len > file.size() - pos - 12) break;
        const std::uint8_t* type = &file[pos + 4];
        const std::uint8_t* data = &file[pos + 8];
        if (!std::memcmp(type, "IHDR", 4) && len >= 13) {
            width = be32(data);
            height = be32(data + 4);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(data, data + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(data, data + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), data, data + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    int channels = color == 0 ? 1 : color == 2 ? 3 : color == 3 ? 1 : color == 4 ? 2 : color == 6 ? 4 : 0;
    if (!width || !height || width > 8192 || height > 8192 || !channels) {
        error = "unsupported or missing image header";
        return std::nullopt;
    }
    if (depth != 8 || interlace) {
        error = "only 8-bit, non-interlaced PNGs are read";
        return std::nullopt;
    }

    std::size_t stride = std::size_t(width) * channels;
    std::vector<std::uint8_t> raw((stride + 1) * height);
    uLongf raw_size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &raw_size, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || raw_size != raw.size()) {
        error = "corrupt image data";
        return std::nullopt;
    }

    // Undo each row's filter in place.
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = &raw[y * (stride + 1) + 1];
        const std::uint8_t* prev = y ? row - (stride + 1) : nullptr;
        int filter = row[-1];
        for (std::size_t i = 0; i < stride; ++i) {
            int a = i >= std::size_t(channels) ? row[i - channels] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= std::size_t(channels) ? prev[i - channels] : 0;
            switch (filter) {
            case 0: break;
            case 1: row[i] = static_cast<std::uint8_t>(row[i] + a); break;
            case 2: row[i] = static_cast<std::uint8_t>(row[i] + b); break;
            case 3: row[i] = static_cast<std::uint8_t>(row[i] + (a + b) / 2); break;
            case 4: row[i] = static_cast<std::uint8_t>(row[i] + paeth(a, b, c)); break;
            default: error = "bad row filter"; return std::nullopt;
            }
        }
    }

    Image image;
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.rgba.resize(std::size_t(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = &raw[y * (stride + 1) + 1];
        std::uint8_t* out = &image.rgba[std::size_t(y) * width * 4];
        for (std::uint32_t x = 0; x < width; ++x, out += 4) {
            const std::uint8_t* p = row + std::size_t(x) * channels;
            switch (color) {
            case 0: out[0] = out[1] = out[2] = p[0]; out[3] = 255; break;
            case 4: out[0] = out[1] = out[2] = p[0]; out[3] = p[1]; break;
            case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
            case 6: std::memcpy(out, p, 4); break;
            case 3: {
                std::size_t i = p[0];
                if (i * 3 + 2 < palette.size()) {
                    out[0] = palette[i * 3];
                    out[1] = palette[i * 3 + 1];
                    out[2] = palette[i * 3 + 2];
                } else {
                    out[0] = out[1] = out[2] = 0;
                }
                out[3] = i < trns.size() ? trns[i] : 255;
                break;
            }
            }
        }
    }
    return image;
}

std::optional<Image> decode_png_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open it";
        return std::nullopt;
    }
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode_png(file, error);
}

Image shrink(const Image& image, int max_side) {
    int f = 1;
    while (image.width / f > max_side || image.height / f > max_side) ++f;
    if (f == 1) return image;
    Image out;
    out.width = image.width / f;
    out.height = image.height / f;
    out.rgba.resize(std::size_t(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int ch = 0; ch < 4; ++ch) {
                unsigned sum = 0;
                for (int dy = 0; dy < f; ++dy)
                    for (int dx = 0; dx < f; ++dx) sum += image.rgba[(std::size_t(y * f + dy) * image.width + (x * f + dx)) * 4 + ch];
                out.rgba[(std::size_t(y) * out.width + x) * 4 + ch] = static_cast<std::uint8_t>(sum / (f * f));
            }
    return out;
}

}  // namespace rfu::platform
