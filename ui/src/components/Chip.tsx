import { chipKeyClass } from '../lib/format'

interface ChipProps {
  labelKey: string
  labelValue: string
}

/**
 * Colored label pill matching the diagram's convention:
 *   env:*   green
 *   app:*   pink
 *   role:*  tan
 *   *       neutral
 *
 * Rendered as a flat monospace pill — no drop shadows, no heavy rounding.
 */
export function Chip({ labelKey, labelValue }: ChipProps) {
  return (
    <span className="chip" data-key={chipKeyClass(labelKey)}>
      <span>{labelKey}</span>
      <span className="chip-sep">:</span>
      <span>{labelValue}</span>
    </span>
  )
}
