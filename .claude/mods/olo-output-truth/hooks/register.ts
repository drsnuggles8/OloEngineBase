import type { EngineInterface, Register } from 'claude-code'

import { analyze, type Finding, formatFindings, normalize, parseTaskNotification } from './analyze'

// Tools whose text can carry a build or test log: a foreground run, or a log read back.
const LOG_TOOLS = ['Bash', 'PowerShell', 'Read'] as const

const FS_READ_LIMIT = 4 * 1024 * 1024
const TAIL_BYTES = 3_000_000
const REPEAT_WINDOW_MS = 10 * 60 * 1000

// The same finding is reported once per window: re-reading a log to look at its errors
// should not repeat the banner every time. Module state; a reload clears it, which costs
// at most one repeat.
const reported = new Map<string, number>()

export const register: Register = on => {
  on('tool.call', { tool: LOG_TOOLS }, async ($, e, next) => {
    const ran = await next(e)
    if (ran.deny !== undefined || typeof ran.text !== 'string' || ran.text.length === 0) {
      return ran
    }
    const findings = await fresh($, analyze(normalize(ran.text, e.tool === 'Read')))
    if (findings.length === 0) {
      return ran
    }
    surface($, findings)
    return { ...ran, context: [...(ran.context ?? []), formatFindings(findings)] }
  })

  // A backgrounded build or test run reports "completed (exit code 0)" in its
  // notification whatever the log says. Read the log's tail and say what it says.
  on('session.append', async ($, e, next) => {
    const textBlocks = e.message.content.filter(
      (b): b is { type: 'text'; text: string } => b.type === 'text' && typeof (b as { text?: unknown }).text === 'string',
    )
    const notification = parseTaskNotification(textBlocks.map(b => b.text).join('\n'))
    if (notification === undefined || !notification.isShellCommand) {
      return next(e)
    }

    const log = await readTail($, notification.outputFile)
    if (log === undefined) {
      return next(e)
    }
    const findings = await fresh($, analyze(normalize(log)))
    if (findings.length === 0) {
      return next(e)
    }
    surface($, findings)
    const note = `${formatFindings(findings)}\n(read from the tail of ${notification.outputFile}; the task status above reflects the wrapper, not the build or test result)`
    return next({ ...e, message: { ...e.message, content: [...e.message.content, { type: 'text', text: note }] } })
  })
}

async function fresh($: EngineInterface, findings: Finding[]): Promise<Finding[]> {
  const now = await $.clock.now()
  return findings.filter(f => {
    const last = reported.get(f.id)
    if (last !== undefined && now - last < REPEAT_WINDOW_MS) {
      return false
    }
    reported.set(f.id, now)
    return true
  })
}

function surface($: EngineInterface, findings: Finding[]): void {
  const first = findings.find(f => f.severity === 'error')
  if (first !== undefined) {
    $.ui.toast(`olo-output-truth: ${first.message.split('\n')[0] ?? 'a failure'}`, { timeoutMs: 8000 })
  }
}

/**
 * A log's text: whole when `$.fs.read` takes it (up to 4 MiB), else its last few MB through
 * this mod's scripts/tail.ps1 (`$.process.run` has no shell, and pwsh is always present
 * here), else `tail` where one is on PATH.
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
