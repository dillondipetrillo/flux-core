#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>

/**
 * Minimal test harness for C unit tests.
 * 
 * Usage:
 *      #include "test_runner.h"
 * 
 *      int main(void)
 *      {
 *          ASSERT(1 + 1 == 2, "basic arithmetic works");
 *          ASSERT(some_function() == expected,
 *              "function returns expected value");
 *          TEST_SUMMARY();
 *      }
 */

static int _tests_run = 0;
static int _tests_passed = 0;

#define ASSERT(condition, message) do {             \
    _tests_run++;                                   \
    if (condition) {                                \
        _tests_passed++;                            \
        printf("    PASS: %s\n", message);          \
    } else {                                        \
        printf("    FAIL: %s (line %d in %s)\n",    \
            message, __LINE__, __FILE__);           \
    }                                               \
} while (0)

#define TEST_SUMMARY() do {                                         \
    printf("\n%d/%d tests passed\n", _tests_passed, _tests_run);    \
    return (_tests_passed == _tests_run) ? 0 : 1;                   \
} while (0)

#endif