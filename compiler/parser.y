/*
 * parser.y — grammar for the policy DSL (inventory + policy sections).
 *
 * RULE SYNTAX (current):
 *   rule := action selector '->' selector ':' port_spec ':' proto_spec ';'
 *
 * Where:
 *   port_spec  := ANY
 *              |  port_item (',' port_item)*
 *   port_item  := NUMBER
 *              |  NUMBER '-' NUMBER     // range, expanded at parse time
 *              |  VAR_PORT               // @var, expanded inline
 *   proto_spec := ANY
 *              |  proto (',' proto)*
 *   proto      := 'TCP' | 'UDP'
 *
 * Both the port and proto sections are mandatory and introduced by ':'.
 * ANY is a top-level alternative — NOT a member of a list — so syntactically
 * there's no way to write ":ANY,443" or ":80-ANY". The grammar itself
 * enforces "ANY stands alone".
 *
 * Port 0 is the numeric equivalent of ANY in single-port position, but
 * illegal in lists and ranges. "ANY in a @var" desugars to port 0 so
 * the internal representation stays uniform.
 *
 * VARIABLE DEFINITIONS:
 *   @name = NUMBER ;                  // single port
 *   @name = NUMBER '-' NUMBER ;       // range
 *   @name = ANY ;                     // wildcard (== 0)
 *   @name = port_item (',' port_item)* ;    // list
 *
 * LABEL VALUES (unchanged semantics):
 *   value := NAME | IP | ANY         // ANY accepted here too so that
 *                                    // labels like `app:ANY` keep working
 *                                    // even though ANY is a keyword.
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

/* Expand an inclusive range [low..high] into a linked list of port_nodes,
 * all sharing the given (line, col). Validates bounds, the no-zero rule,
 * and the no-reversed rule. Returns a non-NULL placeholder on error so
 * parsing can continue. */
static port_node *build_port_range(int low, int high, int line, int col);

/* Validate a single standalone port (not in a list, not in a range). Port 0
 * is LEGAL here and means ANY. Other out-of-range values emit a diagnostic
 * but still produce a node so parsing continues. */
static port_node *build_port_one(int p, int line, int col);

/* Guard used when a port_item is appended to a multi-element list: port 0
 * is forbidden in that context. Emits a diagnostic if it sees one. */
static void forbid_any_in_list(port_node *list, int line, int col);

/* Guard used on a @var's expanded port list: port 0 is only legal if it's
 * the SOLE node in the list. Emits a diagnostic otherwise. */
static void validate_var_port_list(port_node *list, int line, int col);
%}

%locations
%define parse.error verbose

%union {
    char        *str;
    int          num;
    ip_node     *iplist;
    label_node  *label;
    sel_node    *sel;
    port_node   *ports;
    action_kind  act;
    unsigned     protos;
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

/* RHS of a @var definition. Handles:
 *   @v = ANY ;              -> single port_node with port=0
 *   @v = 80 ;               -> single port_node (via port_item)
 *   @v = 80-443 ;           -> expanded range
 *   @v = 80,443,8000-8080 ; -> comma-separated list of items
 * No brackets — those were the old syntax and are gone. */
var_port_value
    : ANY
        {
            $$ = mk_port(0, @1.first_line, @1.first_column);
        }
    | port_list
        { $$ = $1; }
    ;

/* A single label ref — just a key:value pair (no $var nesting). */
label_ref_item
    : NAME ':' value
        {
            $$ = mk_label($1, $3, @1.first_line, @1.first_column);
            free($1); free($3);
        }
    ;

/* A whitespace-separated list of label refs (used only inside [...] in
 * variable definitions). No commas — matches the requested syntax. */
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

/* A label value can be a NAME, an IP (rare but legal), or the keyword ANY.
 * The ANY case lets label values like `app:ANY` keep working even though
 * ANY is a keyword token — the grammar's context disambiguates. */
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

/* Updated rule syntax:  action src '->' dst ':' port_spec ':' proto_spec ';' */
rule
    : action selector ARROW selector rule_ports rule_protos ';'
        { add_rule($1, @1.first_line, @1.first_column, $2, $4, $5, $6); }
    ;

/* The port section, introduced by ':'. ANY is a top-level alternative —
 * the grammar itself prevents ":ANY,443" because ANY is not a port_item. */
rule_ports
    : ':' port_spec             { $$ = $2; }
    | ':' ANY                   { $$ = mk_port(0, @2.first_line, @2.first_column); }
    ;

rule_protos
    : ':' proto_spec            { $$ = $2; }
    | ':' ANY                   { $$ = PROTO_TCP | PROTO_UDP; }
    ;

/* port_spec: a comma-separated list of port_items. A single port_item
 * (just a NUMBER, a NUMBER-NUMBER range, or a @var) is also valid. */
port_spec
    : port_list                 { $$ = $1; }
    ;

port_list
    : port_item                 { $$ = $1; }
    | port_list ',' port_item
        {
            /* About to append — forbid 0 on either side. */
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
                /* Placeholder so parsing continues cleanly. */
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

%%

/* ---------------------------------------------------------------- *
 *  Parser-local helpers                                            *
 * ---------------------------------------------------------------- */

/* Upper bound on how many ports a single range may expand to. Keeps a
 * typo like "1-65535" from silently allocating 65k nodes — users who
 * actually want "any port" should write :ANY. Matches MAX_RULES as a
 * round, easy-to-remember cap. */
#define MAX_RANGE_EXPANSION 4096

static port_node *build_port_one(int p, int line, int col) {
    /* Port 0 is legal here (means ANY in single-port position). Other
     * out-of-range values get a diagnostic but still produce a node so
     * parsing continues — add_rule's second-pass check won't double-fire
     * because the value is clamped into range by this function. */
    if (p < 0 || p > 65535) {
        diag_error(line, col,
            "port %d out of range (must be 0..65535, where 0 = ANY)", p);
        g_semantic_errors++;
        p = 1;  /* placeholder, but legal so we don't cascade */
    }
    return mk_port(p, line, col);
}

static port_node *build_port_range(int low, int high, int line, int col) {
    /* Order of checks matters for clean diagnostics: zero first (most
     * specific message), then reversal, then bounds. */
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

    /* Degenerate low==high is fine — a one-element list. */
    port_node *head = NULL;
    for (int p = low; p <= high; p++) {
        head = port_append(head, mk_port(p, line, col));
    }
    return head;
}

/* Walk a port list; if any node has port==0, emit the combine error
 * pointing at the offending new element's location. */
static void forbid_any_in_list(port_node *list, int line, int col) {
    for (port_node *p = list; p; p = p->next) {
        if (p->port == 0) {
            diag_error(line, col,
                "port 0 (ANY) cannot be combined with other ports; "
                "use ':ANY' alone");
            g_semantic_errors++;
            return;  /* one diagnostic per list combination is enough */
        }
    }
}

/* A @var's expanded port list may contain a single 0 (meaning wildcard)
 * or any number of non-zero ports. "0 mixed with anything" is illegal. */
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

void yyerror(const char *msg) {
    diag_error(yylloc.first_line, yylloc.first_column, "%s", msg);
}
