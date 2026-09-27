// libcworks — microtest, the CK Office test harness
// SPDX-FileCopyrightText: 2026 Dr. Christian Klukas
// SPDX-License-Identifier: MIT
//
// Minimal doctest-compatible harness shared by every CK Office component
// (TEST_CASE, CHECK, CHECK_EQ, CHECK_APPROX, REQUIRE, CHECK_THROWS_AS).
// Deliberately tiny and dependency-free; to switch a project to real
// doctest, replace this include — no test changes needed.
//
// Define CWORKS_MICROTEST_MAIN in exactly one translation unit to get
// the main() that runs all registered cases.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace microtest {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        registry().push_back({std::move(name), std::move(fn)});
    }
};

struct State {
    int checks = 0;
    int failures = 0;
    const char* current_test = "";
};

inline State& state() {
    static State s;
    return s;
}

struct RequireFailed {};

inline void report_failure(const char* file, int line, const std::string& expr,
                           const std::string& detail = {}) {
    ++state().failures;
    std::printf("FAILED: %s\n  %s:%d: %s\n", state().current_test, file, line, expr.c_str());
    if (!detail.empty()) std::printf("  %s\n", detail.c_str());
}

inline bool check(bool ok, const char* file, int line, const char* expr) {
    ++state().checks;
    if (!ok) report_failure(file, line, expr);
    return ok;
}

template <class A, class B>
bool check_eq(const A& a, const B& b, const char* file, int line, const char* expr) {
    ++state().checks;
    const bool ok = (a == b);
    if (!ok) report_failure(file, line, expr);
    return ok;
}

inline bool check_approx(double a, double b, double eps, const char* file, int line,
                         const char* expr) {
    ++state().checks;
    const bool ok = std::abs(a - b) <= eps * std::max({1.0, std::abs(a), std::abs(b)});
    if (!ok) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "left=%.12g right=%.12g", a, b);
        report_failure(file, line, expr, buf);
    }
    return ok;
}

inline int run_all() {
    for (const auto& test : registry()) {
        state().current_test = test.name.c_str();
        try {
            test.fn();
        } catch (const RequireFailed&) {
            // failure already reported
        } catch (const std::exception& e) {
            report_failure("<unknown>", 0, "unexpected exception", e.what());
        }
    }
    std::printf("[microtest] %zu test cases, %d assertions, %d failures\n",
                registry().size(), state().checks, state().failures);
    return state().failures == 0 ? 0 : 1;
}

} // namespace microtest

#define MT_CONCAT_IMPL(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT_IMPL(a, b)

#define TEST_CASE(name)                                                                        \
    static void MT_CONCAT(mt_test_fn_, __LINE__)();                                           \
    static const microtest::Registrar MT_CONCAT(mt_reg_, __LINE__)(                           \
        name, MT_CONCAT(mt_test_fn_, __LINE__));                                              \
    static void MT_CONCAT(mt_test_fn_, __LINE__)()

#define CHECK(expr) microtest::check(static_cast<bool>(expr), __FILE__, __LINE__, #expr)
#define CHECK_EQ(a, b) microtest::check_eq((a), (b), __FILE__, __LINE__, #a " == " #b)
#define CHECK_APPROX(a, b) microtest::check_approx((a), (b), 1e-9, __FILE__, __LINE__, #a " ~= " #b)
#define CHECK_APPROX_EPS(a, b, eps) \
    microtest::check_approx((a), (b), (eps), __FILE__, __LINE__, #a " ~= " #b)

#define REQUIRE(expr)                                                                          \
    do {                                                                                       \
        if (!microtest::check(static_cast<bool>(expr), __FILE__, __LINE__, #expr))             \
            throw microtest::RequireFailed{};                                                  \
    } while (0)

#define CHECK_THROWS_AS(expr, ExceptionType)                                                   \
    do {                                                                                       \
        ++microtest::state().checks;                                                           \
        bool mt_thrown = false;                                                                \
        try {                                                                                  \
            (void)(expr);                                                                      \
        } catch (const ExceptionType&) {                                                       \
            mt_thrown = true;                                                                  \
        } catch (...) {                                                                        \
        }                                                                                      \
        if (!mt_thrown)                                                                        \
            microtest::report_failure(__FILE__, __LINE__,                                      \
                                      #expr " should throw " #ExceptionType);                  \
    } while (0)

#ifdef CWORKS_MICROTEST_MAIN
int main() { return microtest::run_all(); }
#endif
