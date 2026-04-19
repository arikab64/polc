/**
 * Packet evaluator — simulates what the runtime datapath would do:
 *
 *   src IP → EID via ipcache
 *   dst IP → EID
 *   AND(bag_src[src_eid], bag_dst[dst_eid], bag_port[port], bag_proto[proto])
 *   → pick highest-priority matching rule
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
 * which bag produced which bit-set, which AND eliminated which rules,
 * which rule finally won.
 */

import type { Asset, Bags, LoadedDb, Rule, RuleAction } from './types'

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
  /** bag_id the key mapped to, or null if the key wasn't present in the bag. */
  bagId: number | null
  /** Rule IDs set in that bag's bitvector. Empty if bagId is null. */
  ruleIds: number[]
}

export interface Resolution {
  /** The IP the user supplied. */
  ip: string
  /** The asset that owns it, if any. */
  asset: Asset | null
}

export interface SimulatorResult {
  input: SimulatorInput
  /** Resolution of each IP to its owning asset. asset is null if the IP
   *  isn't in the inventory — that short-circuits to DENY. */
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

  // If either IP isn't in the inventory, we can't run the bag lookups
  // at all — no src/dst EID, no bags to AND. Short-circuit to DENY.
  if (!srcResolution.asset || !dstResolution.asset) {
    const missingSide = !srcResolution.asset ? 'source' : 'destination'
    const missingIp = !srcResolution.asset ? input.srcIp : input.dstIp
    return {
      input,
      srcResolution,
      dstResolution,
      bagSteps: [],
      matchingRuleIds: [],
      winningRule: null,
      verdict: 'DENY',
      verdictReason: `${missingSide} IP ${missingIp} has no EID in the inventory — default-deny`,
    }
  }

  // Build the four bag-lookup steps. Each yields a ruleId set (empty for
  // "key not present" which is equivalent to the all-zero bagvec).
  const srcStep = lookupEidBag(db.bags, 'src', srcResolution.asset.eidHex, input.srcIp)
  const dstStep = lookupEidBag(db.bags, 'dst', dstResolution.asset.eidHex, input.dstIp)
  const portStep = lookupPortBag(db.bags, input.port)
  const protoStep = lookupProtoBag(db.bags, input.proto)
  const bagSteps = [srcStep, dstStep, portStep, protoStep]

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

function lookupEidBag(
  bags: Bags,
  bag: 'src' | 'dst',
  eidHex: string,
  ip: string,
): BagLookupStep {
  const entry = bags[bag].find((e) => e.key === eidHex)
  return {
    bag,
    keyLabel: eidHex,
    keyExplain: `${ip} → ipcache → ${eidHex}`,
    bagId: entry?.bagId ?? null,
    ruleIds: entry?.ruleIds ?? [],
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
