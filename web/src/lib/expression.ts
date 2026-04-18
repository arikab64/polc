/**
 * Parser + evaluator for the Enforcement Identities "expression" field.
 *
 * Grammar (matches the gc DSL selector grammar):
 *
 *   expr    := or
 *   or      := and ( "OR"  and )*
 *   and     := prim ( "AND" prim )*
 *   prim    := label | "(" or ")"
 *   label   := NAME ":" NAME
 *   NAME    := [A-Za-z_][A-Za-z0-9_-]*
 *
 * AND binds tighter than OR. Keywords are case-sensitive and must be
 * UPPERCASE, matching the compiler.
 *
 * The evaluator walks the AST for each EID's label set and returns true
 * or false. Parsing errors are returned as `{ ok: false, error }` rather
 * than thrown — the UI types incrementally and most intermediate strings
 * aren't valid expressions.
 */

import type { Label } from './types'

/* -------------------- AST -------------------- */

export type Expr =
  | { kind: 'label'; key: string; value: string }
  | { kind: 'and'; left: Expr; right: Expr }
  | { kind: 'or'; left: Expr; right: Expr }

/* -------------------- lexer -------------------- */

type Token =
  | { type: 'name'; value: string; pos: number }
  | { type: 'punct'; value: ':' | '(' | ')'; pos: number }
  | { type: 'kw'; value: 'AND' | 'OR'; pos: number }

const NAME_RE = /[A-Za-z_][A-Za-z0-9_-]*/y
/** Value token regex — allows the usual NAME shape plus pure-numeric /
 *  dotted-numeric values (IPs, version strings, port numbers as label
 *  values). The gc DSL's `value := NAME | IP`; we lex liberally and let
 *  the parser decide context. */
const VALUE_RE = /[A-Za-z0-9_][A-Za-z0-9_.-]*/y

function lex(src: string): { ok: true; tokens: Token[] } | { ok: false; error: string; pos: number } {
  const tokens: Token[] = []
  let i = 0
  while (i < src.length) {
    const c = src[i]
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r') { i++; continue }
    if (c === ':' || c === '(' || c === ')') {
      tokens.push({ type: 'punct', value: c, pos: i })
      i++
      continue
    }
    // Prefer NAME when the char can start one (letter or _). Only fall
    // through to the looser value pattern if NAME doesn't match but the
    // char is a digit — this happens when a label value is purely numeric.
    NAME_RE.lastIndex = i
    let m = NAME_RE.exec(src)
    if (m && m.index === i) {
      const word = m[0]
      if (word === 'AND' || word === 'OR') {
        tokens.push({ type: 'kw', value: word, pos: i })
      } else {
        tokens.push({ type: 'name', value: word, pos: i })
      }
      i += word.length
      continue
    }
    VALUE_RE.lastIndex = i
    m = VALUE_RE.exec(src)
    if (m && m.index === i) {
      // Tokenize as a name-like value — the parser only accepts these in
      // value position, which is what we want.
      tokens.push({ type: 'name', value: m[0], pos: i })
      i += m[0].length
      continue
    }
    return { ok: false, error: `unexpected character '${c}'`, pos: i }
  }
  return { ok: true, tokens }
}

/* -------------------- parser -------------------- */

export type ParseResult =
  | { ok: true; expr: Expr }
  | { ok: false; error: string; pos: number }

class Parser {
  private i = 0
  constructor(private tokens: Token[]) {}

  private peek(): Token | undefined {
    return this.tokens[this.i]
  }

  private consume(): Token {
    return this.tokens[this.i++]
  }

  private end(): boolean {
    return this.i >= this.tokens.length
  }

  parseOr(): Expr {
    let left = this.parseAnd()
    while (!this.end()) {
      const t = this.peek()
      if (!t || t.type !== 'kw' || t.value !== 'OR') break
      this.consume()
      const right = this.parseAnd()
      left = { kind: 'or', left, right }
    }
    return left
  }

  parseAnd(): Expr {
    let left = this.parsePrim()
    while (!this.end()) {
      const t = this.peek()
      if (!t || t.type !== 'kw' || t.value !== 'AND') break
      this.consume()
      const right = this.parsePrim()
      left = { kind: 'and', left, right }
    }
    return left
  }

  parsePrim(): Expr {
    const t = this.peek()
    if (!t) throw new ParseError('unexpected end of expression', this.endPos())
    if (t.type === 'punct' && t.value === '(') {
      this.consume()
      const inner = this.parseOr()
      const close = this.peek()
      if (!close || close.type !== 'punct' || close.value !== ')') {
        throw new ParseError("missing ')'", close?.pos ?? this.endPos())
      }
      this.consume()
      return inner
    }
    if (t.type === 'name') {
      const keyTok = this.consume() as Extract<Token, { type: 'name' }>
      const colon = this.peek()
      if (!colon || colon.type !== 'punct' || colon.value !== ':') {
        throw new ParseError("expected ':' after label key", colon?.pos ?? this.endPos())
      }
      this.consume()
      const valTok = this.peek()
      if (!valTok || valTok.type !== 'name') {
        throw new ParseError('expected label value', valTok?.pos ?? this.endPos())
      }
      this.consume()
      return { kind: 'label', key: keyTok.value, value: valTok.value }
    }
    throw new ParseError(`unexpected token '${formatToken(t)}'`, t.pos)
  }

  private endPos(): number {
    const last = this.tokens[this.tokens.length - 1]
    return last ? last.pos + 1 : 0
  }
}

class ParseError extends Error {
  constructor(message: string, public pos: number) { super(message) }
}

function formatToken(t: Token): string {
  if (t.type === 'name') return t.value
  if (t.type === 'kw') return t.value
  return t.value
}

export function parseExpression(src: string): ParseResult {
  const lexed = lex(src)
  if (!lexed.ok) return { ok: false, error: lexed.error, pos: lexed.pos }
  if (lexed.tokens.length === 0) {
    return { ok: false, error: 'empty expression', pos: 0 }
  }
  const parser = new Parser(lexed.tokens)
  let expr: Expr
  try {
    expr = parser.parseOr()
  } catch (e) {
    if (e instanceof ParseError) return { ok: false, error: e.message, pos: e.pos }
    throw e
  }
  // Ensure nothing trailing
  if (!(parser as unknown as { end(): boolean }).end()) {
    const t = (parser as unknown as { peek(): Token | undefined }).peek()
    return { ok: false, error: 'unexpected trailing input', pos: t?.pos ?? src.length }
  }
  return { ok: true, expr }
}

/* -------------------- evaluator -------------------- */

/**
 * Evaluate the expression against a label-set. The labelSet is a plain
 * string set of "key:value" strings for fast membership tests.
 */
export function evaluateExpression(expr: Expr, labelSet: Set<string>): boolean {
  switch (expr.kind) {
    case 'label':
      return labelSet.has(`${expr.key}:${expr.value}`)
    case 'and':
      return evaluateExpression(expr.left, labelSet) &&
             evaluateExpression(expr.right, labelSet)
    case 'or':
      return evaluateExpression(expr.left, labelSet) ||
             evaluateExpression(expr.right, labelSet)
  }
}

/** Convenience: convert Label[] to a "key:value" Set for evaluation. */
export function labelSetOf(labels: Label[]): Set<string> {
  return new Set(labels.map((l) => `${l.key}:${l.value}`))
}
