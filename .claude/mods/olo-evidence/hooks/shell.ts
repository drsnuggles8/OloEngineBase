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
      // env -S '<string>' [more]: the string is split into env's own arguments, and may
      // itself start with env options (`env -S '-i gh pr merge 1'` runs `gh pr merge 1`), so
      // the expanded words go back through env, not straight to the command.
      const split =
        exeName(w) !== 'env'
          ? undefined
          : flag === '-S' || flag === '--split-string'
            ? { value: rest[i + 1] ?? '', after: i + 2 }
            : flag.startsWith('--split-string=')
              ? { value: flag.slice('--split-string='.length), after: i + 1 }
              : /^-S./.test(flag)
                ? { value: flag.slice(2), after: i + 1 }
                : undefined
      if (split !== undefined) {
        rest = [w, ...rest.slice(1, i), ...tokenize(split.value), ...rest.slice(split.after)]
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

// PowerShell options that take a value (the rest are switches).
const PWSH_WITH_VALUE = new Set([
  '-executionpolicy', '-ep', '-ex', '-workingdirectory', '-wd', '-windowstyle', '-w', '-configurationname', '-config',
  '-outputformat', '-o', '-of', '-inputformat', '-if', '-in', '-version', '-v', '-settingsfile', '-custompipename', '-psconsolefile',
])
// bash/sh options that take a value.
const SH_WITH_VALUE = new Set(['-o', '+o', '-O', '+O', '--rcfile', '--init-file'])
// A script that runs its own -Command parameter: build-lock.ps1 runs the build it is handed.
const RUNS_ITS_COMMAND = /^build-lock\.ps1$/i

/**
 * The command a shell wrapper runs, read from the wrapper's OWN options only (words after a
 * script operand are the script's arguments, not the shell's): `pwsh/powershell -Command|-c
 * <cmd>`; `bash/sh/zsh/dash` with a single-dash flag group containing `c` (`-c`, `-lc`, `-ec`)
 * then `<cmd>`; `cmd /c|/k <cmd...>`. The one script whose `-Command` IS what runs is
 * build-lock.ps1. Undefined when the statement is no such wrapper.
 */
export function innerCommand(words: readonly string[]): string | undefined {
  const name = exeName(words[0])
  if (name === 'pwsh' || name === 'powershell') {
    for (let i = 1; i < words.length; i += 1) {
      const w = words[i] ?? ''
      const lower = w.toLowerCase()
      if (lower === '-command' || lower === '-c') {
        return words.slice(i + 1).join(' ')
      }
      if (lower === '-file' || lower === '-f') {
        const script = words[i + 1] ?? ''
        if (!RUNS_ITS_COMMAND.test(script.replace(/^.*[\\/]/, ''))) {
          return undefined
        }
        const at = words.findIndex((x, j) => j > i + 1 && /^-command$/i.test(x))
        return at >= 0 ? words[at + 1] : undefined
      }
      if (lower === '-encodedcommand' || lower === '-e' || lower === '-ec') {
        return undefined
      }
      if (!w.startsWith('-')) {
        // A bare operand: pwsh runs it as a script file, Windows PowerShell as a command.
        return name === 'powershell' ? words.slice(i).join(' ') : undefined
      }
      if (PWSH_WITH_VALUE.has(lower)) {
        i += 1
      }
    }
    return undefined
  }
  if (name === 'bash' || name === 'sh' || name === 'zsh' || name === 'dash') {
    for (let i = 1; i < words.length; i += 1) {
      const w = words[i] ?? ''
      if (w === '--' || w === '-') {
        return undefined
      }
      if (SH_WITH_VALUE.has(w)) {
        i += 1
        continue
      }
      if (/^-[A-Za-z]*c[A-Za-z]*$/.test(w)) {
        return words[i + 1]
      }
      if (!/^[-+]/.test(w)) {
        // The script operand: every word after it is the script's.
        return undefined
      }
    }
    return undefined
  }
  if (name === 'cmd') {
    for (let i = 1; i < words.length; i += 1) {
      const w = words[i] ?? ''
      if (/^\/[ck]$/i.test(w)) {
        return words.slice(i + 1).join(' ')
      }
      if (!w.startsWith('/')) {
        return undefined
      }
    }
    return undefined
  }
  return undefined
}
