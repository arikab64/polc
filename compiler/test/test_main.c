/*
 * test_main.c — entry point for the AST unit tests.
 *
 * Each suite_*() function lives in its own test_<area>.c file and
 * registers its tests via test_run(). We call them in dependency order
 * (lower-level utilities first) so a failure in foundational code is
 * visible before its dependents start failing for the same reason.
 */
#include "test.h"
#include <stdio.h>

extern void suite_ip          (void);
extern void suite_lset        (void);
extern void suite_label_table (void);
extern void suite_var         (void);
extern void suite_selector    (void);
extern void suite_subnet      (void);
extern void suite_entity      (void);
extern void suite_rule        (void);
extern void suite_script      (void);

int main(void) {
    printf("\n=== ip ===\n");          suite_ip();
    printf("\n=== lset ===\n");        suite_lset();
    printf("\n=== label_table ===\n"); suite_label_table();
    printf("\n=== var ===\n");         suite_var();
    printf("\n=== selector ===\n");    suite_selector();
    printf("\n=== subnet ===\n");      suite_subnet();
    printf("\n=== entity ===\n");      suite_entity();
    printf("\n=== rule ===\n");        suite_rule();
    printf("\n=== script ===\n");      suite_script();

    test_summary();
    return test_exit_code();
}
