import { memo, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'

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
 * Positioning: the popover uses `position: fixed` so it escapes any
 * overflow-hidden / scrollable ancestor (table-wrap, debug-panel, etc).
 * We measure the trigger's bounding rect on open and pick an origin that
 * keeps the popover on-screen horizontally — right-edge columns flip to
 * right-anchored, near-bottom columns flip to drop-up.
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
  const [pos, setPos] = useState<{
    top: number
    left?: number
    right?: number
  } | null>(null)
  const wrapRef = useRef<HTMLSpanElement>(null)
  const popRef = useRef<HTMLDivElement>(null)

  // Measure trigger + decide popover position. Runs on open and on any
  // scroll/resize while open, so the popover stays glued to the trigger
  // if the user scrolls the table under it.
  useLayoutEffect(() => {
    if (!open || !wrapRef.current) return

    const reposition = () => {
      const trig = wrapRef.current
      if (!trig) return
      const rect = trig.getBoundingClientRect()
      const vw = window.innerWidth
      const POPOVER_WIDTH = 280 // matches the CSS min-width; conservative
      const MARGIN = 8
      const top = rect.bottom + 6

      // If the natural left would push the popover past the viewport's
      // right edge, right-align it against the trigger's right edge
      // instead. Use fixed coordinates: left=rect.left / right=vw-rect.right.
      if (rect.left + POPOVER_WIDTH + MARGIN > vw) {
        setPos({ top, right: Math.max(MARGIN, vw - rect.right) })
      } else {
        setPos({ top, left: Math.max(MARGIN, rect.left) })
      }
    }

    reposition()
    // Re-run on scroll (any ancestor) and resize. `true` on addEventListener
    // catches scroll events from nested scrollers via capture phase.
    window.addEventListener('scroll', reposition, true)
    window.addEventListener('resize', reposition)
    return () => {
      window.removeEventListener('scroll', reposition, true)
      window.removeEventListener('resize', reposition)
    }
  }, [open])

  // close on outside click / Escape
  useEffect(() => {
    if (!open) return
    const onDocClick = (e: MouseEvent) => {
      const t = e.target as Node
      // Click was inside the trigger OR the popover — ignore.
      if (wrapRef.current && wrapRef.current.contains(t)) return
      if (popRef.current && popRef.current.contains(t)) return
      setOpen(false)
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
      {open && pos && (
        <div
          ref={popRef}
          className="col-filter-popover"
          onClick={(e) => e.stopPropagation()}
          style={{
            top: pos.top,
            ...(pos.left != null ? { left: pos.left } : { right: pos.right }),
          }}
        >
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
