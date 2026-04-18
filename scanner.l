/*
 * scanner.l — lexical analyzer for the policy DSL (inventory section)
 *
 * Tracks line + column via YY_USER_ACTION so every token carries a location
 * rectangle (first_line, first_column, last_line, last_column) in yylloc.
 */
%option noyywrap nounput noinput yylineno
%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ast.h"
#include "diag.h"
#include "parser.tab.h"

/* yycolumn is maintained by hand — flex doesn't do columns. Reset on \n. */
static int yycolumn = 1;

/* Run before every token's action: set yylloc to the current span, then
 * advance yycolumn past the matched text. */
#define YY_USER_ACTION                                            \
    yylloc.first_line   = yylineno;                               \
    yylloc.first_column = yycolumn;                               \
    yylloc.last_line    = yylineno;                               \
    yylloc.last_column  = yycolumn + yyleng - 1;                  \
    yycolumn += yyleng;
%}

IP      [0-9]{1,3}"."[0-9]{1,3}"."[0-9]{1,3}"."[0-9]{1,3}
NUMBER  [0-9]+
NAME    [A-Za-z_][A-Za-z0-9_\-\.]*

%%

    /* Section headers (matched before NAME). Case-insensitive optional. */
"inventory"     { return INVENTORY; }
"policy"        { return POLICY; }
"vars"          { return VARS; }

    /* Rule actions — hyphenated forms must come before NAME since NAME
     * would otherwise consume "OVERRIDE-ALLOW" as a single identifier. */
"OVERRIDE-ALLOW" { return OVERRIDE_ALLOW; }
"OVERRIDE-BLOCK" { return OVERRIDE_BLOCK; }
"ALLOW"         { return ALLOW; }
"BLOCK"         { return BLOCK; }

    /* Selector operators. */
"AND"           { return AND; }
"OR"            { return OR; }

    /* Protocols. */
"TCP"           { return TCP; }
"UDP"           { return UDP; }

    /* Rule direction arrow. Must come before IP rule since IPs don't use '->'. */
"->"            { return ARROW; }

    /* Variable references: $foo for label-like, @foo for port-like.
     * The regex uses the same identifier body as NAME so hyphenated names
     * like $backend-apps scan as a single token. */
"$"{NAME}       { yylval.str = strdup(yytext + 1); return VAR_LABEL; }
"@"{NAME}       { yylval.str = strdup(yytext + 1); return VAR_PORT;  }

"=>"            { return FAT_ARROW; }
"="             { return '='; }
"["             { return '['; }
"]"             { return ']'; }
"{"             { return '{'; }
"}"             { return '}'; }
":"             { return ':'; }
","             { return ','; }
";"             { return ';'; }
"("             { return '('; }
")"             { return ')'; }

{IP}            { yylval.str = strdup(yytext); return IP; }
{NUMBER}        { yylval.num = atoi(yytext); return NUMBER; }
{NAME}          { yylval.str = strdup(yytext); return NAME; }

"//".*          { /* line comment */ }
"#".*           { /* shell-style comment */ }

\n              { yycolumn = 1; }
[ \t\r]+        { /* non-newline whitespace — column already advanced */ }

.               {
                    diag_error(yylineno, yylloc.first_column,
                               "unexpected character '%c'", yytext[0]);
                    g_semantic_errors++;
                }

%%
