import type { EngineInterface, Register } from 'claude-code'

import {
  checkCommand,
  contenders,
  formatRefusal,
  isTimingRun,
  loadNote,
  localChecksRunning,
  parseGpuSample,
  parseProcessRows,
  publishes,
  publishingApproved,
  refusals,
} from './guards'

const SHELL_TOOLS = ['Bash', 'PowerShell'] as const
const AUDIT_KEY = 'approved-gated-actions'
const AUDIT_KEEP = 200

const PROCESS_QUERY =
  "Get-CimInstance Win32_Process -Filter \"Name='ninja.exe' OR Name='OloEngine-Tests.exe' OR Name='clang-cl.exe' OR Name='lld-link.exe' OR Name='ctest.exe'\" " +
  '| Select-Object Name,ProcessId,CommandLine,ExecutablePath | ConvertTo-Json -Compress'

export const register: Register = on => {
  on('tool.call', { tool: SHELL_TOOLS }, async ($, e, next) => {
    const command = typeof e.command === 'string' ? e.command : ''
    if (command.length === 0) {
      return next(e)
    }

    const branch = /\bpush\b/.test(command) ? await currentBranch($) : undefined
    const refused = refusals(command, branch)
    if (refused.length > 0) {
      $.ui.toast(`olo-command-guards refused: ${refused.map(v => v.rule).join(', ')}`, { timeoutMs: 6000 })
      return { deny: formatRefusal(refused) }
    }

    const approvedGated = checkCommand(command, branch).filter(v => v.isGated && v.isApproved)
    if (approvedGated.length > 0) {
      await audit($, command, approvedGated.map(v => v.rule))
    }

    // Memory no-pr-before-local-checks-finish: push and open the PR only once every
    // local build and test run of this worktree has finished.
    if (publishes(command) && !publishingApproved(command)) {
      const running = await runningLocalChecks($)
      if (running.length > 0) {
        const list = running.map(r => `${r.name} (pid ${r.pid})`).join(', ')
        return {
          deny:
            `olo-command-guards refused this command:\n- Local checks of this worktree are still running: ${list}. ` +
            'Push and open the PR only after every local build and test run has finished and passed (the user asked for this: no CI overlapping a running local build). ' +
            'Wait for them; draft the PR body meanwhile.',
        }
      }
    }

    if (!isTimingRun(command)) {
      return next(e)
    }
    const note = await loadBeforeTiming($)
    const ran = await next(e)
    return note === undefined || ran.deny !== undefined ? ran : { ...ran, context: [...(ran.context ?? []), note] }
  }).catch(($, e, next) => {
    // The guard itself failed (a process query, a parse). Fall back to the pure text
    // rules alone, which need nothing from the engine, rather than refusing every
    // shell command or letting a gated one through unchecked.
    if (next.called) {
      return next(e)
    }
    const command = typeof e.command === 'string' ? e.command : ''
    const refused = refusals(command)
    return refused.length > 0 ? { deny: formatRefusal(refused) } : next(e)
  })
}

async function currentBranch($: EngineInterface): Promise<string | undefined> {
  try {
    const r = await $.process.run(['git', 'rev-parse', '--abbrev-ref', 'HEAD'], { timeoutMs: 5000 })
    return r.exitCode === 0 ? r.stdout.trim() : undefined
  } catch {
    return undefined
  }
}

async function runningLocalChecks($: EngineInterface) {
  try {
    const top = await $.process.run(['git', 'rev-parse', '--show-toplevel'], { timeoutMs: 5000 })
    if (top.exitCode !== 0) {
      return []
    }
    const ps = await $.process.run(['pwsh', '-NoProfile', '-NonInteractive', '-Command', PROCESS_QUERY], { timeoutMs: 20_000 })
    if (ps.exitCode !== 0) {
      return []
    }
    return localChecksRunning(parseProcessRows(ps.stdout), top.stdout.trim())
  } catch {
    // Fail open: a missing pwsh must not block every push.
    return []
  }
}

async function loadBeforeTiming($: EngineInterface): Promise<string | undefined> {
  try {
    const [gpu, tasks] = await Promise.all([
      $.process.run(['nvidia-smi', '--query-gpu=clocks.gr,power.draw,utilization.gpu', '--format=csv,noheader,nounits'], { timeoutMs: 10_000 }),
      $.process.run(['tasklist', '/FO', 'CSV', '/NH'], { timeoutMs: 10_000 }),
    ])
    const sample = gpu.exitCode === 0 ? parseGpuSample(gpu.stdout) : undefined
    const others = tasks.exitCode === 0 ? contenders(tasks.stdout) : []
    const note = loadNote(sample, others)
    if (note !== undefined) {
      $.ui.toast('olo-command-guards: timing run on a busy box; its timings are contended', { timeoutMs: 8000 })
    }
    return note
  } catch {
    return undefined
  }
}

async function audit($: EngineInterface, command: string, rules: string[]): Promise<void> {
  try {
    const at = new Date(await $.clock.now()).toISOString()
    const previous = await $.store.get(AUDIT_KEY)
    const list = Array.isArray(previous) ? previous : []
    await $.store.set(AUDIT_KEY, [...list, { at, rules, command: command.slice(0, 500) }].slice(-AUDIT_KEEP))
    $.ui.toast(`olo-command-guards: user-approved ${rules.join(', ')} (logged)`, { timeoutMs: 6000 })
  } catch {
    // Logging is best effort; the approval itself stands.
  }
}
