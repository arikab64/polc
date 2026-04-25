import type { DnfTerm } from '../lib/types'
import { Chip } from './Chip'

interface RuleSelectorProps {
  terms: DnfTerm[]
}

/**
 * Render a DNF selector: terms are OR'd, labels within a term are AND'd.
 *
 * Examples:
 *   [ { labels:[app:web] } ]                      → [app:web chip]
 *   [ { labels:[app:web, env:prod] } ]            → [app:web] AND [env:prod]
 *   [ { labels:[app:api] }, { labels:[app:db] } ] → [app:api] OR [app:db]
 *
 * Special cases:
 *
 *   ANY (the SEL_ALL sentinel from LANGUAGE.md §5.2):
 *     The compiler emits one term with an EMPTY label mask for a side
 *     written as bare `ANY` (or a bare subnet — same row of the §5.2
 *     table). The worker round-trips this as `[ { labels: [], undefined:
 *     false } ]`. We render that as the literal "ANY" keyword so the
 *     grid reads back the same vocabulary the user wrote — instead of a
 *     blank cell. Multiple terms where ANY happens to be one of them is
 *     not a thing the compiler emits today (DNF distribution would have
 *     simplified it), but if it ever did, we'd render "ANY" for that
 *     specific term and the rest as normal.
 *
 *   Empty terms list (`terms.length === 0`):
 *     Should be rare — it means "no DNF at all". Render "∅" so it's
 *     obvious in the UI rather than a blank cell.
 *
 *   `undefined` term:
 *     A label was referenced that isn't in the inventory's label table
 *     (typo, deleted EID, etc). The term can never match, so we render
 *     it struck-through. Note: an undefined term is distinct from an
 *     empty (ANY) term — both have no chips, but the `undefined` flag
 *     tells them apart and we render them differently.
 */
export function RuleSelector({ terms }: RuleSelectorProps) {
  if (terms.length === 0) {
    return <span className="selector-empty">∅</span>
  }

  return (
    <span className="selector">
      {terms.map((term, i) => {
        const isAny = !term.undefined && term.labels.length === 0
        return (
          <span
            key={i}
            className={[
              'selector-term',
              term.undefined ? 'selector-term--undefined' : '',
              isAny ? 'selector-term--any' : '',
            ]
              .filter(Boolean)
              .join(' ')}
          >
            {i > 0 && <span className="selector-kw selector-kw--or"> OR </span>}
            {isAny ? (
              <span
                className="selector-any"
                title="ALL_EIDS — bare ANY: matches every EID, including unresolved IPs"
              >
                ANY
              </span>
            ) : (
              term.labels.map((l, j) => (
                <span key={`${l.key}:${l.value}`} className="selector-label">
                  {j > 0 && <span className="selector-kw selector-kw--and"> AND </span>}
                  <Chip labelKey={l.key} labelValue={l.value} />
                </span>
              ))
            )}
          </span>
        )
      })}
    </span>
  )
}
