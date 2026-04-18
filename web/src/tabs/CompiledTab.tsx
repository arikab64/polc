import type { LoadedDb } from '../lib/types'

interface Props {
  db: LoadedDb
}

// eslint-disable-next-line @typescript-eslint/no-unused-vars
export function CompiledTab(_props: Props) {
  return (
    <div className="stub">
      <h2 className="stub-title">Compiled</h2>
      <p className="stub-body">
        This tab will render the compiled runtime state — bagvecs, ipcache, and
        the four bag maps (bag_src, bag_dst, bag_port, bag_proto) that the
        datapath uses at enforcement time. It's the view a simulator engineer
        would open to inspect what polc produced.
      </p>
      <p className="stub-body">Not implemented yet.</p>
    </div>
  )
}
