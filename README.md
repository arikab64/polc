# polc

A compiler for the `gc` network policy DSL. Takes a human-readable policy
source file and produces a SQLite database that downstream tools — network
simulators, visibility dashboards, debuggers, eBPF loaders — can query
directly, without reimplementing any of the policy semantics.

The design follows Cilium's three-bag enforcement model: at runtime, a
packet's src-EID, dst-EID, destination port, and protocol are each mapped
to a bitvector of matching rule ids, and those four bitvectors are ANDed
together. Any bit left standing is a rule that applies. This compiler
produces exactly the data needed to perform that AND.

## Build and run

Requires `gcc`, `flex`, `bison`, and `libsqlite3-dev` (Debian/Ubuntu) or
`sqlite-devel` (Fedora/RHEL). The Makefile uses `pkg-config` when
available, so macOS/Homebrew installs work out of the box.

```
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

  # Port variables — single port or a list.
  @http      = [80 8080];
  @postgres  = 5432;

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

  # A rule: ACTION SRC_SELECTOR -> DST_SELECTOR PORT_SPEC PROTO_LIST ;
  ALLOW app:web-front -> app:api-backend @http TCP;

  # Compound selectors. AND binds tighter than OR.
  ALLOW app:web-front AND $production
        -> app:api-backend AND $production
        @postgres TCP;

  # Bracketed selector list = OR of the items, any mix of vars and literals.
  ALLOW [$backendApps app:web-front] -> role:database [5432 @http] TCP,UDP;

  # No matching entity? Rule is parked in "unresolved" with a warning;
  # compilation still succeeds.
  OVERRIDE-BLOCK $production -> env:staging 22 TCP;
```

### A few grammar rules worth stating

Inside `[...]`, list elements are **whitespace-separated**. The only
comma in the grammar is the protocol list separator (`TCP,UDP`). This
applies uniformly to inventory label lists, selector lists, port lists,
and variable definitions — one convention everywhere.

Variables are a pure macro layer expanded at parse time. A multi-label
`$var` used inside a selector becomes an OR over its members; a
multi-port `@var` inside a `[...]` port list flattens into the list.
`$` and `@` names accept hyphens (`$backend-apps` is valid), and must
be defined before use.

A selector is a boolean expression over `key:value` leaves, combined with
`AND`, `OR`, parentheses, and the `[...]` OR-shorthand. Leaves can also
be `$var` references. Four rule actions exist: `ALLOW`, `BLOCK`,
`OVERRIDE-ALLOW`, `OVERRIDE-BLOCK`.

## Architecture — the compilation pipeline

The compiler runs seven phases in sequence. Each phase has a narrow
responsibility and writes into a well-defined in-memory structure that
the next phase reads. Separation is deliberate: each transition is a
place where you can inspect, verify, or dump the state to understand
what the compiler did.

### Phase 1 — Parse and expand variables

A flex lexer produces tokens with line and column tracking for
diagnostics; a bison LALR(1) grammar produces an AST for the inventory
and policy sections. Variables are resolved *during* parsing — a `$var`
reference looks up the stored label list and inlines a deep copy, so
every downstream phase sees fully expanded content. There is no
second-pass "linker" for variables; once parsing is done, they've
disappeared.

Errors at this phase are gcc-style with file:line:col and a caret
pointing at the offending token. Undefined variables, duplicate
variable definitions, duplicate label keys within one entity, and
malformed IPs are all caught here with precise source positions.

### Phase 2 — Label interning and EID resolution

Every distinct `key:value` pair in the input gets a sequential integer
id, between 1 and 511. The result is a flat **label table**.

Each entity's label list is then flattened to a **512-bit bitset**
where bit *i* is set if the entity has label id *i*. Two entities with
the same label set collapse to the same **Enforcement Identity** (EID),
keyed by a SplitMix64 hash of the bitset. This is the central trick
that makes policy tractable: you don't enforce on individual endpoints,
you enforce on the coarser EID, and the label bitset is the canonical
identity.

EIDs are assigned ordinals in parse order (EID[0], EID[1], …) for
human-friendly display; the hash remains the primary key for all
downstream lookups.

### Phase 3 — IP → EID cache

For the datapath to act, it needs the fastest possible IP → EID
lookup. The compiler builds an open-addressing hashtable keyed by
IP (as a host-order u32), with the EID hash as the value, grown to
75% load factor. Every IP listed against any entity ends up here.
This is what gets shipped to the runtime; on packet arrival the
enforcer does exactly one lookup per direction.

### Phase 4 — Rule DNF compilation

Rule selectors are compiled from their AST form into **Disjunctive
Normal Form**: a flat list of conjunctive terms, each term a set of
labels that must *all* be present. A selector like
`(app:web OR app:api) AND env:prod` becomes
`(app:web AND env:prod) OR (app:api AND env:prod)` — the distributive
law applied at compile time.

DNF is the canonical form for "which EIDs does this selector match?"
An EID matches a term iff its label bitset is a superset of the term's
mask; it matches the whole selector iff it matches at least one term.
This reduces a potentially nested boolean expression to a straight
bitmask-subset check per candidate EID.

Selectors referencing labels that don't exist in the inventory
(typos, `team:phantom`) produce terms flagged as `undefined`. These
terms are always false but are preserved in the DNF for debugging —
the debugger can show *why* a rule fails to match rather than silently
elide the reference.

### Phase 5 — Rule resolution and triage

For each rule, the compiler evaluates its src and dst DNFs against
every EID in the inventory and collects the matching sets. This is
the bridge from symbolic (label world) to concrete (identity world)
enforcement.

Rules where **either** src or dst matched zero EIDs are separated
into an **unresolved** collection. These rules can never fire — they
reference identities that don't exist in the current inventory — and
including them in the bags would just waste bits. A warning is
emitted (gcc-style, at the rule's line) but compilation succeeds.
Unresolved rules still get their DNF preserved so the debugger can
explain the failure later.

### Phase 6 — Bag build and bitvec interning

Four enforcement bags are built by walking the resolved rules and
setting bits in 4096-wide bitvectors:

- **`g_bag_src`**: `EID` → bitvector of rules whose src selector matches this EID
- **`g_bag_dst`**: `EID` → bitvector of rules whose dst selector matches
- **`g_bag_port`**: `port` → bitvector of rules that list this port
- **`g_bag_proto`**: `proto` → bitvector of rules with this proto

But storing a 512-byte bitvector per key would duplicate a lot of
identical content. In practice many EIDs end up with the same src bag
(they're all valid sources for the same rule set); the TCP proto bag
and the web-front src bag might even hold identical bits.

The compiler exploits this by **interning** the bitvectors into a
global store, keyed by small sequential ids. Two reserved ids carry
boundary values: `BAG[0]` is all-zeros (returned on any missing-key
lookup — simulator never branches), `BAG[1]` is all-ones (reserved for
future wildcards). Dynamic ids start at 2 and are assigned in insertion
order with content-hash dedup. The four bags become compact
`key → bag_id` maps; one bagvec entry can serve many keys across
multiple bags.

### Phase 7 — SQLite emission

The final phase walks all the in-memory state and writes it to a
SQLite database. Every write happens inside a single transaction,
using prepared statements bound per row, which makes even large
policies emit in well under a second. `PRAGMA synchronous = OFF`
and `journal_mode = MEMORY` further speed writes — we rebuild the
whole DB on every compile, so durability during write doesn't matter.

The schema splits into two tiers (see "Compilation modes" below).
Indexes are created for every join path a consumer tool is likely to
use. An external simulator can compute "does this packet pass?" as a
four-way join against `bagvec_bit`; a debugger can ask "why doesn't
this rule match this EID?" against `rule_dnf_term` and `eid_label`.

## Compilation modes

The compiler has three orthogonal flags that control output: `--debug`,
`-v` / `--verbose`, and `--dump`.

### Default

No flags: the compiler runs silently on stdout. On stderr it prints a
single-line summary after a successful compile, plus any warnings or
errors along the way:

```
$ polc -i policy.gc
polc: compiled 8 labels, 4 EIDs, 6 rules (5 resolved, 1 unresolved), 9 bagvecs
polc: wrote out.db
```

The `out.db` file contains only the **runtime tables** — the minimum a
datapath loader or simulator needs: the four bag maps, the bagvec store
and its bit rows, the ipcache, the EID table, and the rules with their
actions and proto masks. About 70 KB for a small policy, with no
symbolic information and no source positions.

### `--debug`

Adds the **debug tables** to the output: the label table that maps
ids back to `key:value` names, the entity and entity_ip tables (human
names for IPs), `eid_label` (which labels make up each EID), the
variable tables (`var_label`, `var_label_member`, `var_port`,
`var_port_member`), `rule_port` (denormalized rule → port list), and
the DNF tables (`rule_dnf_term`, `rule_dnf_term_label`) plus the
cached EID match results (`rule_src_eid`, `rule_dst_eid`). The
`rule.src_line`, `rule.src_col`, and `rule.source` columns are also
populated.

These tables are what a debugger needs to answer "why?" — *why*
doesn't my rule fire, *which* labels is this EID missing to satisfy
that term, *what* does `$production` actually expand to. A runtime
enforcer doesn't need any of it.

The debug DB is roughly 3× larger than the runtime-only version. The
`meta` table records a `debug = "true"` row so consumer tools can tell
at a glance which kind of DB they opened.

### `-v` / `--verbose`

Prints the full compilation log to stdout during the compile —
label table, EIDs, ipcache, parsed rules, DNF-resolved rules (with
matched EIDs), and the bag contents in bitvector form. This is what
you want when iterating on the DSL source to see exactly what the
compiler is doing. It does not affect the output DB.

### `--dump`

After writing the DB, re-reads it and produces a tabular text rendering
of every present table. The dump file is written to a sibling of the
output DB with the extension replaced by `.txt` — so `out.db` produces
`out.txt`, `policy.db` produces `policy.txt`, and so on.

Because the dump reads back from SQLite rather than from in-memory
state, it doubles as a round-trip verification: if the dump looks
wrong, the DB is wrong. The content adapts to which tables are present,
so `--dump` on a non-debug DB shows 10 sections, and `--dump --debug`
shows 23. This is the easiest way to inspect what was actually emitted,
share a compile result with a collaborator, or diff two compiles to
see what changed.

## What's in the DB

Both modes produce the same core representation: four key → bag_id
maps, an interned bagvec store, an IP → EID cache, and a rule table.
That's enough for a datapath or simulator to implement policy
enforcement with three integer lookups and one AND. The debug tables
are purely additive — they never change the runtime answer, only the
ability to explain it.
