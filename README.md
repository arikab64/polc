# polc — policy compiler

A compiler for the `gc` policy DSL. Parses a two-or-three-section file
(optional `vars:`, optional `inventory:`, optional `policy:`) and emits
an intermediate representation: label table, enforcement identities
(EIDs), IP → EID cache, and the policy rules.

## Build

```
make
```

Produces `./polc`. Requires `gcc`, `flex`, and `bison`.

## Use

```
./polc policy.gc
```

Reads from stdin if no file is given.

## Language

```
vars:                       # optional — must come first if present
  $production  = env:production;
  $backend     = [ app:api-backend app:postgres ];
  @http        = [80 8080];
  @https       = 443;

inventory:                  # optional
  web-prod-1 [10.0.0.1] => [ app:web-front $production ];
  api-prod-1 [10.0.2.20 10.0.2.21] => [
      app:api-backend
      $production
      role:server
  ];

policy:                     # optional
  ALLOW app:web-front -> app:api-backend @http TCP;
  ALLOW [$backend app:other] -> role:database @https TCP,UDP;
  OVERRIDE-BLOCK $production -> env:staging 22 TCP;
```

### Separators

Inside `[...]` elements are whitespace-separated — everywhere. The only
comma in the grammar is the protocol list separator: `TCP,UDP`.

### Selector grammar

```
selector    := or_expr
or_expr     := and_expr ('OR'  and_expr)*
and_expr    := primary  ('AND' primary )*
primary     := key:value | $var | [sel_list] | (or_expr)
```

AND binds tighter than OR. Parens force grouping.

### Variables

`$var` for labels, `@var` for ports. Used bare for single items, or as
list elements in `[...]`. Multi-label `$var` inside a selector expands
to an OR-tree.

## Files

| File | Role |
|---|---|
| `scanner.l`     | flex lexer: tokens + column tracking |
| `parser.y`      | bison grammar |
| `ast.h`         | AST types and public API |
| `main.c`        | AST constructors, label table, EIDs, ipcache build, printers |
| `diag.h/c`      | gcc-style error reporter with file:line:col + caret |
| `ipcache.h/c`   | IP → EID open-addressing hashtable |
| `Makefile`      | build + `make vim-install` |
| `policy.gc`     | runnable example exercising every feature |
| `bad.inv`       | negative test: duplicate label keys |
| `overflow.inv`  | negative test: 511-label cap |
| `vim/`          | vim syntax highlighting for `*.gc` |

## Output

Five sections, in order:

1. **LABEL TABLE** — every distinct `(key, value)` pair with id 1..511.
2. **ENFORCEMENT IDENTITIES** — entities grouped by label bitset;
   each EID is the SplitMix64 of its bitset.
3. **IPCACHE** — the O(1) IP → EID map, sorted by IP.
4. **POLICY RULES** — parsed rules with expanded selector trees.
5. *(Phase 3: three bags — src_bag, dst_bag, port_bag.)*

## Vim syntax highlighting

```
make vim-install
```

Installs `*.gc` syntax, ftdetect, and ftplugin into `~/.vim/`. Open any
`.gc` file and `:set filetype?` should report `gc`.
