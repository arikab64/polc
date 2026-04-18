import { useMemo, useState } from 'react'
import {
  createColumnHelper,
  flexRender,
  getCoreRowModel,
  getFilteredRowModel,
  useReactTable,
  type ColumnFiltersState,
  type FilterFn,
} from '@tanstack/react-table'
import type { Label, LoadedDb } from '../../lib/types'
import { Chip } from '../../components/Chip'
import { ColumnFilter } from '../../components/ColumnFilter'
import { sortLabels } from '../../lib/format'
import { ExpressionPanel } from './ExpressionPanel'

interface Props {
  db: LoadedDb
}

/** One row per distinct EID. `labels` is the shared label set. */
interface EidRow {
  eidHex: string
  labels: Label[]
}

const columnHelper = createColumnHelper<EidRow>()

const multiSelectString: FilterFn<EidRow> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  return selected.includes(String(row.getValue(columnId)))
}

const multiSelectLabels: FilterFn<EidRow> = (row, _columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  return row.original.labels.some((l) => selected.includes(`${l.key}:${l.value}`))
}

const EMPTY: string[] = []

export function EnforcementIdentitiesTab({ db }: Props) {
  // EIDs derived from assets. Multiple assets can share one EID (same label
  // set), so we collapse to distinct EIDs here — the table shows one row
  // per identity, not per asset.
  const eidRows = useMemo<EidRow[]>(() => {
    const seen = new Map<string, EidRow>()
    for (const a of db.assets) {
      if (seen.has(a.eidHex)) continue
      seen.set(a.eidHex, {
        eidHex: a.eidHex,
        labels: sortLabels(a.labels),
      })
    }
    return [...seen.values()]
  }, [db.assets])

  const allEids = useMemo(() => eidRows.map((r) => r.eidHex), [eidRows])
  const labelOptions = useMemo(
    () =>
      [
        ...new Set(
          eidRows.flatMap((r) => r.labels.map((l) => `${l.key}:${l.value}`)),
        ),
      ].sort(),
    [eidRows],
  )

  const [columnFilters, setColumnFilters] = useState<ColumnFiltersState>([])

  const columns = useMemo(
    () => [
      columnHelper.accessor('eidHex', {
        id: 'eid',
        header: 'EID',
        filterFn: multiSelectString,
        cell: (info) => <span className="col-eid">{info.getValue()}</span>,
      }),
      columnHelper.accessor('labels', {
        id: 'labels',
        header: 'Labels',
        filterFn: multiSelectLabels,
        cell: (info) => (
          <div className="chips">
            {info.getValue().map((l) => (
              <Chip key={`${l.key}:${l.value}`} labelKey={l.key} labelValue={l.value} />
            ))}
          </div>
        ),
      }),
    ],
    [],
  )

  const table = useReactTable({
    data: eidRows,
    columns,
    state: { columnFilters },
    onColumnFiltersChange: setColumnFilters,
    getCoreRowModel: getCoreRowModel(),
    getFilteredRowModel: getFilteredRowModel(),
  })

  const selectedFor = (id: string): string[] =>
    (table.getColumn(id)?.getFilterValue() as string[] | undefined) ?? EMPTY

  const updateFilter = (id: string, next: string[]) => {
    const col = table.getColumn(id)
    if (!col) return
    col.setFilterValue(next.length === 0 ? undefined : next)
  }

  return (
    <div className="rules-layout">
      <div className="rules-layout-main">
        <div className="inv-table-wrap">
          <table className="inv-table">
            <thead>
              <tr>
                <th>
                  EID
                  <ColumnFilter
                    options={allEids}
                    selected={selectedFor('eid')}
                    onChange={(next) => updateFilter('eid', next)}
                    placeholder="search eid..."
                  />
                </th>
                <th>
                  Labels
                  <ColumnFilter
                    options={labelOptions}
                    selected={selectedFor('labels')}
                    onChange={(next) => updateFilter('labels', next)}
                    placeholder="search labels..."
                  />
                </th>
              </tr>
            </thead>
            <tbody>
              {table.getRowModel().rows.map((row) => (
                <tr key={row.id}>
                  {row.getVisibleCells().map((cell) => (
                    <td key={cell.id}>
                      {flexRender(cell.column.columnDef.cell, cell.getContext())}
                    </td>
                  ))}
                </tr>
              ))}
              {table.getRowModel().rows.length === 0 && (
                <tr>
                  <td
                    colSpan={2}
                    style={{ textAlign: 'center', padding: '40px', color: 'var(--ink-dim)' }}
                  >
                    no enforcement identities match the active filters
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </div>
      <ExpressionPanel db={db} />
    </div>
  )
}
