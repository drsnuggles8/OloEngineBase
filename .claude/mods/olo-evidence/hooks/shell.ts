// Quote-aware shell statement splitting, the same as olo-command-guards' (mods are
// independent plugins, so each carries its own copy; keep the two in step).

export type Separator = 'start' | '&&' | '||' | ';' | '|' | '\n' | '&'

export type Statement = { separator: Separator; text: string; words: string[] }

/**
 * Splits a shell command into statements, remembering which separator led into each,
 * and tokenises each one. Quote-aware (single, double, and PowerShell backtick escapes
 * are left as literal characters), so a `;` inside a commit message is not a split.
 */
export function splitStatements(command: string): Statement[] {
  const statements: Statement[] = []
  let current = ''
  let separator: Separator = 'start'
  let quote: '"' | "'" | undefined
  let i = 0

  const flush = (next: Separator) => {
    if (current.trim().length > 0) {
      statements.push({ separator, text: current.trim(), words: tokenize(current) })
    }
    current = ''
    separator = next
  }

  while (i < command.length) {
    const c = command[i] ?? ''
    const two = command.slice(i, i + 2)
    if (quote !== undefined) {
      if (c === '\\' && quote === '"' && i + 1 < command.length) {
        current += two
        i += 2
        continue
      }
      if (c === quote) {
        quote = undefined
      }
      current += c
      i += 1
      continue
    }
    if (c === '"' || c === "'") {
      quote = c
      current += c
      i += 1
      continue
    }
    if (c === '#' && (current.length === 0 || /\s$/.test(current))) {
      // A comment runs to the end of the line; keep it in the text (markers live there)
      // but out of the words.
      const end = command.indexOf('\n', i)
      const stop = end === -1 ? command.length : end
      current += command.slice(i, stop)
      i = stop
      continue
    }
    if (two === '&&' || two === '||') {
      flush(two)
      i += 2
      continue
    }
    if (c === ';' || c === '\n' || c === '|' || (c === '&' && command[i + 1] !== '>' && command[i - 1] !== '>')) {
      flush(c as Separator)
      i += 1
      continue
    }
    current += c
    i += 1
  }
  flush('start')
  return statements
}

/** Words of one statement, quotes removed, a trailing `#` comment dropped. */
export function tokenize(text: string): string[] {
  const words: string[] = []
  let word = ''
  let quote: '"' | "'" | undefined
  let started = false
  for (let i = 0; i < text.length; i += 1) {
    const c = text[i] ?? ''
    if (quote !== undefined) {
      if (c === quote) {
        quote = undefined
      } else {
        word += c
      }
      continue
    }
    if (c === '"' || c === "'") {
      quote = c
      started = true
      continue
    }
    if (c === '#' && !started) {
      break
    }
    if (/\s/.test(c)) {
      if (started) {
        words.push(word)
      }
      word = ''
      started = false
      continue
    }
    word += c
    started = true
  }
  if (started) {
    words.push(word)
  }
  return words
}

export function commentOf(text: string): string {
  let quote: '"' | "'" | undefined
  for (let i = 0; i < text.length; i += 1) {
    const c = text[i] ?? ''
    if (quote !== undefined) {
      if (c === quote) {
        quote = undefined
      }
      continue
    }
    if (c === '"' || c === "'") {
      quote = c
      continue
    }
    if (c === '#' && (i === 0 || /\s/.test(text[i - 1] ?? ''))) {
      return text.slice(i + 1)
    }
  }
  return ''
}

export function exeName(word: string | undefined): string {
  return (word ?? '').replace(/^.*[\\/]/, '').replace(/\.exe$/i, '').toLowerCase()
}

/**
 * A statement's words from the command it runs: drops `&`, `call`, `env`, `time`, `nice`,
 * `VAR=value` assignments and `timeout N`, so `GH_TOKEN=x gh pr merge` is still `gh pr merge`.
 */
export function commandWords(words: readonly string[]): string[] {
  let i = 0
  while (i < words.length) {
    const w = words[i] ?? ''
    if (w === '&' || /^(?:call|env|time|nice|command|builtin)$/i.test(w) || /^\w+=/.test(w)) {
      i += 1
    } else if (/^timeout$/i.test(w)) {
      i += 2
    } else {
      break
    }
  }
  return words.slice(i)
}

/**
 * The command a shell wrapper runs: `pwsh/powershell -Command|-c <cmd>`, `bash/sh -c <cmd>`,
 * `cmd /c <cmd...>`. Undefined when the statement is no such wrapper.
 */
export function innerCommand(words: readonly string[]): string | undefined {
  const name = exeName(words[0])
  const after = (...flags: string[]) => {
    const at = words.findIndex(w => flags.includes(w.toLowerCase()))
    return at >= 0 ? at : undefined
  }
  if (name === 'pwsh' || name === 'powershell') {
    const at = after('-command', '-c')
    return at === undefined ? undefined : words.slice(at + 1).join(' ')
  }
  if (name === 'bash' || name === 'sh' || name === 'zsh') {
    const at = after('-c', '-lc')
    return at === undefined ? undefined : words[at + 1]
  }
  if (name === 'cmd') {
    const at = after('/c', '/k')
    return at === undefined ? undefined : words.slice(at + 1).join(' ')
  }
  return undefined
}
