import { useState } from 'react'
import type { LoadedDb } from '../lib/types'
import { EnforcementIdentitiesTab } from './compiled/EnforcementIdentitiesTab'
import { EndpointTab } from './compiled/EndpointTab'

type InnerTabId = 'eids' | 'endpoint' | 'bags'

interface CompiledTabProps {
  db: LoadedDb
}

/**
 * Compiled tab shell.
 *
 * The "Compiled" section groups all views onto the post-parse, post-resolve
 * internal state of the compiler — primarily things a simulator or engine
 * engineer would want to inspect. It has its own set of inner tabs because
 * the space covers several distinct concepts (EIDs, ipcache, bag vectors).
 */
export function CompiledTab({ db }: CompiledTabProps) {
  const [inner, setInner] = useState<InnerTabId>('eids')

  return (
    <div className="compiled-shell">
      <nav className="inner-tabs" role="tablist">
        <button
          role="tab"
          className="inner-tab"
          aria-selected={inner === 'eids'}
          onClick={() => setInner('eids')}
        >
          Enforcement Identities
        </button>
        <button
          role="tab"
          className="inner-tab"
          aria-selected={inner === 'endpoint'}
          onClick={() => setInner('endpoint')}
        >
          Endpoint
        </button>
        <button
          role="tab"
          className="inner-tab"
          aria-selected={inner === 'bags'}
          onClick={() => setInner('bags')}
        >
          Bag Vectors
        </button>
      </nav>
      <div className="compiled-body">
        {inner === 'eids' && <EnforcementIdentitiesTab db={db} />}
        {inner === 'endpoint' && <EndpointTab db={db} />}
        {inner === 'bags' && <BagsStub />}
      </div>
    </div>
  )
}

function BagsStub() {
  return (
    <div className="stub">
      <h2 className="stub-title">Bag Vectors</h2>
      <p className="stub-body">
        Will render the compiled runtime enforcement state — the four bag maps
        (bag_src, bag_dst, bag_port, bag_proto) and the interned bag-vector
        store they reference. Not implemented yet.
      </p>
    </div>
  )
}
