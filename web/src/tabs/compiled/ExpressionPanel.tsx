import { useEffect, useMemo, useState } from 'react'
import type { Asset, Label, LoadedDb } from '../../lib/types'
import { Chip } from '../../components/Chip'
import {
  evaluateExpression,
  labelSetOf,
  parseExpression,
  type Expr,
} from '../../lib/expression'

interface Props {
  db: LoadedDb
}

/**
 * Always-visible right panel for the Enforcement Identities tab.
 *
 * Three stacked sections:
 *
 *   ┌── Expression: ────────────────────────┐
 *   │  [textarea]                           │
 *   │  (optional red error line)            │
 *   ├── Resolved Assets: ───────────────────┤
 *   │  web-prod-1                           │   ← click to drill down
 *   │  web-prod-2                           │
 *   ├── Details: web-prod-1 ────────────────┤   ← hidden when no asset selected
 *   │  IPs / EID / Labels                   │
 *   └───────────────────────────────────────┘
 *
 * Evaluation runs client-side against the already-loaded assets. See
 * lib/expression.ts for the grammar and parser.
 */
export function ExpressionPanel({ db }: Props) {
  const [text, setText] = useState<string>('')
  const [selectedAssetName, setSelectedAssetName] = useState<string | null>(null)

  // Derived: parse result (invalid syntax -> error state).
  const parseResult = useMemo(() => {
    if (text.trim().length === 0) return null
    return parseExpression(text)
  }, [text])

  // Derived: the set of asset names that match the expression. When the
  // expression is empty or invalid, resolvedAssets is null (panel shows
  // different states in those cases).
  const resolvedAssets = useMemo<Asset[] | null>(() => {
    if (!parseResult || !parseResult.ok) return null
    return resolveAssets(db.assets, parseResult.expr)
  }, [parseResult, db.assets])

  // The currently-selected asset's full record, or null.
  const selectedAsset = useMemo<Asset | null>(() => {
    if (!selectedAssetName) return null
    return db.assets.find((a) => a.name === selectedAssetName) ?? null
  }, [db.assets, selectedAssetName])

  // If the resolved set no longer contains the selected asset (user edited
  // the expression), clear the selection so stale details don't linger.
  useEffect(() => {
    if (
      selectedAssetName &&
      resolvedAssets !== null &&
      !resolvedAssets.some((a) => a.name === selectedAssetName)
    ) {
      setSelectedAssetName(null)
    }
  }, [resolvedAssets, selectedAssetName])

  return (
    <aside className="expression-panel">
      <section className="asset-box">
        <header className="asset-box-title">Expression</header>
        <textarea
          className="expr-input"
          value={text}
          onChange={(e) => setText(e.target.value)}
          placeholder="(env:production OR env:staging) AND role:database"
          spellCheck={false}
          rows={3}
        />
        {parseResult && !parseResult.ok && (
          <div className="expr-error">{parseResult.error}</div>
        )}
      </section>

      <section className="asset-box">
        <header className="asset-box-title">Resolved Assets</header>
        <ResolvedAssetsList
          resolvedAssets={resolvedAssets}
          expressionEmpty={text.trim().length === 0}
          expressionInvalid={!!parseResult && !parseResult.ok}
          selectedAssetName={selectedAssetName}
          onSelectAsset={setSelectedAssetName}
        />
      </section>

      {/* per mockup: "If None resolved asset - hide this sub panel" */}
      {selectedAsset && <AssetDetails asset={selectedAsset} />}
    </aside>
  )
}

/* -------------------- resolved assets list -------------------- */

interface ResolvedProps {
  resolvedAssets: Asset[] | null
  expressionEmpty: boolean
  expressionInvalid: boolean
  selectedAssetName: string | null
  onSelectAsset: (name: string | null) => void
}

function ResolvedAssetsList({
  resolvedAssets,
  expressionEmpty,
  expressionInvalid,
  selectedAssetName,
  onSelectAsset,
}: ResolvedProps) {
  if (expressionEmpty) {
    return <div className="asset-box-hint">Enter an expression to resolve assets</div>
  }
  if (expressionInvalid) {
    return <div className="asset-box-hint">waiting for valid expression...</div>
  }
  if (!resolvedAssets || resolvedAssets.length === 0) {
    return <div className="asset-box-empty">— no assets match —</div>
  }
  return (
    <ul className="asset-list">
      {resolvedAssets.map((a) => {
        const isSelected = a.name === selectedAssetName
        return (
          <li key={a.name}>
            <button
              className={`asset-pill ${isSelected ? 'asset-pill--selected' : ''}`}
              onClick={() => onSelectAsset(isSelected ? null : a.name)}
            >
              {a.name}
            </button>
          </li>
        )
      })}
    </ul>
  )
}

/* -------------------- asset details sub-panel -------------------- */

function AssetDetails({ asset }: { asset: Asset }) {
  return (
    <section className="asset-details">
      <header className="asset-details-title">
        <span className="asset-details-label">Asset Details</span>
        <span className="asset-details-name">{asset.name}</span>
      </header>
      <dl className="asset-details-list">
        <dt>IPs</dt>
        <dd>
          {asset.ips.length === 0 ? (
            <span className="asset-empty">—</span>
          ) : (
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
          {asset.labels.length === 0 ? (
            <span className="asset-empty">—</span>
          ) : (
            <div className="chips">
              {asset.labels.map((l: Label) => (
                <Chip key={`${l.key}:${l.value}`} labelKey={l.key} labelValue={l.value} />
              ))}
            </div>
          )}
        </dd>
      </dl>
    </section>
  )
}

/* -------------------- evaluator glue -------------------- */

/**
 * Return assets matching the parsed expression, preserving db.assets order.
 *
 * Two-pass algorithm:
 *   1. Build a cache keyed by eidHex → boolean (matches?). Many assets
 *      share an EID, so we avoid re-evaluating the same label set.
 *   2. Filter assets by checking the cache for each asset's eidHex.
 */
function resolveAssets(assets: Asset[], expr: Expr): Asset[] {
  const verdictByEid = new Map<string, boolean>()
  const result: Asset[] = []
  for (const a of assets) {
    let verdict = verdictByEid.get(a.eidHex)
    if (verdict === undefined) {
      verdict = evaluateExpression(expr, labelSetOf(a.labels))
      verdictByEid.set(a.eidHex, verdict)
    }
    if (verdict) result.push(a)
  }
  return result
}
