import { useMemo } from 'react'
import type { Asset, Rule } from '../lib/types'
import { Chip } from './Chip'

interface RulesPanelProps {
  /** All assets, used to look up labels/IPs when an asset is clicked. */
  assets: Asset[]
  /** The currently-selected rule, or null if none clicked yet. */
  selectedRule: Rule | null
  /** The asset the user has drilled into for details, or null. */
  selectedAssetName: string | null
  onSelectAsset: (name: string | null) => void
  collapsed: boolean
  onToggle: () => void
}

/**
 * Right-side panel for the Rules tab. Three vertical sections:
 *
 *   ┌─── Source Assets ───────┐
 *   │  web-prod-1              │   ← clicking an asset name
 *   │  web-prod-2              │      selects it for detail
 *   ├─── Dst Assets ──────────┤
 *   │  api-prod-1              │
 *   │  api-prod-2              │
 *   ├─── Details: web-prod-1 ─┤
 *   │  IPs: 10.0.0.1           │   ← appended when an asset is selected
 *   │  Labels: [chips]         │
 *   └──────────────────────────┘
 *
 * Hidden entirely when `collapsed` is true (the toggle button lives in
 * the parent so it stays reachable while the panel is closed).
 */
export function RulesPanel({
  assets,
  selectedRule,
  selectedAssetName,
  onSelectAsset,
  collapsed,
}: RulesPanelProps) {
  // O(1) asset lookup by name, recomputed only when the assets list changes
  const assetsByName = useMemo(() => {
    const m = new Map<string, Asset>()
    for (const a of assets) m.set(a.name, a)
    return m
  }, [assets])

  if (collapsed) return null

  const selectedAsset = selectedAssetName ? assetsByName.get(selectedAssetName) ?? null : null

  return (
    <aside className="rules-panel">
      <AssetListBox
        title="Source Assets"
        names={selectedRule?.matchedSrc ?? []}
        ruleSelected={!!selectedRule}
        selectedAssetName={selectedAssetName}
        onSelectAsset={onSelectAsset}
      />
      <AssetListBox
        title="Dst Assets"
        names={selectedRule?.matchedDst ?? []}
        ruleSelected={!!selectedRule}
        selectedAssetName={selectedAssetName}
        onSelectAsset={onSelectAsset}
      />
      {selectedAsset && <AssetDetails asset={selectedAsset} />}
    </aside>
  )
}

/* -------------------- asset list box -------------------- */

interface AssetListBoxProps {
  title: string
  names: string[]
  /** True when a rule is selected. Drives empty-state copy: "click a rule"
   *  vs "no matches" (rule selected but unresolved / empty side). */
  ruleSelected: boolean
  selectedAssetName: string | null
  onSelectAsset: (name: string | null) => void
}

function AssetListBox({
  title,
  names,
  ruleSelected,
  selectedAssetName,
  onSelectAsset,
}: AssetListBoxProps) {
  return (
    <section className="asset-box">
      <header className="asset-box-title">{title}</header>
      {!ruleSelected && <div className="asset-box-hint">Click a rule to see matches</div>}
      {ruleSelected && names.length === 0 && (
        <div className="asset-box-empty">— no matches —</div>
      )}
      {ruleSelected && names.length > 0 && (
        <ul className="asset-list">
          {names.map((name) => {
            const isSelected = name === selectedAssetName
            return (
              <li key={name}>
                <button
                  className={`asset-pill ${isSelected ? 'asset-pill--selected' : ''}`}
                  onClick={() => onSelectAsset(isSelected ? null : name)}
                >
                  {name}
                </button>
              </li>
            )
          })}
        </ul>
      )}
    </section>
  )
}

/* -------------------- asset details -------------------- */

function AssetDetails({ asset }: { asset: Asset }) {
  return (
    <section className="asset-details">
      <header className="asset-details-title">
        <span className="asset-details-label">Details</span>
        <span className="asset-details-name">{asset.name}</span>
      </header>
      <dl className="asset-details-list">
        <dt>IPs</dt>
        <dd>
          {asset.ips.length === 0 && <span className="asset-empty">—</span>}
          {asset.ips.length > 0 && (
            <ul className="asset-ip-list">
              {asset.ips.map((ip) => (
                <li key={ip}>{ip}</li>
              ))}
            </ul>
          )}
        </dd>
        <dt>EID</dt>
        <dd className="asset-eid">{asset.eidHex}</dd>
        <dt>Labels</dt>
        <dd>
          {asset.labels.length === 0 && <span className="asset-empty">—</span>}
          {asset.labels.length > 0 && (
            <div className="chips">
              {asset.labels.map((l) => (
                <Chip key={`${l.key}:${l.value}`} labelKey={l.key} labelValue={l.value} />
              ))}
            </div>
          )}
        </dd>
      </dl>
    </section>
  )
}
