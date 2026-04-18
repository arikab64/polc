" Vim syntax file
" Language: gc-enforcement policy DSL
" Filename: *.gc

if exists("b:current_syntax") | finish | endif

" Keywords are UPPERCASE (ALLOW, TCP, AND); section headers are lowercase
" (inventory:, policy:). The language is case-sensitive — the scanner is too.
syntax case match

" ---- comments ---------------------------------------------------------
syntax match   gcComment    "#.*$"
syntax match   gcComment    "//.*$"

" ---- section headers -------------------------------------------------
" Must be at column 0 with no leading whitespace. 'containedin=ALLBUT'
" makes sure this is never captured by another item. We define it early
" and rely on Vim's "longest match wins" at a given position; since this
" pattern consumes 'inventory:' (10 chars) vs 'inventory' (9 chars) from
" gcSelectorKey, it would normally win. But gcSelectorKey is shorter by
" only one char and Vim considers ':' a non-keyword, so the :syn priority
" mechanism is needed. We give this an explicit higher priority.
syntax match   gcSection    "^\%(inventory\|policy\)\s*:"

" ---- rule actions -----------------------------------------------------
syntax keyword gcOverride   OVERRIDE-ALLOW OVERRIDE-BLOCK
syntax keyword gcActionAllow ALLOW
syntax keyword gcActionBlock BLOCK

" ---- selector operators ----------------------------------------------
syntax keyword gcLogical    AND OR

" ---- protocols -------------------------------------------------------
syntax keyword gcProto      TCP UDP

" ---- arrows ----------------------------------------------------------
syntax match   gcArrow      "=>"
syntax match   gcArrow      "->"

" ---- numeric literals ------------------------------------------------
" Note on ordering: in Vim syntax, when two :match items start at the
" same column, the LAST-defined one wins. We want the full dotted-quad
" to win over individual octets, so gcIP is defined AFTER gcNumber.
syntax match   gcNumber     "\<\d\+\>"
syntax match   gcIP         "\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}"

" ---- selector leaves (key:value, used in both inventory and policy) --
" Same shape everywhere now — entity labels and rule selector leaves
" share one syntax. The key rule must not fire at column 0 (that would
" compete with gcSection); the value rule anchors off a preceding ':'.
syntax match   gcSelectorKey "\%(^\)\@!\<[A-Za-z_][A-Za-z0-9_-]*\ze\s*:"
syntax match   gcSelectorVal "\%(:\s*\)\@<=[A-Za-z_][A-Za-z0-9_.-]*"

" ---- linking to standard highlight groups --------------------------
" Comments: explicit gray so they render the same across color schemes.
" ctermfg=8 is "bright black" in 16-color terminals (the standard dark-gray slot);
" it degrades gracefully on 8-color terminals and is exact on 256-color ones.
" guifg=#808080 picks a neutral mid-gray for GUI vim.
hi def gcComment            ctermfg=8 guifg=#808080 cterm=italic gui=italic
hi def link gcSection       Special
hi def link gcOverride      Exception
hi def link gcActionAllow   Statement
hi def link gcActionBlock   Error
hi def link gcLogical       Operator
hi def link gcProto         Type
hi def link gcArrow         Operator
hi def link gcIP            Number
hi def link gcNumber        Number
hi def link gcSelectorKey   Identifier
hi def link gcSelectorVal   String

let b:current_syntax = "gc"
