# polc web

A browser-based inspector for `out.db` files produced by `polc`.

The UI loads a compiled policy database directly — no backend, no server-side
processing. The `.db` file is read as bytes, opened by a WebAssembly build of
SQLite inside a Web Worker, and queried for display.

## Status

| Tab       | Status |
| --------- | ------ |
| Inventory | implemented — asset table with per-column multi-select + text filters |
| Compiled  | stub |
| Rules     | stub |

## Prerequisites

The UI only opens databases built with `polc --debug`:

```
polc --debug -i policy.gc -o out.db
```

Runtime-only databases (no `--debug`) are rejected with a clear message —
they don't contain the symbolic tables (`entity`, `entity_ip`, `label`,
`eid_label`) that the inventory tab needs.

## Run locally

```
npm install
npm run dev
```

Then open the URL Vite prints and click **Load database**.

## Project layout

```
web/
├── db.sql                       annotated schema extracted from builder.c
├── index.html                   shell + font imports
├── package.json
├── tsconfig.json
├── vite.config.ts               COOP/COEP headers for sqlite-wasm
└── src/
    ├── main.tsx                 React entry
    ├── App.tsx                  shell: brand, tabs, status line, routing
    ├── styles.css               full design system (CSS vars + components)
    ├── components/
    │   ├── Chip.tsx             colored label pill (env/app/role/default)
    │   ├── ColumnFilter.tsx     header popover: checkbox multi-select + search
    │   └── LoadButton.tsx       file picker trigger
    ├── lib/
    │   ├── types.ts             domain model + worker message contract
    │   ├── format.ts            IP/EID/chip formatting helpers
    │   ├── useSqliteDb.ts       React hook owning the worker lifecycle
    │   └── sqlite.worker.ts     SQLite-WASM worker: loads DB, queries inventory
    └── tabs/
        ├── InventoryTab.tsx     the implemented tab (TanStack Table + filters)
        ├── CompiledTab.tsx      stub
        └── RulesTab.tsx         stub
```

## How the data flows

1. User picks a file via the **Load ...** button
2. `useSqliteDb` reads it as an `ArrayBuffer` and transfers it to
   `sqlite.worker.ts`
3. The worker opens an in-memory DB via `sqlite3_deserialize`, checks that
   the four required debug tables exist, and if so runs three queries
   (entities / entity_ips / eid_label joined with label)
4. Results are shaped into `LoadedDb` — assets with hex-formatted EIDs,
   dotted-quad IPs, sorted label lists, plus the precomputed filter option
   lists — and posted back
5. The UI renders from that payload; filtering is client-side through
   TanStack Table's `columnFilters` state

## Encoding notes

Two SQLite integer columns need careful handling (see `db.sql` header):

- **IPs** are stored as host-order `uint32` packed into `INTEGER`. JS's
  signed shift can mangle the top bit — `format.ts` uses `>>> 0` to
  unsign.
- **EIDs** are 64-bit signed `INTEGER`. The full range exceeds
  `Number.MAX_SAFE_INTEGER`, so they're read as `BigInt` and formatted
  as `0x` + 16 uppercase hex chars for display.
