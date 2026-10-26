#include "core/patches.h"

#include "runtime.h"

#include <fmt/format.h>

#include <cstring>
#include <sstream>

namespace rfu {

namespace {

// "1F 20 03 D5" or "1F2003D5" (the rest of the line, up to "was").
bool parse_hex(const std::vector<std::string>& words, std::size_t& i, std::vector<std::uint8_t>& out) {
    std::string digits;
    for (; i < words.size() && words[i] != "was"; ++i) digits += words[i];
    if (digits.empty() || digits.size() % 2) return false;
    for (std::size_t j = 0; j < digits.size(); j += 2) {
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        int hi = hex(digits[j]), lo = hex(digits[j + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
    }
    return true;
}

}  // namespace

std::vector<CodePatch> parse_patches(const std::string& text, std::vector<std::string>& errors) {
    std::vector<CodePatch> patches;
    std::istringstream lines(text);
    std::string line;
    for (int n = 1; std::getline(lines, line); ++n) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream in(line);
        std::vector<std::string> words;
        for (std::string w; in >> w;) words.push_back(w);
        if (words.empty()) continue;
        auto fail = [&](const std::string& why) { errors.push_back(fmt::format("line {}: {}", n, why)); };
        CodePatch p;
        auto plus = words[0].find('+');
        if (plus == std::string::npos || plus == 0) {
            fail("expected <library>+<offset>, e.g. libPVZ2.so+0x1a2b3c");
            continue;
        }
        p.module = words[0].substr(0, plus);
        try {
            std::size_t used = 0;
            std::string off = words[0].substr(plus + 1);
            p.offset = std::stoull(off, &used, 16);
            if (used != off.size()) throw std::invalid_argument(off);
        } catch (const std::exception&) {
            fail("bad offset in " + words[0]);
            continue;
        }
        std::size_t i = 1;
        if (!parse_hex(words, i, p.bytes)) {
            fail("expected hex bytes after " + words[0]);
            continue;
        }
        if (i < words.size()) {
            ++i;  // "was"
            if (!parse_hex(words, i, p.expected) || p.expected.size() != p.bytes.size()) {
                fail("\"was\" wants as many hex bytes as the patch has");
                continue;
            }
        }
        patches.push_back(std::move(p));
    }
    return patches;
}

std::string apply_patches(Runtime& rt, const std::vector<CodePatch>& patches) {
    // Check everything first, so a patch for another build changes nothing.
    std::vector<GuestAddr> at;
    for (const auto& p : patches) {
        const Module* m = rt.linker().find_module(p.module);
        if (!m) return p.module + " isn't loaded";
        GuestAddr addr = m->base() + p.offset;
        bool inside = false;
        for (const auto& seg : m->segments())
            if (addr >= seg.start && addr + p.bytes.size() <= seg.start + seg.size) inside = true;
        if (!inside) return fmt::format("{}+{:#x} is outside the library", p.module, p.offset);
        if (!p.expected.empty() && std::memcmp(rt.mem().host(addr), p.expected.data(), p.expected.size()) != 0)
            return fmt::format("{}+{:#x} doesn't hold the bytes the patch expects (a different build of the game?)", p.module, p.offset);
        at.push_back(addr);
    }
    for (std::size_t i = 0; i < patches.size(); ++i) {
        std::memcpy(rt.mem().host(at[i]), patches[i].bytes.data(), patches[i].bytes.size());
        rt.invalidate_code(at[i], patches[i].bytes.size());
    }
    return {};
}

}  // namespace rfu
