import { useMemo } from 'react'

interface BitvecRendererProps {
  /** Sorted list of rule IDs whose bit is set. */
  ruleIds: number[]
  /** Total number of bits to represent in the visualization. Usually 4096. */
  totalBits?: number
  /** Pixel size of each cell. Defaults to 4 — small enough that 512
   *  cells fit in ~2048 px, large enough that individual bits are
   *  clickable/readable on a modal. */
  cellSize?: number
}

const BITS_PER_LINE = 512

/**
 * Render a bitvector as a compact grid, 512 cells per line. Set bits are
 * drawn as filled squares; unset area is a flat muted background (one rect
 * total, not 4096). For very dense vectors the grid is precomputed on a
 * tiny canvas rather than as individual SVG rects, but for our debug-DB
 * scale (a few dozen rules) SVG rects are the simpler choice.
 */
export function BitvecRenderer({
  ruleIds,
  totalBits = 4096,
  cellSize = 4,
}: BitvecRendererProps) {
  const lineCount = Math.ceil(totalBits / BITS_PER_LINE)
  const width = BITS_PER_LINE * cellSize
  const height = lineCount * cellSize

  const cells = useMemo(() => {
    const out: JSX.Element[] = []
    for (const rid of ruleIds) {
      if (rid < 0 || rid >= totalBits) continue
      const line = Math.floor(rid / BITS_PER_LINE)
      const col = rid % BITS_PER_LINE
      out.push(
        <rect
          key={rid}
          x={col * cellSize}
          y={line * cellSize}
          width={cellSize}
          height={cellSize}
          fill="var(--accent)"
        >
          <title>{`rule ${rid}`}</title>
        </rect>,
      )
    }
    return out
  }, [ruleIds, totalBits, cellSize])

  return (
    <svg
      className="bitvec"
      width={width}
      height={height}
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="xMinYMin meet"
      role="img"
      aria-label={`${ruleIds.length} of ${totalBits} bits set`}
    >
      <rect
        x={0}
        y={0}
        width={width}
        height={height}
        fill="var(--bitvec-bg)"
      />
      {/* thin separators between 512-bit lines */}
      {Array.from({ length: lineCount - 1 }, (_, i) => (
        <line
          key={i}
          x1={0}
          y1={(i + 1) * cellSize - 0.5}
          x2={width}
          y2={(i + 1) * cellSize - 0.5}
          stroke="var(--bg)"
          strokeWidth={1}
        />
      ))}
      {cells}
    </svg>
  )
}
