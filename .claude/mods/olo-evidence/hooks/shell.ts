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

// Launchers that run the command after them, with the options of theirs that take a value.
// `env -S '<cmd>'` splits its value into the command it runs.
const LAUNCHERS: Record<string, { withValue: string[]; positional?: number }> = {
  env: { withValue: ['-u', '--unset', '-C', '--chdir', '-P'] },
  timeout: { withValue: ['-s', '--signal', '-k', '--kill-after'], positional: 1 },
  nice: { withValue: ['-n', '--adjustment'] },
  time: { withValue: ['-o', '--output', '-f', '--format'] },
  call: { withValue: [] },
  command: { withValue: [] },
  builtin: { withValue: [] },
  exec: { withValue: ['-a'] },
}

/**
 * A statement's words from the command it runs. Drops `&`, `VAR=value` assignments and the
 * launchers above with their own options (`env -i -u X`, `timeout -s KILL 60`, `nice -n 5`),
 * so `env -i GH_TOKEN=x gh pr merge` is still `gh pr merge`. Normalising only exposes the
 * command that runs: it never removes a word the rules would have refused.
 */
export function commandWords(words: readonly string[]): string[] {
  let rest = [...words]
  for (;;) {
    const w = rest[0] ?? ''
    if (w === '&' || /^\w+=/.test(w)) {
      rest = rest.slice(1)
      continue
    }
    const launcher = LAUNCHERS[exeName(w)]
    if (launcher === undefined) {
      return rest
    }
    let i = 1
    while (i < rest.length && (rest[i] ?? '').startsWith('-') && rest[i] !== '-') {
      const flag = rest[i] ?? ''
      if (exeName(w) === 'env' && (flag === '-S' || flag === '--split-string')) {
        // env -S 'cmd args' [more]: the value is the command line.
        rest = [...tokenize(rest[i + 1] ?? ''), ...rest.slice(i + 2)]
        i = -1
        break
      }
      if (flag.startsWith('--split-string=') && exeName(w) === 'env') {
        rest = [...tokenize(flag.slice('--split-string='.length)), ...rest.slice(i + 1)]
        i = -1
        break
      }
      i += launcher.withValue.includes(flag) ? 2 : 1
    }
    if (i === -1) {
      continue
    }
    if (rest[i] === '-') {
      i += 1
    }
    rest = rest.slice(i + (launcher.positional ?? 0))
  }
}

/**
 * The command a shell wrapper runs: `pwsh/powershell -Command|-c <cmd>`, `bash/sh/zsh` with
 * any single-dash flag group containing `c` (`-c`, `-lc`, `-ec`, `-euxc`) then `<cmd>`, and
 * `cmd /c|/k <cmd...>`. Undefined when the statement is no such wrapper.
 */
export function innerCommand(words: readonly string[]): string | undefined {
  const name = exeName(words[0])
  const find = (test: (w: string) => boolean) => {
    const at = words.findIndex((w, i) => i > 0 && test(w))
    return at >= 0 ? at : undefined
  }
  if (name === 'pwsh' || name === 'powershell') {
    const at = find(w => /^-(?:command|c)$/i.test(w))
    return at === undefined ? undefined : words.slice(at + 1).join(' ')
  }
  if (name === 'bash' || name === 'sh' || name === 'zsh' || name === 'dash') {
    const at = find(w => /^-[A-Za-z]*c[A-Za-z]*$/.test(w))
    return at === undefined ? undefined : words[at + 1]
  }
  if (name === 'cmd') {
    const at = find(w => /^\/[ck]$/i.test(w))
    return at === undefined ? undefined : words.slice(at + 1).join(' ')
  }
  return undefined
}
