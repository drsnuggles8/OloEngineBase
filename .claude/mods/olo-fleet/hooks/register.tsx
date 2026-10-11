import { atom, read, update } from 'claude-code'
import type { ElementTable, EngineInterface, Register } from 'claude-code'

import type { FleetPr, FleetSnapshot, FleetWorktree } from '../types'
import {
  liveQueue,
  minutesSince,
  orphanPrs,
  parseLockStatus,
  parsePrs,
  parseWorktrees,
  prByBranch,
  prLine,
  slotLine,
  statusLine,
  summaryMarkdown,
  transitions,
  waiterLine,
  worktreeColor,
  worktreeLine,
} from './fleet'

const PANE = 'olo-fleet'
const REFRESH_MS = 3 * 60 * 1000
const AFTER_PUBLISH_MS = 60 * 1000
const SNAPSHOT = { plugin: 'olo-fleet', key: 'snapshot' } as const
const snapshotAtom = atom(SNAPSHOT, null)

const PR_QUERY =
  'query($owner:String!,$name:String!){repository(owner:$owner,name:$name){pullRequests(states:OPEN,first:50){nodes{' +
  'number title headRefName isDraft mergeable url reviewThreads(first:100){nodes{isResolved}} ' +
  'commits(last:1){nodes{commit{statusCheckRollup{state contexts(first:100){nodes{__typename ' +
  '... on CheckRun{name status conclusion} ... on StatusContext{context state}}}}}}}}}}}'

// One refresh at a time; a timer tick during a slow refresh is dropped, not queued.
let isRefreshing = false

export const register: Register = on => {
  on('session.start', async ($, e, next) => {
    await $.command.register({
      name: 'fleet',
      description: 'Show every OloEngine worktree, its PR, CI and review threads, and who holds or waits for the build lock',
    })
    $.clock.every(REFRESH_MS, () => {
      void refresh($)
    })
    void refresh($)
    return next(e)
  })

  // /fleet always answers in its own transcript row (drawn by the CommandOutput hook
  // below). The VS Code extension reports a mod's pane as placed but never draws it, so
  // the pane is only a bonus for surfaces that do show one (the fullscreen terminal).
  on('command.run', { command: 'fleet' }, async $ => {
    void $.ui.open({ id: PANE, title: 'OloEngine fleet' })
    await refresh($, { force: true })
    const snap = (await $.state.get(SNAPSHOT)).value
    return { text: snap === null || snap === undefined ? 'Could not read the fleet (git or gh failed).' : summaryMarkdown(snap, await $.clock.now()) }
  })

  // After a push or a new PR, CI starts: look again in a minute instead of waiting a full period.
  on('tool.call', { tool: ['Bash', 'PowerShell'] }, async ($, e, next) => {
    const ran = await next(e)
    const command = typeof e.command === 'string' ? e.command : ''
    if (ran.deny === undefined && /\bgit\b[^\n;&|]*\bpush\b|\bgh\s+pr\s+(?:create|ready|merge)\b/.test(command)) {
      $.clock.after(AFTER_PUBLISH_MS, () => {
        void refresh($)
      })
    }
    return ran
  })

  on('ui.render', { component: 'Pane', requestId: PANE }, async ($, e) =>
    drawFleet($, $.ui.resolve(e), await read($, snapshotAtom), Math.max(40, e.props.bodyColumns)),
  )

  // Draws /fleet's own transcript row as the full view; the markdown text stays what the
  // model reads and what a surface that cannot draw the tree shows.
  on('ui.render', { component: 'CommandOutput', props: { command: 'fleet' } }, async ($, e, next) => {
    const snap = await read($, snapshotAtom)
    if (snap === null || e.props.isErrored) {
      return next(e)
    }
    return drawFleet($, $.ui.resolve(e), snap, Math.max(40, e.viewport?.columns ?? 120))
  })
}

async function run($: EngineInterface, argv: string[], timeoutMs = 20_000): Promise<string | undefined> {
  try {
    const r = await $.process.run(argv, { timeoutMs })
    return r.exitCode === 0 ? r.stdout : undefined
  } catch {
    return undefined
  }
}

async function refresh($: EngineInterface, opts: { force?: boolean } = {}): Promise<void> {
  if (isRefreshing) {
    if (!opts.force) {
      return
    }
    // A user asked: wait for the refresh already under way (bounded), then show its result.
    for (let i = 0; i < 120 && isRefreshing; i += 1) {
      await $.clock.sleep(500)
    }
    return
  }
  isRefreshing = true
  try {
    const errors: string[] = []
    const commonDir = (await run($, ['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'], 5000))?.trim()
    if (commonDir === undefined || commonDir.length === 0) {
      return
    }
    const base = commonDir.replace(/[\\/]\.git[\\/]?$/, '')

    const [porcelain, remote, branchOut, lockOut] = await Promise.all([
      run($, ['git', '-C', base, 'worktree', 'list', '--porcelain'], 10_000),
      run($, ['git', '-C', base, 'remote', 'get-url', 'origin'], 5000),
      run($, ['git', 'rev-parse', '--abbrev-ref', 'HEAD'], 5000),
      run($, ['pwsh', '-NoProfile', '-NonInteractive', '-File', `${$.plugin.root}/scripts/lock-status.ps1`, '-CommonDir', commonDir], 20_000),
    ])

    const listed = porcelain === undefined ? [] : parseWorktrees(porcelain)
    if (porcelain === undefined) {
      errors.push('git worktree list failed')
    }
    const dirtyCounts = await Promise.all(
      listed.map(async w => {
        const out = await run($, ['git', '-C', w.path, 'status', '--porcelain'], 30_000)
        return out === undefined ? null : out.split('\n').filter(l => l.trim().length > 0).length
      }),
    )
    const worktrees: FleetWorktree[] = listed.map((w, i) => ({ ...w, dirty: dirtyCounts[i] ?? null }))

    // A failed or garbled GitHub reply keeps the last PRs known (marked stale) rather than
    // an empty list, so the next good reply still compares against real CI states.
    const previous = await $.state.get(SNAPSHOT)
    let prs: FleetPr[] = previous.value?.prs ?? []
    const repo = remote === undefined ? undefined : /github\.com[:/]([^/]+)\/(.+?)(?:\.git)?\s*$/.exec(remote)
    if (repo?.[1] !== undefined && repo[2] !== undefined) {
      const out = await run($, ['gh', 'api', 'graphql', '-f', `query=${PR_QUERY}`, '-F', `owner=${repo[1]}`, '-F', `name=${repo[2]}`], 30_000)
      if (out === undefined) {
        errors.push('GitHub query failed (gh not authenticated, or offline); PR states are from the last good query')
      } else {
        try {
          prs = parsePrs(out)
        } catch {
          errors.push('GitHub replied with something that is not the expected JSON (a proxy page?); PR states are from the last good query')
        }
      }
    }

    let lock = { slots: [] as FleetSnapshot['slots'], queue: [] as FleetSnapshot['queue'] }
    if (lockOut === undefined) {
      errors.push('build-lock status could not be read')
    } else {
      try {
        lock = parseLockStatus(lockOut)
      } catch {
        errors.push('build-lock status was not JSON')
      }
    }

    const snapshot: FleetSnapshot = { takenAt: await $.clock.now(), base, worktrees, prs, ...lock, errors }
    await update($, snapshotAtom, () => snapshot)

    $.ui.status(statusLine(snapshot, branchOut?.trim() ?? null))

    for (const t of transitions(previous.value?.prs, prs)) {
      const text =
        t.to === 'SUCCESS'
          ? `PR #${t.number} CI is green: ${t.title}`
          : `PR #${t.number} CI failed${t.failing.length > 0 ? ` (${t.failing.slice(0, 3).join(', ')})` : ''}: ${t.title}`
      $.ui.toast(text, { timeoutMs: 10_000 })
      void $.ui.notify(text, { title: 'OloEngine CI' }).catch(() => undefined)
    }
  } catch {
    // Every caller fires refresh and forgets it (`void refresh($)`): a rejection here would be
    // unhandled. The next tick tries again.
  } finally {
    isRefreshing = false
  }
}


async function drawFleet($: EngineInterface, el: ElementTable, snap: FleetSnapshot | null, width: number) {
  const { Box, Text, Button } = el
  if (snap === null) {
    return (
      <Box flexDirection="column">
        <Text dimColor>Collecting worktrees, PRs and the build lock…</Text>
      </Box>
    )
  }
  const now = await $.clock.now()
  const prs = prByBranch(snap.prs)
  const clip = (s: string) => (s.length > width ? `${s.slice(0, width - 1)}…` : s)

  return (
    <Box flexDirection="column">
      <Text bold>Worktrees</Text>
      {snap.worktrees.map(w => (
        <Text key={`wt-${w.path}`} wrap="truncate" color={worktreeColor(w, prs.get(w.branch ?? ''))}>
          {clip(worktreeLine(w, prs.get(w.branch ?? '')))}
        </Text>
      ))}
      {orphanPrs(snap).map(p => (
        <Text key={`pr-${p.number}`} wrap="truncate" dimColor>
          {clip(`  (no worktree) #${p.number} ${p.branch}: ${prLine(p)}`)}
        </Text>
      ))}
      <Text> </Text>
      <Text bold>Build lock</Text>
      {snap.slots.map(s => (
        <Text key={`slot-${s.name}`} wrap="truncate" color={s.held ? 'warning' : 'success'}>
          {clip(`  ${slotLine(s, now)}`)}
        </Text>
      ))}
      {liveQueue(snap).map((q, i) => (
        <Text key={`q-${q.enqueued}-${i}`} wrap="truncate" dimColor>
          {clip(`  ${waiterLine(q, i, now)}`)}
        </Text>
      ))}
      {snap.queue.some(q => !q.alive) && (
        <Text dimColor>{`  ${snap.queue.filter(q => !q.alive).length} stale ticket(s) from dead processes (build-lock skips them)`}</Text>
      )}
      {snap.errors.map((err, i) => (
        <Text key={`err-${i}`} color="error" wrap="truncate">
          {clip(`  ${err}`)}
        </Text>
      ))}
      <Box>
        <Text dimColor>{`Updated ${minutesSince(new Date(snap.takenAt).toISOString(), now) ?? 0} min ago  `}</Text>
        <Button key="refresh" label="Refresh" onPress={() => refresh($, { force: true })} />
      </Box>
    </Box>
  )
}
