# polc

A compiler for the `gc` network policy DSL. Takes a human-readable policy
source file and produces a SQLite database that downstream tools — network
simulators, visibility dashboards, debuggers, eBPF loaders — can query
directly, without reimplementing any of the policy semantics.

## Build and run

Requires `gcc`, `flex`, `bison`, and `libsqlite3-dev` (Debian/Ubuntu) or
`sqlite-devel` (Fedora/RHEL). The Makefile uses `pkg-config` when
available, so macOS/Homebrew installs work out of the box.

```bash
make
./polc -i policy.gc
```

Writes `out.db`. Use `-o` to pick a different path, `--debug` to include
symbolic tables, `--dump` to emit a human-readable text rendering
alongside. See the "Compilation modes" section at the bottom.

## The input format

The DSL has three sections. Any or all can be omitted; if present, they
must appear in this order.

```
vars:

  # Label variables — single label or a whitespace-separated list.
  $production   = env:production;
  $backendApps  = [ app:api-backend app:postgres ];

  # Port variables — single port, comma-separated list, or range.
  @http      = 80,8080;
  @api       = 8000-8080;
  @postgres  = 5432;
  @wildcard  = ANY;

inventory:

  # Each entity: name, IP list in [...], then the label set in [...].
  # Labels may be literal k:v pairs or $var references that expand inline.
  web-prod-1 [10.0.0.1] => [ app:web-front $production ];

  api-prod-1 [10.0.2.20 10.0.2.21] => [
      app:api-backend
      $production
      role:server
  ];

  db-prod-1  [10.0.3.50] => [ app:postgres $production role:database ];

policy:

  # A rule: ACTION SRC_SELECTOR -> DST_SELECTOR :PORTS :PROTOS ;
  ALLOW app:web-front -> app:api-backend :80 :TCP;

  # @var expansion + compound selector. AND binds tighter than OR.
  ALLOW app:web-front AND $production
        -> app:api-backend AND $production
        :@postgres :TCP;

  # Bracketed selector list = OR of the items, any mix of vars and literals.
  # Port list + range + proto list.
  ALLOW [$backendApps app:web-front] -> role:database
        :5432,8000-8080 :TCP,UDP;

  # Wildcard port and wildcard protocol.
  ALLOW app:worker AND $production -> role:database :ANY :ANY;

  # No matching entity? Rule is parked in "unresolved" with a warning;
  # compilation still succeeds.
  OVERRIDE-BLOCK $production -> env:staging :22 :TCP;
```

## Syntax Reference

The DSL has a small, uniform grammar. This section is the canonical
description; everything else (pipeline phases, emitted tables) is
downstream of what's defined here.

### Sections

```
file    := ( 'vars:' var_def* )? ( 'inventory:' entity* )? ( 'policy:' rule* )?
```

Sections must appear in this order. Section headers are lowercase.
Comments: `# ...` (shell-style) or `// ...` (C-style), both run to
end of line.

### Identifiers, Literals, Keywords

- **Names** (entity names, label keys, label values) — `[A-Za-z_][A-Za-z0-9_.-]*`.
  So `app:web-front`, `env:production`, `web-prod-1` are all valid.
- **IPs** — dotted-quad IPv4, e.g. `10.0.2.20`.
- **Numbers** — non-negative decimal.
- **Keywords** (case-sensitive, uppercase): `ALLOW`, `BLOCK`, `OVERRIDE-ALLOW`,
  `OVERRIDE-BLOCK`, `AND`, `OR`, `TCP`, `UDP`, `ANY`.
- **Section headers** (lowercase): `vars:`, `inventory:`, `policy:`.

### Variables (`vars:`)

Pure macro layer. Expanded at parse time; variables do not survive into
any downstream phase.

**Label variables** — `$name` references a label or list of labels.

```
$name = key:value ;                           // single label
$name = [ key1:val1 key2:val2 ... ] ;         // whitespace-separated list
```

**Port variables** — `@name` references a port or list of ports.

```
@name = NUMBER ;                              // single port
@name = NUMBER-NUMBER ;                       // range (expanded at parse time)
@name = ANY ;                                 // wildcard (stored as port 0)
@name = port_item (',' port_item)* ;          // comma-separated mix
```

where `port_item` is `NUMBER` or `NUMBER-NUMBER` or another `@var`.

Variable names accept hyphens (`$backend-apps`, `@tls-ports`). A variable
must be defined before it's used; redefinition is an error.

### Inventory

```
entity := NAME '[' ip_list ']' '=>' '[' label_list ']' ';'
```

- `ip_list` — one or more IPs, whitespace-separated.
- `label_list` — one or more labels or `$var` references,
  whitespace-separated. No commas.

Each entity's labels are interned into a 512-bit bitset. Two entities
with identical label sets collapse to the same **Enforcement Identity
(EID)** — that's the unit the runtime enforces on, not individual
entities.

IPs must be unique across the entire inventory. Duplicate label keys within one entity are also an error.

### Policy Rules

```
rule := action selector '->' selector ':' port_spec ':' proto_spec ';'
```

- `action` — one of `ALLOW`, `BLOCK`, `OVERRIDE-ALLOW`, `OVERRIDE-BLOCK`.
- `selector` — boolean expression over `key:value` leaves.
- `port_spec`, `proto_spec` — each introduced by `:`.

Both `:port_spec` and `:proto_spec` are **mandatory**. There is no
"omit to mean ANY" shortcut — write `:ANY` when that's what you mean.

### Selectors

A selector is a boolean expression matching EIDs by their label set.

```
selector  := or_expr
or_expr   := and_expr ( 'OR' and_expr )*
and_expr  := primary  ( 'AND' primary )*
primary   := NAME ':' value              // a literal label
           | $var                        // expands to its stored label(s)
           | '(' or_expr ')'             // parenthesized subexpression
           | '[' sel_list ']'            // OR-shorthand over list items
sel_list  := ( label | $var )+           // whitespace-separated
```

`AND` binds tighter than `OR`. Use parentheses or the `[...]`
OR-shorthand to override.

### Port Spec (`:PORTS`)

```
port_spec  := ANY
            | port_item ( ',' port_item )*
port_item  := NUMBER                              // single port
            | NUMBER '-' NUMBER                   // range
            | @var                                // expand @var's list
```

`ANY` is a top-level alternative — **not** a `port_item` — so the
grammar itself forbids mixing `ANY` with other ports. Ranges are inclusive.
**Port 0** is the numeric equivalent of `ANY` in single-port position.

### Proto Spec (`:PROTOS`)

```
proto_spec := ANY
            | proto ( ',' proto )*
proto      := 'TCP' | 'UDP'
```

`:ANY` expands to all known protocols at compile time — currently
`TCP | UDP`.

## Architecture — the compilation pipeline

The compiler runs seven phases in sequence. Each phase has a narrow
responsibility and writes into a well-defined in-memory structure that
the next phase reads.

1.  **Phase 1 — Parse and expand variables:** Flex/Bison parser produces an AST. Variables are expanded inline during parsing.
2.  **Phase 2 — Label interning and EID resolution:** Labels are mapped to 512-bit bitsets. Entities with the same label set collapse into a single Enforcement Identity (EID).
3.  **Phase 3 — IP → EID cache:** Builds a fast lookup table mapping every inventory IP to its EID hash.
4.  **Phase 4 — Rule DNF compilation:** Rule selectors are converted to Disjunctive Normal Form (OR of ANDs) for efficient matching.
5.  **Phase 5 — Rule resolution and triage:** Matches rules against EIDs. Rules matching no EIDs are flagged as "unresolved".
6.  **Phase 6 — Bag build and bitvec interning:** Builds four enforcement bags (src, dst, port, proto) and interns the resulting bitvectors to save space.
7.  **Phase 7 — SQLite emission:** Writes all in-memory state to a SQLite database.

## Compilation Modes

- **Default:** Minimal runtime tables for enforcers.
- **`--debug`:** Adds symbolic/debug tables (labels, DNF, variable expansions, source positions) for inspector tools.
- **`-v` / `--verbose`:** Prints the full compilation log to stdout.
- **`--dump`:** Writes a human-readable text rendering of the resulting database.
