// Minimal test harness — standard library only, no framework dependency.
//
//   TEST(my_case) { CHECK(1 + 1 == 2); CHECK_EQ(compute(), 42); }
//
// Tests self-register at static-init time; the main() in test_main.cpp runs
// them all and reports failures with file:line.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    const char*           name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int&         failures()     { static int n = 0; return n; }
inline const char*& current_test() { static const char* n = ""; return n; }

struct Registrar {
    Registrar(const char* name, std::function<void()> fn)
    {
        registry().push_back({name, std::move(fn)});
    }
};

inline void report_failure(const char* file, int line, const std::string& what)
{
    ++failures();
    std::printf("FAIL  %s\n      %s:%d: %s\n", current_test(), file, line, what.c_str());
}

// Runs every registered test; returns the process exit code.
inline int run_all()
{
    int run = 0;
    for (const TestCase& t : registry()) {
        current_test() = t.name;
        int before = failures();
        try {
            t.fn();
        } catch (const std::exception& e) {
            report_failure("<exception>", 0, std::string("unhandled: ") + e.what());
        } catch (...) {
            report_failure("<exception>", 0, "unhandled non-std exception");
        }
        ++run;
        if (failures() == before)
            std::printf("ok    %s\n", t.name);
    }
    std::printf("\n%d test(s), %d failure(s)\n", run, failures());
    return failures() == 0 ? 0 : 1;
}

} // namespace testing

#define TEST(name)                                                        \
    static void test_fn_##name();                                        \
    static ::testing::Registrar reg_##name{#name, &test_fn_##name};      \
    static void test_fn_##name()

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond))                                                      \
            ::testing::report_failure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

// Equality check that prints both sides on failure (needs operator<<).
#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        auto va = (a);                                                    \
        auto vb = (b);                                                    \
        if (!(va == vb)) {                                                \
            std::ostringstream oss;                                       \
            oss << "CHECK_EQ(" #a ", " #b "): " << va << " != " << vb;    \
            ::testing::report_failure(__FILE__, __LINE__, oss.str());     \
        }                                                                 \
    } while (0)

#define CHECK_THROWS(expr, exception_type)                                \
    do {                                                                  \
        bool caught = false;                                              \
        try { (void)(expr); }                                             \
        catch (const exception_type&) { caught = true; }                  \
        if (!caught)                                                      \
            ::testing::report_failure(__FILE__, __LINE__,                 \
                "CHECK_THROWS(" #expr ", " #exception_type "): no throw"); \
    } while (0)
