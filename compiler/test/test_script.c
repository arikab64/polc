/*
 * test_script.c — end-to-end script tests.
 *
 * Each test feeds a .gc source string to the parser via fmemopen +
 * yyparse, then asserts on the resulting AST counts: EIDs, members,
 * rules, label table size, $var / @var counts, semantic error count.
 *
 * Forking gives us the rest: every parse runs in its own process with
 * fresh globals, so we never need to call free_all() between tests.
 */
#include "test.h"
#include "../ast.h"

#include <stdio.h>
#include <string.h>

extern int   yyparse(void);
extern FILE *yyin;

/* Parse a source string. On fmemopen failure the test crashes the child
 * (caught by the runner). yyparse's return code is the parser's view of
 * "did the grammar succeed"; semantic errors come through g_semantic_errors. */
static int parse_string(const char *src) {
    /* fmemopen takes a non-const buffer in its prototype but treats it as
     * read-only when the mode is "r". The cast is safe and isolated to
     * this helper. */
    yyin = fmemopen((void *)src, strlen(src), "r");
    if (!yyin) { perror("fmemopen"); _exit(1); }
    int rc = yyparse();
    fclose(yyin);
    return rc;
}

/* ---------- count helpers ----------------------------------------- */

static int count_eids(void) {
    int n = 0;
    for (eid_node *e = eid_list_head(); e; e = e->next) n++;
    return n;
}

static int count_members(const eid_node *e) {
    int n = 0;
    for (const member_node *m = e->members; m; m = m->next) n++;
    return n;
}

static int count_rules(void) {
    int n = 0;
    for (rule_node *r = rule_list_head(); r; r = r->next) n++;
    return n;
}

static int count_labels(void) {
    int n = 0;
    for (label_entry *e = label_list_head(); e; e = e->next) n++;
    return n;
}

static int count_var_labels(void) {
    int n = 0;
    for (var_label_entry *e = var_label_head(); e; e = e->next) n++;
    return n;
}

static int count_var_ports(void) {
    int n = 0;
    for (var_port_entry *e = var_port_head(); e; e = e->next) n++;
    return n;
}

static int count_ports(const port_node *p) {
    int n = 0;
    for (; p; p = p->next) n++;
    return n;
}

/* =================================================================
 *  Tests
 * ================================================================= */

static void empty_source(void) {
    /* Empty file is a legal program (all sections optional). */
    int rc = parse_string("");
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(count_eids(),       0);
    ASSERT_EQ_INT(count_rules(),      0);
    ASSERT_EQ_INT(count_labels(),     0);
    ASSERT_EQ_INT(count_var_labels(), 0);
    ASSERT_EQ_INT(count_var_ports(),  0);
    ASSERT_EQ_INT(g_semantic_errors,  0);
}

static void only_comments(void) {
    /* Both comment styles are recognized; nothing else exists. */
    parse_string("# shell comment\n"
                 "// c-style comment\n");
    ASSERT_EQ_INT(count_eids(),  0);
    ASSERT_EQ_INT(count_rules(), 0);
    ASSERT_EQ_INT(g_semantic_errors, 0);
}

/* ---------- vars: ------------------------------------------------- */

static void vars_count(void) {
    parse_string(
        "vars:\n"
        "  $production = env:production;\n"
        "  $webApps    = [ app:web app:api ];\n"
        "  @http       = 80, 8080;\n"
        "  @ssh        = 22;\n");

    ASSERT_EQ_INT(count_var_labels(), 2);
    ASSERT_EQ_INT(count_var_ports(),  2);
    ASSERT_EQ_INT(count_eids(),       0);
    ASSERT_EQ_INT(count_rules(),      0);
    /* $var defines call mk_label which interns the labels into the
     * global table — three distinct (key,val) pairs so far. */
    ASSERT_EQ_INT(count_labels(), 3);
    ASSERT_EQ_INT(g_semantic_errors, 0);
}

static void var_redefinition_is_error(void) {
    /* Spec §3: redefinition is an error. Label values must be NAMEs
     * (or IP / ANY) per §4 — bare numbers don't lex as label values. */
    parse_string(
        "vars:\n"
        "  $x = a:one;\n"
        "  $x = a:two;\n");
    ASSERT_TRUE(g_semantic_errors > 0);
}

/* ---------- inventory: ------------------------------------------- */

static void inventory_two_distinct_entities_two_eids(void) {
    parse_string(
        "inventory:\n"
        "  alpha [10.0.0.1] => [ app:web ];\n"
        "  beta  [10.0.0.2] => [ app:db  ];\n");

    ASSERT_EQ_INT(count_eids(),  2);
    ASSERT_EQ_INT(count_labels(), 2);     /* app:web, app:db */
    ASSERT_EQ_INT(g_semantic_errors, 0);
    /* Each EID should have exactly one member. */
    for (eid_node *e = eid_list_head(); e; e = e->next)
        ASSERT_EQ_INT(count_members(e), 1);
}

static void inventory_same_labels_collapse(void) {
    /* Two entities with identical label sets share an EID — one EID
     * with two members, IPs unioned. */
    parse_string(
        "inventory:\n"
        "  a1 [10.0.0.1] => [ app:web env:prod ];\n"
        "  a2 [10.0.0.2] => [ app:web env:prod ];\n");

    ASSERT_EQ_INT(count_eids(), 1);
    ASSERT_EQ_INT(count_members(eid_list_head()), 2);
    /* Labels are dedup'd — two unique pairs. */
    ASSERT_EQ_INT(count_labels(), 2);
    ASSERT_EQ_INT(g_semantic_errors, 0);
}

static void inventory_label_order_does_not_matter(void) {
    /* {app:web, env:prod} and {env:prod, app:web} are the same bitset. */
    parse_string(
        "inventory:\n"
        "  a [10.0.0.1] => [ app:web env:prod ];\n"
        "  b [10.0.0.2] => [ env:prod app:web ];\n");
    ASSERT_EQ_INT(count_eids(), 1);
    ASSERT_EQ_INT(count_members(eid_list_head()), 2);
}

static void inventory_three_entities_two_eids(void) {
    parse_string(
        "inventory:\n"
        "  a [10.0.0.1] => [ app:web ];\n"
        "  b [10.0.0.2] => [ app:web ];\n"
        "  c [10.0.0.3] => [ app:db  ];\n");

    ASSERT_EQ_INT(count_eids(), 2);
    /* Member distribution: 2 + 1, in declaration order. */
    eid_node *first  = eid_list_head();
    eid_node *second = first->next;
    ASSERT_EQ_INT(count_members(first),  2);
    ASSERT_EQ_INT(count_members(second), 1);
}

static void inventory_multi_ip_entity_is_one_member(void) {
    /* Multi-IP entity: still ONE member node with an ip_node list. */
    parse_string(
        "inventory:\n"
        "  cluster [10.0.0.1 10.0.0.2 10.0.0.3] => [ app:web ];\n");
    ASSERT_EQ_INT(count_eids(), 1);
    ASSERT_EQ_INT(count_members(eid_list_head()), 1);
    /* Walk the IP list and count. */
    int ips = 0;
    for (ip_node *p = eid_list_head()->members->ips; p; p = p->next) ips++;
    ASSERT_EQ_INT(ips, 3);
}

static void inventory_duplicate_ip_is_error(void) {
    parse_string(
        "inventory:\n"
        "  a [10.0.0.1] => [ app:web ];\n"
        "  b [10.0.0.1] => [ app:db  ];\n");
    ASSERT_TRUE(g_semantic_errors > 0);
}

/* ---------- policy: ---------------------------------------------- */

static void policy_simple_rule_count(void) {
    parse_string(
        "inventory:\n"
        "  a [10.0.0.1] => [ app:web ];\n"
        "  b [10.0.0.2] => [ app:db  ];\n"
        "policy:\n"
        "  ALLOW app:web -> app:db :80 :TCP;\n"
        "  ALLOW app:web -> app:db :443 :TCP;\n");

    ASSERT_EQ_INT(count_rules(), 2);
    /* Rule ids are 0-based and sequential. */
    rule_node *r0 = rule_list_head();
    rule_node *r1 = r0->next;
    ASSERT_EQ_INT(r0->id, 0);
    ASSERT_EQ_INT(r1->id, 1);
}

static void policy_port_range_expands(void) {
    /* :8000-8080 expands into 81 individual port_nodes. */
    parse_string(
        "inventory:\n"
        "  a [10.0.0.1] => [ app:web ];\n"
        "policy:\n"
        "  ALLOW ANY -> ANY :8000-8080 :TCP;\n");

    ASSERT_EQ_INT(count_rules(), 1);
    ASSERT_EQ_INT(count_ports(rule_list_head()->ports), 81);
}

static void policy_var_label_expansion(void) {
    /* $webApps = [a:1 b:2 c:3] used as a side becomes an OR-tree of
     * three leaves — but it's still one rule. */
    parse_string(
        "vars:\n"
        "  $webApps = [ app:web app:api app:gateway ];\n"
        "inventory:\n"
        "  e [10.0.0.1] => [ app:web ];\n"
        "policy:\n"
        "  ALLOW $webApps -> ANY :80 :TCP;\n");

    ASSERT_EQ_INT(count_rules(), 1);
    rule_node *r = rule_list_head();
    /* Top-level src is OR(leaf, OR(leaf, leaf)). */
    ASSERT_EQ_INT(r->src->kind, SEL_OR);
    ASSERT_EQ_INT(r->src->lhs->kind, SEL_LEAF);
    ASSERT_EQ_INT(r->src->rhs->kind, SEL_OR);
}

static void policy_var_port_expansion(void) {
    /* :@http where @http = 80, 8080 — two ports attached to the rule. */
    parse_string(
        "vars:\n"
        "  @http = 80, 8080;\n"
        "inventory:\n"
        "  e [10.0.0.1] => [ app:web ];\n"
        "policy:\n"
        "  ALLOW ANY -> ANY :@http :TCP;\n");

    ASSERT_EQ_INT(count_rules(), 1);
    ASSERT_EQ_INT(count_ports(rule_list_head()->ports), 2);
}

static void policy_proto_any_expands_to_tcp_udp(void) {
    parse_string(
        "policy:\n"
        "  ALLOW ANY -> ANY :80 :ANY;\n");
    ASSERT_EQ_INT(count_rules(), 1);
    ASSERT_EQ_INT(rule_list_head()->protos,
                  (unsigned)(PROTO_TCP | PROTO_UDP));
}

static void policy_any_side_uses_sel_all(void) {
    /* Bare ANY on src: rule_node->src is the SEL_ALL sentinel, not NULL. */
    parse_string(
        "policy:\n"
        "  ALLOW ANY -> ANY :80 :TCP;\n");
    rule_node *r = rule_list_head();
    ASSERT_NOT_NULL(r->src);
    ASSERT_EQ_INT(r->src->kind, SEL_ALL);
    ASSERT_NOT_NULL(r->dst);
    ASSERT_EQ_INT(r->dst->kind, SEL_ALL);
}

/* ---------- combined ---------------------------------------------- */

static void full_program_counts(void) {
    /* Exercises all three sections at once. Counts are pinned so any
     * change in how vars/labels/EIDs/rules are accounted for trips
     * this test. */
    parse_string(
        "vars:\n"
        "  $production = env:production;\n"
        "  $webApps    = [ app:web app:api ];\n"
        "  @http       = 80, 8080;\n"
        "  @ssh        = 22;\n"
        "inventory:\n"
        "  web1 [10.0.0.1] => [ app:web $production ];\n"
        "  web2 [10.0.0.2] => [ app:web $production ];\n"
        "  api1 [10.0.0.3] => [ app:api $production ];\n"
        "  db1  [10.0.0.4] => [ app:db  $production ];\n"
        "policy:\n"
        "  ALLOW $webApps -> app:db :@http :TCP;\n"
        "  ALLOW app:api  -> app:db :@ssh  :TCP;\n");

    ASSERT_EQ_INT(g_semantic_errors, 0);
    ASSERT_EQ_INT(count_var_labels(), 2);
    ASSERT_EQ_INT(count_var_ports(),  2);
    /* Distinct (key,val) pairs: env:production, app:web, app:api, app:db
     * = 4 labels in the table. */
    ASSERT_EQ_INT(count_labels(), 4);
    /* EIDs: web1+web2 collapse → 3 EIDs. */
    ASSERT_EQ_INT(count_eids(), 3);
    ASSERT_EQ_INT(count_rules(), 2);
}

/* ---------- diagnostics ------------------------------------------- */

static void undefined_label_var_is_error(void) {
    parse_string(
        "policy:\n"
        "  ALLOW $undefined -> ANY :80 :TCP;\n");
    ASSERT_TRUE(g_semantic_errors > 0);
}

static void undefined_port_var_is_error(void) {
    parse_string(
        "policy:\n"
        "  ALLOW ANY -> ANY :@undefined :TCP;\n");
    ASSERT_TRUE(g_semantic_errors > 0);
}

static void duplicate_label_key_in_entity_is_error(void) {
    parse_string(
        "inventory:\n"
        "  a [10.0.0.1] => [ app:web app:api ];\n");
    ASSERT_TRUE(g_semantic_errors > 0);
}

void suite_script(void) {
    RUN(empty_source);
    RUN(only_comments);
    RUN(vars_count);
    RUN(var_redefinition_is_error);
    RUN(inventory_two_distinct_entities_two_eids);
    RUN(inventory_same_labels_collapse);
    RUN(inventory_label_order_does_not_matter);
    RUN(inventory_three_entities_two_eids);
    RUN(inventory_multi_ip_entity_is_one_member);
    RUN(inventory_duplicate_ip_is_error);
    RUN(policy_simple_rule_count);
    RUN(policy_port_range_expands);
    RUN(policy_var_label_expansion);
    RUN(policy_var_port_expansion);
    RUN(policy_proto_any_expands_to_tcp_udp);
    RUN(policy_any_side_uses_sel_all);
    RUN(full_program_counts);
    RUN(undefined_label_var_is_error);
    RUN(undefined_port_var_is_error);
    RUN(duplicate_label_key_in_entity_is_error);
}
