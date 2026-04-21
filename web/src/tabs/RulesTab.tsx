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
import type { LoadedDb, Rule } from '../lib/types'
import { formatPorts } from '../lib/format'
import { ColumnFilter } from '../components/ColumnFilter'
import { RuleSelector } from '../components/RuleSelector'
import { ResolvedIcon } from '../components/ResolvedIcon'
import { RulesPanel } from '../components/RulesPanel'

interface RulesTabProps {
  db: LoadedDb
}

const columnHelper = createColumnHelper<Rule>()

/* -------------------- filter functions -------------------- */

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

const EMPTY: string[] = []

export function RulesTab({ db }: RulesTabProps) {
  const [columnFilters, setColumnFilters] = useState<ColumnFiltersState>([])
  const [selectedRuleId, setSelectedRuleId] = useState<number | null>(null)
  const [selectedAssetName, setSelectedAssetName] = useState<string | null>(null)
  const [panelCollapsed, setPanelCollapsed] = useState(false)

  /** The rule object currently selected, or null. */
  const selectedRule = useMemo<Rule | null>(() => {
    if (selectedRuleId == null) return null
    return db.rules.find((r) => r.id === selectedRuleId) ?? null
  }, [db.rules, selectedRuleId])

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
        cell: (info) => <RuleSelector terms={info.getValue()} />,
      }),
      columnHelper.accessor('dst', {
        id: 'dst',
        header: 'Dst',
        enableColumnFilter: false,
        cell: (info) => <RuleSelector terms={info.getValue()} />,
      }),
      columnHelper.accessor('ports', {
        id: 'ports',
        header: 'Ports',
        filterFn: multiSelectArray,
        cell: (info) => (
          <span className="col-ports">{formatPorts(info.getValue())}</span>
        ),
      }),
      columnHelper.accessor('protos', {
        id: 'protos',
        header: 'Protocol',
        filterFn: multiSelectArray,
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
    data: db.rules,
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

  const portOptions = useMemo(() => db.allPorts.map(String), [db.allPorts])

  const handleRowClick = (ruleId: number) => {
    if (ruleId === selectedRuleId) {
      // Clicking the already-selected row deselects it — and clears any
      // asset drill-down, since that was scoped to the rule.
      setSelectedRuleId(null)
      setSelectedAssetName(null)
      return
    }
    setSelectedRuleId(ruleId)
    // Different rule → asset selection no longer applies
    setSelectedAssetName(null)
  }

  return (
    <div className="rules-layout">
      <div className="rules-layout-main">
        <div className="rules-toolbar">
          <button
            className="panel-toggle"
            onClick={() => setPanelCollapsed((c) => !c)}
            aria-expanded={!panelCollapsed}
            aria-controls="rules-side-panel"
          >
            {panelCollapsed ? '◂ Show panel' : 'Hide panel ▸'}
          </button>
        </div>
        <div className="inv-table-wrap">
          <table className="inv-table rules-table">
            <thead>
              <tr>
                <th>
                  Action
                  <ColumnFilter
                    options={db.allActions}
                    selected={selectedFor('action')}
                    onChange={(next) => updateFilter('action', next)}
                    placeholder="search..."
                  />
                </th>
                <th>Src</th>
                <th>Dst</th>
                <th>
                  Ports
                  <ColumnFilter
                    options={portOptions}
                    selected={selectedFor('ports')}
                    onChange={(next) => updateFilter('ports', next)}
                    placeholder="search ports..."
                  />
                </th>
                <th>
                  Protocol
                  <ColumnFilter
                    options={db.allProtos}
                    selected={selectedFor('protos')}
                    onChange={(next) => updateFilter('protos', next)}
                    placeholder="search..."
                  />
                </th>
                <th className="th-rule-id">RuleID</th>
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
                  'row--clickable',
                ]
                  .filter(Boolean)
                  .join(' ')
                return (
                  <tr
                    key={row.id}
                    className={classes}
                    onClick={() => handleRowClick(rule.id)}
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
                    no rules match the active filters
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </div>
      <RulesPanel
        assets={db.assets}
        selectedRule={selectedRule}
        selectedAssetName={selectedAssetName}
        onSelectAsset={setSelectedAssetName}
        collapsed={panelCollapsed}
        onToggle={() => setPanelCollapsed((c) => !c)}
      />
    </div>
  )
}

/* map action → style-variant class (drives chip color) */
function actionVariant(a: string): 'allow' | 'block' | 'override-allow' | 'override-block' {
  if (a === 'ALLOW') return 'allow'
  if (a === 'BLOCK') return 'block'
  if (a === 'OVERRIDE-ALLOW') return 'override-allow'
  return 'override-block'
}
