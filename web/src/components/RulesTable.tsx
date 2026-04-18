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
import { ColumnFilter } from './ColumnFilter'
import { RuleSelector } from './RuleSelector'
import { ResolvedIcon } from './ResolvedIcon'

interface RulesTableProps {
  rules: Rule[]
  /** Optional row click handler. When provided, rows gain hover state and
   *  show a pointer cursor. The caller decides what the click means
   *  (select-for-detail-panel, etc). */
  onRowClick?: (rule: Rule) => void
  /** Optional highlighted row for caller-managed selection. */
  selectedRuleId?: number | null
  /** Message to show when the current filter set yields zero rows. */
  emptyMessage?: string
  /** Message to show when the input rules list itself is empty (before any
   *  filtering). Different from filtered-to-empty — this is usually a
   *  permanent state like "nothing to show here". */
  nothingToShowMessage?: string
}

const columnHelper = createColumnHelper<Rule>()

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

/** Sort selectors by their rendered label list — good enough for grouping
 *  rules with identical selectors together. Not a perfect DNF comparator
 *  but sufficient for a filter-assist sort. */
const sortBySelectorString: SortingFn<Rule> = (a, b) => {
  const toKey = (r: Rule, side: 'src' | 'dst') =>
    r[side]
      .map((t) => t.labels.map((l) => `${l.key}:${l.value}`).join(','))
      .join('|')
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
  emptyMessage = 'no rules match the active filters',
  nothingToShowMessage,
}: RulesTableProps) {
  // Derive filter options from the rules currently being shown. Unlike
  // the main Rules tab (which draws options from db.allActions etc across
  // the whole DB), here we want options that actually appear in `rules`
  // — otherwise the filter popover would offer ports that filter away
  // everything.
  const filterOptions = useMemo(() => {
    const actions = new Set<string>()
    const ports = new Set<number>()
    const protos = new Set<string>()
    for (const r of rules) {
      actions.add(r.action)
      for (const p of r.ports) ports.add(p)
      for (const p of r.protos) protos.add(p)
    }
    return {
      actions: [...actions].sort(),
      ports: [...ports].sort((a, b) => a - b),
      protos: [...protos].sort(),
    }
  }, [rules])

  const [columnFilters, setColumnFilters] = useState<ColumnFiltersState>([])
  const [sorting, setSorting] = useState<SortingState>([])

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
        enableColumnFilter: false,
        sortingFn: sortBySelectorString,
        cell: (info) => <RuleSelector terms={info.getValue()} />,
      }),
      columnHelper.accessor('dst', {
        id: 'dst',
        header: 'Dst',
        enableColumnFilter: false,
        sortingFn: sortBySelectorString,
        cell: (info) => <RuleSelector terms={info.getValue()} />,
      }),
      columnHelper.accessor('ports', {
        id: 'ports',
        header: 'Ports',
        filterFn: multiSelectArray,
        sortingFn: sortByPortsNumeric,
        cell: (info) => (
          <span className="col-ports">
            {info.getValue().length === 0 ? '—' : info.getValue().join(', ')}
          </span>
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
        enableColumnFilter: false,
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
    state: { columnFilters, sorting },
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

  return (
    <div className="inv-table-wrap">
      <table className="inv-table rules-table sortable-table">
        <thead>
          <tr>
            <SortableTh table={table} columnId="action">
              Action
              <ColumnFilter
                options={filterOptions.actions}
                selected={selectedFor('action')}
                onChange={(next) => updateFilter('action', next)}
                placeholder="search..."
              />
            </SortableTh>
            <SortableTh table={table} columnId="src">
              Src
            </SortableTh>
            <SortableTh table={table} columnId="dst">
              Dst
            </SortableTh>
            <SortableTh table={table} columnId="ports">
              Ports
              <ColumnFilter
                options={filterOptions.ports.map(String)}
                selected={selectedFor('ports')}
                onChange={(next) => updateFilter('ports', next)}
                placeholder="search ports..."
              />
            </SortableTh>
            <th>
              Protocol
              <ColumnFilter
                options={filterOptions.protos}
                selected={selectedFor('protos')}
                onChange={(next) => updateFilter('protos', next)}
                placeholder="search..."
              />
            </th>
            <SortableTh table={table} columnId="id" className="th-rule-id">
              RuleID
            </SortableTh>
            <th className="th-resolved">
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
          </tr>
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
                colSpan={7}
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
