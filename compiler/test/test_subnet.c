/*
 * test_subnet.c — subnet_node constructors and the §6.3 sentinels.
 */
#include "test.h"
#include "../ast.h"
#include <stdlib.h>

static void free_subnet_local(subnet_node *s) {
    if (!s) return;
    if (s->kind == SN_AND || s->kind == SN_OR) {
        free_subnet_local(s->lhs);
        free_subnet_local(s->rhs);
    }
    free(s);
}

static void cidr_basic(void) {
    subnet_node *s = mk_subnet_cidr(0x0a000000u, 8, 4, 12);
    ASSERT_EQ_INT(s->kind, SN_CIDR);
    ASSERT_EQ_U64(s->addr, 0x0a000000ULL);
    ASSERT_EQ_INT(s->prefix, 8);
    ASSERT_EQ_INT(s->line, 4);
    ASSERT_EQ_INT(s->col,  12);
    ASSERT_NULL(s->lhs);
    ASSERT_NULL(s->rhs);
    free_subnet_local(s);
}

static void cidr_slash_32_default(void) {
    /* Plain IP without an explicit prefix → /32 is the parser's job;
     * here we just confirm mk_subnet_cidr stores whatever it's given. */
    subnet_node *s = mk_subnet_cidr(0x0a000001u, 32, 1, 1);
    ASSERT_EQ_INT(s->prefix, 32);
    free_subnet_local(s);
}

static void range_basic(void) {
    subnet_node *s = mk_subnet_range(0x0a000001u, 0x0a0000ffu, 6, 18);
    ASSERT_EQ_INT(s->kind, SN_RANGE);
    ASSERT_EQ_U64(s->range_lo, 0x0a000001ULL);
    ASSERT_EQ_U64(s->range_hi, 0x0a0000ffULL);
    ASSERT_EQ_INT(s->line, 6);
    ASSERT_EQ_INT(s->col,  18);
    free_subnet_local(s);
}

static void binop_and(void) {
    subnet_node *l = mk_subnet_cidr(0x0a000000u, 24, 3, 7);
    subnet_node *r = mk_subnet_cidr(0x0a000080u, 25, 9, 11);
    subnet_node *n = subnet_binop(SN_AND, l, r);
    ASSERT_EQ_INT(n->kind, SN_AND);
    ASSERT_EQ_PTR(n->lhs, l);
    ASSERT_EQ_PTR(n->rhs, r);
    /* Binop inherits position from lhs. */
    ASSERT_EQ_INT(n->line, 3);
    ASSERT_EQ_INT(n->col,  7);
    free_subnet_local(n);
}

static void binop_or(void) {
    subnet_node *l = mk_subnet_cidr(0x0a000000u, 24, 1, 1);
    subnet_node *r = mk_subnet_cidr(0x0b000000u, 24, 1, 1);
    subnet_node *n = subnet_binop(SN_OR, l, r);
    ASSERT_EQ_INT(n->kind, SN_OR);
    free_subnet_local(n);
}

/* ---------- §6.3 sentinels ----------------------------------------- */

static void and_any_sentinel(void) {
    /* and-ANY ≡ 0.0.0.1 .. 255.255.255.255, encoded as SN_RANGE because
     * no single CIDR captures "everything except 0.0.0.0". */
    subnet_node *s = mk_subnet_and_any();
    ASSERT_EQ_INT(s->kind, SN_RANGE);
    ASSERT_EQ_U64(s->range_lo, 0x00000001ULL);
    ASSERT_EQ_U64(s->range_hi, 0xffffffffULL);
    free_subnet_local(s);
}

static void or_any_sentinel(void) {
    /* or-ANY ≡ 0.0.0.0/32. The union-identity address; never matches a
     * real packet src/dst so it's effectively a no-op when ORed in. */
    subnet_node *s = mk_subnet_or_any();
    ASSERT_EQ_INT(s->kind, SN_CIDR);
    ASSERT_EQ_U64(s->addr, 0ULL);
    ASSERT_EQ_INT(s->prefix, 32);
    free_subnet_local(s);
}

static void sentinel_constructors_return_fresh_trees(void) {
    /* Each call must return a distinct allocation so callers can free
     * them individually without aliasing. */
    subnet_node *a = mk_subnet_and_any();
    subnet_node *b = mk_subnet_and_any();
    ASSERT_NE_PTR(a, b);
    free_subnet_local(a);
    free_subnet_local(b);

    subnet_node *c = mk_subnet_or_any();
    subnet_node *d = mk_subnet_or_any();
    ASSERT_NE_PTR(c, d);
    free_subnet_local(c);
    free_subnet_local(d);
}

void suite_subnet(void) {
    RUN(cidr_basic);
    RUN(cidr_slash_32_default);
    RUN(range_basic);
    RUN(binop_and);
    RUN(binop_or);
    RUN(and_any_sentinel);
    RUN(or_any_sentinel);
    RUN(sentinel_constructors_return_fresh_trees);
}
