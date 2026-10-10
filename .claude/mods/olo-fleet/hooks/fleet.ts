// Pure parsing and summarising for the fleet view. No engine calls: the tests drive it.

import type { FleetCheckState, FleetPr, FleetSlot, FleetSnapshot, FleetWaiter, FleetWorktree } from '../types'

export function baseName(path: string): string {
  return path.replace(/[\\/]+$/, '').split(/[\\/]/).pop() ?? path
}

/** `git worktree list --porcelain`. The first entry is the main worktree. */
export function parseWorktrees(porcelain: string): Omit<FleetWorktree, 'dirty'>[] {
  const out: Omit<FleetWorktree, 'dirty'>[] = []
  for (const block of porcelain.replace(/\r\n?/g, '\n').split(/\n\n+/)) {
    const path = /^worktree (.+)$/m.exec(block)?.[1]?.trim()
    if (path === undefined) {
      continue
    }
    const head = /^HEAD ([0-9a-f]+)$/m.exec(block)?.[1] ?? ''
    const branch = /^branch refs\/heads\/(.+)$/m.exec(block)?.[1]?.trim() ?? null
    out.push({ path, name: baseName(path), branch, head: head.slice(0, 9), isBase: out.length === 0 })
  }
  return out
}

type RawContext = { name?: string; context?: string; status?: string; conclusion?: string | null; state?: string }

/** The GraphQL answer for open PRs (see PR_QUERY in register). */
export function parsePrs(json: string): FleetPr[] {
  const data = JSON.parse(json) as {
    data?: { repository?: { pullRequests?: { nodes?: unknown[] } } }
  }
  const nodes = data.data?.repository?.pullRequests?.nodes ?? []
  return nodes.flatMap(node => {
    const n = node as {
      number: number
      title: string
      headRefName: string
      url: string
      isDraft: boolean
      mergeable: string
      reviewThreads?: { nodes?: { isResolved: boolean }[] }
      commits?: { nodes?: { commit?: { statusCheckRollup?: { state?: string; contexts?: { nodes?: RawContext[] } } | null } }[] }
    }
    if (typeof n.number !== 'number') {
      return []
    }
    const rollup = n.commits?.nodes?.[0]?.commit?.statusCheckRollup ?? null
    const contexts = rollup?.contexts?.nodes ?? []
    const failing = contexts
      .filter(c => ['FAILURE', 'ERROR', 'TIMED_OUT', 'CANCELLED', 'ACTION_REQUIRED', 'STARTUP_FAILURE'].includes(String(c.conclusion ?? c.state ?? '')))
      .map(c => c.name ?? c.context ?? '?')
    const running = contexts.filter(c => (c.status !== undefined && c.status !== 'COMPLETED') || c.state === 'PENDING').length
    return [
      {
        number: n.number,
        title: n.title,
        branch: n.headRefName,
        url: n.url,
        isDraft: n.isDraft,
        mergeable: n.mergeable,
        checks: (rollup?.state as FleetCheckState | undefined) ?? 'NONE',
        failing,
        running,
        unresolved: (n.reviewThreads?.nodes ?? []).filter(t => !t.isResolved).length,
      },
    ]
  })
}

/** lock-status.ps1's JSON. */
export function parseLockStatus(json: string): { slots: FleetSlot[]; queue: FleetWaiter[] } {
  const raw = JSON.parse(json) as { slots?: FleetSlot[] | FleetSlot; queue?: FleetWaiter[] | FleetWaiter }
  const list = <T>(v: T[] | T | undefined): T[] => (v === undefined ? [] : Array.isArray(v) ? v : [v])
  return { slots: list(raw.slots), queue: list(raw.queue) }
}

export type Transition = { number: number; title: string; from: FleetCheckState; to: FleetCheckState; failing: string[] }

/** CI results that settled since the last snapshot: worth a notification. */
export function transitions(previous: readonly FleetPr[] | undefined, next: readonly FleetPr[]): Transition[] {
  if (previous === undefined) {
    return []
  }
  const before = new Map(previous.map(p => [p.number, p.checks]))
  return next.flatMap(pr => {
    const from = before.get(pr.number)
    const settled = pr.checks === 'SUCCESS' || pr.checks === 'FAILURE' || pr.checks === 'ERROR'
    return from !== undefined && from !== pr.checks && settled ? [{ number: pr.number, title: pr.title, from, to: pr.checks, failing: pr.failing }] : []
  })
}

export function minutesSince(iso: string | null, now: number): number | undefined {
  if (iso === null) {
    return undefined
  }
  const t = Date.parse(iso)
  return Number.isNaN(t) ? undefined : Math.max(0, Math.round((now - t) / 60_000))
}

/** "C:/repos/OloEngine-x/build-cached" from a build command, for naming who builds. */
export function buildTree(command: string | null): string | null {
  const m = command === null ? null : /--build\s+("?)([^"\s]+)\1/.exec(command)
  return m?.[2] ?? null
}

export function describeHolder(slot: FleetSlot, now: number): string {
  const who = slot.worktree ? baseName(slot.worktree) : 'unknown'
  // A sub-worktree a session fanned out into builds under the parent session's name: say which tree.
  const tree = buildTree(slot.command)
  const treeOwner = tree !== null && /[\\/]/.test(tree) ? baseName(tree.replace(/[\\/]build[\w-]*$/, '')) : who
  const where = treeOwner !== who ? ` (${treeOwner})` : ''
  const config = /--config\s+(\w+)/.exec(slot.command ?? '')?.[1]
  const mins = minutesSince(slot.acquired, now)
  return `${who}${where}${config ? ` ${config}` : ''}${mins !== undefined ? `, ${mins} min` : ''}`
}

const CHECK_MARK: Record<FleetCheckState, string> = {
  SUCCESS: 'green',
  FAILURE: 'RED',
  ERROR: 'RED',
  PENDING: 'running',
  EXPECTED: 'waiting',
  NONE: 'no checks',
}

export function checkWord(state: FleetCheckState): string {
  return CHECK_MARK[state]
}

/** The one-line status: this worktree's PR, then the build lock. */
export function statusLine(snapshot: FleetSnapshot, currentBranch: string | null): string {
  const parts: string[] = []
  const mine = snapshot.prs.find(p => p.branch === currentBranch)
  if (mine !== undefined) {
    const threads = mine.unresolved > 0 ? `, ${mine.unresolved} open thread(s)` : ''
    parts.push(`PR #${mine.number} ${checkWord(mine.checks)}${threads}`)
  }
  const held = snapshot.slots.filter(s => s.held).length
  const waiting = snapshot.queue.filter(q => q.alive).length
  parts.push(`build lock ${held}/${snapshot.slots.length} held${waiting > 0 ? `, ${waiting} waiting` : ''}`)
  const red = snapshot.prs.filter(p => p.checks === 'FAILURE' || p.checks === 'ERROR').length
  if (red > 0) {
    parts.push(`${red} red PR(s)`)
  }
  return `fleet: ${parts.join(' · ')}`
}

/** Worktrees whose branch has an open PR, keyed by branch. */
export function prByBranch(prs: readonly FleetPr[]): Map<string, FleetPr> {
  return new Map(prs.map(p => [p.branch, p]))
}

export function prLine(pr: FleetPr): string {
  const parts = [pr.checks === 'PENDING' && pr.running > 0 ? `CI running, ${pr.running} check(s) left` : `CI ${checkWord(pr.checks)}`]
  if (pr.failing.length > 0) {
    parts.push(`failing: ${pr.failing.slice(0, 3).join(', ')}${pr.failing.length > 3 ? ', …' : ''}`)
  }
  if (pr.unresolved > 0) {
    parts.push(`${pr.unresolved} open thread(s)`)
  }
  if (pr.mergeable === 'CONFLICTING') {
    parts.push('CONFLICTS with master')
  }
  if (pr.isDraft) {
    parts.push('draft')
  }
  return parts.join(', ')
}

export function worktreeLine(w: FleetWorktree, pr: FleetPr | undefined): string {
  const dirty = w.dirty === null ? '?' : w.dirty === 0 ? 'clean' : `${w.dirty} changed`
  const branch = w.branch ?? `detached ${w.head}`
  const prText = pr === undefined ? (w.isBase ? '' : 'no PR') : `PR #${pr.number}: ${prLine(pr)}`
  return `  ${w.name}  [${branch}]  ${dirty}${prText ? `  ${prText}` : ''}`
}

export function worktreeColor(w: FleetWorktree, pr: FleetPr | undefined): 'error' | 'success' | 'warning' | 'text' {
  if (pr?.checks === 'FAILURE' || pr?.checks === 'ERROR' || pr?.mergeable === 'CONFLICTING') {
    return 'error'
  }
  if (pr?.unresolved !== undefined && pr.unresolved > 0) {
    return 'warning'
  }
  return pr?.checks === 'SUCCESS' ? 'success' : 'text'
}

export function orphanPrs(snap: FleetSnapshot): FleetPr[] {
  return snap.prs.filter(p => !snap.worktrees.some(w => w.branch === p.branch))
}

export function liveQueue(snap: FleetSnapshot): FleetWaiter[] {
  return snap.queue.filter(q => q.alive)
}

export function slotLine(slot: FleetSlot, now: number): string {
  return `${slot.name}: ${slot.held ? `held by ${describeHolder(slot, now)}` : 'free'}`
}

export function waiterLine(waiter: FleetWaiter, index: number, now: number): string {
  const as: FleetSlot = { name: '', held: false, worktree: waiter.worktree, command: waiter.command, acquired: waiter.enqueued }
  return `${index + 1}. ${describeHolder(as, now)} waiting`
}

/**
 * The fleet as markdown: what /fleet answers where no pane can be placed. It is also
 * the text the model reads for that command row, so it stays short.
 */
export function summaryMarkdown(snap: FleetSnapshot, now: number): string {
  const prs = prByBranch(snap.prs)
  const lines = ['**Worktrees**', '']
  for (const w of snap.worktrees) {
    lines.push(`- ${worktreeLine(w, prs.get(w.branch ?? '')).trim()}`)
  }
  for (const p of orphanPrs(snap)) {
    lines.push(`- (no worktree) PR #${p.number} \`${p.branch}\`: ${prLine(p)}`)
  }
  lines.push('', '**Build lock**', '')
  for (const s of snap.slots) {
    lines.push(`- ${slotLine(s, now)}`)
  }
  liveQueue(snap).forEach((q, i) => lines.push(`- ${waiterLine(q, i, now)}`))
  const stale = snap.queue.length - liveQueue(snap).length
  if (stale > 0) {
    lines.push(`- ${stale} stale ticket(s) from dead processes`)
  }
  for (const err of snap.errors) {
    lines.push(`- error: ${err}`)
  }
  return lines.join('\n')
}
