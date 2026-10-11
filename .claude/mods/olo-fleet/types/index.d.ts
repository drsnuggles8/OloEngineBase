export type FleetCheckState = 'SUCCESS' | 'FAILURE' | 'ERROR' | 'PENDING' | 'EXPECTED' | 'NONE'

export type FleetWorktree = {
  path: string
  name: string
  branch: string | null
  head: string
  isBase: boolean
  /** Lines of `git status --porcelain`; null when it could not be read in time. */
  dirty: number | null
}

export type FleetPr = {
  number: number
  title: string
  branch: string
  url: string
  isDraft: boolean
  mergeable: string
  checks: FleetCheckState
  failing: string[]
  running: number
  unresolved: number
}

export type FleetSlot = {
  name: string
  held: boolean
  worktree: string | null
  command: string | null
  acquired: string | null
}

export type FleetWaiter = {
  worktree: string
  command: string
  enqueued: string
  alive: boolean
}

export type FleetSnapshot = {
  takenAt: number
  base: string
  worktrees: FleetWorktree[]
  prs: FleetPr[]
  slots: FleetSlot[]
  queue: FleetWaiter[]
  errors: string[]
}

declare module 'claude-code' {
  interface PluginState {
    'olo-fleet': { snapshot: FleetSnapshot | null }
  }
}
