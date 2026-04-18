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
import type { Asset, LoadedDb } from '../lib/types'
import { Chip } from '../components/Chip'
import { ColumnFilter } from '../components/ColumnFilter'

interface InventoryTabProps {
  db: LoadedDb
}

const columnHelper = createColumnHelper<Asset>()

/** Filter fn: row's value must be in the selected-array (if non-empty). */
const multiSelectString: FilterFn<Asset> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  const rowValue = row.getValue(columnId) as string
  return selected.includes(rowValue)
}

/** Filter fn: row's array value must include at least one selected entry. */
const multiSelectStringArray: FilterFn<Asset> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  const rowArray = row.getValue(columnId) as string[]
  return rowArray.some((v) => selected.includes(v))
}

/** Filter fn: row's labels array must include at least one selected "key:value". */
const multiSelectLabels: FilterFn<Asset> = (row, _columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  const labels = row.original.labels
  return labels.some((l) => selected.includes(`${l.key}:${l.value}`))
}

const EMPTY: string[] = []

export function InventoryTab({ db }: InventoryTabProps) {
  // Single source of truth for column filters — owned by TanStack Table
  // via columnFilters + onColumnFiltersChange. Previously we mirrored this
  // into four useState arrays and rebuilt a fresh columnFilters array inside
  // `state` on every render, which defeated the table's identity check and
  // caused an update loop when a filter was toggled.
  const [columnFilters, setColumnFilters] = useState<ColumnFiltersState>([])

  const columns = useMemo(
    () => [
      columnHelper.accessor('name', {
        id: 'asset',
        header: 'Asset',
        filterFn: multiSelectString,
        cell: (info) => <span className="col-asset">{info.getValue()}</span>,
      }),
      columnHelper.accessor('ips', {
        id: 'ips',
        header: 'IPs',
        filterFn: multiSelectStringArray,
        cell: (info) => {
          const ips = info.getValue()
          return (
            <div className="col-ips">
              <div className="ip-stack">
                {ips.map((ip) => (
                  <span key={ip}>{ip}</span>
                ))}
              </div>
            </div>
          )
        },
      }),
      columnHelper.accessor('labels', {
        id: 'labels',
        header: 'Labels',
        filterFn: multiSelectLabels,
        cell: (info) => {
          const labels = info.getValue()
          return (
            <div className="chips">
              {labels.map((l) => (
                <Chip key={`${l.key}:${l.value}`} labelKey={l.key} labelValue={l.value} />
              ))}
            </div>
          )
        },
      }),
      columnHelper.accessor('eidHex', {
        id: 'eid',
        header: 'EID',
        filterFn: multiSelectString,
        cell: (info) => <span className="col-eid">{info.getValue()}</span>,
      }),
    ],
    [],
  )

  const table = useReactTable({
    data: db.assets,
    columns,
    state: { columnFilters },
    onColumnFiltersChange: setColumnFilters,
    getCoreRowModel: getCoreRowModel(),
    getFilteredRowModel: getFilteredRowModel(),
  })

  // Read each column's current filter value straight off the table —
  // the popover's `selected` prop stays in sync automatically.
  const selectedFor = (id: string): string[] =>
    (table.getColumn(id)?.getFilterValue() as string[] | undefined) ?? EMPTY

  // Write back through the column API. An empty list clears the filter
  // entirely (undefined) rather than storing [] — semantically "no filter",
  // not "filter matching zero of N options".
  const updateFilter = (id: string, next: string[]) => {
    const col = table.getColumn(id)
    if (!col) return
    col.setFilterValue(next.length === 0 ? undefined : next)
  }

  const labelOptions = useMemo(
    () => db.allLabels.map((l) => `${l.key}:${l.value}`),
    [db.allLabels],
  )

  return (
    <div className="inv-table-wrap">
      <table className="inv-table">
        <thead>
          <tr>
            <th>
              Asset
              <ColumnFilter
                options={db.allAssetNames}
                selected={selectedFor('asset')}
                onChange={(next) => updateFilter('asset', next)}
                placeholder="search assets..."
              />
            </th>
            <th>
              IPs
              <ColumnFilter
                options={db.allIps}
                selected={selectedFor('ips')}
                onChange={(next) => updateFilter('ips', next)}
                placeholder="search ips..."
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
            <th>
              EID
              <ColumnFilter
                options={db.allEidHex}
                selected={selectedFor('eid')}
                onChange={(next) => updateFilter('eid', next)}
                placeholder="search eid..."
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
              <td colSpan={4} style={{ textAlign: 'center', padding: '40px', color: 'var(--ink-dim)' }}>
                no assets match the active filters
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}
