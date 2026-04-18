import { memo, useEffect, useMemo, useRef, useState } from 'react'

interface ColumnFilterProps {
  /** all available options for this column (the distinct set) */
  options: string[]
  /** the currently-selected subset. empty = no filter (match all). */
  selected: string[]
  onChange: (next: string[]) => void
  /** optional render override for each option label in the list */
  renderOption?: (value: string) => React.ReactNode
  placeholder?: string
}

/**
 * Column-header filter popover.
 *
 * Matches the diagram annotation: "Columns support filtering with
 * multi-selection of checkboxes + textbox search values".
 *
 * UX:
 *   - trigger shows ⌄ when inactive, ●N when N items are selected
 *   - popover has a text search that narrows the checkbox list
 *   - clicking a row toggles it; no separate apply button
 *   - "clear" in footer resets; "all (shown)" selects the currently visible
 *   - outside click / Escape closes
 *
 * Implementation note: the clickable row contains a native checkbox.
 * We wire `onClick` ONLY on the row and stop propagation from the
 * checkbox itself — if we also wire the checkbox's onChange to the
 * same handler we get two toggles per click (one from the input, one
 * bubbled to the row) and the net effect is no change.
 */
function ColumnFilterImpl({
  options,
  selected,
  onChange,
  renderOption,
  placeholder = 'search...',
}: ColumnFilterProps) {
  const [open, setOpen] = useState(false)
  const [search, setSearch] = useState('')
  const wrapRef = useRef<HTMLSpanElement>(null)

  // close on outside click / Escape
  useEffect(() => {
    if (!open) return
    const onDocClick = (e: MouseEvent) => {
      if (wrapRef.current && !wrapRef.current.contains(e.target as Node)) {
        setOpen(false)
      }
    }
    const onEsc = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false)
    }
    document.addEventListener('mousedown', onDocClick)
    document.addEventListener('keydown', onEsc)
    return () => {
      document.removeEventListener('mousedown', onDocClick)
      document.removeEventListener('keydown', onEsc)
    }
  }, [open])

  const selectedSet = useMemo(() => new Set(selected), [selected])

  const filtered = useMemo(() => {
    const q = search.trim().toLowerCase()
    if (!q) return options
    return options.filter((o) => o.toLowerCase().includes(q))
  }, [options, search])

  const toggle = (value: string) => {
    if (selectedSet.has(value)) {
      onChange(selected.filter((v) => v !== value))
    } else {
      onChange([...selected, value])
    }
  }

  const active = selected.length > 0

  return (
    <span className="col-filter" ref={wrapRef}>
      <button
        className="col-filter-trigger"
        data-active={active}
        onClick={(e) => {
          e.stopPropagation()
          setOpen((o) => !o)
        }}
        title={active ? `${selected.length} selected` : 'filter'}
        aria-label="filter column"
      >
        {active ? `● ${selected.length}` : '⌄'}
      </button>
      {open && (
        <div className="col-filter-popover" onClick={(e) => e.stopPropagation()}>
          <input
            type="search"
            placeholder={placeholder}
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            autoFocus
          />
          <ul className="col-filter-list">
            {filtered.length === 0 && (
              <li className="col-filter-item" style={{ color: 'var(--ink-dim)' }}>
                no matches
              </li>
            )}
            {filtered.map((opt) => (
              <li
                key={opt}
                className="col-filter-item"
                onClick={() => toggle(opt)}
              >
                {/* Purely visual — the row handles the toggle. We make
                    the checkbox non-interactive so a click on it doesn't
                    fire its own onChange AND bubble to the <li>, which
                    would net to zero change. */}
                <input
                  type="checkbox"
                  checked={selectedSet.has(opt)}
                  readOnly
                  tabIndex={-1}
                  style={{ pointerEvents: 'none' }}
                />
                <span>{renderOption ? renderOption(opt) : opt}</span>
              </li>
            ))}
          </ul>
          <div className="col-filter-footer">
            <button onClick={() => onChange([])}>clear</button>
            <button
              onClick={() => {
                const visibleSet = new Set(filtered)
                const everyVisibleSelected = filtered.every((f) => selectedSet.has(f))
                if (everyVisibleSelected) {
                  // visible set is entirely selected — clicking again clears the visible
                  onChange(selected.filter((v) => !visibleSet.has(v)))
                } else {
                  // union of current selection and the currently-visible subset
                  onChange([...new Set([...selected, ...filtered])])
                }
              }}
            >
              all (shown)
            </button>
          </div>
        </div>
      )}
    </span>
  )
}

/** memoized — a parent re-render with unchanged props is a no-op here. */
export const ColumnFilter = memo(ColumnFilterImpl)
