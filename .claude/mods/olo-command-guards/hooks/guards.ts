// Pure command analysis for the guards. No engine calls: the tests drive every rule.
//
// Text matching on a shell command is best effort by nature. Every rule here is biased
// the same way the repo's Python guards are: a false positive costs one re-issued call
// with a clear reason, a false negative costs the incident the rule was written after.

/** The marker that lets a gated action through once the user approved that action. */
export const APPROVAL_MARKER = 'OLO_USER_APPROVED'

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

/** The words after `git [-C dir] [-c k=v]...`, starting at the subcommand; undefined if not git. */
export function gitArgs(words: string[]): string[] | undefined {
  const first = (words[0] ?? '').replace(/^.*[\\/]/, '').replace(/\.exe$/i, '')
  if (first !== 'git') {
    return undefined
  }
  let i = 1
  while (i < words.length) {
    const w = words[i] ?? ''
    if (w === '-C' || w === '-c' || w === '--git-dir' || w === '--work-tree') {
      i += 2
    } else if (w.startsWith('--git-dir=') || w.startsWith('--work-tree=') || w === '--no-pager') {
      i += 1
    } else {
      break
    }
  }
  return words.slice(i)
}

export type Violation = {
  rule: string
  /** True for CLAUDE.md's "gated in every context" actions: the approval marker lets them through. */
  isGated: boolean
  /** The statement that broke the rule carries the approval marker in its own trailing comment. */
  isApproved: boolean
  reason: string
}

/**
 * The trailing comment of one statement (`# ...`, outside any quotes), or ''. The approval
 * marker counts only here: inside a quoted argument (a PR body, an issue comment) it is
 * data, and on one statement of a chain it approves that statement alone.
 */
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

function isApprovedStatement(statement: Statement): boolean {
  return commentOf(statement.text).includes(APPROVAL_MARKER)
}

const PROTECTED_BRANCHES = new Set(['master', 'main'])

type Found = Omit<Violation, 'isApproved'>

function pushViolations(args: string[], branch: string | undefined): Found[] {
  const rest = args.slice(1)
  const flags = rest.filter(w => w.startsWith('-'))
  const positional = rest.filter(w => !w.startsWith('-'))
  const out: Found[] = []
  const suggestion = branch && !PROTECTED_BRANCHES.has(branch) ? `git push -u origin ${branch}` : 'git push -u origin feature/<slug>'

  const isForce =
    flags.some(f => f === '-f' || f === '--force' || f.startsWith('--force-with-lease') || f === '--force-if-includes') ||
    positional.slice(1).some(r => r.startsWith('+'))
  if (isForce) {
    out.push({
      rule: 'force-push',
      isGated: true,
      reason: 'A force push is gated in every context (CLAUDE.md, Committing and publishing): ask the user first.',
    })
  }

  const destinations = positional.slice(1).map(r => {
    const dest = r.includes(':') ? r.slice(r.lastIndexOf(':') + 1) : r
    return dest.replace(/^\+/, '').replace(/^refs\/heads\//, '')
  })
  const pushesHeadFromProtected = positional.length >= 2 && destinations.includes('HEAD') && branch !== undefined && PROTECTED_BRANCHES.has(branch)
  if (destinations.some(d => PROTECTED_BRANCHES.has(d)) || pushesHeadFromProtected) {
    out.push({
      rule: 'push-to-master',
      isGated: true,
      reason: 'Pushing to master is gated in every context (CLAUDE.md, Committing and publishing): work lands through a PR.',
    })
  }

  const isWholeRepoForm = flags.some(f => f === '--tags' || f === '--all' || f === '--mirror' || f === '--delete' || f === '-d')
  if (positional.length < 2 && !isWholeRepoForm) {
    out.push({
      rule: 'bare-push',
      isGated: false,
      reason: `Never a bare \`git push\`: under push.default it can land on master, which has happened here. Name the remote and the branch: \`${suggestion}\`.`,
    })
  }
  return out
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

/**
 * Every rule a command breaks. `branch` is the current branch, when known, for the
 * suggestion and for `git push origin HEAD` from master. A shell wrapper is looked into:
 * its own trailing comment approves what it runs.
 */
export function checkCommand(command: string, branch?: string): Violation[] {
  const all: Violation[] = []
  for (const statement of splitStatements(command)) {
    const out: Found[] = []
    const words = commandWords(statement.words)
    const inner = innerCommand(words)
    if (inner !== undefined) {
      const outerApproved = isApprovedStatement(statement)
      all.push(...checkCommand(inner, branch).map(v => ({ ...v, isApproved: v.isApproved || outerApproved })))
      continue
    }
    const args = gitArgs(words)
    const sub = args?.[0]

    if (args !== undefined && sub === 'commit' && args.includes('--amend') && statement.separator !== 'start' && statement.separator !== '&&') {
      out.push({
        rule: 'chained-amend',
        isGated: false,
        reason:
          `\`git commit --amend\` runs after \`${statement.separator === '\n' ? 'a newline' : statement.separator}\`, so it runs even when the command before it failed. ` +
          'A failed `git revert -q` followed by `; git commit --amend` rewrote an already-pushed commit here (#1405). Chain with `&&`, or better, pass the message to the first command.',
      })
    }

    if (args !== undefined && (sub === 'push' || sub === 'commit') && args.includes('--no-verify')) {
      out.push({
        rule: 'no-verify',
        isGated: true,
        reason: 'Skipping hooks with --no-verify needs the user to have asked for it: fix what the hook reports instead.',
      })
    }

    if (args !== undefined && sub === 'push') {
      out.push(...pushViolations(args, branch))
    }

    if (args !== undefined && sub === 'reset' && args.includes('--hard')) {
      out.push({
        rule: 'reset-hard',
        isGated: true,
        reason: '`git reset --hard` is gated in every context (CLAUDE.md): it discards work. Prefer `git stash` or a new branch, or ask the user.',
      })
    }

    const first = (words[0] ?? '').replace(/^.*[\\/]/, '').replace(/\.exe$/i, '')
    if (first === 'gh' && words[1] === 'pr' && words[2] === 'merge') {
      out.push({ rule: 'pr-merge', isGated: true, reason: 'Merging a PR is the user\'s call in every context (CLAUDE.md).' })
    }
    if (first === 'gh' && words[1] === 'issue' && words[2] === 'close') {
      out.push({
        rule: 'issue-close',
        isGated: true,
        reason: 'Closing an issue is the user\'s call (CLAUDE.md): post the evidence, recommend closure, let the user close.',
      })
    }

    if (statement.separator === '|' && /^(?:Set-Content|sc|Add-Content|ac|Out-File)$/i.test(words[0] ?? '') && words.some(w => /^-NoNewline$/i.test(w))) {
      out.push({
        rule: 'set-content-nonewline',
        isGated: false,
        reason:
          'A pipe into `Set-Content -NoNewline` joins every line into ONE: the pipe hands over an array of lines and -NoNewline drops the separator between them. ' +
          'It flattened a GLSL file and a scene here (#1360). Use Bash redirection (`git show <sha>:path > path`) or the Write/Edit tools.',
      })
    }

    const approved = isApprovedStatement(statement)
    all.push(...out.map(v => ({ ...v, isApproved: approved })))
  }
  return all
}

/** What the guard refuses: everything ungated, and a gated rule unless its own statement carries the marker. */
export function refusals(command: string, branch?: string): Violation[] {
  return checkCommand(command, branch).filter(v => !v.isGated || !v.isApproved)
}

export function formatRefusal(violations: readonly Violation[]): string {
  const gated = violations.some(v => v.isGated)
  const lines = violations.map(v => `- ${v.reason}`)
  if (gated) {
    lines.push(
      `If the user explicitly approved THIS action in this conversation, re-issue it with the literal marker ${APPROVAL_MARKER} in a trailing comment (it is logged). Otherwise ask them.`,
    )
  }
  return `olo-command-guards refused this command:\n${lines.join('\n')}`
}

function isPublishing(s: Statement): boolean {
  const words = commandWords(s.words)
  const inner = innerCommand(words)
  if (inner !== undefined) {
    return publishes(inner)
  }
  const args = gitArgs(words)
  return (args?.[0] === 'push' && !args.includes('--dry-run') && !args.includes('-n')) || (exeName(words[0]) === 'gh' && words[1] === 'pr' && words[2] === 'create')
}

/** Does the command publish work (a push, a new PR)? Local checks must have finished first. */
export function publishes(command: string): boolean {
  return splitStatements(command).some(isPublishing)
}

/** Every publishing statement carries the approval marker in its own (or its wrapper's) trailing comment. */
export function publishingApproved(command: string): boolean {
  const publishing = splitStatements(command).filter(isPublishing)
  return (
    publishing.length > 0 &&
    publishing.every(s => {
      if (isApprovedStatement(s)) {
        return true
      }
      const inner = innerCommand(commandWords(s.words))
      return inner !== undefined && publishingApproved(inner)
    })
  )
}

// ------------------------------------------------------------ timing runs ---

const TIMING_FLAGS = /--olo-perf-rebase|--olo-capture-manifest|--olo-perf\b/
const TIMING_FILTER = /--gtest_filter=\S*(?:Perf|Timing|Cost|Baseline|Benchmark|Budget|Throughput|Latency)/i

/** A run whose numbers mean something only on a quiet GPU. */
export function isTimingRun(command: string): boolean {
  return /OloEngine-Tests|OloEditor|OloRuntime/i.test(command) && (TIMING_FLAGS.test(command) || TIMING_FILTER.test(command))
}

export type GpuSample = { clockMHz: number; powerW: number; utilization: number }

/** Parses `nvidia-smi --query-gpu=clocks.gr,power.draw,utilization.gpu --format=csv,noheader,nounits`. */
export function parseGpuSample(stdout: string): GpuSample | undefined {
  const line = stdout.split(/\r?\n/).find(l => l.trim().length > 0)
  if (line === undefined) {
    return undefined
  }
  const [clock, power, util] = line.split(',').map(v => Number.parseFloat(v.trim()))
  if (clock === undefined || power === undefined || util === undefined || [clock, power, util].some(Number.isNaN)) {
    return undefined
  }
  return { clockMHz: clock, powerW: power, utilization: util }
}

// Heavy processes that contend with a measurement, by image name.
const CONTENDERS = /^(?:OloEngine-Tests|OloEditor|OloRuntime|OloServer|ninja|lld-link|clang-cl|cl|link|MSBuild|blender)\.exe$/i

/** Image names from `tasklist /FO CSV /NH` that contend with a measurement. */
export function contenders(tasklistCsv: string): string[] {
  const out = new Map<string, number>()
  for (const line of tasklistCsv.split(/\r?\n/)) {
    const name = /^"([^"]+)"/.exec(line)?.[1]
    if (name !== undefined && CONTENDERS.test(name)) {
      out.set(name, (out.get(name) ?? 0) + 1)
    }
  }
  return [...out].map(([name, count]) => (count > 1 ? `${name} x${count}` : name))
}

/**
 * Whether the box looked busy before a timing run. At its 210 MHz floor the 4090 reads
 * ~30% utilisation from the compositor alone, so utilisation is not the signal: the
 * graphics clock (busy above ~1 GHz) and the power draw (busy above ~60 W) are.
 * Memory: check-for-other-gpu-load-before-trusting-timings.
 */
export function loadNote(sample: GpuSample | undefined, others: string[]): string | undefined {
  const busyGpu = sample !== undefined && (sample.clockMHz > 1000 || sample.powerW > 60)
  if (!busyGpu && others.length === 0) {
    return undefined
  }
  const parts = [
    busyGpu && sample ? `the GPU was busy before this run started (graphics clock ${sample.clockMHz} MHz, ${sample.powerW} W)` : '',
    others.length > 0 ? `other heavy processes were running: ${others.join(', ')}` : '',
  ].filter(Boolean)
  return (
    `olo-command-guards: this is a timing run, and ${parts.join('; ')}. ` +
    'GPU and frame timings from it are contended: do not record or compare them as a baseline. Pixels and correctness results are unaffected. ' +
    'Check `nvidia-smi` and the process list again and re-run timing on a quiet box (memory: check-for-other-gpu-load-before-trusting-timings).'
  )
}

// ----------------------------------------------------- local checks running ---

// Not cmake: a configure is no check, and an IDE's cmake would pin the guard shut.
const LOCAL_CHECK_IMAGES = /^(?:ninja|OloEngine-Tests|clang-cl|lld-link|ctest)\.exe$/i

export type ProcessRow = { name: string; pid: number; commandLine: string; executablePath: string }

/** Parses the JSON of `Get-CimInstance Win32_Process | Select Name,ProcessId,CommandLine,ExecutablePath | ConvertTo-Json`. */
export function parseProcessRows(json: string): ProcessRow[] {
  if (json.trim().length === 0) {
    return []
  }
  const raw: unknown = JSON.parse(json)
  const list = Array.isArray(raw) ? raw : [raw]
  return list.flatMap(r => {
    if (typeof r !== 'object' || r === null) {
      return []
    }
    const o = r as Record<string, unknown>
    return [
      {
        name: String(o.Name ?? ''),
        pid: Number(o.ProcessId ?? 0),
        commandLine: String(o.CommandLine ?? ''),
        executablePath: String(o.ExecutablePath ?? ''),
      },
    ]
  })
}

/** Builds and test runs that belong to the worktree at `root` (paths compared case-insensitively, either slash). */
export function localChecksRunning(rows: readonly ProcessRow[], root: string): ProcessRow[] {
  const norm = (p: string) => p.replace(/\\/g, '/').toLowerCase()
  const prefix = `${norm(root).replace(/\/$/, '')}/`
  return rows.filter(r => LOCAL_CHECK_IMAGES.test(r.name) && (norm(r.commandLine).includes(prefix) || norm(r.executablePath).startsWith(prefix)))
}
