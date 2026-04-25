/*
 * test_label_table.c — label_intern dedup, label_lookup, label_lookup_id,
 * and the mk_label helper that ties an interned id to a label_node.
 */
#include "test.h"
#include "../ast.h"

/* ---------- label_intern ------------------------------------------- */

static void intern_assigns_first_id_one(void) {
    /* Bit 0 of the bitset is reserved as a sentinel ("no label"), so
     * real ids start at 1. */
    int id = label_intern("app", "web", 1, 1);
    ASSERT_EQ_INT(id, 1);
}

static void intern_dedups_same_pair(void) {
    int a = label_intern("app", "web", 1, 1);
    int b = label_intern("app", "web", 2, 5);
    ASSERT_EQ_INT(a, b);
}

static void intern_distinct_keys_get_distinct_ids(void) {
    int a = label_intern("app", "web", 1, 1);
    int b = label_intern("env", "prod", 1, 1);
    ASSERT_FALSE(a == b);
}

static void intern_same_key_different_val_distinct(void) {
    int a = label_intern("app", "web", 1, 1);
    int b = label_intern("app", "api", 1, 1);
    ASSERT_FALSE(a == b);
}

static void intern_ids_are_dense_and_sequential(void) {
    /* Successive distinct interns get 1, 2, 3, ... so they pack into
     * the bitset without gaps. */
    int a = label_intern("k1", "v1", 1, 1);
    int b = label_intern("k2", "v2", 1, 1);
    int c = label_intern("k3", "v3", 1, 1);
    ASSERT_EQ_INT(a, 1);
    ASSERT_EQ_INT(b, 2);
    ASSERT_EQ_INT(c, 3);
}

/* ---------- label_lookup ------------------------------------------- */

static void lookup_by_id_hits(void) {
    int id = label_intern("role", "server", 1, 1);
    const char *k = NULL, *v = NULL;
    ASSERT_EQ_INT(label_lookup(id, &k, &v), 1);
    ASSERT_EQ_STR(k, "role");
    ASSERT_EQ_STR(v, "server");
}

static void lookup_by_id_misses(void) {
    /* Unknown id with empty table → 0. */
    const char *k = NULL, *v = NULL;
    ASSERT_EQ_INT(label_lookup(42, &k, &v), 0);
}

static void lookup_by_id_with_null_outs(void) {
    /* Both out-pointers are optional — passing NULL must not crash. */
    int id = label_intern("k", "v", 1, 1);
    ASSERT_EQ_INT(label_lookup(id, NULL, NULL), 1);
}

/* ---------- label_lookup_id ---------------------------------------- */

static void lookup_id_hits_after_intern(void) {
    int id = label_intern("env", "prod", 1, 1);
    ASSERT_EQ_INT(label_lookup_id("env", "prod"), id);
}

static void lookup_id_misses_when_not_interned(void) {
    label_intern("env", "prod", 1, 1);
    ASSERT_EQ_INT(label_lookup_id("env", "stage"), 0);
}

static void lookup_id_misses_on_empty_table(void) {
    ASSERT_EQ_INT(label_lookup_id("anything", "really"), 0);
}

/* ---------- mk_label ----------------------------------------------- */

static void mk_label_interns_and_records_id(void) {
    label_node *n = mk_label("app", "web", 9, 4);
    ASSERT_NOT_NULL(n);
    ASSERT_EQ_STR(n->key, "app");
    ASSERT_EQ_STR(n->val, "web");
    ASSERT_EQ_INT(n->line, 9);
    ASSERT_EQ_INT(n->col,  4);
    ASSERT_TRUE(n->id >= 1);
    /* The recorded id must match what label_lookup_id returns. */
    ASSERT_EQ_INT(label_lookup_id("app", "web"), n->id);
    free(n->key); free(n->val); free(n);
}

static void mk_label_strdups_inputs(void) {
    /* The key/val on the node must outlive the input strings. */
    char k[] = "key";
    char v[] = "val";
    label_node *n = mk_label(k, v, 1, 1);
    /* Mutate the source buffers — the node should be unaffected. */
    k[0] = 'X'; v[0] = 'Y';
    ASSERT_EQ_STR(n->key, "key");
    ASSERT_EQ_STR(n->val, "val");
    free(n->key); free(n->val); free(n);
}

/* ---------- label_append ------------------------------------------- */

static void label_append_chains_in_order(void) {
    label_node *a = mk_label("k1", "v1", 1, 1);
    label_node *b = mk_label("k2", "v2", 1, 1);
    label_node *c = mk_label("k3", "v3", 1, 1);
    label_node *head = label_append(NULL, a);
    head = label_append(head, b);
    head = label_append(head, c);
    ASSERT_EQ_PTR(head, a);
    ASSERT_EQ_PTR(head->next, b);
    ASSERT_EQ_PTR(head->next->next, c);
    ASSERT_NULL(head->next->next->next);
    free(a->key); free(a->val); free(a);
    free(b->key); free(b->val); free(b);
    free(c->key); free(c->val); free(c);
}

void suite_label_table(void) {
    RUN(intern_assigns_first_id_one);
    RUN(intern_dedups_same_pair);
    RUN(intern_distinct_keys_get_distinct_ids);
    RUN(intern_same_key_different_val_distinct);
    RUN(intern_ids_are_dense_and_sequential);
    RUN(lookup_by_id_hits);
    RUN(lookup_by_id_misses);
    RUN(lookup_by_id_with_null_outs);
    RUN(lookup_id_hits_after_intern);
    RUN(lookup_id_misses_when_not_interned);
    RUN(lookup_id_misses_on_empty_table);
    RUN(mk_label_interns_and_records_id);
    RUN(mk_label_strdups_inputs);
    RUN(label_append_chains_in_order);
}
