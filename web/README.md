# polc web

Browser-based inspector for `out.db` files produced by
[`polc`](../). The database is loaded directly in the browser via
SQLite-WASM running inside a Web Worker — no backend, no server, no
network calls after the initial page load.

## Quick start

```
npm install
npm run dev
```

Open the URL Vite prints, click **Load database**, and pick an
`out.db` file.

## Requirements

- Node 20+ and npm
- A database built with `polc --debug`:
  ```
  polc --debug -i policy.gc -o out.db
  ```
  Runtime-only databases (no `--debug`) are rejected — the UI needs the
  symbolic tables for display.

## Other useful commands

```
npm run build     # produce a static bundle in dist/
npm run preview   # serve the built bundle locally for a sanity check
npx tsc -b        # type-check without emitting
```

## What you can do

- **Inventory** — browse assets, IPs, and labels
- **Rules** — inspect policy rules, their selectors, and resolved EIDs
- **Compiled** — the post-compile view: Enforcement Identities (with a
  label-expression evaluator), Endpoint (ipcache), and Bag Vectors
- **Simulator** — enter a packet (src/dst IP, port, protocol) and see
  whether it's allowed or denied, with a full debug trace of the bag
  lookups that led to the verdict
