/*
 * Minimal test harness (no third-party framework): TEST() registers a case, CHECK*()
 * records failures, test_main.cpp runs every case with fresh fakes.
 */

#pragma once
#include <cstdio>
#include <string>
#include <vector>

namespace test
{
    struct Case {
        const char* name;
        void (*fn)();
    };
    std::vector<Case>& cases();
    void fail(const char* file, int line, const std::string& what);

    struct Registrar {
        Registrar(const char* name, void (*fn)()) { cases().push_back({name, fn}); }
    };

    inline std::string show(const std::string& s) { return "\"" + s + "\""; }
    inline std::string show(const char* s) { return show(std::string(s ? s : "(null)")); }
    template <typename T> std::string show(const T& v) { return std::to_string(v); }
}

#define TEST(name)                                                   \
    static void name();                                              \
    static test::Registrar name##_registrar(#name, name);            \
    static void name()

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");      \
        }                                                            \
    } while (0)

#define CHECK_EQ(actual, expected)                                   \
    do {                                                             \
        auto _a = (actual);                                          \
        auto _e = (expected);                                        \
        if (!(_a == _e)) {                                           \
            test::fail(__FILE__, __LINE__, "CHECK_EQ(" #actual ", " #expected "): got " + \
                       test::show(_a) + ", expected " + test::show(_e)); \
        }                                                            \
    } while (0)
