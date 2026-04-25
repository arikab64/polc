/*
 * test_selector.c — sel_node constructors and sel_from_labels folding.
 */
#include "test.h"
#include "../ast.h"
#include <stdlib.h>

/* sel_from_labels is recursive but free is not exposed; main.c's
 * free_sel is static. We replicate the walk here so tests don't leak
 * (relevant only when run without fork — fork-per-test would clean up
 * anyway, but keeping it tidy for valgrind users). */
static void free_sel_local(sel_node *s) {
    if (!s) return;
    switch (s->kind) {
        case SEL_LEAF: free(s->key); free(s->val);              break;
        case SEL_AND:
        case SEL_OR:   free_sel_local(s->lhs);
                       free_sel_local(s->rhs);                  break;
        case SEL_ALL:                                           break;
    }
    free(s);
}

static void leaf_basic(void) {
    sel_node *n = sel_leaf("app", "web", 4, 7);
    ASSERT_NOT_NULL(n);
    ASSERT_EQ_INT(n->kind, SEL_LEAF);
    ASSERT_EQ_STR(n->key, "app");
    ASSERT_EQ_STR(n->val, "web");
    ASSERT_EQ_INT(n->line, 4);
    ASSERT_EQ_INT(n->col,  7);
    ASSERT_NULL(n->lhs);
    ASSERT_NULL(n->rhs);
    free_sel_local(n);
}

static void leaf_strdups_inputs(void) {
    char k[] = "k";
    char v[] = "v";
    sel_node *n = sel_leaf(k, v, 1, 1);
    k[0] = 'X'; v[0] = 'Y';
    ASSERT_EQ_STR(n->key, "k");
    ASSERT_EQ_STR(n->val, "v");
    free_sel_local(n);
}

static void binop_and(void) {
    sel_node *l = sel_leaf("a", "1", 3, 5);
    sel_node *r = sel_leaf("b", "2", 9, 11);
    sel_node *n = sel_binop(SEL_AND, l, r);
    ASSERT_EQ_INT(n->kind, SEL_AND);
    ASSERT_EQ_PTR(n->lhs, l);
    ASSERT_EQ_PTR(n->rhs, r);
    /* binop position is inherited from lhs. */
    ASSERT_EQ_INT(n->line, 3);
    ASSERT_EQ_INT(n->col,  5);
    free_sel_local(n);
}

static void binop_or(void) {
    sel_node *l = sel_leaf("a", "1", 1, 1);
    sel_node *r = sel_leaf("b", "2", 1, 1);
    sel_node *n = sel_binop(SEL_OR, l, r);
    ASSERT_EQ_INT(n->kind, SEL_OR);
    free_sel_local(n);
}

static void all_sentinel(void) {
    sel_node *n = sel_all();
    ASSERT_NOT_NULL(n);
    ASSERT_EQ_INT(n->kind, SEL_ALL);
    /* SEL_ALL has no payload — both leaf strings and child pointers
     * stay zero/NULL so the walkers can dispatch on kind alone. */
    ASSERT_NULL(n->key);
    ASSERT_NULL(n->val);
    ASSERT_NULL(n->lhs);
    ASSERT_NULL(n->rhs);
    free_sel_local(n);
}

/* ---------- sel_from_labels --------------------------------------- */

static void from_labels_single_returns_leaf(void) {
    /* One-element list collapses to a plain leaf — no OR wrapper. */
    label_node *l = mk_label("only", "one", 5, 5);
    sel_node *s = sel_from_labels(l);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_INT(s->kind, SEL_LEAF);
    ASSERT_EQ_STR(s->key, "only");
    ASSERT_EQ_STR(s->val, "one");
    free_sel_local(s);
}

static void from_labels_three_right_folds(void) {
    /* [a, b, c] -> OR(a, OR(b, c)). */
    label_node *a = mk_label("a", "1", 1, 1);
    label_node *b = mk_label("b", "2", 1, 1);
    label_node *c = mk_label("c", "3", 1, 1);
    label_node *head = label_append(NULL, a);
    head = label_append(head, b);
    head = label_append(head, c);

    sel_node *s = sel_from_labels(head);
    ASSERT_NOT_NULL(s);
    ASSERT_EQ_INT(s->kind, SEL_OR);

    /* Top-level lhs is leaf "a:1". */
    ASSERT_NOT_NULL(s->lhs);
    ASSERT_EQ_INT(s->lhs->kind, SEL_LEAF);
    ASSERT_EQ_STR(s->lhs->key, "a");

    /* Top-level rhs is OR(b, c). */
    ASSERT_NOT_NULL(s->rhs);
    ASSERT_EQ_INT(s->rhs->kind, SEL_OR);
    ASSERT_NOT_NULL(s->rhs->lhs);
    ASSERT_EQ_STR(s->rhs->lhs->key, "b");
    ASSERT_NOT_NULL(s->rhs->rhs);
    ASSERT_EQ_STR(s->rhs->rhs->key, "c");

    free_sel_local(s);
}

static void from_labels_two_or_pair(void) {
    label_node *a = mk_label("a", "1", 1, 1);
    label_node *b = mk_label("b", "2", 1, 1);
    label_node *head = label_append(NULL, a);
    head = label_append(head, b);

    sel_node *s = sel_from_labels(head);
    ASSERT_EQ_INT(s->kind, SEL_OR);
    ASSERT_EQ_INT(s->lhs->kind, SEL_LEAF);
    ASSERT_EQ_INT(s->rhs->kind, SEL_LEAF);
    free_sel_local(s);
}

void suite_selector(void) {
    RUN(leaf_basic);
    RUN(leaf_strdups_inputs);
    RUN(binop_and);
    RUN(binop_or);
    RUN(all_sentinel);
    RUN(from_labels_single_returns_leaf);
    RUN(from_labels_two_or_pair);
    RUN(from_labels_three_right_folds);
}
