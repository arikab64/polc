import { useEffect, useMemo, useState } from 'react'
import type { Asset, LoadedDb, Rule } from '../lib/types'
import {
  simulate,
  type Protocol,
  type Resolution,
  type SimulatorResult,
} from '../lib/simulator'
import { IpPickerModal } from '../components/IpPickerModal'
import { Chip } from '../components/Chip'
import { ResolvedIcon } from '../components/ResolvedIcon'
import { RuleSelector } from '../components/RuleSelector'
import { RulesTable } from '../components/RulesTable'
import { bitvecToHex, truncateHex } from '../lib/format'

interface Props {
  db: LoadedDb
}

const VECTOR_BYTES = 512

/**
 * Simulator tab.
 *
 * Three vertical sections: the packet specification form on top, the
 * verdict panel, and the debug trace showing how the verdict was reached.
 *
 * Evaluation runs entirely in-process against the already-loaded data —
 * see lib/simulator.ts.
 */
export function SimulatorTab({ db }: Props) {
  const [srcIp, setSrcIp] = useState('')
  const [dstIp, setDstIp] = useState('')
  const [portText, setPortText] = useState('')
  const [proto, setProto] = useState<Protocol>('TCP')
  const [picker, setPicker] = useState<'src' | 'dst' | null>(null)
  const [result, setResult] = useState<SimulatorResult | null>(null)
  const [formError, setFormError] = useState<string | null>(null)

  const handleSend = () => {
    setFormError(null)
    // Input validation — best to catch obvious errors here rather than
    // let them fall through to DENY with a confusing explanation.
    if (!isValidIp(srcIp)) {
      setFormError('source IP is not a valid IPv4 address')
      setResult(null)
      return
    }
    if (!isValidIp(dstIp)) {
      setFormError('destination IP is not a valid IPv4 address')
      setResult(null)
      return
    }
    const portNum = Number(portText)
    if (!Number.isInteger(portNum) || portNum < 0 || portNum > 65535) {
      setFormError('port must be an integer 0-65535')
      setResult(null)
      return
    }

    const r = simulate(db, { srcIp, dstIp, port: portNum, proto })
    setResult(r)
  }

  const handleClear = () => {
    setSrcIp('')
    setDstIp('')
    setPortText('')
    setProto('TCP')
    setResult(null)
    setFormError(null)
  }

  return (
    <div className="sim-tab">
      {/* ================ TOP: input form ================ */}
      <section className="sim-input">
        <header className="sim-section-title">Packet</header>
        <div className="sim-grid">
          <IpInput
            label="Source IP"
            value={srcIp}
            onChange={setSrcIp}
            onOpenPicker={() => setPicker('src')}
          />
          <IpInput
            label="Destination IP"
            value={dstIp}
            onChange={setDstIp}
            onOpenPicker={() => setPicker('dst')}
          />
          <div className="sim-field sim-field--port">
            <label className="sim-field-label">Port</label>
            <input
              type="text"
              className="sim-text-input"
              value={portText}
              onChange={(e) => setPortText(e.target.value)}
              placeholder="e.g. 443"
              list="sim-port-options"
              inputMode="numeric"
            />
            <datalist id="sim-port-options">
              {db.allPorts.map((p) => (
                <option key={p} value={p} />
              ))}
            </datalist>
          </div>
          <div className="sim-field sim-field--proto">
            <label className="sim-field-label">Protocol</label>
            <div className="sim-radio-group">
              {(['TCP', 'UDP'] as const).map((p) => (
                <label key={p} className="sim-radio">
                  <input
                    type="radio"
                    name="sim-proto"
                    value={p}
                    checked={proto === p}
                    onChange={() => setProto(p)}
                  />
                  {p}
                </label>
              ))}
            </div>
          </div>
          <div className="sim-field sim-field--actions">
            {/* invisible spacer label keeps the buttons vertically aligned
             *  with the other fields' inputs, whose labels sit above them. */}
            <span className="sim-field-label" aria-hidden="true">&nbsp;</span>
            <div className="sim-actions-inline">
              <button className="sim-clear" onClick={handleClear} type="button">
                Clear
              </button>
              <button className="sim-send" onClick={handleSend} type="button">
                SEND
              </button>
            </div>
          </div>
        </div>
        {formError && <div className="sim-form-error">{formError}</div>}
      </section>

      {/* ================ MIDDLE: verdict ================ */}
      <section className="sim-verdict">
        {result ? (
          <VerdictPanel result={result} />
        ) : (
          <div className="sim-verdict-idle">
            Enter a packet and press SEND to evaluate
          </div>
        )}
      </section>

      {/* ================ BOTTOM: debug ================ */}
      {result && <DebugPanel db={db} result={result} />}

      {picker && (
        <IpPickerModal
          db={db}
          slot={picker}
          onPick={(ip) => (picker === 'src' ? setSrcIp(ip) : setDstIp(ip))}
          onClose={() => setPicker(null)}
        />
      )}
    </div>
  )
}

/* -------------------- input with picker button -------------------- */

function IpInput({
  label,
  value,
  onChange,
  onOpenPicker,
}: {
  label: string
  value: string
  onChange: (v: string) => void
  onOpenPicker: () => void
}) {
  return (
    <div className="sim-field">
      <label className="sim-field-label">{label}</label>
      <div className="sim-input-with-button">
        <input
          type="text"
          className="sim-text-input"
          value={value}
          onChange={(e) => onChange(e.target.value)}
          placeholder="e.g. 10.0.0.1"
        />
        <button
          type="button"
          className="sim-picker-btn"
          onClick={onOpenPicker}
          aria-label={`Pick ${label} from inventory`}
          title={`Pick from inventory`}
        >
          {/* Icon: a small database / picker glyph */}
          <svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true">
            <ellipse cx="8" cy="3.5" rx="5.5" ry="2" stroke="currentColor" strokeWidth="1.2" />
            <path d="M2.5 3.5 V8 Q2.5 10 8 10 Q13.5 10 13.5 8 V3.5" stroke="currentColor" strokeWidth="1.2" fill="none" />
            <path d="M2.5 8 V12.5 Q2.5 14.5 8 14.5 Q13.5 14.5 13.5 12.5 V8" stroke="currentColor" strokeWidth="1.2" fill="none" />
          </svg>
        </button>
      </div>
    </div>
  )
}

/* -------------------- verdict panel -------------------- */

function VerdictPanel({ result }: { result: SimulatorResult }) {
  const cls = `sim-verdict-box sim-verdict-box--${result.verdict.toLowerCase()}`
  return (
    <div className={cls}>
      <div className="sim-verdict-label">Verdict</div>
      <div className="sim-verdict-value">{result.verdict}</div>
      <div className="sim-verdict-reason">{result.verdictReason}</div>
    </div>
  )
}

/* -------------------- debug panel -------------------- */

function DebugPanel({ db, result }: { db: LoadedDb; result: SimulatorResult }) {
  // Which bag row is currently selected in the Bag lookups section.
  // Null = show a hint on the right panel. The selection is local to
  // this one debug panel — simulating again resets it.
  const [selectedBag, setSelectedBag] =
    useState<SimulatorResult['bagSteps'][number]['bag'] | null>(null)

  // Reset selection when a new simulation arrives.
  useEffect(() => {
    setSelectedBag(null)
  }, [result])

  const selectedStep =
    selectedBag != null ? result.bagSteps.find((s) => s.bag === selectedBag) ?? null : null

  // Rules referenced by the selected bag row. We look them up in the
  // full db.rules (rather than just result.matchingRuleIds) because the
  // "Rules" column of a single bag lists all rules that bag touches,
  // not just those that survived the AND.
  const selectedRules: Rule[] = useMemo(() => {
    if (!selectedStep) return []
    return selectedStep.ruleIds
      .map((id) => db.rules.find((r) => r.id === id))
      .filter((r): r is Rule => r !== undefined)
  }, [selectedStep, db.rules])

  return (
    <section className="sim-debug">
      <header className="sim-section-title">Debug</header>

      {/* Endpoints: same shape as Endpoint tab's rows */}
      <div className="sim-debug-subsection">
        <h4 className="sim-debug-sub-title">Endpoints</h4>
        <table className="inv-table sim-endpoint-table">
          <thead>
            <tr>
              <th>Side</th>
              <th>IP</th>
              <th>EID</th>
              <th>Asset</th>
              <th>Labels</th>
            </tr>
          </thead>
          <tbody>
            <EndpointRow side="src" resolution={result.srcResolution} />
            <EndpointRow side="dst" resolution={result.dstResolution} />
          </tbody>
        </table>
      </div>

      {/* Bag lookups — split into left table + right associated-rules grid */}
      {result.bagSteps.length > 0 && (
        <div className="sim-debug-subsection">
          <h4 className="sim-debug-sub-title">Bag lookups</h4>
          <div className="sim-bag-split">
            <div className="sim-bag-split-left">
              <table className="inv-table sim-bag-table">
                <thead>
                  <tr>
                    <th>Bag</th>
                    <th>Key</th>
                    <th>Vector (hex)</th>
                    <th>Rules</th>
                  </tr>
                </thead>
                <tbody>
                  {result.bagSteps.map((step) => {
                    const hex = bitvecToHex(step.ruleIds, VECTOR_BYTES)
                    const isSelected = step.bag === selectedBag
                    return (
                      <tr
                        key={step.bag}
                        className={[
                          'row--clickable',
                          isSelected ? 'row--selected' : '',
                        ].filter(Boolean).join(' ')}
                        onClick={() =>
                          setSelectedBag(isSelected ? null : step.bag)
                        }
                      >
                        <td>
                          <code className="sim-bag-name">bag_{step.bag}</code>
                        </td>
                        <td>
                          <span className="sim-bag-key">{step.keyLabel}</span>
                          {step.keyExplain && (
                            <span className="sim-bag-key-hint">{step.keyExplain}</span>
                          )}
                        </td>
                        <td>
                          <code className="sim-bag-hex" title={hex}>
                            {step.bagId === null ? (
                              <span className="sim-bag-miss">(not in bag)</span>
                            ) : (
                              truncateHex(hex)
                            )}
                          </code>
                        </td>
                        <td className="sim-bag-rules">
                          {step.ruleIds.length === 0
                            ? '—'
                            : `{${step.ruleIds.join(', ')}}`}
                        </td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
            <aside className="sim-bag-split-right">
              <header className="sim-bag-split-right-head">
                <span className="sim-bag-split-right-title">Associated Rules</span>
                {selectedStep ? (
                  <span className="sim-bag-split-right-meta">
                    from <code>bag_{selectedStep.bag}</code> ·{' '}
                    {selectedRules.length} rule{selectedRules.length === 1 ? '' : 's'}
                  </span>
                ) : (
                  <span className="sim-bag-split-right-meta">
                    click a bag row on the left
                  </span>
                )}
              </header>
              {selectedStep ? (
                <RulesTable
                  rules={selectedRules}
                  visibleColumns={['id', 'src', 'dst', 'ports', 'protos']}
                  nothingToShowMessage="— bag is empty —"
                  emptyMessage="no rules match the active filters"
                />
              ) : (
                <div className="sim-bag-split-right-hint">
                  Select a bag to view the rules referenced by its bit vector.
                </div>
              )}
            </aside>
          </div>
        </div>
      )}

      {/* AND calculation */}
      {result.bagSteps.length > 0 && (
        <div className="sim-debug-subsection">
          <h4 className="sim-debug-sub-title">Calculation</h4>
          <div className="sim-calc">
            <div className="sim-calc-line">
              <code>
                {result.bagSteps
                  .map((s) => `{${s.ruleIds.join(',')}}`)
                  .join('  ∩  ')}
              </code>
            </div>
            <div className="sim-calc-arrow">=</div>
            <div className="sim-calc-result">
              {result.matchingRuleIds.length === 0 ? (
                <span className="sim-calc-empty">∅ (empty — default-deny)</span>
              ) : (
                <code>{`{${result.matchingRuleIds.join(', ')}}`}</code>
              )}
            </div>
          </div>
        </div>
      )}

      {/* Winning rule */}
      {result.winningRule && (
        <div className="sim-debug-subsection">
          <h4 className="sim-debug-sub-title">Winning rule</h4>
          <WinningRuleBlock rule={result.winningRule} />
        </div>
      )}
    </section>
  )
}

function EndpointRow({
  side,
  resolution,
}: {
  side: 'src' | 'dst'
  resolution: Resolution
}) {
  const asset: Asset | null = resolution.asset
  return (
    <tr>
      <td>
        <span className={`sim-side sim-side--${side}`}>{side}</span>
      </td>
      <td>
        <span className="col-ips">{resolution.ip}</span>
      </td>
      <td>
        {asset ? (
          <span className="col-eid">{asset.eidHex}</span>
        ) : (
          <span className="sim-miss">— not found —</span>
        )}
      </td>
      <td>
        {asset ? (
          <span className="col-asset">{asset.name}</span>
        ) : (
          <span className="sim-miss">—</span>
        )}
      </td>
      <td>
        {asset && asset.labels.length > 0 ? (
          <div className="chips">
            {asset.labels.map((l) => (
              <Chip key={`${l.key}:${l.value}`} labelKey={l.key} labelValue={l.value} />
            ))}
          </div>
        ) : (
          <span className="sim-miss">—</span>
        )}
      </td>
    </tr>
  )
}

function WinningRuleBlock({ rule }: { rule: Rule }) {
  return (
    <div className="sim-winner">
      <header className="sim-winner-head">
        <span className="sim-winner-id">rule #{rule.id}</span>
        <span className={`action action--${actionVariant(rule.action)}`}>
          {rule.action}
        </span>
        <ResolvedIcon resolved={rule.resolved} />
      </header>
      <div className="sim-winner-body">
        <div className="sim-winner-row">
          <span className="sim-winner-label">src</span>
          <RuleSelector terms={rule.src} />
        </div>
        <div className="sim-winner-row">
          <span className="sim-winner-label">dst</span>
          <RuleSelector terms={rule.dst} />
        </div>
        {rule.ports.length > 0 && (
          <div className="sim-winner-row">
            <span className="sim-winner-label">ports</span>
            <span className="col-ports">{rule.ports.join(', ')}</span>
          </div>
        )}
        {rule.protos.length > 0 && (
          <div className="sim-winner-row">
            <span className="sim-winner-label">proto</span>
            <span className="col-protos">
              {rule.protos.map((p) => (
                <span key={p} className="proto-chip">{p}</span>
              ))}
            </span>
          </div>
        )}
      </div>
    </div>
  )
}

function actionVariant(
  a: string,
): 'allow' | 'block' | 'override-allow' | 'override-block' {
  if (a === 'ALLOW') return 'allow'
  if (a === 'BLOCK') return 'block'
  if (a === 'OVERRIDE-ALLOW') return 'override-allow'
  return 'override-block'
}

/* -------------------- utilities -------------------- */

function isValidIp(s: string): boolean {
  const p = s.trim().split('.')
  if (p.length !== 4) return false
  return p.every((seg) => {
    if (!/^\d{1,3}$/.test(seg)) return false
    const n = Number(seg)
    return n >= 0 && n <= 255
  })
}
