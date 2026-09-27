// ckdiagram tests — the committed-golden comparison, shared by every suite
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Two guarantees, and they are not the same one:
//
//   * everywhere (incl. Windows): the output is byte-stable on a re-render,
//     so the producer is deterministic on its own platform — no clock, no
//     locale, no map-iteration dependence. The caller checks that; it needs
//     both renders and this file never sees them.
//   * on unix: the bytes match the committed golden — the cross-platform
//     pin. It holds across unix libms (glibc/macOS) but not MSVC, whose
//     transcendentals and snprintf tie-rounding differ in the last place and
//     cross cplot's %.2f grid at a few coordinates. Bit-identical rendered
//     bytes on Windows need deterministic transcendental math suite-wide —
//     the separate S-12 item — so every byte-exact rendering golden in the
//     suite is unix-only for the same reason.
//
// Golden location: $CDIAGRAM_GOLDEN_DIR (set by ctest) or ./golden.
// Regenerate after an intentional change with CDIAGRAM_GOLDEN_UPDATE=1 — and
// review the diff: a golden change is a rendering change.
#pragma once

#include <cworks/microtest.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace cdiagram_test {

inline std::string golden_dir() {
    const char* dir = std::getenv("CDIAGRAM_GOLDEN_DIR");
    return dir ? dir : "tests/mermaid/golden";
}

inline bool golden_update() { return std::getenv("CDIAGRAM_GOLDEN_UPDATE") != nullptr; }

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// The committed-golden byte comparison is a cross-platform guarantee that
/// holds across unix libms but not MSVC (see the file header).
constexpr bool kByteExactPinned =
#ifdef _WIN32
    false;
#else
    true;
#endif

/// Compare `actual` against the committed golden file `filename` (a name
/// within the golden directory, extension included), or rewrite it under
/// CDIAGRAM_GOLDEN_UPDATE.
///
/// Every golden in the suite goes through here, whatever it holds — a
/// rendered SVG, a layout projection in JSON. A test that reads its golden
/// with its own std::ifstream is a golden the documented regeneration does
/// not reach, which is how one goes stale and stays stale.
///
/// On a mismatch the whole produced document is reported, because the useful
/// question about a broken golden is never "did it differ" but "what does it
/// look like now" — and a diff against the committed file answers that only
/// if the new bytes are in the log.
inline void check_golden_file(const std::string& filename, const std::string& actual,
                              const char* file, int line) {
    const std::string path = golden_dir() + "/" + filename;
    if (golden_update()) {
        std::ofstream out(path, std::ios::binary);
        out << actual;
        microtest::check(out.good(), file, line, ("wrote golden " + path).c_str());
        return;
    }
    if constexpr (!kByteExactPinned) return;

    const std::string expected = read_file(path);
    if (actual == expected) {
        microtest::check(true, file, line, ("golden " + filename).c_str());
        return;
    }
    microtest::report_failure(file, line, "golden case '" + filename + "' matches " + path,
                              "got:\n" + actual);
}

/// The rendered-SVG spelling of the above: `name` is a bare stem and
/// ".expected.svg" is appended.
inline void check_golden(const std::string& name, const std::string& actual, const char* file,
                         int line) {
    check_golden_file(name + ".expected.svg", actual, file, line);
}

} // namespace cdiagram_test

#define CHECK_GOLDEN(name, actual) ::cdiagram_test::check_golden((name), (actual), __FILE__, __LINE__)
#define CHECK_GOLDEN_FILE(filename, actual)                                                        \
    ::cdiagram_test::check_golden_file((filename), (actual), __FILE__, __LINE__)
