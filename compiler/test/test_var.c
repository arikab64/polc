/*
 * test_var.c — $var (label) and @var (port) macro tables.
 *
 * The contract per ast.h: var_*_lookup returns a freshly-allocated deep
 * copy so the caller can splice it into a larger AST without corrupting
 * the stored definition.
 */
#include "test.h"
#include "../ast.h"

/* ---------- $var label ---------------------------------------------- */

static void var_label_define_then_lookup(void) {
    label_node *l = mk_label("env", "prod", 1, 1);
    var_label_define("production", l, 1, 1);

    label_node *got = var_label_lookup("production");
    ASSERT_NOT_NULL(got);
    ASSERT_EQ_STR(got->key, "env");
    ASSERT_EQ_STR(got->val, "prod");
    ASSERT_NULL(got->next);
    free(got->key); free(got->val); free(got);
}

static void var_label_lookup_undefined_returns_null(void) {
    label_node *got = var_label_lookup("nope");
    ASSERT_NULL(got);
}

static void var_label_lookup_is_deep_copy(void) {
    /* The list returned must not alias the stored one — otherwise
     * splicing it into a rule's AST would corrupt the var table. */
    label_node *stored = mk_label("k", "v", 1, 1);
    var_label_define("v1", stored, 1, 1);

    label_node *a = var_label_lookup("v1");
    label_node *b = var_label_lookup("v1");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NE_PTR(a, b);                  /* different list heads     */
    ASSERT_NE_PTR(a->key, b->key);        /* deep copy of strings too */
    free(a->key); free(a->val); free(a);
    free(b->key); free(b->val); free(b);
}

static void var_label_redefine_is_rejected(void) {
    /* Per the spec, redefinition is a compilation error. The first
     * definition stays; g_semantic_errors goes up. */
    label_node *first  = mk_label("k", "first",  1, 1);
    label_node *second = mk_label("k", "second", 2, 1);

    int before = g_semantic_errors;
    var_label_define("dupvar", first,  1, 1);
    var_label_define("dupvar", second, 2, 1);
    ASSERT_TRUE(g_semantic_errors > before);

    label_node *got = var_label_lookup("dupvar");
    ASSERT_NOT_NULL(got);
    ASSERT_EQ_STR(got->val, "first");
    free(got->key); free(got->val); free(got);
}

static void var_label_list_preserved(void) {
    /* Bracketed list var: define $apps = [a:1, b:2, c:3] and verify
     * the lookup returns three nodes in original order. */
    label_node *l1 = mk_label("a", "1", 1, 1);
    label_node *l2 = mk_label("b", "2", 1, 1);
    label_node *l3 = mk_label("c", "3", 1, 1);
    label_node *head = label_append(NULL, l1);
    head = label_append(head, l2);
    head = label_append(head, l3);
    var_label_define("apps", head, 1, 1);

    label_node *got = var_label_lookup("apps");
    ASSERT_NOT_NULL(got);
    ASSERT_EQ_STR(got->key, "a");
    ASSERT_NOT_NULL(got->next);
    ASSERT_EQ_STR(got->next->key, "b");
    ASSERT_NOT_NULL(got->next->next);
    ASSERT_EQ_STR(got->next->next->key, "c");
    ASSERT_NULL(got->next->next->next);

    while (got) {
        label_node *n = got->next;
        free(got->key); free(got->val); free(got);
        got = n;
    }
}

/* ---------- @var port ----------------------------------------------- */

static void var_port_define_then_lookup(void) {
    port_node *p = mk_port(80, 1, 1);
    var_port_define("http", p, 1, 1);

    port_node *got = var_port_lookup("http");
    ASSERT_NOT_NULL(got);
    ASSERT_EQ_INT(got->port, 80);
    ASSERT_NULL(got->next);
    free(got);
}

static void var_port_lookup_undefined_returns_null(void) {
    ASSERT_NULL(var_port_lookup("nope"));
}

static void var_port_lookup_is_deep_copy(void) {
    port_node *stored = mk_port(443, 1, 1);
    var_port_define("https", stored, 1, 1);

    port_node *a = var_port_lookup("https");
    port_node *b = var_port_lookup("https");
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NE_PTR(a, b);
    free(a); free(b);
}

static void var_port_list_preserved(void) {
    /* @http = 80, 8080. */
    port_node *p80   = mk_port(80,   1, 1);
    port_node *p8080 = mk_port(8080, 1, 1);
    port_node *head  = port_append(NULL, p80);
    head = port_append(head, p8080);
    var_port_define("http", head, 1, 1);

    port_node *got = var_port_lookup("http");
    ASSERT_NOT_NULL(got);
    ASSERT_EQ_INT(got->port, 80);
    ASSERT_NOT_NULL(got->next);
    ASSERT_EQ_INT(got->next->port, 8080);
    ASSERT_NULL(got->next->next);

    while (got) { port_node *n = got->next; free(got); got = n; }
}

void suite_var(void) {
    RUN(var_label_define_then_lookup);
    RUN(var_label_lookup_undefined_returns_null);
    RUN(var_label_lookup_is_deep_copy);
    RUN(var_label_redefine_is_rejected);
    RUN(var_label_list_preserved);
    RUN(var_port_define_then_lookup);
    RUN(var_port_lookup_undefined_returns_null);
    RUN(var_port_lookup_is_deep_copy);
    RUN(var_port_list_preserved);
}
