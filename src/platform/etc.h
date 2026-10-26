#pragma once

// Software decoding of the ETC texture formats the game's PTX textures use, for GL drivers
// that don't take them as compressed textures (most desktop drivers).

#include <cstdint>
#include <span>
#include <vector>

namespace rfu::platform {

// GL internal formats.
constexpr std::uint32_t kGL_ETC1_RGB8_OES = 0x8D64;
constexpr std::uint32_t kGL_COMPRESSED_RGB8_ETC2 = 0x9274;
constexpr std::uint32_t kGL_COMPRESSED_RGBA8_ETC2_EAC = 0x9278;

// True for a format decode_etc understands.
bool is_etc_format(std::uint32_t gl_format);

// Decodes a width x height ETC1 / ETC2 RGB / ETC2 RGBA8 image to RGBA8 rows. Returns an
// empty vector when `data` is too short.
std::vector<std::uint8_t> decode_etc(std::uint32_t gl_format, std::span<const std::uint8_t> data, int width, int height);

}  // namespace rfu::platform
