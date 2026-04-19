import { useMemo, useState } from 'react'
import type { Asset, Label, LoadedDb } from '../lib/types'
import { Chip } from './Chip'

interface Props {
  db: LoadedDb
  /** Which slot the picker is feeding — used only for the modal title. */
  slot: 'src' | 'dst'
  onPick: (ip: string) => void
  onClose: () => void
}

type Mode = 'ip' | 'asset' | 'label'

/**
 * Three-mode IP picker. User opens it from the icon next to a src/dst
 * IP textbox and either:
 *
 *   1. IP List  — flat list of every IP in the inventory, each row shows
 *                 the IP and its owning asset. One click fills the box.
 *   2. Asset    — list assets; clicking an asset with a single IP fills
 *                 the box immediately. Multi-IP assets expand to show
 *                 each IP under the asset name.
 *   3. Label    — pick a label, see which assets/IPs match. Useful for
 *                 "pick a production database" without remembering the
 *                 exact asset name.
 *
 * On pick the modal closes. No Apply button — pick-and-go is faster and
 * the user can reopen the picker if they got the wrong one.
 */
export function IpPickerModal({ db, slot, onPick, onClose }: Props) {
  const [mode, setMode] = useState<Mode>('ip')
  const [query, setQuery] = useState('')

  const handlePick = (ip: string) => {
    onPick(ip)
    onClose()
  }

  return (
    <div
      className="picker-modal-backdrop"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
    >
      <div
        className="picker-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <header className="picker-modal-header">
          <span className="picker-modal-title">
            Pick {slot === 'src' ? 'source' : 'destination'} IP
          </span>
          <button
            className="bitvec-modal-close"
            onClick={onClose}
            aria-label="Close"
          >
            ×
          </button>
        </header>

        <nav className="picker-modes" role="tablist">
          <button
            role="tab"
            className="picker-mode"
            aria-selected={mode === 'ip'}
            onClick={() => setMode('ip')}
          >
            By IP
          </button>
          <button
            role="tab"
            className="picker-mode"
            aria-selected={mode === 'asset'}
            onClick={() => setMode('asset')}
          >
            By Asset
          </button>
          <button
            role="tab"
            className="picker-mode"
            aria-selected={mode === 'label'}
            onClick={() => setMode('label')}
          >
            By Label
          </button>
        </nav>

        <div className="picker-search">
          <input
            type="text"
            className="picker-search-input"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder={
              mode === 'ip' ? 'filter IPs or asset names...' :
              mode === 'asset' ? 'filter assets...' :
              'filter key:value labels...'
            }
            autoFocus
          />
        </div>

        <div className="picker-body">
          {mode === 'ip' && <ByIpList db={db} query={query} onPick={handlePick} />}
          {mode === 'asset' && <ByAssetList db={db} query={query} onPick={handlePick} />}
          {mode === 'label' && <ByLabelList db={db} query={query} onPick={handlePick} />}
        </div>
      </div>
    </div>
  )
}

/* -------------------- mode: IP list -------------------- */

function ByIpList({
  db,
  query,
  onPick,
}: {
  db: LoadedDb
  query: string
  onPick: (ip: string) => void
}) {
  // Flatten assets into one row per IP, preserving the owning asset.
  const rows = useMemo(() => {
    const out: { ip: string; asset: Asset }[] = []
    for (const a of db.assets) {
      for (const ip of a.ips) out.push({ ip, asset: a })
    }
    return out.sort((a, b) => ipNum(a.ip) - ipNum(b.ip))
  }, [db.assets])

  const filtered = useMemo(() => {
    if (!query.trim()) return rows
    const q = query.toLowerCase()
    return rows.filter(
      (r) => r.ip.includes(q) || r.asset.name.toLowerCase().includes(q),
    )
  }, [rows, query])

  if (filtered.length === 0) {
    return <div className="picker-empty">no matches</div>
  }

  return (
    <ul className="picker-list">
      {filtered.map(({ ip, asset }) => (
        <li key={ip}>
          <button className="picker-row" onClick={() => onPick(ip)}>
            <span className="picker-row-primary">{ip}</span>
            <span className="picker-row-secondary">{asset.name}</span>
          </button>
        </li>
      ))}
    </ul>
  )
}

/* -------------------- mode: by asset -------------------- */

function ByAssetList({
  db,
  query,
  onPick,
}: {
  db: LoadedDb
  query: string
  onPick: (ip: string) => void
}) {
  const filtered = useMemo(() => {
    if (!query.trim()) return db.assets
    const q = query.toLowerCase()
    return db.assets.filter((a) => a.name.toLowerCase().includes(q))
  }, [db.assets, query])

  if (filtered.length === 0) {
    return <div className="picker-empty">no matches</div>
  }

  return (
    <ul className="picker-list">
      {filtered.map((asset) => {
        // Single-IP asset: one row; multi-IP: header + nested rows.
        if (asset.ips.length <= 1) {
          return (
            <li key={asset.name}>
              <button
                className="picker-row"
                onClick={() => asset.ips[0] && onPick(asset.ips[0])}
                disabled={asset.ips.length === 0}
              >
                <span className="picker-row-primary">{asset.name}</span>
                <span className="picker-row-secondary">
                  {asset.ips[0] ?? '— no IPs —'}
                </span>
              </button>
            </li>
          )
        }
        return (
          <li key={asset.name} className="picker-group">
            <div className="picker-group-head">
              <span className="picker-row-primary">{asset.name}</span>
              <span className="picker-row-secondary">{asset.ips.length} IPs</span>
            </div>
            <ul className="picker-sublist">
              {asset.ips.map((ip) => (
                <li key={ip}>
                  <button className="picker-row picker-row--nested" onClick={() => onPick(ip)}>
                    <span className="picker-row-primary">{ip}</span>
                  </button>
                </li>
              ))}
            </ul>
          </li>
        )
      })}
    </ul>
  )
}

/* -------------------- mode: by label -------------------- */

function ByLabelList({
  db,
  query,
  onPick,
}: {
  db: LoadedDb
  query: string
  onPick: (ip: string) => void
}) {
  // Group: label (k:v) -> assets that have it
  const grouped = useMemo(() => {
    const map = new Map<string, { label: Label; assets: Asset[] }>()
    for (const a of db.assets) {
      for (const l of a.labels) {
        const key = `${l.key}:${l.value}`
        const entry = map.get(key)
        if (entry) {
          entry.assets.push(a)
        } else {
          map.set(key, { label: l, assets: [a] })
        }
      }
    }
    return [...map.entries()]
      .map(([key, v]) => ({ key, ...v }))
      .sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0))
  }, [db.assets])

  const filtered = useMemo(() => {
    if (!query.trim()) return grouped
    const q = query.toLowerCase()
    return grouped.filter((g) => g.key.toLowerCase().includes(q))
  }, [grouped, query])

  if (filtered.length === 0) {
    return <div className="picker-empty">no matches</div>
  }

  return (
    <ul className="picker-list">
      {filtered.map(({ key, label, assets }) => (
        <li key={key} className="picker-group">
          <div className="picker-group-head">
            <Chip labelKey={label.key} labelValue={label.value} />
            <span className="picker-row-secondary">
              {assets.length} asset{assets.length === 1 ? '' : 's'}
            </span>
          </div>
          <ul className="picker-sublist">
            {assets.flatMap((a) =>
              a.ips.map((ip) => (
                <li key={`${a.name}-${ip}`}>
                  <button className="picker-row picker-row--nested" onClick={() => onPick(ip)}>
                    <span className="picker-row-primary">{ip}</span>
                    <span className="picker-row-secondary">{a.name}</span>
                  </button>
                </li>
              )),
            )}
          </ul>
        </li>
      ))}
    </ul>
  )
}

/* -------------------- helpers -------------------- */

function ipNum(dotted: string): number {
  const p = dotted.split('.')
  return (
    (((Number(p[0]) & 0xff) << 24) |
      ((Number(p[1]) & 0xff) << 16) |
      ((Number(p[2]) & 0xff) << 8) |
      (Number(p[3]) & 0xff)) >>> 0
  )
}
