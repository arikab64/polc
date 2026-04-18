import { useCallback, useEffect, useRef, useState } from 'react'
import type { LoadedDb, WorkerRequest, WorkerResponse } from '../lib/types'

export type DbState =
  | { status: 'idle' }
  | { status: 'loading'; filename: string }
  | { status: 'ready'; db: LoadedDb }
  | { status: 'error'; message: string; code?: string }

/**
 * Owns the sqlite Worker. One worker instance per page; reused across
 * loads. Rejects non-debug DBs via an error with code=MISSING_DEBUG_TABLES
 * so the UI can render a targeted message.
 */
export function useSqliteDb() {
  const workerRef = useRef<Worker | null>(null)
  const [state, setState] = useState<DbState>({ status: 'idle' })

  useEffect(() => {
    const w = new Worker(new URL('../lib/sqlite.worker.ts', import.meta.url), {
      type: 'module',
    })
    w.onmessage = (e: MessageEvent<WorkerResponse>) => {
      const msg = e.data
      if (msg.kind === 'ok') {
        setState({ status: 'ready', db: msg.data })
      } else {
        setState({ status: 'error', message: msg.message, code: msg.code })
      }
    }
    w.onerror = (ev) => {
      setState({
        status: 'error',
        message: ev.message || 'Worker error',
      })
    }
    workerRef.current = w
    return () => {
      w.terminate()
      workerRef.current = null
    }
  }, [])

  const loadFile = useCallback(async (file: File) => {
    const w = workerRef.current
    if (!w) return
    setState({ status: 'loading', filename: file.name })
    const buffer = await file.arrayBuffer()
    const req: WorkerRequest = { kind: 'open', buffer, filename: file.name }
    // transfer the buffer — we don't need a copy on this side
    w.postMessage(req, [buffer])
  }, [])

  return { state, loadFile }
}
