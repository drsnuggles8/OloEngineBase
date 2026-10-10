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
  outcomeOf,
  parseInvocations,
  prProblems,
  redirectTarget,
  summaryMarkdown,
  unwaived,
  visualFrom,
} from './ledger'

const KEEP_RUNS = 200
const KEEP_VISUALS = 200
const FS_READ_LIMIT = 4 * 1024 * 1024
const TAIL_BYTES = 3_000_000

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

    if (command.length > 0 && isPrPublish(command)) {
      const refusal = await prGate($, command)
      if (refusal !== undefined) {
        $.ui.toast('olo-evidence: the PR is not backed by the record yet', { timeoutMs: 8000 })
        return { deny: refusal }
      }
    }

    const ran = await next(e)
    if (ran.deny !== undefined) {
      return ran
    }

    const visual = visualFrom(e.tool, { file_path: e.tool === 'Read' ? e.file_path : undefined, command })
    if (visual !== null && ran.isError !== true) {
      await record($, ledger => ({
        ...ledger,
        visuals: [...ledger.visuals, { at: 0, kind: visual, what: e.tool === 'Read' ? e.file_path : command.slice(0, 200) }].slice(-KEEP_VISUALS),
      }))
    }

    const invocations = command.length > 0 ? parseInvocations(command) : []
    if (invocations.length > 0) {
      const text = typeof ran.text === 'string' ? ran.text : ''
      const logPath = backgroundOutputPath(text) ?? redirectTarget(command)
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
    return ran
  }).catch(($, e, next) => next(e))
}

function isPrPublish(command: string): boolean {
  return /\bgh\s+pr\s+create\b/.test(command) || (/\bgh\s+pr\s+edit\b/.test(command) && /--body(?:-file)?\b|\s-[bF]\s/.test(command))
}

async function prGate($: EngineInterface, command: string): Promise<string | undefined> {
  if (command.includes(APPROVAL_MARKER)) {
    return undefined
  }
  const id = await identity($)
  if (id === undefined) {
    return undefined
  }
  const ledger = await load($, id.key)
  const judged = await judgeAll($, ledger)
  const changed = await changedFiles($)
  const body = await prBody($, command)
  const problems = unwaived(prProblems(judged, ledger.visuals, changed, body), command)
  if (problems.length === 0) {
    return undefined
  }
  const waivers = [...new Set(problems.flatMap(p => p.waivedBy))]
  return [
    `olo-evidence refused this: the record for ${id.branch} does not back the PR yet.`,
    ...problems.map(p => `- ${p.message}`),
    `Run /evidence to see the record.${waivers.includes(LEDGER_MARKER) ? ` If those runs happened outside this record (an earlier session), add ${LEDGER_MARKER} to the command and say so in the PR body.` : ''}`,
  ].join('\n')
}

/** The PR body from --body / -b or --body-file / -F; '' when the command carries none. */
async function prBody($: EngineInterface, command: string): Promise<string> {
  const file = /(?:--body-file|\s-F)\s+("?)([^"\s]+)\1/.exec(command)?.[2]
  if (file !== undefined) {
    try {
      return await $.fs.read(file)
    } catch {
      return ''
    }
  }
  const inline = /(?:--body|\s-b)\s+(?:"((?:[^"\\]|\\.)*)"|'([^']*)')/.exec(command)
  return inline?.[1] ?? inline?.[2] ?? ''
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
  if (identityCache !== undefined && now - identityCache.at < 60_000) {
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

async function readTail($: EngineInterface, path: string): Promise<string | undefined> {
  try {
    const stat = await $.fs.stat(path)
    if (stat.kind !== 'file') {
      return undefined
    }
    if (stat.size <= FS_READ_LIMIT) {
      return await $.fs.read(path)
    }
    const tail = await $.process.run(['tail', '-c', String(TAIL_BYTES), path], { timeoutMs: 15_000 })
    return tail.exitCode === 0 ? tail.stdout : undefined
  } catch {
    return undefined
  }
}
