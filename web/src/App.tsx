import { useState } from 'react'
import { LoadButton } from './components/LoadButton'
import { useSqliteDb, type DbState } from './lib/useSqliteDb'
import { compactNum } from './lib/format'
import { InventoryTab } from './tabs/InventoryTab'
import { CompiledTab } from './tabs/CompiledTab'
import { RulesTab } from './tabs/RulesTab'
import { SimulatorTab } from './tabs/SimulatorTab'

type TabId = 'inventory' | 'compiled' | 'rules' | 'simulator'

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
        {db && (
          <>
            <div className="statusline statusline--inline">
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
            </div>
            <LoadButton onLoad={loadFile} label="Reload ..." />
          </>
        )}
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
          aria-selected={activeTab === 'rules'}
          onClick={() => setActiveTab('rules')}
        >
          Rules
          {db && <span className="tab-count">{compactNum(db.stats.rules)}</span>}
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
          aria-selected={activeTab === 'simulator'}
          onClick={() => setActiveTab('simulator')}
        >
          Simulator
        </button>
      </nav>

      <main className="main">
        <Body state={state} activeTab={activeTab} onLoad={loadFile} />
      </main>
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
  // Keep all tabs mounted so their per-tab state (simulator inputs,
  // expression text, filters, etc.) survives navigation. Only the active
  // tab is visible; the rest are hidden via CSS. Mounting cost is
  // negligible — the data is already in memory, each tab just renders
  // its own subset once.
  const db = state.db
  return (
    <>
      <TabPane active={activeTab === 'inventory'}>
        <InventoryTab db={db} />
      </TabPane>
      <TabPane active={activeTab === 'rules'}>
        <RulesTab db={db} />
      </TabPane>
      <TabPane active={activeTab === 'compiled'}>
        <CompiledTab db={db} />
      </TabPane>
      <TabPane active={activeTab === 'simulator'}>
        <SimulatorTab db={db} />
      </TabPane>
    </>
  )
}

/** Wrapper that hides its children when `active` is false instead of
 *  unmounting them. Keeps tab state alive across navigation. */
function TabPane({ active, children }: { active: boolean; children: React.ReactNode }) {
  return (
    <div className="tab-pane" hidden={!active} aria-hidden={!active}>
      {children}
    </div>
  )
}
