/**
 * Domain types for the inventory and rules tabs.
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

export type RuleAction = 'ALLOW' | 'BLOCK' | 'OVERRIDE-ALLOW' | 'OVERRIDE-BLOCK'

/**
 * One term in a DNF selector: a conjunction (AND) of labels.
 *
 *   app:web AND env:production   → one DnfTerm with two labels
 *
 * Multiple terms on the same side are OR'd.
 */
export interface DnfTerm {
  labels: Label[]
  /** true if the term references a label that isn't interned in the DB —
   *  it can never match anything. Render muted / strikethrough. */
  undefined: boolean
}

export interface Rule {
  id: number
  action: RuleAction
  /** DNF terms for the src selector, OR'd together. Empty → matches nothing. */
  src: DnfTerm[]
  /** DNF terms for the dst selector, OR'd together. Empty → matches nothing. */
  dst: DnfTerm[]
  /** rule_port, ascending. */
  ports: number[]
  /** decoded from proto_mask; subset of {'TCP','UDP'}. */
  protos: ('TCP' | 'UDP')[]
  /** resolved=1 means at least one real EID matched both sides; 0 means
   *  the rule is parked as unresolved (referenced labels/EIDs that don't
   *  exist). Compilation succeeds for unresolved rules but they have no
   *  runtime effect. */
  resolved: boolean
  /** Entity names whose EID matches the src selector — from rule_src_eid
   *  joined to entity. May be empty. */
  matchedSrc: string[]
  /** Entity names whose EID matches the dst selector. */
  matchedDst: string[]
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
  rules: Rule[]
  /* --- inventory filter options --- */
  /** distinct (key,value) pairs across the whole inventory — for the
   * Labels column filter popover */
  allLabels: Label[]
  /** distinct asset names — for the Asset column filter popover */
  allAssetNames: string[]
  /** distinct IPs — for the IPs column filter popover */
  allIps: string[]
  /** distinct EID hex strings — for the EID column filter popover */
  allEidHex: string[]
  /* --- rules filter options --- */
  /** distinct actions present in this DB — for the Action column filter */
  allActions: RuleAction[]
  /** distinct ports across all rules — for the Ports column filter */
  allPorts: number[]
  /** distinct protos present — for the Protocol column filter */
  allProtos: ('TCP' | 'UDP')[]
}

/** Messages from main thread to worker. */
export type WorkerRequest =
  | { kind: 'open'; buffer: ArrayBuffer; filename: string }

/** Messages from worker to main thread. */
export type WorkerResponse =
  | { kind: 'ok'; data: LoadedDb }
  | { kind: 'error'; message: string; code?: 'MISSING_DEBUG_TABLES' }
