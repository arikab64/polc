# polc

A network policy toolchain. Write your policy in a small declarative
language, compile it once, and consume the result from anywhere — eBPF
datapaths, simulators, debuggers, or visualization tools.

## What this is

`polc` is a **policy compiler** for declarative network access
control. Rules are typically written over higher-level identities —
`app:web-front`, `env:production`, `role:database` — but the language
also accepts CIDR subnets and specific IPs as first-class clauses, so
a rule can mix labels and address literals freely (e.g. *"allow
`app:api` only when the source IP is in `10.0.0.0/16`"*). The
compiler resolves which IPs back which identity, evaluates every
rule's selectors against the inventory, and emits a precomputed
lookup table that downstream enforcers can consume directly.

The input is a single source file in the **`gc`** DSL with three
sections:

- **`vars:`** — macro definitions for reusable label sets and port
  ranges. Pure parse-time substitution; variables don't survive into
  the compiled output.
- **`inventory:`** — the mapping from IP addresses to label sets.
  Entities sharing the same label set collapse into a single
  **Enforcement Identity (EID)**, which is the unit the runtime
  actually keys on — not individual IPs.
- **`policy:`** — the rules. Each rule names a source selector, a
  destination selector, a port spec, a proto spec, and an action
  (`ALLOW`, `BLOCK`, `OVERRIDE-ALLOW`, `OVERRIDE-BLOCK`).

A one-line taste: `ALLOW app:web-front -> app:api-backend :8080 :TCP;`

### Output: a self-contained SQLite database

Picking SQLite instead of a custom binary format means every consumer
— kernel loader, simulator, debugger, web UI — reads the same file
with the same well-known query interface, with no shared C headers or
codec libraries between them. The DB has two tiers: a small "runtime"
core of bag tables for enforcers, plus optional symbolic/debug tables
(`--debug`) for tools that need to explain results back to a human.

## Approach

The toolchain is built around a **compile-once / consume-anywhere**
contract. All policy semantics — variable expansion, label interning,
identity collapsing, selector evaluation — live in the compiler. The
output is a plain SQLite database that any downstream tool can read
without reimplementing the language.

Concretely, the DB encodes the policy as four bitvector "bags" indexed
by src-EID, dst-EID, destination port, and protocol. To decide whether
a packet is permitted, an enforcer does three integer lookups and ANDs
the four bitvectors together; any rule whose bit survives applies.
That's the entire runtime algorithm.

This means the same `out.db` can be loaded by:

- an **eBPF loader** that pins the bags into maps for in-kernel enforcement,
- a **simulator** answering "would this packet pass?" via a SQL join,
- a **debugger** answering "why doesn't this rule match?" via the
  optional debug tables (`--debug`),
- the **web UI** in [`web/`](web/), which does both of the above
  interactively in the browser via sqlite-wasm.

## The `gc` language

Policy is written in **gc**, a small DSL with three sections: `vars`
(macros), `inventory` (IP → label set), and `policy` (rules). The
full grammar and semantics are in
**[`compiler/LANGUAGE.md`](compiler/LANGUAGE.md)**.

A short example:

```
vars:
  $production = env:production;
  @api        = 8000-8080;

inventory:
  web-prod-1 [10.0.0.1]            => [ app:web-front  $production ];
  api-prod-1 [10.0.2.20 10.0.2.21] => [ app:api-backend $production ];
  db-prod-1  [10.0.3.50]           => [ app:postgres   $production role:database ];

policy:
  # web pods may reach the API on the api-port range
  ALLOW app:web-front -> app:api-backend :@api :TCP;

  # API in production may reach postgres
  ALLOW app:api-backend AND $production -> role:database :5432 :TCP;

  # nothing in production may talk to staging, ever
  OVERRIDE-BLOCK $production -> env:staging :ANY :ANY;
```

A rule reads as: *action source-selector* `->` *destination-selector*
`:ports` `:protos`.

## Build and run

Compile a policy:

```bash
cd compiler
make                          # needs gcc, flex, bison, libsqlite3-dev
./polc -i policy.gc           # writes out.db
./polc -i policy.gc --debug   # adds symbolic tables for inspectors
```

Inspect a compiled DB in the browser:

```bash
cd web
npm install
npm run dev                   # open the printed URL, load out.db
```

## Repo layout

```
compiler/    The polc compiler (C, flex, bison, sqlite). Language spec lives here.
web/         Browser-based inspector for compiled DBs (Vite + React + sqlite-wasm).
```
