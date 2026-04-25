/*
 * test_entity.c — add_entity behavior.
 *
 * Contract from ast.h:
 *   - Folds the label list into a 512-bit bitset (label ids).
 *   - Finds-or-creates the EID with that bitset (so entities sharing
 *     a label set collapse into one identity, with the IP set unioned
 *     under it).
 *   - Attaches the entity (name + ips) as a member of the EID.
 *   - Reports diag errors for duplicate label keys / duplicate IPs.
 *
 * add_entity consumes its inputs (ips list and labels list).
 */
#include "test.h"
#include "../ast.h"

static int eid_count(void) {
    int n = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next) n++;
    return n;
}

static int member_count(const eid_node *e) {
    int n = 0;
    for (const member_node *m = e->members; m; m = m->next) n++;
    return n;
}

/* ---------- single entity ------------------------------------------ */

static void single_entity_creates_one_eid(void) {
    ip_node *ips = mk_ip("10.0.0.1", 1, 1);
    label_node *labs = mk_label("app", "web", 1, 1);

    add_entity("alpha", 1, 1, ips, labs);

    ASSERT_EQ_INT(eid_count(), 1);
    eid_node *e = eid_list_head();
    ASSERT_NOT_NULL(e);
    ASSERT_EQ_INT(member_count(e), 1);
    ASSERT_EQ_STR(e->members->name, "alpha");
}

static void eid_hash_matches_lset_hash(void) {
    /* The EID's stored hash is just lset_hash(labels). Verify that
     * relationship survives add_entity. */
    label_node *labs = mk_label("k", "v", 1, 1);
    int id = labs->id;        /* whatever label_intern gave back */
    add_entity("e", 1, 1, mk_ip("10.0.0.1", 1, 1), labs);

    label_set expect; lset_clear(&expect); lset_set(&expect, id);
    ASSERT_EQ_U64(eid_list_head()->hash, lset_hash(&expect));
}

/* ---------- collapsing / distinct EIDs ----------------------------- */

static void same_labels_collapse_to_one_eid(void) {
    /* Two entities with identical label sets share an EID; the IP set
     * is just unioned under it via two member nodes. */
    add_entity("a", 1, 1,
               mk_ip("10.0.0.1", 1, 1),
               mk_label("app", "web", 1, 1));
    add_entity("b", 2, 1,
               mk_ip("10.0.0.2", 2, 1),
               mk_label("app", "web", 2, 1));

    ASSERT_EQ_INT(eid_count(), 1);
    ASSERT_EQ_INT(member_count(eid_list_head()), 2);
    /* First-seen-first ordering — "a" then "b". */
    ASSERT_EQ_STR(eid_list_head()->members->name, "a");
    ASSERT_EQ_STR(eid_list_head()->members->next->name, "b");
}

static void different_labels_create_distinct_eids(void) {
    add_entity("a", 1, 1,
               mk_ip("10.0.0.1", 1, 1),
               mk_label("app", "web", 1, 1));
    add_entity("b", 2, 1,
               mk_ip("10.0.0.2", 2, 1),
               mk_label("app", "api", 2, 1));

    ASSERT_EQ_INT(eid_count(), 2);
    /* Each EID has exactly one member. */
    for (eid_node *e = eid_list_head(); e; e = e->next)
        ASSERT_EQ_INT(member_count(e), 1);
    /* Hashes differ. */
    eid_node *e0 = eid_list_head();
    ASSERT_FALSE(e0->hash == e0->next->hash);
}

static void multi_label_collapse(void) {
    /* {app:web, env:prod} == {env:prod, app:web} (label order doesn't
     * matter — bitset is unordered). */
    label_node *l1 = label_append(mk_label("app", "web",  1, 1),
                                  mk_label("env", "prod", 1, 1));
    label_node *l2 = label_append(mk_label("env", "prod", 2, 1),
                                  mk_label("app", "web",  2, 1));
    add_entity("a", 1, 1, mk_ip("10.0.0.1", 1, 1), l1);
    add_entity("b", 2, 1, mk_ip("10.0.0.2", 2, 1), l2);

    ASSERT_EQ_INT(eid_count(), 1);
    ASSERT_EQ_INT(member_count(eid_list_head()), 2);
}

/* ---------- diagnostics --------------------------------------------- */

static void duplicate_label_key_is_error(void) {
    /* {app:web, app:api} — same key twice. Increments g_semantic_errors. */
    label_node *labs = label_append(mk_label("app", "web", 1, 1),
                                    mk_label("app", "api", 1, 1));
    int before = g_semantic_errors;
    add_entity("e", 1, 1, mk_ip("10.0.0.1", 1, 1), labs);
    ASSERT_TRUE(g_semantic_errors > before);
}

static void duplicate_ip_within_entity_is_error(void) {
    /* Same IP listed twice in one entity. */
    ip_node *ips = ip_append(mk_ip("10.0.0.1", 1, 1),
                             mk_ip("10.0.0.1", 1, 5));
    int before = g_semantic_errors;
    add_entity("e", 1, 1, ips, mk_label("k", "v", 1, 1));
    ASSERT_TRUE(g_semantic_errors > before);
}

static void duplicate_ip_across_entities_is_error(void) {
    add_entity("a", 1, 1,
               mk_ip("10.0.0.1", 1, 1),
               mk_label("k", "v", 1, 1));
    int before = g_semantic_errors;
    add_entity("b", 2, 1,
               mk_ip("10.0.0.1", 2, 1),
               mk_label("k", "w", 2, 1));
    ASSERT_TRUE(g_semantic_errors > before);
}

void suite_entity(void) {
    RUN(single_entity_creates_one_eid);
    RUN(eid_hash_matches_lset_hash);
    RUN(same_labels_collapse_to_one_eid);
    RUN(different_labels_create_distinct_eids);
    RUN(multi_label_collapse);
    RUN(duplicate_label_key_is_error);
    RUN(duplicate_ip_within_entity_is_error);
    RUN(duplicate_ip_across_entities_is_error);
}
