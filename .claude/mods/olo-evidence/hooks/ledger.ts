// Pure core of the evidence ledger: recognising build and test invocations, reading their
// outcome from a log, and judging a branch's evidence against what a PR claims.

import type { EvidenceLedger, EvidenceOutcome, EvidenceRun, EvidenceRunKind, EvidenceVisual } from '../types'
import { commandWords, commentOf, exeName, innerCommand, splitStatements, type Statement } from './shell'

// ------------------------------------------------------------ invocations ---

export type Invocation = { kind: EvidenceRunKind; config: string; scope: string }

const CONFIGS = /^(?:Debug|Release|RelWithDebInfo|MinSizeRel|Dist)$/i

function cap(s: string): string {
  return s.length === 0 ? s : s[0]!.toUpperCase() + s.slice(1).toLowerCase()
}

/** The word after `flag`, or the value of `flag=value`. */
function flagValue(words: string[], ...flags: string[]): string | undefined {
  for (let i = 0; i < words.length; i += 1) {
    const w = words[i] ?? ''
    for (const f of flags) {
      if (w === f) {
        return words[i + 1]
      }
      if (w.startsWith(`${f}=`)) {
        return w.slice(f.length + 1)
      }
    }
  }
  return undefined
}

function config(value: string | undefined): string {
  return value !== undefined && CONFIGS.test(value) ? cap(value) : '?'
}

/**
 * Every build and test run a shell command starts (a `build && test` line is two). Only a
 * statement whose COMMAND is the build or test counts: `rg OloEngine-Tests`, `git log --grep
 * ctest` or a PR body naming the exe start nothing, and `--target OloEngine-Tests` is a target.
 * A shell wrapper (`pwsh -Command`, build-lock.ps1's form, `bash -c`, `cmd /c`) is looked into.
 */
export function parseInvocations(command: string): Invocation[] {
  const out: Invocation[] = []
  for (const statement of splitStatements(command)) {
    const words = commandWords(statement.words)
    const name = exeName(words[0])
    const inner = innerCommand(words)

    if (inner !== undefined) {
      out.push(...parseInvocations(inner))
    } else if (name === 'cmake' && words[1] === '--build') {
      const tree = (words[2] ?? '').replace(/\\/g, '/').replace(/\/$/, '').split('/').pop() ?? ''
      const at = words.indexOf('--target')
      const targets: string[] = []
      for (let i = at + 1; at >= 0 && i < words.length && !(words[i] ?? '-').startsWith('-'); i += 1) {
        targets.push(words[i] ?? '')
      }
      out.push({ kind: 'build', config: config(flagValue(words, '--config')), scope: `${tree}${targets.length > 0 ? ` ${targets.join(' ')}` : ''}` })
    } else if (name === 'oloengine-tests') {
      if (words.includes('--gtest_list_tests')) {
        continue
      }
      // A path segment names the config (build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe).
      const fromPath = /[\\/](Debug|Release|RelWithDebInfo|MinSizeRel|Dist)[\\/][^\\/]*$/i.exec(words[0] ?? '')?.[1]
      out.push({ kind: 'test', config: config(fromPath), scope: flagValue(words, '--gtest_filter') ?? '(all)' })
    } else if (name === 'ctest') {
      if (words.includes('-N') || words.includes('--show-only')) {
        continue
      }
      const regex = flagValue(words, '-R', '--tests-regex')
      out.push({ kind: 'test', config: config(flagValue(words, '-C', '--build-config')), scope: regex ? `ctest -R ${regex}` : 'ctest (all)' })
    }
  }
  return out
}

/** Where a command sends its output: the last file redirect or tee target. */
export function redirectTarget(command: string): string | null {
  const targets: string[] = []
  for (const m of command.matchAll(/(?:^|[^0-9&>])(?:\*|[12])?>>?\s*("?)([^\s"|;&<>]+)\1/g)) {
    targets.push(m[2] ?? '')
  }
  for (const m of command.matchAll(/\|\s*(?:tee(?:\s+-a)?|Tee-Object(?:\s+-(?:FilePath|Append))*|Out-File(?:\s+-(?:FilePath|Append|Encoding\s+\S+))*)\s+("?)([^\s"|;&<>]+)\1/gi)) {
    targets.push(m[2] ?? '')
  }
  const real = targets.filter(t => t.length > 0 && !/^(?:&\d|\/dev\/null|\$null|nul)$/i.test(t))
  return real.length === 0 ? null : real[real.length - 1]!
}

/** A backgrounded shell command's output file, from the tool's own reply. */
export function backgroundOutputPath(toolText: string): string | null {
  return /Output is being written to:\s*(\S+\.output)/.exec(toolText)?.[1] ?? null
}

// ---------------------------------------------------------------- outcome ---

const NINJA_STOPPED = /^ninja: build stopped: (.*)$/m
const NINJA_FAILED = /^FAILED: /m
const MSBUILD_FAILED = /^\s*Build FAILED\.\s*$|^\s*[1-9]\d* Error\(s\)\s*$/m
const BUILD_DONE = /^\[build-lock\] released|^ninja: no work to do\.|^\s*Build succeeded\.|^\[(\d+)\/\1\] Linking/m
const GTEST_HEADER = /^\[==========\] Running (\d+) tests? from \d+ test suites?\./gm
const GTEST_DONE = /^\[==========\] \d+ tests? from \d+ test suites? ran\./m

function unique(items: string[]): string[] {
  return [...new Set(items)]
}

export function buildOutcome(log: string): EvidenceOutcome {
  const stopped = NINJA_STOPPED.exec(log)
  if (stopped || NINJA_FAILED.test(log) || MSBUILD_FAILED.test(log)) {
    const first = log.split('\n').find(l => /: (?:fatal )?error[: ]|error (?:C|LNK)\d{4}/.test(l))?.trim()
    return { state: 'failed', detail: first ? `failed: ${first.slice(0, 160)}` : `failed${stopped ? `: ${stopped[1]}` : ''}`, failed: [], ran: 0, skipped: 0 }
  }
  if (BUILD_DONE.test(log)) {
    return { state: 'passed', detail: 'built', failed: [], ran: 0, skipped: 0 }
  }
  return { state: /\[build-lock\] (?:acquired|waiting|queued)|^\[\d+\/\d+\]/m.test(log) ? 'running' : 'unknown', detail: 'no result in the log yet', failed: [], ran: 0, skipped: 0 }
}

const CTEST_SUMMARY = /^\s*(\d+)% tests passed, (\d+) tests? failed out of (\d+)/m

/** ctest's own summary, which it prints without -V where no gtest header appears. */
function ctestOutcome(log: string): EvidenceOutcome | undefined {
  const m = CTEST_SUMMARY.exec(log)
  if (m === null) {
    return undefined
  }
  const failedCount = Number(m[2])
  const total = Number(m[3])
  const list = log.slice(log.indexOf('The following tests FAILED:'))
  const names = failedCount > 0 ? unique([...list.matchAll(/^\s*\d+ - (\S+) \(/gm)].map(x => x[1] ?? '')) : []
  if (total === 0) {
    return { state: 'failed', detail: 'ctest ran no tests', failed: [], ran: 0, skipped: 0 }
  }
  return failedCount > 0
    ? { state: 'failed', detail: `${failedCount} failed of ${total} (ctest)`, failed: names, ran: total, skipped: 0 }
    : { state: 'passed', detail: `${total} passed (ctest)`, failed: [], ran: total, skipped: 0 }
}

export function testOutcome(log: string): EvidenceOutcome {
  if (/^No tests were found!!!\s*$/m.test(log)) {
    return { state: 'failed', detail: 'ctest found no tests', failed: [], ran: 0, skipped: 0 }
  }
  const ctest = ctestOutcome(log)
  if (ctest !== undefined) {
    return ctest
  }
  const starts = [...log.matchAll(GTEST_HEADER)]
  if (starts.length === 0) {
    return { state: 'unknown', detail: 'no gtest output in the log', failed: [], ran: 0, skipped: 0 }
  }
  let ran = 0
  let skipped = 0
  let zero = 0
  let open = false
  const failed: string[] = []
  starts.forEach((m, i) => {
    const seg = log.slice(m.index ?? 0, starts[i + 1]?.index ?? log.length)
    if (Number(m[1]) === 0) {
      zero += 1
      return
    }
    const runs = unique([...seg.matchAll(/^\[ RUN      \] (\S+)/gm)].map(x => x[1] ?? ''))
    ran += runs.length
    skipped += unique([...seg.matchAll(/^\[  SKIPPED \] (\S+)/gm)].map(x => x[1] ?? '')).filter(n => runs.includes(n)).length
    failed.push(...[...seg.matchAll(/^\[  FAILED  \] ([^\s,]+\.[^\s,]+)/gm)].map(x => x[1] ?? ''))
    if (!GTEST_DONE.test(seg)) {
      open = true
    }
  })
  const names = unique(failed)
  if (zero > 0 && ran === 0) {
    return { state: 'failed', detail: 'the filter selected no tests', failed: [], ran: 0, skipped: 0 }
  }
  if (names.length > 0) {
    return { state: 'failed', detail: `${names.length} failed of ${ran}`, failed: names, ran, skipped }
  }
  if (open) {
    return { state: 'running', detail: `ended inside a test after ${ran} (still running, or it died)`, failed: [], ran, skipped }
  }
  if (ran > 0 && skipped === ran) {
    return { state: 'failed', detail: `all ${ran} skipped: nothing verified`, failed: [], ran, skipped }
  }
  return { state: 'passed', detail: `${ran - skipped} passed${skipped > 0 ? `, ${skipped} skipped` : ''}`, failed: [], ran, skipped }
}

/** NULs (the engine logger), CRs and ANSI colour sequences (gtest with --gtest_color) out. */
export function cleanLog(text: string): string {
  return text
    .replace(/\u0000/g, '')
    .replace(/\u001b\[[0-9;]*[A-Za-z]/g, '')
    .replace(/\r\n?/g, '\n')
}

export function outcomeOf(kind: EvidenceRunKind, log: string): EvidenceOutcome {
  const clean = cleanLog(log)
  return kind === 'build' ? buildOutcome(clean) : testOutcome(clean)
}

/** Does this output carry a result at all (so it is worth keeping as the captured outcome)? */
export function carriesResult(kind: EvidenceRunKind, text: string): boolean {
  return outcomeOf(kind, text).state !== 'unknown'
}

// ---------------------------------------------------------------- visuals ---

// No leading \b: an MCP tool name puts `__` (word characters) right before `olo_`.
const LIVE_CAPTURE = /olo_(?:screenshot|render_capture_target|render_probe_pixel|probe_pixel|render_frame_breakdown)\b/
const LIVE_CHECK = /olo_(?:shader_errors|log_tail)\b/

/** What a tool call contributes to the visual-evidence record, if anything. */
export function visualFrom(tool: string, input: { file_path?: string; command?: string }): EvidenceVisual['kind'] | null {
  if (tool === 'Read' && typeof input.file_path === 'string' && /\.png$/i.test(input.file_path)) {
    return 'png-viewed'
  }
  const text = `${tool} ${input.command ?? ''}`
  if (LIVE_CAPTURE.test(text)) {
    return 'live-capture'
  }
  if (LIVE_CHECK.test(text)) {
    return 'live-check'
  }
  return null
}

// --------------------------------------------------------------- judgement ---

export type Judged = { run: EvidenceRun; outcome: EvidenceOutcome }

/** The outcome a run stands at: its log when that can be read, else what was captured. */
export function judge(run: EvidenceRun, log: string | undefined): Judged {
  const fromLog = log === undefined ? undefined : outcomeOf(run.kind, log)
  const outcome =
    fromLog !== undefined && fromLog.state !== 'unknown'
      ? fromLog
      : run.captured ?? { state: 'unknown' as const, detail: log === undefined ? 'no output recorded' : 'no result in its output', failed: [], ran: 0, skipped: 0 }
  return { run, outcome }
}

export function runKey(run: EvidenceRun): string {
  return `${run.kind}|${run.config}|${run.scope}`
}

/** The latest run of each distinct (kind, config, scope): earlier ones are superseded. */
export function latestPerKey(judged: readonly Judged[]): Judged[] {
  const latest = new Map<string, Judged>()
  for (const j of [...judged].sort((a, b) => a.run.at - b.run.at)) {
    latest.set(runKey(j.run), j)
  }
  return [...latest.values()]
}

const RENDER_PATH = /^(?:OloEngine\/src\/OloEngine\/(?:Renderer|RHI)\/|OloEngine\/src\/Platform\/(?:OpenGL|Vulkan)\/|OloEditor\/assets\/shaders\/)|\.(?:glsl|vert|frag|comp|geom|tesc|tese|mesh|task|rgen|rchit|rahit|rmiss)$/
const CODE_PATH = /\.(?:cpp|h|hpp|inl|cs|glsl|vert|frag|comp|cmake)$|CMakeLists\.txt$/
const SAYS_NO_FRAME = /could ?not inspect a frame|cannot inspect a frame|no frame could be inspected|not visually verified|could not run the editor/i

export const LEDGER_MARKER = 'OLO_LEDGER_INCOMPLETE'
export const APPROVAL_MARKER = 'OLO_USER_APPROVED'

export type PrProblem = { rule: string; message: string; waivedBy: string[] }

/**
 * What stands between this branch's evidence and the PR. `body` is the PR text ('' when
 * unknown), `changed` the files the branch changes against master.
 */
export function prProblems(judged: readonly Judged[], visuals: readonly EvidenceVisual[], changed: readonly string[], body: string): PrProblem[] {
  const problems: PrProblem[] = []
  const latest = latestPerKey(judged)
  const codeChanged = changed.some(f => CODE_PATH.test(f))

  // A run counts as evidence only once its outcome is known: one whose log is gone, or that
  // printed no result, proves nothing either way.
  const known = judged.filter(j => j.outcome.state !== 'unknown')
  if (codeChanged && known.length === 0) {
    problems.push({
      rule: 'no-runs',
      message:
        judged.length === 0
          ? 'The branch changes code, and no local build or test run is recorded for it. Build and run the relevant tests first (CLAUDE.md, Definition of done).'
          : 'The branch changes code, and no recorded build or test run has a known outcome (their logs are gone or printed no result). Run them again so the result is read.',
      waivedBy: [LEDGER_MARKER, APPROVAL_MARKER],
    })
  }

  for (const { run, outcome } of latest) {
    if (outcome.state === 'unknown' && known.length > 0) {
      problems.push({
        rule: 'unproven-run',
        message: `The latest ${run.kind} ${run.config} ${run.scope} has no known outcome (${outcome.detail}). Re-run it, or say in the PR body where its result is.`,
        waivedBy: [LEDGER_MARKER, APPROVAL_MARKER],
      })
    }
    if (outcome.state === 'running') {
      problems.push({
        rule: 'still-running',
        message: `${run.kind} ${run.config} ${run.scope} has not finished (${outcome.detail}). Push and open the PR only after every local check has finished.`,
        waivedBy: [APPROVAL_MARKER],
      })
    }
    if (outcome.state === 'failed') {
      const unexplained = outcome.failed.filter(name => !body.includes(name))
      const explained = outcome.failed.length > 0 && unexplained.length === 0
      if (!explained) {
        problems.push({
          rule: 'failed-run',
          message:
            `The latest ${run.kind} ${run.config} ${run.scope} ${outcome.detail}` +
            `${unexplained.length > 0 ? ` (${unexplained.slice(0, 6).join(', ')})` : ''}. Re-run it green, or, if a failure is pre-existing and unrelated, name each failing test in the PR body with its evidence.`,
          waivedBy: [APPROVAL_MARKER],
        })
      }
    }
  }

  for (const config of ['Debug', 'Release'] as const) {
    if (claims(body, config) && !latest.some(j => j.run.config === config && j.outcome.state === 'passed')) {
      problems.push({
        rule: `claims-${config.toLowerCase()}`,
        message: `The PR body claims a ${config} result, but no ${config} build or test run passed in the record. A claim needs its run (memory: no-pr-before-local-checks-finish).`,
        waivedBy: [LEDGER_MARKER, APPROVAL_MARKER],
      })
    }
  }

  if (changed.some(f => RENDER_PATH.test(f)) && !SAYS_NO_FRAME.test(body)) {
    const missing = [
      visuals.some(v => v.kind === 'png-viewed') ? '' : 'no evidence PNG was looked at (Read it)',
      visuals.some(v => v.kind === 'live-capture') ? '' : 'no live editor capture (olo_screenshot / olo_render_capture_target)',
    ].filter(Boolean)
    if (missing.length > 0) {
      problems.push({
        rule: 'visual-evidence',
        message:
          `The branch changes renderer or shader code, and ${missing.join(' and ')}. CLAUDE.md: a rendering change is not done on unit tests alone. ` +
          'If you cannot inspect a frame, say so in the PR body ("could not inspect a frame").',
        waivedBy: [APPROVAL_MARKER],
      })
    }
  }
  return problems
}

// A claim is a line that names the configuration AND a verdict; naming Debug in prose
// ("a Debug-only failure would cost a CI round") claims nothing.
const VERDICT = /\b(?:pass(?:ed|es|ing)?|green|verified|ran|built|succeeded)\b|✅|\b0 fail/i

export function claims(body: string, config: 'Debug' | 'Release'): boolean {
  const named = new RegExp(`\\b${config}\\b`)
  return body.split('\n').some(line => named.test(line) && VERDICT.test(line))
}

/**
 * The problems no marker waives. `markers` is the trailing comment of the `gh pr` statement
 * (see prStatement): a marker inside a quoted PR body or another statement is data.
 */
export function unwaived(problems: readonly PrProblem[], markers: string): PrProblem[] {
  return problems.filter(p => !p.waivedBy.some(marker => markers.includes(marker)))
}

/**
 * The `gh pr create` (or `gh pr edit` that sets the body) statement of a command, if any:
 * through prefixes (`GH_TOKEN=x`, `env`) and shell wrappers (`pwsh -Command`, `bash -c`,
 * `cmd /c`). Its words are the gh command's own; its comment is the gh statement's plus the
 * wrapper's, so a marker on either counts and a quoted one on neither.
 */
export function prStatement(command: string): Statement | undefined {
  for (const s of splitStatements(command)) {
    const words = commandWords(s.words)
    const inner = innerCommand(words)
    if (inner !== undefined) {
      const found = prStatement(inner)
      if (found !== undefined) {
        return { ...found, text: `${found.text} # ${commentOf(s.text)}` }
      }
      continue
    }
    if (exeName(words[0]) !== 'gh' || words[1] !== 'pr') {
      continue
    }
    if (words[2] === 'create' || (words[2] === 'edit' && words.some(x => /^(?:--body|--body-file|-b|-F)(?:=|$)/.test(x)))) {
      return { ...s, words }
    }
  }
  return undefined
}

/** Where that statement's PR body comes from: a file, inline text, or neither. */
export function prBodySource(statement: Statement): { file?: string; inline?: string } {
  const file = flagValue(statement.words, '--body-file', '-F')
  if (file !== undefined) {
    return { file }
  }
  const inline = flagValue(statement.words, '--body', '-b')
  return inline !== undefined ? { inline } : {}
}

export function markersOf(statement: Statement): string {
  return commentOf(statement.text)
}

// ---------------------------------------------------------------- display ---

function ago(at: number, now: number): string {
  const min = Math.max(0, Math.round((now - at) / 60_000))
  return min < 60 ? `${min} min ago` : `${Math.round(min / 60)} h ago`
}

const MARK: Record<EvidenceOutcome['state'], string> = { passed: 'PASS', failed: 'FAIL', running: 'RUNNING', unknown: '?' }

export function summaryMarkdown(judged: readonly Judged[], visuals: readonly EvidenceVisual[], problems: readonly PrProblem[], branch: string, now: number): string {
  const lines = [`**Evidence for \`${branch}\`**`, '']
  const latest = new Set(latestPerKey(judged))
  const recent = [...judged].sort((a, b) => b.run.at - a.run.at).slice(0, 25)
  if (recent.length === 0) {
    lines.push('- no build or test run recorded yet')
  }
  for (const j of recent) {
    const superseded = latest.has(j) ? '' : ' (superseded)'
    lines.push(`- ${MARK[j.outcome.state]} ${j.run.kind} ${j.run.config} ${j.run.scope}: ${j.outcome.detail}, ${ago(j.run.at, now)}${superseded}`)
  }
  const count = (k: EvidenceVisual['kind']) => visuals.filter(v => v.kind === k).length
  lines.push('', `**Visual**: ${count('png-viewed')} PNG(s) looked at, ${count('live-capture')} live capture(s), ${count('live-check')} live log/shader check(s)`, '')
  lines.push(problems.length === 0 ? '**Ready for a PR** as far as the record shows.' : '**Before a PR:**')
  for (const p of problems) {
    lines.push(`- ${p.message}`)
  }
  return lines.join('\n')
}

export function emptyLedger(): EvidenceLedger {
  return { runs: [], visuals: [] }
}
