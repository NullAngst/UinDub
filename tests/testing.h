// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
// Minimal test harness: no dependencies, one binary per test file, CTest runs them.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace udtest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

inline int& failures() {
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int runAll() {
    int failedCases = 0;
    const char* filter = std::getenv("UD_TEST_FILTER");
    for (const Case& c : registry()) {
        if (filter && *filter && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = failures();
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        if (failures() != before) {
            ++failedCases;
            std::printf("[ FAIL ] %s\n", c.name);
        } else {
            std::printf("[  OK  ] %s\n", c.name);
        }
        std::fflush(stdout);
    }
    std::printf("%zu cases, %d failed\n", registry().size(), failedCases);
    return failedCases ? 1 : 0;
}

}  // namespace udtest

#define UD_CONCAT2(a, b) a##b
#define UD_CONCAT(a, b) UD_CONCAT2(a, b)
#define TEST_CASE(name)                                                             \
    static void UD_CONCAT(test_fn_, __LINE__)();                                    \
    static udtest::Registrar UD_CONCAT(test_reg_, __LINE__)(name, UD_CONCAT(test_fn_, __LINE__)); \
    static void UD_CONCAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            ++udtest::failures();                                                   \
        }                                                                           \
    } while (0)

#define CHECK_MSG(cond, ...)                                                        \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("  %s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond);  \
            std::printf(__VA_ARGS__);                                               \
            std::printf("\n");                                                      \
            ++udtest::failures();                                                   \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const auto _va = (a);                                                       \
        const auto _vb = (b);                                                       \
        if (!(_va == _vb)) {                                                        \
            std::printf("  %s:%d: CHECK_EQ failed: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                        (long long)_va, (long long)_vb);                            \
            ++udtest::failures();                                                   \
        }                                                                           \
    } while (0)

#define TEST_MAIN() \
    int main() { return udtest::runAll(); }
