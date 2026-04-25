/**
 * Packet evaluator — simulates what the runtime datapath would do:
 *
 *   src IP → EID via ipcache (or ANY_EID if not in inventory)
 *   dst IP → EID via ipcache (or ANY_EID if not in inventory)
 *
 *   src_vec   = bagvec[bag_src[src_eid]]   |  bagvec[bag_src[ANY_EID]]
 *   dst_vec   = bagvec[bag_dst[dst_eid]]   |  bagvec[bag_dst[ANY_EID]]
 *   port_vec  = bagvec[bag_port[port]]
 *   proto_vec = bagvec[bag_proto[proto]]
 *
 *   matching = src_vec & dst_vec & port_vec & proto_vec
 *   → pick highest-priority matching rule
 *
 * The OR with `bag_*[ANY_EID]` is the runtime side of the ALL_EIDS
 * selector (LANGUAGE.md §5.2 / §6.2): a rule with `ANY` on its src or
 * dst contributed its bit to the bag's ANY_EID slot only — never to
 * the per-EID slots. The OR fuses the two streams so the rule fires
 * for every EID, including unresolved IPs (which have no concrete
 * slot at all).
 *
 * Rule precedence (highest to lowest):
 *   1. OVERRIDE-BLOCK — an explicit deny trumps everything
 *   2. OVERRIDE-ALLOW — explicit allow above the normal allow/block
 *   3. ALLOW          — normal allow
 *   4. BLOCK          — normal block (deny)
 *
 * Default action if no rule matches is DENY. This is standard for
 * Cilium-style policy enforcement and matches what the runtime would
 * produce.
 *
 * The evaluator returns a rich trace so the UI can show each step:
 * which bag produced which bit-set, which rules came from the concrete
 * EID vs the ALL_EIDS slot, which AND eliminated which rules, which
 * rule finally won.
 */

import type { Asset, Bags, LoadedDb, Rule, RuleAction } from './types'
import { ANY_EID_HEX } from './format'

export type Protocol = 'TCP' | 'UDP'

export interface SimulatorInput {
  srcIp: string
  dstIp: string
  port: number
  proto: Protocol
}

export type Verdict = 'ALLOW' | 'DENY'

/** One step in the per-bag evaluation trace. */
export interface BagLookupStep {
  bag: 'src' | 'dst' | 'port' | 'proto'
  /** The key we looked up (IP, EID hex, port as string, or proto). */
  keyLabel: string
  /** Human explanation of how we arrived at the key (e.g. "ipcache → EID"). */
  keyExplain?: string
  /** bag_id the key mapped to, or null if the key wasn't present in the bag.
   *  For src/dst this is the *concrete-EID* bag_id only; the ANY_EID
   *  slot — if any — is reflected via `anyEidRuleIds`. */
  bagId: number | null
  /**
   * Rule IDs whose bit is set after considering this step's contribution
   * to the AND. Equal to (concrete-EID bits) ∪ (ANY_EID bits) for src/dst,
   * and just the bag's bits for port/proto.
   *
   * This is what gets fed into the four-way intersection.
   */
  ruleIds: number[]
  /**
   * src/dst only: rule IDs from the bag's concrete-EID slot. Empty if
   * the IP didn't resolve (so there was no concrete EID to look up) or
   * if the key wasn't present in the bag. Excludes the ANY_EID slot's
   * contribution — see `anyEidRuleIds` for that.
   */
  eidRuleIds?: number[]
  /**
   * src/dst only: rule IDs from the bag's ANY_EID slot. These are the
   * bits owned by rules whose side was `SEL_ALL` (bare `ANY` / bare
   * subnet). Always OR'd into `ruleIds` regardless of whether the IP
   * resolved.
   */
  anyEidRuleIds?: number[]
}

export interface Resolution {
  /** The IP the user supplied. */
  ip: string
  /** The asset that owns it, if any. Null when the IP isn't in the
   *  inventory — in that case the IP is treated as ANY_EID for bag
   *  lookups, so a missing asset no longer short-circuits to DENY. */
  asset: Asset | null
}

export interface SimulatorResult {
  input: SimulatorInput
  /** Resolution of each IP to its owning asset. asset is null if the
   *  IP isn't in the inventory; the simulator still runs in that case
   *  via the ANY_EID fallback. */
  srcResolution: Resolution
  dstResolution: Resolution
  /** Per-bag lookup trace. Always four entries in order src/dst/port/proto. */
  bagSteps: BagLookupStep[]
  /** Rules that survived the AND of all four bag vectors, ordered ascending. */
  matchingRuleIds: number[]
  /** The rule that wins under precedence, or null if matchingRuleIds is empty. */
  winningRule: Rule | null
  verdict: Verdict
  /** Short human-readable explanation of why we reached that verdict. */
  verdictReason: string
}

/* -------------------- public entry point -------------------- */

export function simulate(db: LoadedDb, input: SimulatorInput): SimulatorResult {
  const srcResolution = resolveIp(db, input.srcIp)
  const dstResolution = resolveIp(db, input.dstIp)

  // Build the four bag-lookup steps. Unresolved IPs no longer
  // short-circuit to DENY — they fall through to the ANY_EID slot, which
  // by spec catches every rule whose side was the ALL_EIDS selector. If
  // there's no such rule (and no concrete-EID match), src_vec or dst_vec
  // is empty and the AND is too — we'll DENY further down for the right
  // reason ("no rule covered the lookup key") rather than the wrong one
  // ("IP not in inventory").
  const srcStep = lookupEidBag(db.bags, 'src', srcResolution, input.srcIp)
  const dstStep = lookupEidBag(db.bags, 'dst', dstResolution, input.dstIp)
  const portStep = lookupPortBag(db.bags, input.port)
  const protoStep = lookupProtoBag(db.bags, input.proto)
  const bagSteps: BagLookupStep[] = [srcStep, dstStep, portStep, protoStep]

  // AND of the four rule-id sets.
  const matching = intersectAll([
    new Set(srcStep.ruleIds),
    new Set(dstStep.ruleIds),
    new Set(portStep.ruleIds),
    new Set(protoStep.ruleIds),
  ])
  const matchingRuleIds = [...matching].sort((a, b) => a - b)

  if (matchingRuleIds.length === 0) {
    // Figure out *which* bag killed it so we can tell the user which
    // axis didn't match — helpful for debugging.
    const emptyBags = bagSteps.filter((s) => s.ruleIds.length === 0).map((s) => s.bag)
    const reason =
      emptyBags.length > 0
        ? `no rule matched — ${emptyBags.join(', ')} bag${emptyBags.length === 1 ? '' : 's'} had no entries for the lookup key`
        : 'no rule remained after intersecting the four bag vectors — default-deny'
    return {
      input,
      srcResolution,
      dstResolution,
      bagSteps,
      matchingRuleIds: [],
      winningRule: null,
      verdict: 'DENY',
      verdictReason: reason,
    }
  }

  // Apply precedence.
  const matchingRules = matchingRuleIds
    .map((id) => db.rules.find((r) => r.id === id))
    .filter((r): r is Rule => r !== undefined)

  const winningRule = pickWinningRule(matchingRules)
  const verdict = verdictOf(winningRule.action)
  const verdictReason = explainWin(winningRule, matchingRules)

  return {
    input,
    srcResolution,
    dstResolution,
    bagSteps,
    matchingRuleIds,
    winningRule,
    verdict,
    verdictReason,
  }
}

/* -------------------- resolution helpers -------------------- */

function resolveIp(db: LoadedDb, ip: string): Resolution {
  const asset = db.assets.find((a) => a.ips.includes(ip)) ?? null
  return { ip, asset }
}

/**
 * Look up an EID-keyed bag (src or dst) for a given resolution.
 *
 * Always consults the ANY_EID slot in addition to the concrete-EID
 * slot. The result's `ruleIds` is the union, which is what feeds the
 * four-way AND. `eidRuleIds` and `anyEidRuleIds` keep the two streams
 * separate so the trace UI can show "this came from your asset" vs
 * "this came from a SEL_ALL rule".
 *
 * For unresolved IPs (`resolution.asset == null`) there is no concrete
 * slot to look up — `eidRuleIds` is empty and the entire `ruleIds`
 * comes from the ANY_EID slot. This is what implements the
 * "unresolved IP → ANY_EID" semantics from LANGUAGE.md §6.2.
 */
function lookupEidBag(
  bags: Bags,
  bag: 'src' | 'dst',
  resolution: Resolution,
  ip: string,
): BagLookupStep {
  const asset = resolution.asset
  const concrete = asset
    ? bags[bag].find((e) => e.key === asset.eidHex) ?? null
    : null
  const anyEntry = bags[bag].find((e) => e.key === ANY_EID_HEX) ?? null

  const eidRuleIds = concrete?.ruleIds ?? []
  const anyEidRuleIds = anyEntry?.ruleIds ?? []

  // Union — JS Set keeps it small even if the lists are long.
  const union = new Set<number>(eidRuleIds)
  for (const r of anyEidRuleIds) union.add(r)
  const ruleIds = [...union].sort((a, b) => a - b)

  // Display key + explainer. Resolved IP shows its EID; unresolved IP
  // shows the ANY_EID hex so the user can see the fallback in action.
  const keyLabel = asset ? asset.eidHex : ANY_EID_HEX
  const keyExplain = asset
    ? `${ip} → ipcache → ${asset.eidHex}`
    : `${ip} not in ipcache → ANY_EID`

  return {
    bag,
    keyLabel,
    keyExplain,
    bagId: concrete?.bagId ?? null,
    ruleIds,
    eidRuleIds,
    anyEidRuleIds,
  }
}

function lookupPortBag(bags: Bags, port: number): BagLookupStep {
  const entry = bags.port.find((e) => e.key === String(port))
  return {
    bag: 'port',
    keyLabel: String(port),
    bagId: entry?.bagId ?? null,
    ruleIds: entry?.ruleIds ?? [],
  }
}

function lookupProtoBag(bags: Bags, proto: Protocol): BagLookupStep {
  const entry = bags.proto.find((e) => e.key === proto)
  return {
    bag: 'proto',
    keyLabel: proto,
    bagId: entry?.bagId ?? null,
    ruleIds: entry?.ruleIds ?? [],
  }
}

/* -------------------- precedence logic -------------------- */

const ACTION_RANK: Record<RuleAction, number> = {
  'OVERRIDE-BLOCK': 0, // highest priority
  'OVERRIDE-ALLOW': 1,
  'ALLOW':          2,
  'BLOCK':          3,
}

/**
 * Pick the winning rule from a set that all matched. Lower ACTION_RANK
 * wins; ties broken by rule id ascending (first-defined wins, mirroring
 * the compiler's source-order assignment of rule IDs).
 */
function pickWinningRule(rules: Rule[]): Rule {
  return [...rules].sort((a, b) => {
    const rd = ACTION_RANK[a.action] - ACTION_RANK[b.action]
    if (rd !== 0) return rd
    return a.id - b.id
  })[0]
}

function verdictOf(action: RuleAction): Verdict {
  return action === 'ALLOW' || action === 'OVERRIDE-ALLOW' ? 'ALLOW' : 'DENY'
}

function explainWin(winner: Rule, all: Rule[]): string {
  if (all.length === 1) {
    return `rule #${winner.id} (${winner.action}) was the only match`
  }
  const overridden = all.filter((r) => r.id !== winner.id).map((r) => `#${r.id} ${r.action}`)
  return `${all.length} rules matched; rule #${winner.id} (${winner.action}) wins by precedence over ${overridden.join(', ')}`
}

/* -------------------- set intersection -------------------- */

function intersectAll(sets: Set<number>[]): Set<number> {
  if (sets.length === 0) return new Set()
  // Start from the smallest set — fewest items to iterate through.
  const sorted = [...sets].sort((a, b) => a.size - b.size)
  const result = new Set<number>()
  for (const x of sorted[0]) {
    let allContain = true
    for (let i = 1; i < sorted.length; i++) {
      if (!sorted[i].has(x)) { allContain = false; break }
    }
    if (allContain) result.add(x)
  }
  return result
}
