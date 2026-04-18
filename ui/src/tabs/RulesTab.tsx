import type { LoadedDb } from '../lib/types'

interface Props {
  db: LoadedDb
}

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export function RulesTab(_props: Props) {
  return (
    <div className="stub">
      <h2 className="stub-title">Rules</h2>
      <p className="stub-body">
        This tab will render the policy rules — action, src and dst selectors
        (reconstructed from rule_dnf_term), port list, protocol mask, and
        resolution status. Clicking a rule will show which EIDs it matched on
        each side, pulled from rule_src_eid / rule_dst_eid.
      </p>
      <p className="stub-body">Not implemented yet.</p>
    </div>
  )
}
