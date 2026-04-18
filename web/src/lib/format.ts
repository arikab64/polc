/**
 * Pure helpers shared by UI and worker.
 *
 * IMPORTANT: These have to handle the quirks of how polc stores
 * numeric data (see db.sql header for the full story):
 *
 *   IPs      — INTEGER, host-order uint32. May round-trip as negative
 *              through a signed read. We mask with >>> 0 to get the
 *              unsigned value, then extract bytes.
 *
 *   EIDs     — INTEGER, 64-bit signed. The full range doesn't fit in
 *              a JS Number safely, so we must read them via BigInt.
 *              We format as 0x + 16 hex chars. sqlite-wasm returns
 *              large ints as BigInt when bigint support is on; we
 *              also accept number here for robustness.
 */

/** Render a host-order uint32 as a dotted-quad string. */
export function ipToString(ip: number | bigint): string {
  const n = typeof ip === 'bigint' ? Number(BigInt.asUintN(32, ip)) : ip >>> 0
  return [(n >>> 24) & 0xff, (n >>> 16) & 0xff, (n >>> 8) & 0xff, n & 0xff].join('.')
}

/** Format a 64-bit signed int EID hash as 0x + 16 uppercase hex chars. */
export function eidToHex(hash: number | bigint): string {
  const b = typeof hash === 'bigint' ? BigInt.asUintN(64, hash) : BigInt.asUintN(64, BigInt(hash))
  return '0x' + b.toString(16).padStart(16, '0').toUpperCase()
}

/**
 * Map a label key to a chip-color class. Three well-known keys
 * (env / app / role) get distinct palettes matching the diagram;
 * everything else falls back to a neutral chip.
 */
export function chipKeyClass(key: string): 'env' | 'app' | 'role' | 'default' {
  if (key === 'env') return 'env'
  if (key === 'app') return 'app'
  if (key === 'role') return 'role'
  return 'default'
}

/** Sort labels for deterministic display: by key, then value. */
export function sortLabels<T extends { key: string; value: string }>(labels: T[]): T[] {
  return [...labels].sort((a, b) => {
    if (a.key !== b.key) return a.key < b.key ? -1 : 1
    return a.value < b.value ? -1 : a.value > b.value ? 1 : 0
  })
}

/** Compact numeric formatter for the tab-counters. */
export function compactNum(n: number): string {
  if (n < 1000) return String(n)
  if (n < 1_000_000) return (n / 1000).toFixed(n < 10_000 ? 1 : 0) + 'k'
  return (n / 1_000_000).toFixed(1) + 'M'
}

/**
 * Render a sparse bitvector (given as a list of set rule IDs) as a hex
 * string of `byteCount` bytes (2 hex chars each).
 *
 * Endianness: LSB-first per byte, byte 0 covers rules 0..7. So rules
 * 0,1,2,3,4 set → last byte is 0x1F and the full hex reads ...001F.
 * Result is uppercase with a `0x` prefix.
 */
export function bitvecToHex(ruleIds: number[], byteCount: number): string {
  const bytes = new Uint8Array(byteCount)
  for (const rid of ruleIds) {
    if (rid < 0) continue
    const byteIdx = rid >>> 3
    const bitIdx = rid & 7
    if (byteIdx >= byteCount) continue
    bytes[byteIdx] |= 1 << bitIdx
  }
  // Render MSB first — reverse the array into a hex string.
  let out = ''
  for (let i = byteCount - 1; i >= 0; i--) {
    out += bytes[i].toString(16).padStart(2, '0').toUpperCase()
  }
  return '0x' + out
}

/**
 * Truncate a long hex string for table display. Keeps the first `head`
 * and last `tail` hex chars joined with an ellipsis — skips truncation
 * if the string is short enough to fit naturally.
 */
export function truncateHex(hex: string, head = 4, tail = 8): string {
  // `0x` + hex chars; preserve the prefix
  const bare = hex.startsWith('0x') ? hex.slice(2) : hex
  if (bare.length <= head + tail + 1) return hex
  return '0x' + bare.slice(0, head) + '…' + bare.slice(-tail)
}
