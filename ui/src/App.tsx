import { useState } from 'react'
import { LoadButton } from './components/LoadButton'
import { useSqliteDb, type DbState } from './lib/useSqliteDb'
import { compactNum } from './lib/format'
import type { LoadedDb } from './lib/types'
import { InventoryTab } from './tabs/InventoryTab'
import { CompiledTab } from './tabs/CompiledTab'
import { RulesTab } from './tabs/RulesTab'

type TabId = 'inventory' | 'compiled' | 'rules'

export function App() {
  const { state, loadFile } = useSqliteDb()
  const [activeTab, setActiveTab] = useState<TabId>('inventory')

  const db = state.status === 'ready' ? state.db : null

  return (
    <div className="app">
      <header className="app-header">
        <div className="brand">
          <span className="brand-mark">polc</span>
          <span className="brand-sub">policy compiler / inspector</span>
        </div>
        {db && <LoadButton onLoad={loadFile} label="Reload ..." />}
      </header>

      <nav className="tabs" role="tablist">
        <button
          role="tab"
          className="tab"
          aria-selected={activeTab === 'inventory'}
          onClick={() => setActiveTab('inventory')}
        >
          Inventory
          {db && <span className="tab-count">{compactNum(db.stats.inventory)}</span>}
        </button>
        <button
          role="tab"
          className="tab"
          aria-selected={activeTab === 'compiled'}
          onClick={() => setActiveTab('compiled')}
        >
          Compiled
        </button>
        <button
          role="tab"
          className="tab"
          aria-selected={activeTab === 'rules'}
          onClick={() => setActiveTab('rules')}
        >
          Rules
          {db && <span className="tab-count">{compactNum(db.stats.rules)}</span>}
        </button>
      </nav>

      {db && <StatusLine db={db} />}

      <main className="main">
        <Body state={state} activeTab={activeTab} onLoad={loadFile} />
      </main>
    </div>
  )
}

/* ---------------- */

function StatusLine({ db }: { db: LoadedDb }) {
  return (
    <div className="statusline">
      <span className="statusline-item">
        <span className="statusline-key">rules:</span>
        <span className="statusline-value">{db.stats.rules}</span>
      </span>
      <span className="statusline-item">
        <span className="statusline-key">inventory:</span>
        <span className="statusline-value">{db.stats.inventory}</span>
      </span>
      <span className="statusline-item">
        <span className="statusline-key">bin=</span>
        <span className="statusline-value">{db.filename}</span>
      </span>
      <span className="statusline-spacer" />
    </div>
  )
}

/* ---------------- */

function Body({
  state,
  activeTab,
  onLoad,
}: {
  state: DbState
  activeTab: TabId
  onLoad: (file: File) => void
}) {
  if (state.status === 'idle') {
    return (
      <div className="state-panel">
        <div className="state-panel-title">no database loaded</div>
        <div className="state-panel-msg">
          Load an <code>out.db</code> produced by{' '}
          <span style={{ fontFamily: 'var(--font-mono)' }}>polc --debug</span> to
          inspect its inventory, compiled enforcement state, and policy rules.
        </div>
        <LoadButton onLoad={onLoad} variant="primary" label="Load database" />
      </div>
    )
  }

  if (state.status === 'loading') {
    return (
      <div className="state-panel">
        <div className="state-panel-title">loading</div>
        <div className="state-panel-msg" style={{ fontFamily: 'var(--font-mono)', fontSize: 12 }}>
          opening {state.filename}...
        </div>
      </div>
    )
  }

  if (state.status === 'error') {
    return (
      <div className="state-panel state-panel--error">
        <div className="state-panel-title">
          {state.code === 'MISSING_DEBUG_TABLES' ? 'non-debug database' : 'cannot open database'}
        </div>
        <div className="state-panel-msg">{state.message}</div>
        {state.code === 'MISSING_DEBUG_TABLES' && (
          <div className="state-panel-hint">
            $ polc --debug -i policy.gc -o out.db
          </div>
        )}
        <LoadButton onLoad={onLoad} label="Try another file" />
      </div>
    )
  }

  // ready
  const db = state.db
  if (activeTab === 'inventory') return <InventoryTab db={db} />
  if (activeTab === 'compiled') return <CompiledTab db={db} />
  return <RulesTab db={db} />
}
