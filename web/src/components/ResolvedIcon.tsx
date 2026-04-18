interface ResolvedIconProps {
  resolved: boolean
}

/**
 * V / X icon for the rule's resolved status.
 *
 * Resolved rules (both selectors matched at least one real EID) → checkmark
 * in the green/env chip color.
 * Unresolved rules (referenced labels that don't exist on any entity) →
 * cross in the error red.
 *
 * Rendered as inline SVG so the strokes stay crisp at any zoom and don't
 * need a font for icon glyphs.
 */
export function ResolvedIcon({ resolved }: ResolvedIconProps) {
  if (resolved) {
    return (
      <svg
        className="resolved-icon resolved-icon--ok"
        width="14"
        height="14"
        viewBox="0 0 14 14"
        aria-label="resolved"
        role="img"
      >
        <path
          d="M2.5 7.5 L5.5 10.5 L11.5 3.5"
          fill="none"
          stroke="currentColor"
          strokeWidth="1.8"
          strokeLinecap="round"
          strokeLinejoin="round"
        />
      </svg>
    )
  }
  return (
    <svg
      className="resolved-icon resolved-icon--no"
      width="14"
      height="14"
      viewBox="0 0 14 14"
      aria-label="unresolved"
      role="img"
    >
      <path
        d="M3 3 L11 11 M11 3 L3 11"
        fill="none"
        stroke="currentColor"
        strokeWidth="1.8"
        strokeLinecap="round"
      />
    </svg>
  )
}
