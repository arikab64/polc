/*
 * parser.y — grammar for the policy DSL (inventory + policy sections)
 *
 * File layout:
 *   file        := section*
 *   section     := 'inventory' ':' entity*
 *                | 'policy'    ':' rule*
 *
 * Entity (unchanged):
 *   entity      := NAME '[' ip_list ']' '=>' '[' label_list ']' ';'
 *   label_list  := label (',' label)*
 *   label       := NAME ':' value     // same shape as rule selector leaves
 *
 * Rule:
 *   rule        := action selector '->' selector '[' port_list ']' proto_list ';'
 *   action      := 'ALLOW' | 'BLOCK' | 'OVERRIDE-ALLOW' | 'OVERRIDE-BLOCK'
 *   selector    := or_expr
 *   or_expr     := and_expr ('OR'  and_expr)*
 *   and_expr    := primary  ('AND' primary )*
 *   primary     := NAME ':' value | '(' or_expr ')'
 *   port_list   := NUMBER+       (whitespace-separated)
 *   proto_list  := proto (',' proto)*
 *   proto       := 'TCP' | 'UDP'
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

/* ---- nonterminal types ---- */
%type <iplist> ip_list
%type <label>  label label_list label_or_var label_ref_item label_ref_list sel_list
%type <str>    value
%type <act>    action
%type <sel>    selector or_expr and_expr primary
%type <ports>  port_list port_or_var rule_ports
%type <protos> proto_list proto

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
            /* A single label item on the RHS — wrap it in a list for
             * var_label_define. $1 includes the '$' stripped by scanner. */
            var_label_define($1, $3, @1.first_line, @1.first_column);
            free($1);
        }
    | VAR_LABEL '=' '[' label_ref_list ']' ';'
        {
            var_label_define($1, $4, @1.first_line, @1.first_column);
            free($1);
        }
    | VAR_PORT  '=' NUMBER ';'
        {
            var_port_define($1,
                            mk_port($3, @3.first_line, @3.first_column),
                            @1.first_line, @1.first_column);
            free($1);
        }
    | VAR_PORT  '=' '[' port_list ']' ';'
        {
            var_port_define($1, $4, @1.first_line, @1.first_column);
            free($1);
        }
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

/* Either a literal k:v pair or a $var expansion. Returns a label_node list
 * (a single leaf for literals, a multi-node list for $var expansions). */
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
    ;

/* ------------------------------------------------------------- policy */

rule_list
    : /* empty */
    | rule_list rule
    ;

rule
    : action selector ARROW selector rule_ports proto_list ';'
        { add_rule($1, @1.first_line, @1.first_column, $2, $4, $5, $6); }
    ;

/* Ports in a rule. Brackets are only required when there's more than one
 * element. Single items — either a NUMBER or an @var — stand alone. */
rule_ports
    : NUMBER                            { $$ = mk_port($1, @1.first_line, @1.first_column); }
    | VAR_PORT
        {
            $$ = var_port_lookup($1);
            if (!$$) {
                diag_error(@1.first_line, @1.first_column,
                    "undefined variable @%s", $1);
                g_semantic_errors++;
            }
            free($1);
        }
    | '[' port_list ']'                 { $$ = $2; }
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
                /* Fabricate a placeholder leaf so the rest of the rule
                 * still parses; it won't match anything at bag-build time. */
                $$ = sel_leaf("<error>", $1, @1.first_line, @1.first_column);
            } else {
                $$ = sel_from_labels(labels);   /* consumes labels */
            }
            free($1);
        }
    | '(' or_expr ')'           { $$ = $2; }
    | '[' sel_list ']'          { $$ = sel_from_labels($2); }
    ;

/* Whitespace-separated list of label refs inside a selector '[...]'. Each
 * element may be a literal k:v pair or a $var (which expands into multiple
 * labels). The whole list becomes an OR-tree via sel_from_labels. */
sel_list
    : label_or_var                      { $$ = $1; }
    | sel_list label_or_var             { $$ = label_append($1, $2); }
    ;

port_list
    : port_or_var                       { $$ = $1; }
    | port_list port_or_var             { $$ = port_append($1, $2); }
    ;

port_or_var
    : NUMBER                            { $$ = mk_port($1, @1.first_line, @1.first_column); }
    | VAR_PORT
        {
            $$ = var_port_lookup($1);
            if (!$$) {
                diag_error(@1.first_line, @1.first_column,
                    "undefined variable @%s", $1);
                g_semantic_errors++;
            }
            free($1);
        }
    ;

proto_list
    : proto                     { $$ = $1; }
    | proto_list ',' proto      { $$ = $1 | $3; }
    ;

proto
    : TCP                       { $$ = PROTO_TCP; }
    | UDP                       { $$ = PROTO_UDP; }
    ;

%%

void yyerror(const char *msg) {
    diag_error(yylloc.first_line, yylloc.first_column, "%s", msg);
}
