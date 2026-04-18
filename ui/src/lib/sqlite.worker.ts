/// <reference lib="webworker" />
/**
 * SQLite Web Worker.
 *
 * Runs sqlite-wasm off the main thread so large DBs don't jank the UI.
 * Accepts an ArrayBuffer (the loaded .db file), opens it in memory,
 * validates that it was built with `polc --debug`, and returns the
 * fully-shaped LoadedDb payload to the main thread.
 *
 * Why we do the full shape-up here instead of per-query-from-UI:
 * the inventory table is the only view that touches this data, the
 * file is already fully in memory, and doing it all at load time
 * lets the UI layer stay pure-React with no async query plumbing.
 */

import sqlite3InitModule, {
  type Database,
  type Sqlite3Static,
} from '@sqlite.org/sqlite-wasm'

import type { Asset, Label, LoadedDb, WorkerRequest, WorkerResponse } from './types'
import { eidToHex, ipToString, sortLabels } from './format'

/* The four symbolic tables the inventory tab needs. All four live in
 * SCHEMA_DEBUG_SQL in builder.c, so their absence == non-debug build. */
const REQUIRED_DEBUG_TABLES = ['entity', 'entity_ip', 'label', 'eid_label'] as const

let sqlite3: Sqlite3Static | null = null

async function initSqlite(): Promise<Sqlite3Static> {
  if (sqlite3) return sqlite3
  sqlite3 = await sqlite3InitModule({
    // silence the default console spam — worker logs go through postMessage
    print: () => {},
    printErr: () => {},
  })
  return sqlite3
}

function post(msg: WorkerResponse) {
  self.postMessage(msg)
}

/* ------------------------------------------------------------------ */
/* validation                                                         */
/* ------------------------------------------------------------------ */

function missingDebugTables(db: Database): string[] {
  const found = new Set<string>()
  db.exec({
    sql: `SELECT name FROM sqlite_master
           WHERE type='table' AND name IN (${REQUIRED_DEBUG_TABLES.map(() => '?').join(',')})`,
    bind: [...REQUIRED_DEBUG_TABLES],
    rowMode: 'array',
    callback: (row) => {
      found.add(row[0] as string)
    },
  })
  return REQUIRED_DEBUG_TABLES.filter((t) => !found.has(t))
}

/* ------------------------------------------------------------------ */
/* queries                                                            */
/* ------------------------------------------------------------------ */

interface RawAssetRow {
  name: string
  eid: bigint | number
}
interface RawLabelRow {
  eid: bigint | number
  key: string
  value: string
}
interface RawIpRow {
  entity_name: string
  ip: bigint | number
}

function loadAssets(db: Database): Asset[] {
  // 1. One row per entity, straight from the entity table.
  const rawAssets: RawAssetRow[] = []
  db.exec({
    sql: 'SELECT name, eid_hash AS eid FROM entity ORDER BY name',
    rowMode: 'object',
    callback: (row) => {
      rawAssets.push(row as unknown as RawAssetRow)
    },
  })

  // 2. All (entity, ip) pairs — grouped by entity_name in JS.
  //    Cheaper than a GROUP_CONCAT + post-parse and keeps IPs as numbers
  //    until ipToString formats them.
  const ipsByAsset = new Map<string, string[]>()
  db.exec({
    sql: 'SELECT entity_name, ip FROM entity_ip',
    rowMode: 'object',
    callback: (row) => {
      const r = row as unknown as RawIpRow
      const list = ipsByAsset.get(r.entity_name) ?? []
      list.push(ipToString(r.ip))
      ipsByAsset.set(r.entity_name, list)
    },
  })

  // 3. All (eid, label) triples — grouped by EID.
  //    An EID can be shared by multiple entities (same label set), so
  //    looking up by EID here is cheaper than by entity_name.
  const labelsByEid = new Map<string, Label[]>()
  db.exec({
    sql: `SELECT el.eid_hash AS eid, l.key, l.value
          FROM eid_label el
          JOIN label l ON l.id = el.label_id`,
    rowMode: 'object',
    callback: (row) => {
      const r = row as unknown as RawLabelRow
      const key = eidToHex(r.eid)
      const list = labelsByEid.get(key) ?? []
      list.push({ key: r.key, value: r.value })
      labelsByEid.set(key, list)
    },
  })

  return rawAssets.map((row) => {
    const eidHex = eidToHex(row.eid)
    const ips = (ipsByAsset.get(row.name) ?? []).sort()
    const labels = sortLabels(labelsByEid.get(eidHex) ?? [])
    return { name: row.name, eidHex, ips, labels }
  })
}

function countRow(db: Database, sql: string): number {
  let result = 0
  db.exec({
    sql,
    rowMode: 'array',
    callback: (row) => {
      result = Number(row[0])
    },
  })
  return result
}

function distinct<T>(values: Iterable<T>): T[] {
  return [...new Set(values)]
}

/* ------------------------------------------------------------------ */
/* message handler                                                    */
/* ------------------------------------------------------------------ */

async function handleOpen(buffer: ArrayBuffer, filename: string): Promise<void> {
  const sq = await initSqlite()

  // Copy the buffer into sqlite's heap and open a transient in-memory DB
  // from it. `deserialize` is the canonical way to hand a blob to SQLite.
  const bytes = new Uint8Array(buffer)
  const p = sq.wasm.allocFromTypedArray(bytes)
  const db = new sq.oo1.DB()

  try {
    const rc = sq.capi.sqlite3_deserialize(
      db.pointer!,
      'main',
      p,
      bytes.byteLength,
      bytes.byteLength,
      sq.capi.SQLITE_DESERIALIZE_RESIZEABLE,
    )
    if (rc !== 0) {
      post({ kind: 'error', message: `SQLite could not open file (code ${rc})` })
      return
    }

    // Gate: reject non-debug builds up-front with a specific error code
    // so the UI can render a tailored message.
    const missing = missingDebugTables(db)
    if (missing.length > 0) {
      post({
        kind: 'error',
        code: 'MISSING_DEBUG_TABLES',
        message:
          `This database was compiled without --debug. ` +
          `Missing tables: ${missing.join(', ')}. ` +
          `Rebuild with: polc --debug -i policy.gc`,
      })
      return
    }

    const assets = loadAssets(db)
    const stats = {
      rules: countRow(db, 'SELECT COUNT(*) FROM rule'),
      inventory: countRow(db, 'SELECT COUNT(*) FROM entity'),
    }

    // Precompute distinct filter-option lists once here, so the UI
    // doesn't have to re-derive them on every render.
    const allLabels = sortLabels(
      distinct(
        assets.flatMap((a) => a.labels.map((l) => `${l.key}:${l.value}`)),
      ).map((s) => {
        const i = s.indexOf(':')
        return { key: s.slice(0, i), value: s.slice(i + 1) }
      }),
    )
    const allAssetNames = assets.map((a) => a.name).sort()
    const allIps = distinct(assets.flatMap((a) => a.ips)).sort((a, b) => {
      // numeric sort on dotted-quad
      const pa = a.split('.').map(Number)
      const pb = b.split('.').map(Number)
      for (let i = 0; i < 4; i++) if (pa[i] !== pb[i]) return pa[i] - pb[i]
      return 0
    })
    const allEidHex = distinct(assets.map((a) => a.eidHex)).sort()

    const payload: LoadedDb = {
      filename,
      stats,
      assets,
      allLabels,
      allAssetNames,
      allIps,
      allEidHex,
    }
    post({ kind: 'ok', data: payload })
  } catch (err) {
    post({
      kind: 'error',
      message: err instanceof Error ? err.message : String(err),
    })
  } finally {
    db.close()
    // allocFromTypedArray -> sqlite3_deserialize with RESIZEABLE: sqlite
    // takes ownership of `p`, so we do NOT free it ourselves.
  }
}

self.onmessage = (e: MessageEvent<WorkerRequest>) => {
  const msg = e.data
  if (msg.kind === 'open') {
    handleOpen(msg.buffer, msg.filename).catch((err) => {
      post({
        kind: 'error',
        message: err instanceof Error ? err.message : String(err),
      })
    })
  }
}
