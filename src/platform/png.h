#pragma once

// A small PNG reader, for the game's launcher icon (taken out of its APK by pvz2conv): 8-bit
// greyscale, grey + alpha, RGB, RGBA and palette images, not interlaced.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rfu::platform {

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4 bytes, top row first
};

std::optional<Image> decode_png(const std::vector<std::uint8_t>& file, std::string& error);
std::optional<Image> decode_png_file(const std::filesystem::path& path, std::string& error);

// Scales down by a whole factor (averaging each block) until neither side is over max_side.
Image shrink(const Image& image, int max_side);

}  // namespace rfu::platform
