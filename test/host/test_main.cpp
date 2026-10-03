/*
 * Runs every registered test case; exits non-zero if any check failed.
 * Optional argument: a substring to run only matching cases.
 */

#include "test.hpp"
#include "fake_idf.hpp"
#include <cstring>

static int s_failures = 0;

std::vector<test::Case>& test::cases()
{
    static std::vector<Case> all;
    return all;
}

void test::fail(const char* file, int line, const std::string& what)
{
    s_failures++;
    printf("    FAIL %s:%d: %s\n", file, line, what.c_str());
}

int main(int argc, char** argv)
{
    int run = 0;
    int failed_cases = 0;
    for (const auto& c : test::cases()) {
        if (argc > 1 && !strstr(c.name, argv[1])) {
            continue;
        }
        fake::reset();
        int before = s_failures;
        c.fn();
        run++;
        if (s_failures != before) {
            failed_cases++;
            printf("FAIL %s\n", c.name);
        } else {
            printf("ok   %s\n", c.name);
        }
    }
    printf("\n%d test cases, %d failed\n", run, failed_cases);
    return (failed_cases == 0 && run > 0) ? 0 : 1;
}
