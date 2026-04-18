import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// sqlite-wasm uses SharedArrayBuffer when available (OPFS/worker mode).
// Dev + preview servers need COOP/COEP headers for that to work.
// Falls back to in-memory mode if headers aren't present — still fine for us.
export default defineConfig({
  plugins: [react()],
  server: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  preview: {
    headers: {
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
  },
  optimizeDeps: {
    exclude: ['@sqlite.org/sqlite-wasm'],
  },
  worker: {
    format: 'es',
  },
})
