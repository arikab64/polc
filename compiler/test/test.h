/*
 * test.h — minimal unit-test scaffold.
 *
 * No external deps. Each test runs in a forked child process so a
 * failed assertion (or a segfault) only kills that one test, and so
 * every test starts with fresh global state. This matters because
 * main.c keeps the label table, EID list, rule list, and var tables
 * as file-scope state with no public reset hook.
 *
 * Assertions print "<file>:<line>: <reason>" to stderr and _exit(1).
 * The runner's parent process records pass/fail per test and prints a
 * summary at the end.
 */
#ifndef POLC_TEST_H
#define POLC_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Run one test function in a forked child. */
void test_run(const char *name, void (*fn)(void));
/* Print "N passed, M failed" and reset counters. */
void test_summary(void);
/* 0 if every test passed, 1 otherwise. */
int  test_exit_code(void);

/* ---------- assertion macros --------------------------------------- */

#define TEST_FAIL_(fmt, ...)                                           \
    do {                                                               \
        fprintf(stderr, "    %s:%d: " fmt "\n",                        \
                __FILE__, __LINE__, ##__VA_ARGS__);                    \
        _exit(1);                                                      \
    } while (0)

#define ASSERT_TRUE(c)                                                 \
    do { if (!(c)) TEST_FAIL_("ASSERT_TRUE(%s)", #c); } while (0)

#define ASSERT_FALSE(c)                                                \
    do { if  ((c)) TEST_FAIL_("ASSERT_FALSE(%s)", #c); } while (0)

#define ASSERT_NULL(p)                                                 \
    do { if ((p) != NULL)                                              \
        TEST_FAIL_("ASSERT_NULL(%s) — got non-null", #p); } while (0)

#define ASSERT_NOT_NULL(p)                                             \
    do { if ((p) == NULL)                                              \
        TEST_FAIL_("ASSERT_NOT_NULL(%s) — got null", #p); } while (0)

#define ASSERT_EQ_INT(a, b)                                            \
    do {                                                               \
        long long _a = (long long)(a);                                 \
        long long _b = (long long)(b);                                 \
        if (_a != _b)                                                  \
            TEST_FAIL_("ASSERT_EQ_INT(%s, %s): %lld != %lld",          \
                       #a, #b, _a, _b);                                \
    } while (0)

#define ASSERT_EQ_U64(a, b)                                            \
    do {                                                               \
        unsigned long long _a = (unsigned long long)(a);               \
        unsigned long long _b = (unsigned long long)(b);               \
        if (_a != _b)                                                  \
            TEST_FAIL_("ASSERT_EQ_U64(%s, %s): 0x%llx != 0x%llx",      \
                       #a, #b, _a, _b);                                \
    } while (0)

#define ASSERT_EQ_PTR(a, b)                                            \
    do {                                                               \
        const void *_a = (a);                                          \
        const void *_b = (b);                                          \
        if (_a != _b)                                                  \
            TEST_FAIL_("ASSERT_EQ_PTR(%s, %s): %p != %p",              \
                       #a, #b, _a, _b);                                \
    } while (0)

#define ASSERT_NE_PTR(a, b)                                            \
    do {                                                               \
        const void *_a = (a);                                          \
        const void *_b = (b);                                          \
        if (_a == _b)                                                  \
            TEST_FAIL_("ASSERT_NE_PTR(%s, %s): both %p",               \
                       #a, #b, _a);                                    \
    } while (0)

#define ASSERT_EQ_STR(a, b)                                            \
    do {                                                               \
        const char *_a = (a);                                          \
        const char *_b = (b);                                          \
        if (!_a || !_b || strcmp(_a, _b) != 0)                         \
            TEST_FAIL_("ASSERT_EQ_STR(%s, %s): \"%s\" != \"%s\"",      \
                       #a, #b, _a ? _a : "(null)", _b ? _b : "(null)"); \
    } while (0)

/* Convenience: registers a function whose name is also the test name. */
#define RUN(fn) test_run(#fn, fn)

#endif /* POLC_TEST_H */
