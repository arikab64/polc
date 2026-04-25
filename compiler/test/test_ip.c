/*
 * test_ip.c — IP parsing/formatting and the ip_node list helpers.
 */
#include "test.h"
#include "../ast.h"

/* ---------- ip_parse ----------------------------------------------- */

static void ip_parse_basic(void) {
    uint32_t out = 0;
    ASSERT_EQ_INT(ip_parse("10.0.0.1", 1, 1, &out), 1);
    ASSERT_EQ_U64(out, 0x0a000001ULL);
}

static void ip_parse_zero(void) {
    uint32_t out = 0xdeadbeef;
    ASSERT_EQ_INT(ip_parse("0.0.0.0", 1, 1, &out), 1);
    ASSERT_EQ_U64(out, 0ULL);
}

static void ip_parse_max(void) {
    uint32_t out = 0;
    ASSERT_EQ_INT(ip_parse("255.255.255.255", 1, 1, &out), 1);
    ASSERT_EQ_U64(out, 0xffffffffULL);
}

static void ip_parse_overflow_octet(void) {
    /* "256.0.0.0" — first octet exceeds 255. Out must be unchanged. */
    uint32_t out = 0xdeadbeef;
    ASSERT_EQ_INT(ip_parse("256.0.0.0", 1, 1, &out), 0);
    ASSERT_EQ_U64(out, 0xdeadbeefULL);
}

static void ip_parse_too_few_octets(void) {
    uint32_t out = 0xdeadbeef;
    ASSERT_EQ_INT(ip_parse("1.2.3", 1, 1, &out), 0);
    ASSERT_EQ_U64(out, 0xdeadbeefULL);
}

static void ip_parse_trailing_garbage(void) {
    uint32_t out = 0xdeadbeef;
    ASSERT_EQ_INT(ip_parse("1.2.3.4x", 1, 1, &out), 0);
    ASSERT_EQ_U64(out, 0xdeadbeefULL);
}

/* ---------- ip_fmt ------------------------------------------------- */

static void ip_fmt_basic(void) {
    char buf[16];
    ASSERT_EQ_PTR(ip_fmt(0x0a000001u, buf), buf);
    ASSERT_EQ_STR(buf, "10.0.0.1");
}

static void ip_fmt_zero(void) {
    char buf[16];
    ip_fmt(0u, buf);
    ASSERT_EQ_STR(buf, "0.0.0.0");
}

static void ip_fmt_max(void) {
    char buf[16];
    ip_fmt(0xffffffffu, buf);
    ASSERT_EQ_STR(buf, "255.255.255.255");
}

static void ip_fmt_roundtrip(void) {
    /* Parse → format → parse round-trip preserves the value. */
    uint32_t a = 0;
    ASSERT_EQ_INT(ip_parse("172.16.254.1", 1, 1, &a), 1);
    char buf[16];
    ip_fmt(a, buf);
    uint32_t b = 0;
    ASSERT_EQ_INT(ip_parse(buf, 1, 1, &b), 1);
    ASSERT_EQ_U64(a, b);
}

/* ---------- mk_ip / ip_append -------------------------------------- */

static void mk_ip_valid(void) {
    ip_node *n = mk_ip("10.0.0.1", 7, 13);
    ASSERT_NOT_NULL(n);
    ASSERT_EQ_INT(n->valid, 1);
    ASSERT_EQ_U64(n->addr, 0x0a000001ULL);
    ASSERT_EQ_INT(n->line, 7);
    ASSERT_EQ_INT(n->col,  13);
    ASSERT_NULL(n->next);
    free(n);
}

static void mk_ip_invalid(void) {
    /* Octet > 255 → valid=0. The struct still gets allocated so the
     * parser can keep collecting following IPs and report position. */
    ip_node *n = mk_ip("999.0.0.0", 1, 1);
    ASSERT_NOT_NULL(n);
    ASSERT_EQ_INT(n->valid, 0);
    free(n);
}

static void ip_append_grows_list(void) {
    ip_node *a = mk_ip("10.0.0.1", 1, 1);
    ip_node *b = mk_ip("10.0.0.2", 1, 1);
    ip_node *c = mk_ip("10.0.0.3", 1, 1);
    ip_node *head = ip_append(NULL, a);
    head = ip_append(head, b);
    head = ip_append(head, c);

    ASSERT_EQ_PTR(head, a);
    ASSERT_EQ_PTR(head->next, b);
    ASSERT_EQ_PTR(head->next->next, c);
    ASSERT_NULL(head->next->next->next);

    free(a); free(b); free(c);
}

static void ip_append_to_null_returns_node(void) {
    ip_node *n = mk_ip("10.0.0.1", 1, 1);
    ip_node *head = ip_append(NULL, n);
    ASSERT_EQ_PTR(head, n);
    free(n);
}

void suite_ip(void) {
    RUN(ip_parse_basic);
    RUN(ip_parse_zero);
    RUN(ip_parse_max);
    RUN(ip_parse_overflow_octet);
    RUN(ip_parse_too_few_octets);
    RUN(ip_parse_trailing_garbage);
    RUN(ip_fmt_basic);
    RUN(ip_fmt_zero);
    RUN(ip_fmt_max);
    RUN(ip_fmt_roundtrip);
    RUN(mk_ip_valid);
    RUN(mk_ip_invalid);
    RUN(ip_append_grows_list);
    RUN(ip_append_to_null_returns_node);
}
