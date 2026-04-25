import { useMemo, useState } from 'react'
import type { BagEntry, LoadedDb, Rule } from '../../lib/types'
import { ColumnFilter } from '../../components/ColumnFilter'
import { RulesTable } from '../../components/RulesTable'
import {
  bitvecToHex,
  formatBagEidKey,
  isAnyEid,
  truncateHex,
} from '../../lib/format'
import { BitvecRenderer } from './BitvecRenderer'

interface Props {
  db: LoadedDb
}

/** Which bag the listbox is currently pointing at. */
type BagName = 'src' | 'dst' | 'port' | 'proto'

const BAG_OPTIONS: { id: BagName; label: string; keyHeader: string }[] = [
  { id: 'src',   label: 'bag_src   (source EIDs)',      keyHeader: 'EID' },
  { id: 'dst',   label: 'bag_dst   (destination EIDs)', keyHeader: 'EID' },
  { id: 'port',  label: 'bag_port  (ports)',            keyHeader: 'Port' },
  { id: 'proto', label: 'bag_proto (protocols)',        keyHeader: 'Protocol' },
]

/**
 * Full bitvector size used everywhere in the compiled output. The runtime
 * uses a 4096-bit vector per bag entry, packed into 512 bytes. Source
 * of truth: `SCHEMA_RUNTIME_SQL` in the compiler's builder.c (bagvec
 * table has a BLOB of 512 bytes).
 */
const VECTOR_BITS = 4096
const VECTOR_BYTES = VECTOR_BITS / 8

/**
 * Map an entry's raw key (e.g. EID hex) to the display form shown to
 * the user (e.g. "ALL_EIDS" for the ANY_EID sentinel). For bags whose
 * keys are already user-facing (port number, protocol name) this is
 * the identity. We use the display form as both the visible label AND
 * the value tracked by the filter popover, so the filter's text-search
 * matches what the user reads.
 */
function displayKey(bag: BagName, key: string): string {
  if (bag === 'src' || bag === 'dst') return formatBagEidKey(key)
  return key
}

export function BagVectorTab({ db }: Props) {
  // Which bag is visible right now. src by default — it's the most
  // commonly-inspected one since it's keyed on source EIDs (who's
  // initiating traffic).
  const [bag, setBag] = useState<BagName>('src')
  const [selectedKey, setSelectedKey] = useState<string | null>(null)
  const [modalEntry, setModalEntry] = useState<BagEntry | null>(null)
  /** Filter values are display-keys (so searching "ALL_EIDS" works). */
  const [keyFilter, setKeyFilter] = useState<string[]>([])

  const currentEntries: BagEntry[] = db.bags[bag]
  const currentMeta = BAG_OPTIONS.find((o) => o.id === bag)!

  // Filter only by key, per spec. The popover's option list and selected
  // values are display-keys; we compare against each entry's display-key
  // when filtering — the underlying `entry.key` (raw hex / port / proto)
  // is unchanged.
  const filteredEntries = useMemo(() => {
    if (keyFilter.length === 0) return currentEntries
    const sel = new Set(keyFilter)
    return currentEntries.filter((e) => sel.has(displayKey(bag, e.key)))
  }, [currentEntries, keyFilter, bag])

  const allKeys = useMemo(
    () => currentEntries.map((e) => displayKey(bag, e.key)),
    [currentEntries, bag],
  )

  // When switching bags, discard any prior selection — keys aren't
  // compatible across bag types.
  const handleBagChange = (next: BagName) => {
    setBag(next)
    setSelectedKey(null)
    setKeyFilter([])
  }

  const selectedEntry: BagEntry | null =
    selectedKey != null
      ? currentEntries.find((e) => e.key === selectedKey) ?? null
      : null

  const associatedRules: Rule[] = useMemo(() => {
    if (!selectedEntry) return []
    return selectedEntry.ruleIds
      .map((id) => db.rules.find((r) => r.id === id))
      .filter((r): r is Rule => r !== undefined)
  }, [selectedEntry, db.rules])

  return (
    <div className="bagvec-tab">
      <header className="bagvec-toolbar">
        <label className="bagvec-select-label">
          Bag
          <select
            className="bagvec-select"
            value={bag}
            onChange={(e) => handleBagChange(e.target.value as BagName)}
          >
            {BAG_OPTIONS.map((o) => (
              <option key={o.id} value={o.id}>
                {o.label}
              </option>
            ))}
          </select>
        </label>
        <span className="bagvec-count">
          {currentEntries.length} {currentEntries.length === 1 ? 'entry' : 'entries'}
        </span>
        <button
          type="button"
          className="inline-clear"
          onClick={() => {
            setSelectedKey(null)
            setKeyFilter([])
          }}
          disabled={selectedKey === null && keyFilter.length === 0}
          title="Clear selection and filter"
        >
          Clear
        </button>
      </header>

      {/* ===== TOP: bag vector table ===== */}
      <section className="bagvec-top">
        <div className="inv-table-wrap">
          <table className="inv-table">
            <thead>
              <tr>
                <th className="bagvec-key-col">
                  {currentMeta.keyHeader}
                  <ColumnFilter
                    options={allKeys}
                    selected={keyFilter}
                    onChange={setKeyFilter}
                    placeholder="search..."
                  />
                </th>
                <th>Vector</th>
              </tr>
            </thead>
            <tbody>
              {filteredEntries.map((entry) => {
                const isSelected = entry.key === selectedKey
                const hex = bitvecToHex(entry.ruleIds, VECTOR_BYTES)
                const display = truncateHex(hex)
                const isAnyEidRow =
                  (bag === 'src' || bag === 'dst') && isAnyEid(entry.key)
                return (
                  <tr
                    key={entry.key}
                    className={[
                      isSelected ? 'row-selected' : '',
                      isAnyEidRow ? 'bagvec-row--any-eid' : '',
                    ]
                      .filter(Boolean)
                      .join(' ')}
                    onClick={() =>
                      setSelectedKey(isSelected ? null : entry.key)
                    }
                  >
                    <td>{renderKey(bag, entry.key)}</td>
                    <td>
                      <button
                        type="button"
                        className="bagvec-hex"
                        title={hex}
                        onClick={(ev) => {
                          // Don't also toggle row selection when clicking
                          // the hex — it's a distinct action.
                          ev.stopPropagation()
                          setModalEntry(entry)
                        }}
                      >
                        {display}
                      </button>
                      <span className="bagvec-bit-count">
                        {entry.ruleIds.length} bit{entry.ruleIds.length === 1 ? '' : 's'}
                      </span>
                    </td>
                  </tr>
                )
              })}
              {filteredEntries.length === 0 && (
                <tr>
                  <td
                    colSpan={2}
                    style={{
                      textAlign: 'center',
                      padding: '32px',
                      color: 'var(--ink-dim)',
                    }}
                  >
                    {currentEntries.length === 0
                      ? 'no entries in this bag'
                      : 'no entries match the filter'}
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </section>

      {/* ===== BOTTOM: associated rules for selected row ===== */}
      <section className="bagvec-bottom">
        <header className="bagvec-bottom-header">
          <span className="bagvec-bottom-title">Associated Rules</span>
          {selectedEntry && (
            <span className="bagvec-bottom-meta">
              for {currentMeta.keyHeader.toLowerCase()}{' '}
              <span className="bagvec-bottom-key">
                {displayKey(bag, selectedEntry.key)}
              </span>
              {(bag === 'src' || bag === 'dst') &&
                isAnyEid(selectedEntry.key) && (
                  <span className="bagvec-bottom-anyeid-hint">
                    {' '}— matches every EID, including unresolved IPs
                  </span>
                )}
            </span>
          )}
        </header>
        {!selectedEntry ? (
          <div className="bagvec-bottom-hint">
            Click a row above to see its referenced rules
          </div>
        ) : (
          <RulesTable
            rules={associatedRules}
            nothingToShowMessage="— no rules referenced —"
            emptyMessage="no rules match the active filters"
          />
        )}
      </section>

      {modalEntry && (
        <BitvecModal
          entry={modalEntry}
          bag={bag}
          onClose={() => setModalEntry(null)}
        />
      )}
    </div>
  )
}

/* -------------------- modal -------------------- */

interface BitvecModalProps {
  entry: BagEntry
  bag: BagName
  onClose: () => void
}

/** Full 4096-bit grid visualization, shown when the user clicks a hex. */
function BitvecModal({ entry, bag, onClose }: BitvecModalProps) {
  const keyHeader = BAG_OPTIONS.find((o) => o.id === bag)!.keyHeader
  const hex = bitvecToHex(entry.ruleIds, VECTOR_BYTES)

  return (
    <div
      className="bitvec-modal-backdrop"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
    >
      <div
        className="bitvec-modal"
        onClick={(e) => e.stopPropagation()}
      >
        <header className="bitvec-modal-header">
          <div className="bitvec-modal-titles">
            <span className="bitvec-modal-label">{keyHeader}</span>
            <span className="bitvec-modal-key">{renderKey(bag, entry.key)}</span>
          </div>
          <button
            className="bitvec-modal-close"
            onClick={onClose}
            aria-label="Close"
          >
            ×
          </button>
        </header>
        <div className="bitvec-modal-stats">
          <span>
            <span className="bitvec-modal-stats-label">bits set</span>
            <span className="bitvec-modal-stats-value">{entry.ruleIds.length}</span>
            <span className="bitvec-modal-stats-total">/ {VECTOR_BITS}</span>
          </span>
          <span>
            <span className="bitvec-modal-stats-label">bag_id</span>
            <span className="bitvec-modal-stats-value">{entry.bagId}</span>
          </span>
        </div>
        <div className="bitvec-modal-hex">
          <span className="bitvec-modal-hex-label">hex</span>
          <code className="bitvec-modal-hex-value">{hex}</code>
        </div>
        <div className="bitvec-modal-grid">
          <BitvecRenderer ruleIds={entry.ruleIds} />
        </div>
      </div>
    </div>
  )
}

/* -------------------- key rendering -------------------- */

/**
 * Port / proto / EID keys deserve slightly different visual treatment —
 * EIDs are long hex hashes best shown in mono; ports are just numbers;
 * protos are short uppercase labels.
 *
 * For src/dst bags, the ANY_EID sentinel renders as the literal string
 * "ALL_EIDS" with a distinct class (`col-eid--any`) so it's visually
 * clear this row is the wildcard slot and not a concrete EID.
 */
function renderKey(bag: BagName, key: string) {
  if (bag === 'src' || bag === 'dst') {
    if (isAnyEid(key)) {
      return (
        <span
          className="col-eid col-eid--any"
          title="ANY_EID — bits OR'd into every per-EID lookup at runtime"
        >
          ALL_EIDS
        </span>
      )
    }
    return <span className="col-eid">{key}</span>
  }
  if (bag === 'port') {
    return <span className="col-ports">{key}</span>
  }
  return <span className="proto-chip">{key}</span>
}
