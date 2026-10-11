export type EvidenceRunKind = 'build' | 'test'

export type EvidenceOutcome = {
  state: 'passed' | 'failed' | 'running' | 'unknown'
  /** One line: what the log says. */
  detail: string
  failed: string[]
  ran: number
  skipped: number
}

export type EvidenceRun = {
  id: string
  at: number
  kind: EvidenceRunKind
  command: string
  config: string
  /** Builds: the tree and targets. Tests: the gtest filter or ctest regex. */
  scope: string
  /** Where the output went (a redirect, or a background task's output file). */
  logPath: string | null
  /** The outcome read from the tool's own output, when it carried one. */
  captured: EvidenceOutcome | null
}

export type EvidenceVisual = {
  at: number
  kind: 'png-viewed' | 'live-capture' | 'live-check'
  what: string
}

export type EvidenceLedger = {
  runs: EvidenceRun[]
  visuals: EvidenceVisual[]
}
