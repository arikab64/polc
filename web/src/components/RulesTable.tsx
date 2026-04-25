import { useMemo, useState } from 'react'
import {
  createColumnHelper,
  flexRender,
  getCoreRowModel,
  getFilteredRowModel,
  getSortedRowModel,
  useReactTable,
  type ColumnFiltersState,
  type FilterFn,
  type SortingState,
  type SortingFn,
} from '@tanstack/react-table'
import type { Rule } from '../lib/types'
import { formatPorts } from '../lib/format'
import { ColumnFilter } from './ColumnFilter'
import { RuleSelector } from './RuleSelector'
import { ResolvedIcon } from './ResolvedIcon'

/** Column IDs that RulesTable knows how to render. */
export type RulesTableColumnId =
  | 'action'
  | 'src'
  | 'dst'
  | 'ports'
  | 'protos'
  | 'id'
  | 'resolved'

const ALL_COLUMNS: RulesTableColumnId[] = [
  'action', 'src', 'dst', 'ports', 'protos', 'id', 'resolved',
]

interface RulesTableProps {
  rules: Rule[]
  /** Optional row click handler. When provided, rows gain hover state and
   *  show a pointer cursor. The caller decides what the click means
   *  (select-for-detail-panel, etc). */
  onRowClick?: (rule: Rule) => void
  /** Optional highlighted row for caller-managed selection. */
  selectedRuleId?: number | null
  /** Subset of columns to show and in what order. Defaults to all seven. */
  visibleColumns?: RulesTableColumnId[]
  /** Message to show when the current filter set yields zero rows. */
  emptyMessage?: string
  /** Message to show when the input rules list itself is empty (before any
   *  filtering). Different from filtered-to-empty — this is usually a
   *  permanent state like "nothing to show here". */
  nothingToShowMessage?: string
}

const columnHelper = createColumnHelper<Rule>()

/* -------------------- helpers -------------------- */

/**
 * A side is "ANY" iff its DNF is exactly one term with no labels and not
 * flagged undefined. This is how the compiler emits a `SEL_ALL` selector
 * (LANGUAGE.md §5.2): one empty-mask term whose `lset_subset(empty, _)`
 * is always true, which during compile-time matched every EID. The web
 * round-trips it as `[ { labels: [], undefined: false } ]`.
 *
 * Note: an `undefined` term also has zero labels, but it can never
 * match anything — so it's NOT the same thing as ANY. We disambiguate
 * via `term.undefined`.
 */
function sideIsAny(terms: Rule['src']): boolean {
  return terms.length === 1 && terms[0].labels.length === 0 && !terms[0].undefined
}

/* -------------------- filters -------------------- */

const multiSelectString: FilterFn<Rule> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  return selected.includes(String(row.getValue(columnId)))
}

const multiSelectArray: FilterFn<Rule> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  const rowArr = row.getValue(columnId) as (string | number)[]
  return rowArr.some((v) => selected.includes(String(v)))
}

const resolvedFilter: FilterFn<Rule> = (row, _columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  const key = row.original.resolved ? 'resolved' : 'unresolved'
  return selected.includes(key)
}

/**
 * Filter a selector side (src or dst) by label strings. A rule passes if
 * any of its DNF terms on that side contains any of the selected
 * `key:value` labels.
 *
 * ANY (SEL_ALL) special-case: a side written as bare `ANY` matches every
 * label by definition (LANGUAGE.md §5.2 row 1 / §6.2). So an ANY side
 * always passes any label filter — otherwise selecting "app:web" in the
 * Src filter would hide a rule like `ALLOW ANY -> app:web` even though
 * that rule does cover app:web sources.
 */
const selectorLabelFilter: FilterFn<Rule> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  const terms = row.getValue(columnId) as Rule['src']
  if (sideIsAny(terms)) return true
  for (const term of terms) {
    for (const lbl of term.labels) {
      if (selected.includes(`${lbl.key}:${lbl.value}`)) return true
    }
  }
  return false
}

/** Filter rule IDs as strings — TanStack compares IDs as numbers by
 *  default, so we coerce here to match the popover's string options. */
const ruleIdFilter: FilterFn<Rule> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  return selected.includes(String(row.getValue(columnId)))
}

/* -------------------- sort comparators -------------------- */

/** Sort by ports numerically (smallest first), comparing the first port
 *  of each row's port list. Empty port lists go last. */
const sortByPortsNumeric: SortingFn<Rule> = (a, b) => {
  const ap = a.original.ports[0]
  const bp = b.original.ports[0]
  if (ap === undefined && bp === undefined) return 0
  if (ap === undefined) return 1
  if (bp === undefined) return -1
  return ap - bp
}

/**
 * Sort selectors by their rendered label list — good enough for grouping
 * rules with identical selectors together. Not a perfect DNF comparator
 * but sufficient for a filter-assist sort.
 *
 * ANY sides sort under the literal key "ANY" so all `ANY -> ...` rules
 * group together at the same position regardless of dst contents.
 */
const sortBySelectorString: SortingFn<Rule> = (a, b) => {
  const sideKey = (terms: Rule['src']) => {
    if (sideIsAny(terms)) return 'ANY'
    return terms
      .map((t) => t.labels.map((l) => `${l.key}:${l.value}`).join(','))
      .join('|')
  }
  const toKey = (r: Rule, side: 'src' | 'dst') => sideKey(r[side])
  // columnId comes in as the sort state — but we don't have access here
  // without the SortingFn being parameterized. Fall back to comparing
  // both sides concatenated. Callers pass this comparator to whichever
  // column needs it; the comparator is parameterized by column via the
  // accessor, so only the correct side is actually used in practice.
  const aKey = toKey(a.original, 'src') + '//' + toKey(a.original, 'dst')
  const bKey = toKey(b.original, 'src') + '//' + toKey(b.original, 'dst')
  return aKey < bKey ? -1 : aKey > bKey ? 1 : 0
}

const EMPTY: string[] = []

export function RulesTable({
  rules,
  onRowClick,
  selectedRuleId,
  visibleColumns = ALL_COLUMNS,
  emptyMessage = 'no rules match the active filters',
  nothingToShowMessage,
}: RulesTableProps) {
  // Derive filter options from the rules currently being shown. Unlike
  // the main Rules tab (which draws options from db.allActions etc across
  // the whole DB), here we want options that actually appear in `rules`
  // — otherwise the filter popover would offer ports that filter away
  // everything.
  //
  // Note: ANY sides contribute no labels here (a SEL_ALL side has one
  // empty-labels term). That's intentional — "ANY" isn't a label, so it
  // doesn't belong in the label-filter popover. The selectorLabelFilter
  // above lets ANY rules pass any label filter unconditionally instead.
  const filterOptions = useMemo(() => {
    const actions = new Set<string>()
    const ports = new Set<number>()
    const protos = new Set<string>()
    const ruleIds = new Set<number>()
    const srcLabels = new Set<string>()
    const dstLabels = new Set<string>()
    for (const r of rules) {
      actions.add(r.action)
      ruleIds.add(r.id)
      for (const p of r.ports) ports.add(p)
      for (const p of r.protos) protos.add(p)
      for (const term of r.src)
        for (const l of term.labels) srcLabels.add(`${l.key}:${l.value}`)
      for (const term of r.dst)
        for (const l of term.labels) dstLabels.add(`${l.key}:${l.value}`)
    }
    return {
      actions: [...actions].sort(),
      ports: [...ports].sort((a, b) => a - b),
      protos: [...protos].sort(),
      ruleIds: [...ruleIds].sort((a, b) => a - b).map(String),
      srcLabels: [...srcLabels].sort(),
      dstLabels: [...dstLabels].sort(),
    }
  }, [rules])

  const [columnFilters, setColumnFilters] = useState<ColumnFiltersState>([])
  const [sorting, setSorting] = useState<SortingState>([])

  // Derive visibility + order from visibleColumns prop. visibility hides
  // cells; order controls the left-to-right column sequence.
  const columnVisibility = useMemo(() => {
    const visSet = new Set(visibleColumns)
    const vis: Record<string, boolean> = {}
    for (const id of ALL_COLUMNS) vis[id] = visSet.has(id)
    return vis
  }, [visibleColumns])

  const columns = useMemo(
    () => [
      columnHelper.accessor('action', {
        id: 'action',
        header: 'Action',
        filterFn: multiSelectString,
        cell: (info) => {
          const a = info.getValue()
          return <span className={`action action--${actionVariant(a)}`}>{a}</span>
        },
      }),
      columnHelper.accessor('src', {
        id: 'src',
        header: 'Src',
        filterFn: selectorLabelFilter,
        sortingFn: sortBySelectorString,
        cell: (info) => <RuleSelector terms={info.getValue()} />,
      }),
      columnHelper.accessor('dst', {
        id: 'dst',
        header: 'Dst',
        filterFn: selectorLabelFilter,
        sortingFn: sortBySelectorString,
        cell: (info) => <RuleSelector terms={info.getValue()} />,
      }),
      columnHelper.accessor('ports', {
        id: 'ports',
        header: 'Ports',
        filterFn: multiSelectArray,
        sortingFn: sortByPortsNumeric,
        cell: (info) => (
          <span className="col-ports">{formatPorts(info.getValue())}</span>
        ),
      }),
      columnHelper.accessor('protos', {
        id: 'protos',
        header: 'Protocol',
        filterFn: multiSelectArray,
        enableSorting: false,
        cell: (info) => (
          <span className="col-protos">
            {info.getValue().map((p) => (
              <span key={p} className="proto-chip">
                {p}
              </span>
            ))}
          </span>
        ),
      }),
      columnHelper.accessor('id', {
        id: 'id',
        header: 'RuleID',
        filterFn: ruleIdFilter,
        cell: (info) => <span className="col-rule-id">{info.getValue()}</span>,
      }),
      columnHelper.accessor((r) => (r.resolved ? 'resolved' : 'unresolved'), {
        id: 'resolved',
        header: 'Resolved',
        filterFn: resolvedFilter,
        cell: (info) => <ResolvedIcon resolved={info.row.original.resolved} />,
      }),
    ],
    [],
  )

  const table = useReactTable({
    data: rules,
    columns,
    state: { columnFilters, sorting, columnVisibility, columnOrder: visibleColumns },
    onColumnFiltersChange: setColumnFilters,
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getFilteredRowModel: getFilteredRowModel(),
    getSortedRowModel: getSortedRowModel(),
  })

  const selectedFor = (id: string): string[] =>
    (table.getColumn(id)?.getFilterValue() as string[] | undefined) ?? EMPTY

  const updateFilter = (id: string, next: string[]) => {
    const col = table.getColumn(id)
    if (!col) return
    col.setFilterValue(next.length === 0 ? undefined : next)
  }

  // Input is empty AND caller gave us a specific message for that state —
  // show the friendlier message instead of the generic filter-empty one.
  if (rules.length === 0 && nothingToShowMessage) {
    return <div className="rules-table-empty">{nothingToShowMessage}</div>
  }

  const renderHeader = (id: RulesTableColumnId) => {
    switch (id) {
      case 'action':
        return (
          <SortableTh key="action" table={table} columnId="action">
            Action
            <ColumnFilter
              options={filterOptions.actions}
              selected={selectedFor('action')}
              onChange={(next) => updateFilter('action', next)}
              placeholder="search..."
            />
          </SortableTh>
        )
      case 'src':
        return (
          <SortableTh key="src" table={table} columnId="src">
            Src
            <ColumnFilter
              options={filterOptions.srcLabels}
              selected={selectedFor('src')}
              onChange={(next) => updateFilter('src', next)}
              placeholder="search labels..."
            />
          </SortableTh>
        )
      case 'dst':
        return (
          <SortableTh key="dst" table={table} columnId="dst">
            Dst
            <ColumnFilter
              options={filterOptions.dstLabels}
              selected={selectedFor('dst')}
              onChange={(next) => updateFilter('dst', next)}
              placeholder="search labels..."
            />
          </SortableTh>
        )
      case 'ports':
        return (
          <SortableTh key="ports" table={table} columnId="ports">
            Ports
            <ColumnFilter
              options={filterOptions.ports.map(String)}
              selected={selectedFor('ports')}
              onChange={(next) => updateFilter('ports', next)}
              placeholder="search ports..."
            />
          </SortableTh>
        )
      case 'protos':
        return (
          <th key="protos">
            Protocol
            <ColumnFilter
              options={filterOptions.protos}
              selected={selectedFor('protos')}
              onChange={(next) => updateFilter('protos', next)}
              placeholder="search..."
            />
          </th>
        )
      case 'id':
        return (
          <SortableTh key="id" table={table} columnId="id" className="th-rule-id">
            RuleID
            <ColumnFilter
              options={filterOptions.ruleIds}
              selected={selectedFor('id')}
              onChange={(next) => updateFilter('id', next)}
              placeholder="search id..."
            />
          </SortableTh>
        )
      case 'resolved':
        return (
          <th key="resolved" className="th-resolved">
            Resolved
            <ColumnFilter
              options={['resolved', 'unresolved']}
              selected={selectedFor('resolved')}
              onChange={(next) => updateFilter('resolved', next)}
              renderOption={(v) => (
                <span style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
                  <ResolvedIcon resolved={v === 'resolved'} />
                  {v}
                </span>
              )}
              placeholder="search..."
            />
          </th>
        )
    }
  }

  return (
    <div className="inv-table-wrap">
      <table className="inv-table rules-table sortable-table">
        <thead>
          <tr>{visibleColumns.map((id) => renderHeader(id))}</tr>
        </thead>
        <tbody>
          {table.getRowModel().rows.map((row) => {
            const rule = row.original
            const isSelected = rule.id === selectedRuleId
            const classes = [
              rule.resolved ? '' : 'row--unresolved',
              isSelected ? 'row--selected' : '',
              onRowClick ? 'row--clickable' : '',
            ]
              .filter(Boolean)
              .join(' ')
            return (
              <tr
                key={row.id}
                className={classes}
                onClick={onRowClick ? () => onRowClick(rule) : undefined}
              >
                {row.getVisibleCells().map((cell) => (
                  <td key={cell.id}>
                    {flexRender(cell.column.columnDef.cell, cell.getContext())}
                  </td>
                ))}
              </tr>
            )
          })}
          {table.getRowModel().rows.length === 0 && (
            <tr>
              <td
                colSpan={visibleColumns.length}
                style={{ textAlign: 'center', padding: '40px', color: 'var(--ink-dim)' }}
              >
                {emptyMessage}
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}

/* -------------------- sortable th -------------------- */

function SortableTh({
  table,
  columnId,
  className,
  children,
}: {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  table: any
  columnId: string
  className?: string
  children: React.ReactNode
}) {
  const col = table.getColumn(columnId)
  const sort = col?.getIsSorted() as 'asc' | 'desc' | false
  const glyph = sort === 'asc' ? '↑' : sort === 'desc' ? '↓' : '↕'
  const active = !!sort

  return (
    <th
      className={`th-sortable ${active ? 'th-sortable--active' : ''} ${className ?? ''}`.trim()}
      onClick={() => col?.toggleSorting()}
    >
      <span className="th-sort-glyph" aria-hidden="true">
        {glyph}
      </span>
      {children}
    </th>
  )
}

/* -------------------- misc -------------------- */

function actionVariant(
  a: string,
): 'allow' | 'block' | 'override-allow' | 'override-block' {
  if (a === 'ALLOW') return 'allow'
  if (a === 'BLOCK') return 'block'
  if (a === 'OVERRIDE-ALLOW') return 'override-allow'
  return 'override-block'
}
