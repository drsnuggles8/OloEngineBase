// Pure analysis of build and test output. No engine calls here, so the tests can
// exercise every rule without a session.
//
// Each rule exists because the failure it catches was hit on this repo and read as
// success (see the memory notes named beside each rule).

export type Severity = 'error' | 'warn'

export type Finding = {
  /** Stable identity, so the same finding is reported once and not on every re-read. */
  id: string
  severity: Severity
  message: string
}

const MAX_LISTED = 12
const MAX_LINE = 220

/**
 * Normalises tool output for line-anchored matching: drops NULs (the engine logger
 * embeds them, which is why `grep` calls the test log binary), folds CRLF, and for
 * the Read tool strips its `cat -n` line-number gutter.
 */
export function normalize(text: string, isNumberedListing = false): string {
  let t = text.replace(/\u0000/g, '').replace(/\r\n?/g, '\n')
  if (isNumberedListing) {
    t = t.replace(/^ *\d+\t/gm, '')
  }
  return t
}

function clip(line: string): string {
  const one = line.trim()
  return one.length > MAX_LINE ? `${one.slice(0, MAX_LINE)}…` : one
}

function unique<T>(items: Iterable<T>): T[] {
  return [...new Set(items)]
}

// ---------------------------------------------------------------- builds ---

const NINJA_STOPPED = /^ninja: build stopped: (.*)$/m
const NINJA_FAILED_STEP = /^FAILED: (.*)$/gm
const MSBUILD_FAILED = /^\s*Build FAILED\.\s*$/m
const MSBUILD_ERRORS = /^\s*([1-9]\d*) Error\(s\)\s*$/m
const ERROR_LINE =
  /(?:\berror\s+(?:C|LNK|MSB)\d{4}\b|: (?:fatal )?error:|^\s*(?:lld-link|ld\.lld|clang-cl|cl)(?:\.exe)?: (?:fatal )?error\b|\bundefined symbol\b|\bfatal error\b)/im

/**
 * A build failure that the surrounding command may report as success: build-lock.ps1
 * propagates ninja's status (since #788), but a run piped into `tail` or `tee` reports
 * the pipe's last command, and that is how "exit code 0" builds with errors were read
 * as green here. Memory: build-lock-wrapper-swallows-ninja-exit-code.
 */
export function analyzeBuild(text: string): Finding[] {
  const stopped = NINJA_STOPPED.exec(text)
  const msbuild = MSBUILD_FAILED.test(text) || MSBUILD_ERRORS.test(text)
  const failedSteps = [...text.matchAll(NINJA_FAILED_STEP)].map(m => m[1] ?? '')
  if (!stopped && !msbuild && failedSteps.length === 0) {
    return []
  }

  const errorLines = unique(
    text
      .split('\n')
      .filter(line => ERROR_LINE.test(line))
      .map(clip),
  ).slice(0, 6)

  const exitedZero = /\[exited with code 0\]|exit code 0\b/.test(text)
  const headline = stopped
    ? `ninja: build stopped: ${stopped[1]}`
    : msbuild
      ? 'MSBuild reported Build FAILED'
      : `${failedSteps.length} ninja step(s) FAILED`

  const lines = [
    `BUILD FAILED (${headline}).`,
    exitedZero
      ? 'The exit code 0 reported for it is NOT evidence of success: build-lock.ps1 passes on the status of the LAST command in its -Command, so a pipe (`| tail`, `| tee`) or a statement after the build masks the build\'s status.'
      : 'Read the log, not the exit status: a pipe (`| tail`, `| tee`) reports its last command\'s status, not the build\'s.',
    errorLines.length > 0 ? `First errors:\n${errorLines.map(l => `  ${l}`).join('\n')}` : '',
    failedSteps.length > 0 && errorLines.length === 0
      ? `Failed steps:\n${unique(failedSteps.map(clip)).slice(0, 4).map(l => `  ${l}`).join('\n')}`
      : '',
  ].filter(Boolean)

  return [
    {
      id: `build:${headline}:${errorLines[0] ?? failedSteps[0] ?? ''}`,
      severity: 'error',
      message: lines.join('\n'),
    },
  ]
}

// ----------------------------------------------------------------- gtest ---

const GTEST_RUNNING = /^\[==========\] Running (\d+) tests? from (\d+) test suites?\.?\s*$/gm
const GTEST_DONE = /^\[==========\] (\d+) tests? from (\d+) test suites? ran\./m
const GTEST_FILTER = /^Note: Google Test filter = (.*)$/m
const GTEST_RUN = /^\[ RUN      \] (\S+)/gm
const GTEST_OK = /^\[       OK \] (\S+)/gm
const GTEST_FAILED = /^\[  FAILED  \] ([^\s,]+\.[^\s,]+)/gm
const GTEST_SKIPPED = /^\[  SKIPPED \] (\S+)/gm
const CTEST_NONE = /^No tests were found!!!\s*$/m

function names(text: string, re: RegExp): string[] {
  return unique([...text.matchAll(re)].map(m => m[1] ?? ''))
}

/** Splits a log into one segment per `[==========] Running` header (sharded or repeated runs). */
function gtestSegments(text: string): string[] {
  const starts = [...text.matchAll(GTEST_RUNNING)].map(m => m.index ?? 0)
  return starts.map((start, i) => text.slice(start, starts[i + 1] ?? text.length))
}

function listed(items: string[]): string {
  const shown = items.slice(0, MAX_LISTED).join(', ')
  return items.length > MAX_LISTED ? `${shown}, … (+${items.length - MAX_LISTED} more)` : shown
}

/**
 * gtest / ctest output that looks fine and is not.
 * Memories: new-test-file-needs-cmakelists-entry, attribution-filter-must-select-the-test,
 * build-lock-wrapper-swallows-ninja-exit-code (the two-space `[  FAILED  ]` count trap),
 * taskstop-leaves-bash-loop-children-running.
 */
export function analyzeTests(text: string): Finding[] {
  const findings: Finding[] = []
  const filter = GTEST_FILTER.exec(text)?.[1]?.trim()

  if (CTEST_NONE.test(text)) {
    findings.push({
      id: 'ctest:none',
      severity: 'error',
      message:
        'ctest found NO tests for this selection: nothing ran. Check the -R regex against `ctest -N`, and that the test is registered.',
    })
  }

  const segments = gtestSegments(text)
  if (segments.length === 0) {
    return findings
  }

  let zeroRuns = 0
  const failed = new Set<string>()
  const incomplete: string[] = []
  let ran = 0
  let skipped = 0

  for (const segment of segments) {
    const header = /\[==========\] Running (\d+) tests?/.exec(segment)
    const declared = Number(header?.[1] ?? '0')
    if (declared === 0) {
      zeroRuns += 1
      continue
    }

    const runs = names(segment, GTEST_RUN)
    const oks = new Set(names(segment, GTEST_OK))
    const fails = names(segment, GTEST_FAILED)
    const skips = new Set(names(segment, GTEST_SKIPPED))
    for (const name of fails) {
      failed.add(name)
    }
    ran += runs.length
    skipped += [...skips].filter(name => runs.includes(name)).length

    if (!GTEST_DONE.test(segment)) {
      const last = runs[runs.length - 1]
      const settled = (name: string) => oks.has(name) || fails.includes(name) || skips.has(name)
      if (last !== undefined && !settled(last)) {
        incomplete.push(last)
      }
    }
  }

  if (zeroRuns > 0) {
    findings.push({
      id: `gtest:zero:${filter ?? ''}`,
      severity: 'error',
      message: [
        `The gtest filter${filter ? ` \`${filter}\`` : ''} selected NO tests: nothing ran, so nothing passed.`,
        'Check the suite name with `--gtest_list_tests` and the same filter (an evidence PNG name is not a suite name),',
        'and that a new test .cpp is listed in OloEngine/tests/CMakeLists.txt (it lists every source; an unlisted file is never built).',
      ].join(' '),
    })
  }

  if (incomplete.length > 0) {
    findings.push({
      id: `gtest:incomplete:${incomplete.join(',')}`,
      severity: 'error',
      message:
        `The log ends inside ${listed(incomplete)} with no result line and no final summary: the run is either still going or the process died there (crash, abort, timeout). ` +
        'Treat it as NOT passed. If you stopped it, check for a surviving OloEngine-Tests.exe / bash loop from THIS worktree before rebuilding.',
    })
  }

  const failedNames = [...failed]
  if (failedNames.length > 0) {
    findings.push({
      id: `gtest:failed:${failedNames.join(',')}`,
      severity: 'error',
      message: `${failedNames.length} test(s) FAILED: ${listed(failedNames)}.`,
    })
  }

  if (ran > 0 && skipped === ran) {
    findings.push({
      id: `gtest:all-skipped:${filter ?? ''}:${ran}`,
      severity: 'warn',
      message:
        `All ${ran} selected test(s) SKIPPED: nothing was verified. Evidence tests skip without a GL 4.6 context and Vulkan device tests without a device; say so rather than reporting them as passing.`,
    })
  }

  return findings
}

// ------------------------------------------------------------------ both ---

export function analyze(text: string): Finding[] {
  return [...analyzeBuild(text), ...analyzeTests(text)]
}

export function formatFindings(findings: readonly Finding[]): string {
  const body = findings.map(f => `- ${f.severity === 'error' ? 'ERROR' : 'WARNING'}: ${f.message}`).join('\n')
  return `olo-output-truth (a mod that reads build/test output for traps this repo has hit):\n${body}`
}

/** Pulls the output file and status out of a background task's notification text. */
export function parseTaskNotification(
  text: string,
): { outputFile: string; summary: string; isShellCommand: boolean } | undefined {
  if (!text.includes('<task-notification>')) {
    return undefined
  }
  const outputFile = /<output-file>([^<]+)<\/output-file>/.exec(text)?.[1]?.trim()
  if (!outputFile) {
    return undefined
  }
  const summary = /<summary>([\s\S]*?)<\/summary>/.exec(text)?.[1]?.trim() ?? ''
  // Only a shell command's log is worth reading: an agent's .output is its JSONL transcript.
  const isShellCommand = /^Background command\b/.test(summary)
  return { outputFile, summary, isShellCommand }
}
