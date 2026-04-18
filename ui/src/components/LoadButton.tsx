import { useRef } from 'react'

interface LoadButtonProps {
  onLoad: (file: File) => void
  /** "primary" style for the initial empty-state CTA; default for header */
  variant?: 'primary' | 'default'
  label?: string
}

export function LoadButton({ onLoad, variant = 'default', label = 'Load ...' }: LoadButtonProps) {
  const inputRef = useRef<HTMLInputElement>(null)

  return (
    <>
      <input
        ref={inputRef}
        type="file"
        accept=".db,.sqlite,.sqlite3,application/octet-stream"
        style={{ display: 'none' }}
        onChange={(e) => {
          const file = e.target.files?.[0]
          if (file) onLoad(file)
          // reset so selecting the same file twice re-fires
          e.target.value = ''
        }}
      />
      <button
        className={`load-btn ${variant === 'primary' ? 'load-btn--primary' : ''}`}
        onClick={() => inputRef.current?.click()}
      >
        {label}
      </button>
    </>
  )
}
