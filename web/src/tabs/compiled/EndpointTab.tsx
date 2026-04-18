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
import type { Label, LoadedDb } from '../../lib/types'
import { Chip } from '../../components/Chip'
import { ColumnFilter } from '../../components/ColumnFilter'

interface Props {
  db: LoadedDb
}

/**
 * One row per IP in the ipcache. The ipcache is a flat IP → EID map that
 * the datapath uses as its first lookup; rendered here together with the
 * owning asset and its labels so operators can answer "what does this IP
 * belong to" without cross-referencing tabs.
 *
 * Derived client-side from db.assets. Every IP in a debug build's ipcache
 * also appears in entity_ip (the compiler builds ipcache *from* entity_ip),
 * so flattening assets.flatMap gives the same set.
 */
interface EndpointRow {
  ip: string
  /** numeric form of the IP for correct sort order — 10.0.0.10 > 10.0.0.2 */
  ipNum: number
  eidHex: string
  asset: string
  labels: Label[]
}

const columnHelper = createColumnHelper<EndpointRow>()

/* -------------------- filters -------------------- */

const multiSelectString: FilterFn<EndpointRow> = (row, columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  return selected.includes(String(row.getValue(columnId)))
}

const multiSelectLabels: FilterFn<EndpointRow> = (row, _columnId, filterValue) => {
  const selected = filterValue as string[] | undefined
  if (!selected || selected.length === 0) return true
  return row.original.labels.some((l) => selected.includes(`${l.key}:${l.value}`))
}

/* -------------------- sort comparators -------------------- */

/** Sort IPs numerically by their uint32 value, not as strings. */
const sortByIpNum: SortingFn<EndpointRow> = (a, b) => {
  const av = a.original.ipNum
  const bv = b.original.ipNum
  return av === bv ? 0 : av < bv ? -1 : 1
}

/** Sort labels by their rendered (sorted) string form, so identical sets
 *  group together. */
const sortByLabels: SortingFn<EndpointRow> = (a, b) => {
  const as = a.original.labels.map((l) => `${l.key}:${l.value}`).join(',')
  const bs = b.original.labels.map((l) => `${l.key}:${l.value}`).join(',')
  return as === bs ? 0 : as < bs ? -1 : 1
}

const EMPTY: string[] = []

/** Parse "a.b.c.d" into a uint32 for comparison. Used once per row at
 *  derivation time, not on every sort click. */
function ipToNum(dotted: string): number {
  const p = dotted.split('.')
  return (
    (((Number(p[0]) & 0xff) << 24) |
      ((Number(p[1]) & 0xff) << 16) |
      ((Number(p[2]) & 0xff) << 8) |
      (Number(p[3]) & 0xff)) >>> 0
  )
}

export function EndpointTab({ db }: Props) {
  // Flatten db.assets into one row per IP.
  const rows = useMemo<EndpointRow[]>(() => {
    const out: EndpointRow[] = []
    for (const a of db.assets) {
      for (const ip of a.ips) {
        out.push({
          ip,
          ipNum: ipToNum(ip),
          eidHex: a.eidHex,
          asset: a.name,
          labels: a.labels,
        })
      }
    }
    return out
  }, [db.assets])

  const allIps = useMemo(
    () => [...new Set(rows.map((r) => r.ip))].sort((a, b) => ipToNum(a) - ipToNum(b)),
    [rows],
  )
  const allEids = useMemo(() => [...new Set(rows.map((r) => r.eidHex))].sort(), [rows])
  const allAssets = useMemo(() => [...new Set(rows.map((r) => r.asset))].sort(), [rows])
  const labelOptions = useMemo(
    () =>
      [
        ...new Set(rows.flatMap((r) => r.labels.map((l) => `${l.key}:${l.value}`))),
      ].sort(),
    [rows],
  )

  const [columnFilters, setColumnFilters] = useState<ColumnFiltersState>([])
  const [sorting, setSorting] = useState<SortingState>([{ id: 'ip', desc: false }])

  const columns = useMemo(
    () => [
      columnHelper.accessor('ip', {
        id: 'ip',
        header: 'IP',
        filterFn: multiSelectString,
        sortingFn: sortByIpNum,
        cell: (info) => <span className="col-ips">{info.getValue()}</span>,
      }),
      columnHelper.accessor('eidHex', {
        id: 'eid',
        header: 'EID',
        filterFn: multiSelectString,
        cell: (info) => <span className="col-eid">{info.getValue()}</span>,
      }),
      columnHelper.accessor('asset', {
        id: 'asset',
        header: 'Asset',
        filterFn: multiSelectString,
        cell: (info) => <span className="col-asset">{info.getValue()}</span>,
      }),
      columnHelper.accessor('labels', {
        id: 'labels',
        header: 'Labels',
        filterFn: multiSelectLabels,
        sortingFn: sortByLabels,
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
    data: rows,
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

  return (
    <div className="inv-table-wrap">
      <table className="inv-table sortable-table">
        <thead>
          <tr>
            <SortableTh table={table} columnId="ip">
              IP
              <ColumnFilter
                options={allIps}
                selected={selectedFor('ip')}
                onChange={(next) => updateFilter('ip', next)}
                placeholder="search ip..."
              />
            </SortableTh>
            <SortableTh table={table} columnId="eid">
              EID
              <ColumnFilter
                options={allEids}
                selected={selectedFor('eid')}
                onChange={(next) => updateFilter('eid', next)}
                placeholder="search eid..."
              />
            </SortableTh>
            <SortableTh table={table} columnId="asset">
              Asset
              <ColumnFilter
                options={allAssets}
                selected={selectedFor('asset')}
                onChange={(next) => updateFilter('asset', next)}
                placeholder="search asset..."
              />
            </SortableTh>
            <SortableTh table={table} columnId="labels">
              Labels
              <ColumnFilter
                options={labelOptions}
                selected={selectedFor('labels')}
                onChange={(next) => updateFilter('labels', next)}
                placeholder="search labels..."
              />
            </SortableTh>
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
                colSpan={4}
                style={{ textAlign: 'center', padding: '40px', color: 'var(--ink-dim)' }}
              >
                no endpoints match the active filters
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}

/* -------------------- sortable column header -------------------- */

/**
 * Clickable column header that toggles sort. Renders an indicator glyph
 * to the left of the label text: ↕ for idle, ↑ for ascending, ↓ for
 * descending. Clicking cycles asc → desc → unsorted.
 *
 * The filter popover (if any) is a child passed in alongside the label —
 * it has its own click handler that stopPropagations, so clicking the
 * filter ⌄ trigger doesn't also trigger sort.
 */
function SortableTh({
  table,
  columnId,
  children,
}: {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  table: any
  columnId: string
  children: React.ReactNode
}) {
  const col = table.getColumn(columnId)
  const sort = col?.getIsSorted() as 'asc' | 'desc' | false
  const glyph = sort === 'asc' ? '↑' : sort === 'desc' ? '↓' : '↕'
  const active = !!sort

  return (
    <th
      className={`th-sortable ${active ? 'th-sortable--active' : ''}`}
      onClick={() => col?.toggleSorting()}
    >
      <span className="th-sort-glyph" aria-hidden="true">
        {glyph}
      </span>
      {children}
    </th>
  )
}
