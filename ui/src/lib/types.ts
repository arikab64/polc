/**
 * Domain types for the inventory tab.
 *
 * Shape mirrors the out.db schema (see ../../db.sql).
 * EIDs and IPs are kept as strings at the UI boundary — the raw DB
 * values are 64-bit signed ints which JS `number` can't safely hold
 * for the full range of EID hashes. We serialize as hex ("0x…") for
 * EIDs and dotted-quad for IPs before crossing out of the worker.
 */

export interface Label {
  key: string
  value: string
}

export interface Asset {
  /** entity.name — the human-readable asset name */
  name: string
  /** entity.eid_hash, formatted as 0x + 16 hex chars, uppercase */
  eidHex: string
  /** dotted-quad IP strings, sorted lexicographically */
  ips: string[]
  /** labels attached to this asset's EID, sorted by key then value */
  labels: Label[]
}

export interface InventoryStats {
  rules: number
  inventory: number
}

export interface LoadedDb {
  /** source filename, as displayed after `bin=` */
  filename: string
  stats: InventoryStats
  assets: Asset[]
  /** distinct (key,value) pairs across the whole inventory — for the
   * Labels column filter popover */
  allLabels: Label[]
  /** distinct asset names — for the Asset column filter popover */
  allAssetNames: string[]
  /** distinct IPs — for the IPs column filter popover */
  allIps: string[]
  /** distinct EID hex strings — for the EID column filter popover */
  allEidHex: string[]
}

/** Messages from main thread to worker. */
export type WorkerRequest =
  | { kind: 'open'; buffer: ArrayBuffer; filename: string }

/** Messages from worker to main thread. */
export type WorkerResponse =
  | { kind: 'ok'; data: LoadedDb }
  | { kind: 'error'; message: string; code?: 'MISSING_DEBUG_TABLES' }
