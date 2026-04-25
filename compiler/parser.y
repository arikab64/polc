/*
 * parser.y — grammar for the policy DSL (vars + inventory + policy sections).
 *
 * RULE SYNTAX (current — LANGUAGE.md §5.1):
 *   rule := action side '->' side ':' port_spec ':' proto_spec ';'
 *
 * SIDE SYNTAX (LANGUAGE.md §5.2 / §7):
 *   side          := ANY
 *                 |  selector [ ('AND' | 'OR') subnet_clause ]
 *                 |  subnet_expr
 *
 *   selector      := and_expr ( 'OR'  and_expr )*
 *   and_expr      := primary  ( 'AND' primary )*
 *   primary       := NAME ':' value
 *                 |  VAR_LABEL
 *                 |  '(' selector ')'
 *                 |  '[' label_ref+ ']'
 *
 *   subnet_clause := cidr | '(' subnet_expr ')'
 *   subnet_expr   := sub_and ( 'OR'  sub_and )*
 *   sub_and       := sub_prim ( 'AND' sub_prim )*
 *   sub_prim      := cidr | '(' subnet_expr ')'
 *
 *   cidr          := IP [ '/' NUMBER ]            ; default prefix is /32
 *
 * Each side compiles to the (L, S_and, S_or) triple of LANGUAGE.md §5.2.
 * Where the source omits a clause the parser MATERIALIZES the §6.3
 * sentinel (and-ANY = 0.0.0.1..255.255.255.255, or-ANY = 0.0.0.0/32)
 * so downstream phases never have to special-case NULL.
 *
 * Side-form → triple mapping (table from §5.2):
 *
 *   ANY                    → L=ALL_EIDS,  S_and=and-ANY, S_or=or-ANY
 *   selector               → L=selector,  S_and=and-ANY, S_or=or-ANY
 *   selector AND subnet    → L=selector,  S_and=subnet,  S_or=or-ANY
 *   selector OR  subnet    → L=selector,  S_and=and-ANY, S_or=subnet
 *   subnet                 → L=ALL_EIDS,  S_and=subnet,  S_or=or-ANY
 *
 * "ALL_EIDS" is encoded as a SEL_ALL sentinel sel_node — every rule_side
 * carries a real selector, so downstream phases dispatch uniformly on
 * sel->kind without a separate "no selector" flag.
 *
 * AMBIGUITY NOTE — top-level AND/OR between selector and subnet:
 *   "app:db AND 10.0.0.0/16" — at the boundary, AND/OR can either be
 *   the selector's outer operator or the side-level join. Disambiguation
 *   by single-token lookahead works ONLY when the RHS starts with IP:
 *   selector primaries cannot start with IP, so `selector AND <IP>` is
 *   unambiguously the side-level join.
 *
 *   When the RHS is parenthesised — `selector AND (...)` — single-token
 *   lookahead can't tell whether the parens hold a selector subexpression
 *   or a subnet_expr. Bison resolves this with its default
 *   shift-over-reduce, which (because the side-level AND/OR arms are
 *   listed AFTER the bare-selector arm and inner parens are also a
 *   primary in the selector grammar) prefers extending the selector.
 *
 *   Concrete consequence: the §9.3 example
 *
 *     ALLOW app:api-backend AND (10.0.2.20 OR 10.0.2.21) -> ... ;
 *
 *   parses as `selector AND (selector-primary)`, and the parser will
 *   then fail when "10.0.2.20" is offered as a primary. To write a
 *   parenthesised subnet attached to a selector, the user must move
 *   the AND/OR INSIDE the parens — or we extend the lexer to emit a
 *   distinct `IP_CIDR` token so the subnet path can be opened on
 *   first-token lookahead. The latter is the cleaner fix and is
 *   tracked as a follow-up; the present grammar accepts everything
 *   else from §9 and §5.2.
 *
 * VARIABLE / PORT / PROTO sections are unchanged from the previous
 * revision; included here verbatim for a complete file.
 */

%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "diag.h"

int  yylex(void);
extern int yylineno;
void yyerror(const char *msg);

/* ---- parser-local helpers (defined below the grammar) ---------------- */
static port_node   *build_port_one  (int p, int line, int col);
static port_node   *build_port_range(int low, int high, int line, int col);
static void         forbid_any_in_list(port_node *list, int line, int col);
static void         validate_var_port_list(port_node *list, int line, int col);

/* CIDR helpers — defined below the grammar. */
static subnet_node *build_cidr(const char *ip_text, int prefix,
                               int prefix_given, int line, int col);
%}

%locations
%define parse.error verbose

%union {
    char         *str;
    int           num;
    ip_node      *iplist;
    label_node   *label;
    sel_node     *sel;
    subnet_node  *subnet;
    rule_side    *side;
    port_node    *ports;
    action_kind   act;
    unsigned      protos;
}

/* ---- terminals ---- */
%token <str> NAME IP VAR_LABEL VAR_PORT
%token <num> NUMBER

%token FAT_ARROW ARROW
%token INVENTORY POLICY VARS
%token ALLOW BLOCK OVERRIDE_ALLOW OVERRIDE_BLOCK
%token AND OR
%token TCP UDP
%token ANY

/* ---- nonterminal types ---- */
%type <iplist> ip_list
%type <label>  label label_list label_or_var label_ref_item label_ref_list sel_list
%type <str>    value
%type <act>    action
%type <sel>    selector or_expr and_expr primary
%type <subnet> subnet_clause subnet_expr sub_and sub_prim cidr
%type <side>   side
%type <ports>  port_spec port_list port_item rule_ports var_port_value
%type <protos> proto_spec proto_list proto rule_protos

%%

/* --------------------------------------------------------------- top */

file
    : /* empty */
    | file section
    ;

section
    : INVENTORY ':' entity_list
    | POLICY    ':' rule_list
    | VARS      ':' var_list
    ;

/* ----------------------------------------------------------- vars */

var_list
    : /* empty */
    | var_list var_def
    ;

var_def
    : VAR_LABEL '=' label_ref_item ';'
        {
            var_label_define($1, $3, @1.first_line, @1.first_column);
            free($1);
        }
    | VAR_LABEL '=' '[' label_ref_list ']' ';'
        {
            var_label_define($1, $4, @1.first_line, @1.first_column);
            free($1);
        }
    | VAR_PORT  '=' var_port_value ';'
        {
            validate_var_port_list($3, @1.first_line, @1.first_column);
            var_port_define($1, $3, @1.first_line, @1.first_column);
            free($1);
        }
    ;

var_port_value
    : ANY            { $$ = mk_port(0, @1.first_line, @1.first_column); }
    | port_list      { $$ = $1; }
    ;

label_ref_item
    : NAME ':' value
        {
            $$ = mk_label($1, $3, @1.first_line, @1.first_column);
            free($1); free($3);
        }
    ;

label_ref_list
    : label_ref_item                    { $$ = $1; }
    | label_ref_list label_ref_item     { $$ = label_append($1, $2); }
    ;

/* ---------------------------------------------------------- inventory */

entity_list
    : /* empty */
    | entity_list entity
    ;

entity
    : NAME '[' ip_list ']' FAT_ARROW '[' label_list ']' ';'
        { add_entity($1, @1.first_line, @1.first_column, $3, $7); free($1); }
    ;

ip_list
    : IP                 { $$ = mk_ip($1, @1.first_line, @1.first_column); free($1); }
    | ip_list IP         { $$ = ip_append($1, mk_ip($2, @2.first_line, @2.first_column)); free($2); }
    ;

label_list
    : label_or_var                      { $$ = $1; }
    | label_list label_or_var           { $$ = label_append($1, $2); }
    ;

label_or_var
    : label                             { $$ = $1; }
    | VAR_LABEL
        {
            $$ = var_label_lookup($1);
            if (!$$) {
                diag_error(@1.first_line, @1.first_column,
                    "undefined variable $%s", $1);
                g_semantic_errors++;
            }
            free($1);
        }
    ;

label
    : NAME ':' value            {
                                    $$ = mk_label($1, $3, @1.first_line, @1.first_column);
                                    free($1); free($3);
                                }
    ;

value
    : NAME                      { $$ = $1; }
    | IP                        { $$ = $1; }
    | ANY                       { $$ = strdup("ANY"); }
    ;

/* ------------------------------------------------------------- policy */

rule_list
    : /* empty */
    | rule_list rule
    ;

rule
    : action side ARROW side rule_ports rule_protos ';'
        { add_rule($1, @1.first_line, @1.first_column, $2, $4, $5, $6); }
    ;

/* ------------------------------------------------------------- side
 *
 * Five forms, each producing a fully-populated rule_side. Sentinels are
 * inserted for whichever clauses the source omitted, so add_rule can
 * always trust S_and and S_or to be non-NULL.
 *
 * Note the explicit `selector AND subnet_clause` / `selector OR
 * subnet_clause` arms: they pull AND/OR out of the selector grammar at
 * the point where the RHS is a CIDR. See header comment for why this
 * is unambiguous.
 */
side
    : ANY
        { $$ = mk_side_any(); }
    | selector
        { $$ = mk_side_sel($1); }
    | selector AND subnet_clause
        { $$ = mk_side_and($1, $3); }
    | selector OR  subnet_clause
        { $$ = mk_side_or ($1, $3); }
    | subnet_expr
        { $$ = mk_side_subn($1); }
    ;

/* ---------------------------------------------------------- selector */

selector
    : or_expr                   { $$ = $1; }
    ;

or_expr
    : and_expr                  { $$ = $1; }
    | or_expr OR and_expr       { $$ = sel_binop(SEL_OR,  $1, $3); }
    ;

and_expr
    : primary                   { $$ = $1; }
    | and_expr AND primary      { $$ = sel_binop(SEL_AND, $1, $3); }
    ;

primary
    : NAME ':' value            {
                                    $$ = sel_leaf($1, $3, @1.first_line, @1.first_column);
                                    free($1); free($3);
                                }
    | VAR_LABEL
        {
            label_node *labels = var_label_lookup($1);
            if (!labels) {
                diag_error(@1.first_line, @1.first_column,
                    "undefined variable $%s", $1);
                g_semantic_errors++;
                $$ = sel_leaf("<e>", $1, @1.first_line, @1.first_column);
            } else {
                $$ = sel_from_labels(labels);
            }
            free($1);
        }
    | '(' or_expr ')'           { $$ = $2; }
    | '[' sel_list ']'          { $$ = sel_from_labels($2); }
    ;

sel_list
    : label_or_var                      { $$ = $1; }
    | sel_list label_or_var             { $$ = label_append($1, $2); }
    ;

/* ------------------------------------------------------------ subnets
 *
 * Mirrors the selector shape: OR of ANDs of primaries, primaries are
 * either CIDRs or parenthesised subexpressions.
 *
 * `subnet_clause` (used after `selector AND/OR ...`) is the restricted
 * form: a single CIDR or a parenthesised subnet_expr. With single-token
 * lookahead, only the bare-CIDR arm here is reachable when this clause
 * follows a selector — see the AMBIGUITY NOTE in the file header. The
 * `(subnet_expr)` arm is kept so bare-subnet sides like
 *
 *     ALLOW (10.0.2.20 OR 10.0.2.21) -> ... ;
 *
 * still parse via the `subnet_expr` arm of `side`. The §9.3 example
 *
 *     ALLOW app:api-backend AND (10.0.2.20 OR 10.0.2.21) -> ... ;
 *
 * is the case that requires the lexer-level CIDR follow-up. */
subnet_clause
    : cidr                          { $$ = $1; }
    | '(' subnet_expr ')'           { $$ = $2; }
    ;

subnet_expr
    : sub_and                       { $$ = $1; }
    | subnet_expr OR sub_and        { $$ = subnet_binop(SN_OR, $1, $3); }
    ;

sub_and
    : sub_prim                      { $$ = $1; }
    | sub_and AND sub_prim          { $$ = subnet_binop(SN_AND, $1, $3); }
    ;

sub_prim
    : cidr                          { $$ = $1; }
    | '(' subnet_expr ')'           { $$ = $2; }
    ;

/* CIDR — IP, optionally followed by '/' NUMBER. Plain IP is /32. */
cidr
    : IP
        {
            $$ = build_cidr($1, 32, /*prefix_given=*/0,
                            @1.first_line, @1.first_column);
            free($1);
        }
    | IP '/' NUMBER
        {
            $$ = build_cidr($1, $3, /*prefix_given=*/1,
                            @1.first_line, @1.first_column);
            free($1);
        }
    ;

/* ----------------------------------------------------- ports & protos */

rule_ports
    : ':' port_spec             { $$ = $2; }
    | ':' ANY                   { $$ = mk_port(0, @2.first_line, @2.first_column); }
    ;

rule_protos
    : ':' proto_spec            { $$ = $2; }
    | ':' ANY                   { $$ = PROTO_TCP | PROTO_UDP; }
    ;

port_spec
    : port_list                 { $$ = $1; }
    ;

port_list
    : port_item                 { $$ = $1; }
    | port_list ',' port_item
        {
            forbid_any_in_list($1, @3.first_line, @3.first_column);
            forbid_any_in_list($3, @3.first_line, @3.first_column);
            $$ = port_append($1, $3);
        }
    ;

port_item
    : NUMBER
        { $$ = build_port_one($1, @1.first_line, @1.first_column); }
    | NUMBER '-' NUMBER
        { $$ = build_port_range($1, $3, @1.first_line, @1.first_column); }
    | VAR_PORT
        {
            port_node *pl = var_port_lookup($1);
            if (!pl) {
                diag_error(@1.first_line, @1.first_column,
                    "undefined variable @%s", $1);
                g_semantic_errors++;
                pl = mk_port(1, @1.first_line, @1.first_column);
            }
            free($1);
            $$ = pl;
        }
    ;

proto_spec
    : proto_list                { $$ = $1; }
    ;

proto_list
    : proto                     { $$ = $1; }
    | proto_list ',' proto      { $$ = $1 | $3; }
    ;

proto
    : TCP                       { $$ = PROTO_TCP; }
    | UDP                       { $$ = PROTO_UDP; }
    ;

action
    : ALLOW          { $$ = ACT_ALLOW; }
    | BLOCK          { $$ = ACT_BLOCK; }
    | OVERRIDE_ALLOW { $$ = ACT_OVERRIDE_ALLOW; }
    | OVERRIDE_BLOCK { $$ = ACT_OVERRIDE_BLOCK; }
    ;

%%

/* ---------------------------------------------------------------- *
 *  Parser-local helpers                                            *
 * ---------------------------------------------------------------- */

/* Upper bound on how many ports a single range may expand to. */
#define MAX_RANGE_EXPANSION 4096

static port_node *build_port_one(int p, int line, int col) {
    if (p < 0 || p > 65535) {
        diag_error(line, col,
            "port %d out of range (must be 0..65535, where 0 = ANY)", p);
        g_semantic_errors++;
        p = 1;
    }
    return mk_port(p, line, col);
}

static port_node *build_port_range(int low, int high, int line, int col) {
    if (low == 0 || high == 0) {
        diag_error(line, col,
            "port 0 is reserved for ANY and cannot appear in a range");
        g_semantic_errors++;
        return mk_port(1, line, col);
    }
    if (low > high) {
        diag_error(line, col,
            "port range %d-%d is reversed (low-high required)", low, high);
        g_semantic_errors++;
        return mk_port(1, line, col);
    }
    if (low < 1 || low > 65535) {
        diag_error(line, col,
            "port %d out of range (must be 1..65535)", low);
        g_semantic_errors++;
        return mk_port(1, line, col);
    }
    if (high < 1 || high > 65535) {
        diag_error(line, col,
            "port %d out of range (must be 1..65535)", high);
        g_semantic_errors++;
        return mk_port(1, line, col);
    }
    int count = high - low + 1;
    if (count > MAX_RANGE_EXPANSION) {
        diag_error(line, col,
            "port range %d-%d expands to %d ports (max %d); "
            "use :ANY for a full wildcard or narrow the range",
            low, high, count, MAX_RANGE_EXPANSION);
        g_semantic_errors++;
        return mk_port(low, line, col);
    }
    port_node *head = NULL;
    for (int p = low; p <= high; p++) {
        head = port_append(head, mk_port(p, line, col));
    }
    return head;
}

static void forbid_any_in_list(port_node *list, int line, int col) {
    for (port_node *p = list; p; p = p->next) {
        if (p->port == 0) {
            diag_error(line, col,
                "port 0 (ANY) cannot be combined with other ports; "
                "use ':ANY' alone");
            g_semantic_errors++;
            return;
        }
    }
}

static void validate_var_port_list(port_node *list, int line, int col) {
    int has_zero = 0, total = 0;
    for (port_node *p = list; p; p = p->next) {
        if (p->port == 0) has_zero = 1;
        total++;
    }
    if (has_zero && total > 1) {
        diag_error(line, col,
            "@var: port 0 (ANY) cannot be combined with other ports");
        g_semantic_errors++;
    }
}

/* ---------------------------------------------------------------- *
 *  CIDR construction                                               *
 * ---------------------------------------------------------------- */

/* Build a SN_CIDR node from textual IP + prefix.
 *
 *   prefix_given == 1  -> caller provided "/N", N is in `prefix`
 *   prefix_given == 0  -> implicit /32 (single host)
 *
 * Validates:
 *   • IP is parseable (delegated to ip_parse, which emits its own diag)
 *   • prefix is in 0..32
 *   • CIDR is canonical — host bits must be clear (W001 warning, not error)
 *
 * Always returns a non-NULL node so parsing continues; on parse failure
 * the placeholder is 0.0.0.0/32 which is the or-ANY identity (harmless).
 */
static subnet_node *build_cidr(const char *ip_text, int prefix,
                               int prefix_given, int line, int col)
{
    uint32_t addr = 0;
    int      ok   = ip_parse(ip_text, line, col, &addr);
    if (!ok) {
        /* ip_parse already emitted E?? for malformed octets. */
        return mk_subnet_cidr(0, 32, line, col);
    }

    if (prefix_given) {
        if (prefix < 0 || prefix > 32) {
            diag_error(line, col,
                "CIDR prefix /%d out of range (must be 0..32)", prefix);
            g_semantic_errors++;
            prefix = 32;
        }
    }

    /* Canonicalise: zero out host bits. If they were non-zero, that's
     * W001 — non-canonical CIDR — a warning, not an error. */
    uint32_t mask = (prefix == 0) ? 0u
                                  : (uint32_t)0xFFFFFFFFu << (32 - prefix);
    uint32_t net  = addr & mask;
    if (net != addr) {
        char buf_orig[16], buf_canon[16];
        diag_warning(line, col,
            "non-canonical CIDR %s/%d — host bits set; using %s/%d",
            ip_fmt(addr, buf_orig), prefix,
            ip_fmt(net,  buf_canon), prefix);
    }

    return mk_subnet_cidr(net, prefix, line, col);
}

void yyerror(const char *msg) {
    diag_error(yylloc.first_line, yylloc.first_column, "%s", msg);
}
