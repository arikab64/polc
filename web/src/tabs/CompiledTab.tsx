import { useState } from 'react'
import type { LoadedDb } from '../lib/types'
import { EnforcementIdentitiesTab } from './compiled/EnforcementIdentitiesTab'
import { EndpointTab } from './compiled/EndpointTab'
import { BagVectorTab } from './compiled/BagVectorTab'

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
        <div className="tab-pane" hidden={inner !== 'eids'} aria-hidden={inner !== 'eids'}>
          <EnforcementIdentitiesTab db={db} />
        </div>
        <div className="tab-pane" hidden={inner !== 'endpoint'} aria-hidden={inner !== 'endpoint'}>
          <EndpointTab db={db} />
        </div>
        <div className="tab-pane" hidden={inner !== 'bags'} aria-hidden={inner !== 'bags'}>
          <BagVectorTab db={db} />
        </div>
      </div>
    </div>
  )
}
