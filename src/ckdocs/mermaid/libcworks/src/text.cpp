// libcworks — shared utilities for CK Office
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
#include "cworks/text.hpp"

#include <algorithm>

namespace cworks {

bool valid_utf8(std::string_view text) noexcept {
    const auto continuation = [](unsigned char byte) {
        return (byte & 0xC0U) == 0x80U;
    };
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first =
            static_cast<unsigned char>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1 >= text.size() ||
                !continuation(
                    static_cast<unsigned char>(text[index + 1])))
                return false;
            index += 2;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2 >= text.size()) return false;
            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            const unsigned char third =
                static_cast<unsigned char>(text[index + 2]);
            if (!continuation(third)) return false;
            if (first == 0xE0U) {
                if (second < 0xA0U || second > 0xBFU) return false;
            } else if (first == 0xEDU) {
                if (second < 0x80U || second > 0x9FU) return false;
            } else if (!continuation(second)) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3 >= text.size()) return false;
            const unsigned char second =
                static_cast<unsigned char>(text[index + 1]);
            const unsigned char third =
                static_cast<unsigned char>(text[index + 2]);
            const unsigned char fourth =
                static_cast<unsigned char>(text[index + 3]);
            if (!continuation(third) || !continuation(fourth))
                return false;
            if (first == 0xF0U) {
                if (second < 0x90U || second > 0xBFU) return false;
            } else if (first == 0xF4U) {
                if (second < 0x80U || second > 0x8FU) return false;
            } else if (!continuation(second)) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

std::string closest_match(const std::string& needle,
                          const std::vector<std::string>& candidates) {
    // Two DP rows, reused for every candidate: a suggestion is computed on
    // an error path that may run once per offending cell of a large sheet.
    std::vector<std::size_t> prev, cur;
    const auto distance = [&](const std::string& b) {
        prev.resize(b.size() + 1);
        cur.resize(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
        for (std::size_t i = 1; i <= needle.size(); ++i) {
            cur[0] = i;
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const std::size_t cost = needle[i - 1] == b[j - 1] ? 0 : 1;
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, cur);
        }
        return prev[b.size()];
    };
    const auto common_prefix = [&](const std::string& b) {
        std::size_t n = 0;
        while (n < needle.size() && n < b.size() && needle[n] == b[n]) ++n;
        return n;
    };
    const std::size_t threshold = std::max<std::size_t>(2, needle.size() / 3);
    const std::string* best = nullptr;
    std::size_t best_distance = threshold;
    std::size_t best_prefix = 0;
    for (const auto& candidate : candidates) {
        // The edit distance is at least the length difference, so a
        // candidate that cannot reach the best so far is skipped unmeasured.
        const std::size_t gap = candidate.size() > needle.size()
                                    ? candidate.size() - needle.size()
                                    : needle.size() - candidate.size();
        if (gap > best_distance) continue;
        const std::size_t d = distance(candidate);
        if (d > best_distance) continue;
        // Equally distant candidates are told apart by how much of the
        // needle's start they keep: a typo rarely hits the first letters,
        // so `sn` suggests `sin` rather than `ln`. A remaining tie keeps
        // the earlier candidate, so the answer depends on nothing but the
        // arguments.
        const std::size_t prefix = common_prefix(candidate);
        if (!best || d < best_distance || prefix > best_prefix) {
            best = &candidate;
            best_distance = d;
            best_prefix = prefix;
        }
    }
    return best ? *best : std::string{};
}

std::string trim(const std::string& text) {
    std::size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) --b;
    return text.substr(a, b - a);
}

std::size_t code_points(std::string_view text) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
    }
    return count;
}

std::string sanitize_terminal(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) continue;  // C0 controls + DEL
        out += ch;                            // printable ASCII and UTF-8 bytes
    }
    return out;
}

std::string percent_encode(std::string_view value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                                c == '_' || c == '~';
        if (unreserved) {
            out += ch;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

} // namespace cworks
