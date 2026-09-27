// libcworks — shared foundation of the CWorks suite
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cworks/base64.hpp"

#include <cstdint>

namespace cworks {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value_of(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

} // namespace

std::string encode_base64(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t at = 0;
    for (; at + 2 < bytes.size(); at += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8) |
                                static_cast<unsigned char>(bytes[at + 2]);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
    }
    if (at < bytes.size()) {
        const bool two = at + 1 < bytes.size();
        const std::uint32_t v =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 16) |
            (two ? static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8 : 0u);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += two ? kAlphabet[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

std::optional<std::string> decode_base64(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t at = 0; at < text.size(); at += 4) {
        const bool last = at + 4 == text.size();
        int values[4];
        std::size_t padding = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text[at + i];
            if (c == '=') {
                // Padding ends the last quantum only, after at least two
                // characters, and nothing but padding follows it.
                if (!last || i < 2) return std::nullopt;
                ++padding;
                values[i] = 0;
                continue;
            }
            if (padding != 0) return std::nullopt;
            values[i] = value_of(c);
            if (values[i] < 0) return std::nullopt;
        }
        const std::uint32_t v = (static_cast<std::uint32_t>(values[0]) << 18) |
                                (static_cast<std::uint32_t>(values[1]) << 12) |
                                (static_cast<std::uint32_t>(values[2]) << 6) |
                                static_cast<std::uint32_t>(values[3]);
        // The bits past the encoded bytes must be zero: one encoding per
        // byte string.
        if ((padding == 1 && (v & 0xFF) != 0) || (padding == 2 && (v & 0xFFFF) != 0))
            return std::nullopt;
        out += static_cast<char>((v >> 16) & 0xFF);
        if (padding < 2) out += static_cast<char>((v >> 8) & 0xFF);
        if (padding < 1) out += static_cast<char>(v & 0xFF);
    }
    return out;
}

} // namespace cworks
