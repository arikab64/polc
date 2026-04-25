/*
 * test.c — fork-per-test runner.
 *
 * Each test_run() call forks. The child runs the test function; an
 * assertion failure calls _exit(1), an uncaught signal (segfault, abort)
 * terminates the child with a non-zero status. The parent records
 * pass/fail and continues.
 *
 * Forking gives us two things for free:
 *   1. Crash isolation — one test cannot break the next.
 *   2. Fresh global state per test — no need for ast_reset() helpers.
 */
#include "test.h"

#include <sys/wait.h>

static int g_passed = 0;
static int g_failed = 0;

void test_run(const char *name, void (*fn)(void)) {
    /* Flush so the child doesn't inherit a partial buffer and double-print. */
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "  ERROR  %s: fork failed\n", name);
        g_failed++;
        return;
    }
    if (pid == 0) {
        /* Child: run the test. Failure path is _exit(1) inside the
         * assertion macros; success falls through and we _exit(0). */
        fn();
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "  ERROR  %s: waitpid failed\n", name);
        g_failed++;
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("  PASS   %s\n", name);
        g_passed++;
        return;
    }

    if (WIFEXITED(status)) {
        printf("  FAIL   %s  (exit %d)\n", name, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("  CRASH  %s  (signal %d: %s)\n",
               name, WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        printf("  FAIL   %s  (status 0x%x)\n", name, (unsigned)status);
    }
    g_failed++;
}

void test_summary(void) {
    int total = g_passed + g_failed;
    printf("\n%d test%s: %d passed, %d failed\n",
           total, total == 1 ? "" : "s", g_passed, g_failed);
}

int test_exit_code(void) { return g_failed ? 1 : 0; }
