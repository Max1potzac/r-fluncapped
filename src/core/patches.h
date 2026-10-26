#pragma once

// Code patches: small fixes written as bytes to put at offsets of a loaded library, the way
// RFLib patches libPVZ2. A patch file (--patch) has one change per line:
//
//   <library>+<offset> <bytes> [was <bytes>]
//
// e.g. "libPVZ2.so+0x1a2b3c 1F 20 03 D5 was 00 00 80 52". The offset is from the library's load
// address (what a disassembler shows for the .so), bytes are hex, spaces optional. "was" gives
// the bytes expected there: when any of them don't match (a different build of the game) the
// whole file is left out. '#' starts a comment; "# name:" and "# description:" lines are what
// the launcher shows.

#include <cstdint>
#include <string>
#include <vector>

namespace rfu {

class Runtime;

struct CodePatch {
    std::string module;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> expected;  // empty: not checked
};

std::vector<CodePatch> parse_patches(const std::string& text, std::vector<std::string>& errors);

// Applies every patch or none of them; returns why not, or an empty string.
std::string apply_patches(Runtime& rt, const std::vector<CodePatch>& patches);

}  // namespace rfu
