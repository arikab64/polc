/*
 * test_rule.c — add_rule and the rule_side carriers feeding it.
 *
 * Contract from ast.h:
 *   - rule ids are 0-based and assigned sequentially.
 *   - Every rule_node carries a non-NULL src/dst sel_node (SEL_ALL
 *     when the side was bare ANY or a bare subnet).
 *   - src_sand/src_sor/dst_sand/dst_sor are never NULL — sentinels
 *     are materialized when missing.
 *   - add_rule consumes the rule_side wrappers (frees them).
 */
#include "test.h"
#include "../ast.h"

static int rule_count(void) {
    int n = 0;
    for (rule_node *r = rule_list_head(); r; r = r->next) n++;
    return n;
}

/* ---------- ANY -> ANY --------------------------------------------- */

static void any_to_any_populates_sel_all_and_sentinels(void) {
    add_rule(ACT_ALLOW, 1, 1,
             mk_side_any(), mk_side_any(),
             mk_port(0, 1, 1),         /* port 0 == ANY wildcard */
             PROTO_TCP | PROTO_UDP);

    rule_node *r = rule_list_head();
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(r->id, 0);
    ASSERT_EQ_INT(r->action, ACT_ALLOW);

    /* Both sides: SEL_ALL with sentinel sand/sor. */
    ASSERT_NOT_NULL(r->src);
    ASSERT_EQ_INT(r->src->kind, SEL_ALL);
    ASSERT_NOT_NULL(r->src_sand);
    ASSERT_EQ_INT(r->src_sand->kind, SN_RANGE);   /* and-ANY */
    ASSERT_NOT_NULL(r->src_sor);
    ASSERT_EQ_INT(r->src_sor->kind, SN_CIDR);     /* or-ANY */
    ASSERT_EQ_INT(r->src_sor->prefix, 32);

    ASSERT_NOT_NULL(r->dst);
    ASSERT_EQ_INT(r->dst->kind, SEL_ALL);
    ASSERT_NOT_NULL(r->dst_sand);
    ASSERT_NOT_NULL(r->dst_sor);

    ASSERT_EQ_INT(r->protos, (unsigned)(PROTO_TCP | PROTO_UDP));
}

/* ---------- selector -> selector ----------------------------------- */

static void selector_to_selector(void) {
    sel_node *src_sel = sel_leaf("app", "web", 1, 1);
    sel_node *dst_sel = sel_leaf("app", "api", 1, 1);
    add_rule(ACT_BLOCK, 7, 3,
             mk_side_sel(src_sel), mk_side_sel(dst_sel),
             mk_port(80, 7, 20),
             PROTO_TCP);

    rule_node *r = rule_list_head();
    ASSERT_NOT_NULL(r);
    ASSERT_EQ_INT(r->action, ACT_BLOCK);
    ASSERT_EQ_PTR(r->src, src_sel);
    ASSERT_EQ_PTR(r->dst, dst_sel);
    /* Sand/sor are sentinels (mk_side_sel materializes them). */
    ASSERT_EQ_INT(r->src_sand->kind, SN_RANGE);
    ASSERT_EQ_INT(r->src_sor->kind,  SN_CIDR);
    ASSERT_EQ_INT(r->dst_sand->kind, SN_RANGE);
    ASSERT_EQ_INT(r->dst_sor->kind,  SN_CIDR);
    ASSERT_EQ_INT(r->protos, PROTO_TCP);
    ASSERT_EQ_INT(r->ports->port, 80);
    ASSERT_EQ_INT(r->line, 7);
    ASSERT_EQ_INT(r->col,  3);
}

/* ---------- bare subnet rule_side ---------------------------------- */

static void bare_subnet_side_uses_sel_all(void) {
    /* §5.2 row 5: a bare subnet means ALL_EIDS on the selector slot,
     * with the user's subnet in S_and and or-ANY in S_or. */
    subnet_node *cidr = mk_subnet_cidr(0x0a000001u, 32, 1, 1);
    add_rule(ACT_ALLOW, 1, 1,
             mk_side_any(), mk_side_subn(cidr),
             mk_port(443, 1, 1),
             PROTO_TCP);

    rule_node *r = rule_list_head();
    ASSERT_NOT_NULL(r);
    /* dst selector is the SEL_ALL sentinel (no user-supplied selector). */
    ASSERT_EQ_INT(r->dst->kind, SEL_ALL);
    /* dst_sand IS the user-supplied subnet (same pointer). */
    ASSERT_EQ_PTR(r->dst_sand, cidr);
    /* dst_sor is or-ANY. */
    ASSERT_EQ_INT(r->dst_sor->kind, SN_CIDR);
    ASSERT_EQ_U64(r->dst_sor->addr, 0ULL);
}

/* ---------- id assignment ------------------------------------------ */

static void ids_are_zero_based_and_sequential(void) {
    add_rule(ACT_ALLOW, 1, 1, mk_side_any(), mk_side_any(),
             mk_port(0, 1, 1), PROTO_TCP);
    add_rule(ACT_BLOCK, 2, 1, mk_side_any(), mk_side_any(),
             mk_port(0, 2, 1), PROTO_UDP);
    add_rule(ACT_OVERRIDE_ALLOW, 3, 1, mk_side_any(), mk_side_any(),
             mk_port(0, 3, 1), PROTO_TCP | PROTO_UDP);

    ASSERT_EQ_INT(rule_count(), 3);
    rule_node *r0 = rule_list_head();
    rule_node *r1 = r0->next;
    rule_node *r2 = r1->next;
    ASSERT_EQ_INT(r0->id, 0);
    ASSERT_EQ_INT(r1->id, 1);
    ASSERT_EQ_INT(r2->id, 2);
    ASSERT_EQ_INT(r0->action, ACT_ALLOW);
    ASSERT_EQ_INT(r1->action, ACT_BLOCK);
    ASSERT_EQ_INT(r2->action, ACT_OVERRIDE_ALLOW);
}

/* ---------- ports & protos ----------------------------------------- */

static void port_list_attached_in_order(void) {
    port_node *p1 = mk_port(80,   1, 1);
    port_node *p2 = mk_port(443,  1, 1);
    port_node *p3 = mk_port(8080, 1, 1);
    port_node *head = port_append(NULL, p1);
    head = port_append(head, p2);
    head = port_append(head, p3);

    add_rule(ACT_ALLOW, 1, 1, mk_side_any(), mk_side_any(),
             head, PROTO_TCP);

    rule_node *r = rule_list_head();
    ASSERT_NOT_NULL(r->ports);
    ASSERT_EQ_INT(r->ports->port, 80);
    ASSERT_EQ_INT(r->ports->next->port, 443);
    ASSERT_EQ_INT(r->ports->next->next->port, 8080);
    ASSERT_NULL(r->ports->next->next->next);
}

static void protos_bitmask_round_trip(void) {
    add_rule(ACT_ALLOW, 1, 1, mk_side_any(), mk_side_any(),
             mk_port(0, 1, 1), PROTO_UDP);
    add_rule(ACT_ALLOW, 2, 1, mk_side_any(), mk_side_any(),
             mk_port(0, 2, 1), PROTO_TCP | PROTO_UDP);

    rule_node *r0 = rule_list_head();
    rule_node *r1 = r0->next;
    ASSERT_EQ_INT(r0->protos, PROTO_UDP);
    ASSERT_EQ_INT(r1->protos, (unsigned)(PROTO_TCP | PROTO_UDP));
}

void suite_rule(void) {
    RUN(any_to_any_populates_sel_all_and_sentinels);
    RUN(selector_to_selector);
    RUN(bare_subnet_side_uses_sel_all);
    RUN(ids_are_zero_based_and_sequential);
    RUN(port_list_attached_in_order);
    RUN(protos_bitmask_round_trip);
}
