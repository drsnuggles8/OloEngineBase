import { expect, test } from 'claude-code/testing'

import type { EvidenceRun, EvidenceVisual } from '../types'
import {
  backgroundOutputPath,
  buildOutcome,
  claims,
  judge,
  latestPerKey,
  parseInvocations,
  prProblems,
  redirectTarget,
  summaryMarkdown,
  testOutcome,
  unwaived,
  visualFrom,
} from '../hooks/ledger'

const LOCKED_BUILD =
  "pwsh -NoProfile -File .claude/skills/run-oloengine/build-lock.ps1 -Command 'cmake --build build-cached --target OloEngine-Tests OloEditor --config Debug --parallel 6' > C:/t/build.log 2>&1"

test('invocations: a locked build, a test run, both on one line, ctest', () => {
  expect(parseInvocations(LOCKED_BUILD)).toEqual([{ kind: 'build', config: 'Debug', scope: 'build-cached OloEngine-Tests OloEditor' }])
  expect(parseInvocations('build-cached\\OloEngine\\tests\\Release\\OloEngine-Tests.exe --gtest_filter=Foliage*.* 2>&1 | tee t.log')).toEqual([
    { kind: 'test', config: 'Release', scope: 'Foliage*.*' },
  ])
  expect(parseInvocations('cmake --build build-cached --config Debug && build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe').map(i => i.kind)).toEqual(['build', 'test'])
  expect(parseInvocations('ctest --test-dir build -C Debug -R Water')).toEqual([{ kind: 'test', config: 'Debug', scope: 'ctest -R Water' }])
  expect(parseInvocations('OloEngine-Tests.exe --gtest_list_tests --gtest_filter=X.*')).toEqual([])
  expect(parseInvocations('ctest -N')).toEqual([])
  expect(parseInvocations('git log --oneline')).toEqual([])
})

test('redirects: the file a run writes to, not 2>&1 or /dev/null', () => {
  expect(redirectTarget(LOCKED_BUILD)).toBe('C:/t/build.log')
  expect(redirectTarget('x 2>&1 | tee -a C:/t/run.log')).toBe('C:/t/run.log')
  expect(redirectTarget('x | Tee-Object -FilePath out.txt')).toBe('out.txt')
  expect(redirectTarget('x > /dev/null 2>&1')).toBeNull()
  expect(redirectTarget('x')).toBeNull()
  expect(backgroundOutputPath('Command running in background with ID: b1. Output is being written to: C:\\T\\tasks\\b1.output')).toBe('C:\\T\\tasks\\b1.output')
})

test('build outcomes: failed, built, still going', () => {
  expect(buildOutcome("FAILED: x.obj\nFoo.cpp(3,1): error: nope\nninja: build stopped: subcommand failed.\n[build-lock] released (pid=1)").state).toBe('failed')
  expect(buildOutcome('[2055/2055] Linking CXX executable OloEngine-Tests.exe\n[build-lock] released (pid=1)').state).toBe('passed')
  expect(buildOutcome('ninja: no work to do.').state).toBe('passed')
  expect(buildOutcome('[build-lock] acquired (pid=1)\n[12/2055] Building CXX object a.obj').state).toBe('running')
})

test('test outcomes: pass, fail, zero, died, all skipped', () => {
  const head = (n: number) => `[==========] Running ${n} tests from 1 test suite.`
  expect(testOutcome(`${head(1)}\n[ RUN      ] A.B\n[       OK ] A.B (1 ms)\n[==========] 1 test from 1 test suite ran. (1 ms total)`).state).toBe('passed')
  expect(testOutcome(`${head(1)}\n[ RUN      ] A.B\n[  FAILED  ] A.B (1 ms)\n[==========] 1 test from 1 test suite ran. (1 ms total)`)).toEqual(
    expect.objectContaining({ state: 'failed', failed: ['A.B'] }),
  )
  expect(testOutcome('[==========] Running 0 tests from 0 test suites.\n[==========] 0 tests from 0 test suites ran.').detail).toContain('selected no tests')
  expect(testOutcome(`${head(5)}\n[ RUN      ] A.B`).state).toBe('running')
  expect(testOutcome(`${head(1)}\n[ RUN      ] A.B\n[  SKIPPED ] A.B (0 ms)\n[==========] 1 test from 1 test suite ran.`).detail).toContain('skipped')
})

test('visuals: PNGs read, live captures and log checks', () => {
  expect(visualFrom('Read', { file_path: 'C:/r/OloEditor/assets/tests/visual/Water_GL.png' })).toBe('png-viewed')
  expect(visualFrom('Bash', { command: 'curl -s localhost:7391/mcp -d \'{"name":"olo_screenshot"}\'' })).toBe('live-capture')
  expect(visualFrom('mcp__olo__olo_render_capture_target', {})).toBe('live-capture')
  expect(visualFrom('Bash', { command: 'call olo_shader_errors' })).toBe('live-check')
  expect(visualFrom('Read', { file_path: 'a.cpp' })).toBeNull()
})

const run = (over: Partial<EvidenceRun>): EvidenceRun => ({
  id: 'r',
  at: 1,
  kind: 'test',
  command: '',
  config: 'Debug',
  scope: 'A.*',
  logPath: null,
  captured: null,
  ...over,
})
const passed = { state: 'passed' as const, detail: '3 passed', failed: [], ran: 3, skipped: 0 }
const failedAB = { state: 'failed' as const, detail: '1 failed of 3', failed: ['A.B'], ran: 3, skipped: 0 }

test('a later run supersedes an earlier one with the same key', () => {
  const judged = [judge(run({ at: 1, captured: failedAB }), undefined), judge(run({ at: 2, captured: passed }), undefined)]
  expect(latestPerKey(judged).map(j => j.outcome.state)).toEqual(['passed'])
})

test('PR problems: no runs, failing runs, unbacked claims, missing visual evidence', () => {
  const none = prProblems([], [], ['OloEngine/src/OloEngine/Scene/Scene.cpp'], '')
  expect(none.map(p => p.rule)).toEqual(['no-runs'])

  const failing = [judge(run({ captured: failedAB }), undefined)]
  expect(prProblems(failing, [], ['a.cpp'], '').map(p => p.rule)).toEqual(['failed-run'])
  // Naming the failing test in the body (as pre-existing) clears it.
  expect(prProblems(failing, [], ['a.cpp'], 'A.B fails on master too (#1234)').map(p => p.rule)).toEqual([])

  const green = [judge(run({ captured: passed }), undefined)]
  expect(prProblems(green, [], ['a.cpp'], 'Verified on Debug and Release.').map(p => p.rule)).toEqual(['claims-release'])

  const shader = ['OloEditor/assets/shaders/include/PBRCommon.glsl']
  const views: EvidenceVisual[] = [{ at: 1, kind: 'png-viewed', what: 'x.png' }]
  expect(prProblems(green, views, shader, '').map(p => p.rule)).toEqual(['visual-evidence'])
  expect(prProblems(green, [...views, { at: 1, kind: 'live-capture', what: 'olo_screenshot' }], shader, '')).toEqual([])
  expect(prProblems(green, [], shader, 'I could not inspect a frame: no GPU in this session.')).toEqual([])
})

test('markers waive only what they cover', () => {
  const problems = prProblems([], [], ['OloEditor/assets/shaders/a.glsl'], 'Debug')
  expect(unwaived(problems, 'gh pr create OLO_LEDGER_INCOMPLETE').map(p => p.rule)).toEqual(['visual-evidence'])
  expect(unwaived(problems, 'gh pr create OLO_USER_APPROVED')).toEqual([])
})

test('the summary lists runs newest first, marks superseded ones, and states readiness', () => {
  const judged = [judge(run({ at: 1, captured: failedAB }), undefined), judge(run({ at: 2, captured: passed }), undefined)]
  const md = summaryMarkdown(judged, [], [], 'feature/x', 3 * 60_000)
  expect(md.indexOf('PASS')).toBeLessThan(md.indexOf('FAIL'))
  expect(md).toContain('(superseded)')
  expect(md).toContain('Ready for a PR')
})

test('a claim is a configuration and a verdict on one line, not a mention', () => {
  expect(claims('Full Debug suite: 7,170 tests, 0 failures, green.', 'Debug')).toBe(true)
  expect(claims('| Release | GL Forward | passed |', 'Release')).toBe(true)
  expect(claims('A PR body that claimed a Debug run that never happened.', 'Debug')).toBe(false)
  expect(claims('Debug-only failures reach master because CI never builds Debug.', 'Debug')).toBe(false)
  expect(claims('Release notes.\nTests passed.', 'Release')).toBe(false)
})
