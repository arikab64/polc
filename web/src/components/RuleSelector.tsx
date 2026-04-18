import type { DnfTerm } from '../lib/types'
import { Chip } from './Chip'

interface RuleSelectorProps {
  terms: DnfTerm[]
}

/**
 * Render a DNF selector: terms are OR'd, labels within a term are AND'd.
 *
 * Examples:
 *   [ { labels:[app:web] } ]                    → [app:web chip]
 *   [ { labels:[app:web, env:prod] } ]          → [app:web] AND [env:prod]
 *   [ { labels:[app:api] }, { labels:[app:db] } ] → [app:api] OR [app:db]
 *
 * An `undefined` term (label referenced that isn't in the DB) is rendered
 * struck-through with a muted tone — it can never match.
 *
 * A completely empty selector (no terms — should be rare) falls back to
 * a dim "∅" so it's obvious in the UI rather than blank.
 */
export function RuleSelector({ terms }: RuleSelectorProps) {
  if (terms.length === 0) {
    return <span className="selector-empty">∅</span>
  }

  return (
    <span className="selector">
      {terms.map((term, i) => (
        <span
          key={i}
          className={`selector-term ${term.undefined ? 'selector-term--undefined' : ''}`}
        >
          {i > 0 && <span className="selector-kw selector-kw--or"> OR </span>}
          {term.labels.map((l, j) => (
            <span key={`${l.key}:${l.value}`} className="selector-label">
              {j > 0 && <span className="selector-kw selector-kw--and"> AND </span>}
              <Chip labelKey={l.key} labelValue={l.value} />
            </span>
          ))}
        </span>
      ))}
    </span>
  )
}
