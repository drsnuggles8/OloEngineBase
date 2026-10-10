import type { EngineInterface, Register } from 'claude-code'

import type { EvidenceLedger, EvidenceRun, EvidenceVisual } from '../types'
import {
  APPROVAL_MARKER,
  backgroundOutputPath,
  carriesResult,
  emptyLedger,
  judge,
  type Judged,
  LEDGER_MARKER,
  markersOf,
  outcomeOf,
  parseInvocations,
  prBodySource,
  prProblems,
  prStatement,
  redirectTarget,
  summaryMarkdown,
  unwaived,
  visualFrom,
} from './ledger'

const KEEP_RUNS = 200
const KEEP_VISUALS = 200
const FS_READ_LIMIT = 4 * 1024 * 1024
const TAIL_BYTES = 3_000_000
const IDENTITY_TTL_MS = 60_000

// A command that can move HEAD or the checkout: the cached branch identity is dropped after it.
const MOVES_HEAD = /\bgit\b[^\n]*\b(?:switch|checkout|worktree|rebase|merge|reset|pull|branch)\b|(?:^|[;&|]\s*)(?:cd|pushd|Set-Location)\s/

// Writes to one branch's ledger are serialised: parallel tool calls must not lose entries.
let writes: Promise<unknown> = Promise.resolve()
let identityCache: { at: number; key: string; branch: string; top: string } | undefined

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({ name: 'evidence', description: "This branch's recorded builds, test runs and visual checks, and what still stands before a PR" })
    return next(e)
  })

  on('command.run', { command: 'evidence' }, async $ => {
    const id = await identity($)
    if (id === undefined) {
      return { text: 'Not in a git checkout: no evidence ledger.' }
    }
    const ledger = await load($, id.key)
    const judged = await judgeAll($, ledger)
    const changed = await changedFiles($)
    const problems = prProblems(judged, ledger.visuals, changed, '')
    return { text: summaryMarkdown(judged, ledger.visuals, problems, id.branch, await $.clock.now()) }
  })

  // Record runs and visual checks; gate PR creation on the record.
  on('tool.call', async ($, e, next) => {
    const command = e.tool === 'Bash' || e.tool === 'PowerShell' ? e.command : ''

    if (command.length > 0) {
      const refusal = await prGate($, command)
      if (refusal !== undefined) {
        $.ui.toast('olo-evidence: the PR is not backed by the record yet', { timeoutMs: 8000 })
        return { deny: refusal }
      }
    }

    const invocations = command.length > 0 ? parseInvocations(command) : []
    const redirect = invocations.length > 0 ? redirectTarget(command) : null
    if (redirect !== null) {
      // The new run is about to overwrite this log: earlier runs judged from it keep the
      // outcome it holds NOW, or they would be judged by the new run's output.
      await settle($, () => freezeRunsOn($, redirect))
    }

    const ran = await next(e)
    if (ran.deny !== undefined) {
      return ran
    }

    // Recording is best effort and must never fail the call it records: the tool has run.
    await settle($, async () => {
      if (MOVES_HEAD.test(command)) {
        identityCache = undefined
      }
      const visual = visualFrom(e.tool, { file_path: e.tool === 'Read' ? e.file_path : undefined, command })
      if (visual !== null && ran.isError !== true) {
        await record($, ledger => ({
          ...ledger,
          visuals: [...ledger.visuals, { at: 0, kind: visual, what: e.tool === 'Read' ? e.file_path : command.slice(0, 200) }].slice(-KEEP_VISUALS),
        }))
      }
      if (invocations.length > 0) {
        const text = typeof ran.text === 'string' ? ran.text : ''
        const logPath = backgroundOutputPath(text) ?? redirect
        await record($, ledger => ({
          ...ledger,
          runs: [
            ...ledger.runs,
            ...invocations.map(
              (inv, i): EvidenceRun => ({
                id: `${e.tool_use_id}-${i}`,
                at: 0,
                kind: inv.kind,
                command: command.slice(0, 300),
                config: inv.config,
                scope: inv.scope,
                logPath,
                captured: carriesResult(inv.kind, text) ? outcomeOf(inv.kind, text) : null,
              }),
            ),
          ].slice(-KEEP_RUNS),
        }))
      }
    })
    return ran
  })
    // Only the gate runs before next(e); if it throws, the call goes ahead (fail open), and
    // where next was already called, next(e) replays its result without running anything twice.
    .catch(($, e, next) => next(e))
}

/** Runs best-effort work; a failure is swallowed so the tool call it rides on is unaffected. */
async function settle($: EngineInterface, work: () => Promise<void>): Promise<void> {
  try {
    await work()
  } catch {
    // The record misses one entry; the session goes on.
  }
}

async function prGate($: EngineInterface, command: string): Promise<string | undefined> {
  const statement = prStatement(command)
  if (statement === undefined) {
    return undefined
  }
  const markers = markersOf(statement)
  if (markers.includes(APPROVAL_MARKER)) {
    return undefined
  }
  const id = await identity($)
  if (id === undefined) {
    return undefined
  }
  const ledger = await load($, id.key)
  const judged = await judgeAll($, ledger)
  const changed = await changedFiles($)
  const source = prBodySource(statement)
  const body = source.file !== undefined ? await readSmall($, source.file) ?? '' : source.inline ?? ''
  const problems = unwaived(prProblems(judged, ledger.visuals, changed, body), markers)
  if (problems.length === 0) {
    return undefined
  }
  const waivers = [...new Set(problems.flatMap(p => p.waivedBy))]
  return [
    `olo-evidence refused this: the record for ${id.branch} does not back the PR yet.`,
    ...problems.map(p => `- ${p.message}`),
    `Run /evidence to see the record.${waivers.includes(LEDGER_MARKER) ? ` If those runs happened outside this record (an earlier session), end the gh command with a comment carrying ${LEDGER_MARKER} and say so in the PR body.` : ''}`,
  ].join('\n')
}

/** Earlier runs that read their outcome from `path` take what it says now, and stop reading it. */
async function freezeRunsOn($: EngineInterface, path: string): Promise<void> {
  const id = await identity($)
  if (id === undefined) {
    return
  }
  const same = (p: string | null) => p !== null && p.replace(/\\/g, '/').toLowerCase() === path.replace(/\\/g, '/').toLowerCase()
  const ledger = await load($, id.key)
  if (!ledger.runs.some(r => same(r.logPath))) {
    return
  }
  const log = await readTail($, path)
  await record($, current => ({
    ...current,
    runs: current.runs.map(run => {
      if (!same(run.logPath)) {
        return run
      }
      const outcome = log === undefined ? undefined : outcomeOf(run.kind, log)
      return { ...run, logPath: null, captured: outcome !== undefined && outcome.state !== 'unknown' ? outcome : run.captured }
    }),
  }))
}

async function judgeAll($: EngineInterface, ledger: EvidenceLedger): Promise<Judged[]> {
  return Promise.all(ledger.runs.map(async run => judge(run, run.logPath === null ? undefined : await readTail($, run.logPath))))
}

async function changedFiles($: EngineInterface): Promise<string[]> {
  try {
    const r = await $.process.run(['git', 'diff', '--name-only', 'origin/master...HEAD'], { timeoutMs: 20_000 })
    return r.exitCode === 0 ? r.stdout.split('\n').map(l => l.trim()).filter(Boolean) : []
  } catch {
    return []
  }
}

async function identity($: EngineInterface): Promise<{ key: string; branch: string; top: string } | undefined> {
  const now = await $.clock.now()
  if (identityCache !== undefined && now - identityCache.at < IDENTITY_TTL_MS) {
    return identityCache
  }
  try {
    const [top, branch] = await Promise.all([
      $.process.run(['git', 'rev-parse', '--show-toplevel'], { timeoutMs: 5000 }),
      $.process.run(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], { timeoutMs: 5000 }),
    ])
    if (top.exitCode !== 0 || branch.exitCode !== 0) {
      return undefined
    }
    const t = top.stdout.trim()
    const b = branch.stdout.trim()
    identityCache = { at: now, key: `ledger:${t.toLowerCase()}|${b}`, branch: b, top: t }
    return identityCache
  } catch {
    return undefined
  }
}

async function load($: EngineInterface, key: string): Promise<EvidenceLedger> {
  const stored = (await $.store.get(key)) as EvidenceLedger | undefined
  return stored !== undefined && Array.isArray(stored.runs) && Array.isArray(stored.visuals) ? stored : emptyLedger()
}

/** Applies `change` to this branch's ledger, stamping new entries with the time. */
async function record($: EngineInterface, change: (ledger: EvidenceLedger) => EvidenceLedger): Promise<void> {
  const id = await identity($)
  if (id === undefined) {
    return
  }
  const now = await $.clock.now()
  const step = writes.then(async () => {
    const before = await load($, id.key)
    const after = change(before)
    const stamp = <T extends { at: number }>(xs: T[]) => xs.map(x => (x.at === 0 ? { ...x, at: now } : x))
    await $.store.set(id.key, { runs: stamp(after.runs), visuals: stamp(after.visuals) as EvidenceVisual[] })
  })
  writes = step.catch(() => undefined)
  await step
}

async function readSmall($: EngineInterface, path: string): Promise<string | undefined> {
  try {
    const stat = await $.fs.stat(path)
    return stat.kind === 'file' && stat.size <= FS_READ_LIMIT ? await $.fs.read(path) : undefined
  } catch {
    return undefined
  }
}

/**
 * A log's text: whole when `$.fs.read` takes it (up to 4 MiB), else its last few MB through
 * this mod's scripts/tail.ps1 (pwsh is always present here), else `tail` where one exists.
 */
async function readTail($: EngineInterface, path: string): Promise<string | undefined> {
  try {
    const stat = await $.fs.stat(path)
    if (stat.kind !== 'file') {
      return undefined
    }
    if (stat.size <= FS_READ_LIMIT) {
      return await $.fs.read(path)
    }
  } catch {
    return undefined
  }
  const attempts: string[][] = [
    ['pwsh', '-NoProfile', '-NonInteractive', '-File', `${$.plugin.root}/scripts/tail.ps1`, '-Path', path, '-Bytes', String(TAIL_BYTES)],
    ['tail', '-c', String(TAIL_BYTES), path],
  ]
  for (const argv of attempts) {
    try {
      const r = await $.process.run(argv, { timeoutMs: 15_000 })
      if (r.exitCode === 0) {
        return r.stdout
      }
    } catch {
      // Try the next way.
    }
  }
  return undefined
}
