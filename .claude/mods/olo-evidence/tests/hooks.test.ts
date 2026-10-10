import { expect, mock, test } from 'claude-code/testing'
import type { On } from 'claude-code'

// Beneath the plugin: git answers for a feature branch that changed a shader, a store in
// memory, and a tool layer that "runs" whatever it is asked.
function world(on: On, stdoutFor: (argv: string) => string) {
  const store = new Map<string, unknown>()
  on('store.get', ($, e) => ({ value: store.get(e.key) }))
  on('store.set', ($, e) => {
    store.set(e.key, e.value)
    return { value: undefined }
  })
  on('process.run', ($, e) => {
    const argv = e.argv.join(' ')
    const ok = (stdout: string) => ({ value: { exitCode: 0, stdout, stderr: '', isStdoutTruncated: false, isStderrTruncated: false } })
    if (argv.includes('--show-toplevel')) return ok('C:/repos/OloEngine-x\n')
    if (argv.includes('--abbrev-ref')) return ok('feature/x\n')
    if (argv.includes('diff --name-only')) return ok('OloEditor/assets/shaders/include/PBRCommon.glsl\n')
    return ok(stdoutFor(argv))
  })
  return store
}

const FAILED_TESTS = [
  '[==========] Running 2 tests from 1 test suite.',
  '[ RUN      ] Water.A',
  '[       OK ] Water.A (1 ms)',
  '[ RUN      ] Water.B',
  '[  FAILED  ] Water.B (1 ms)',
  '[==========] 2 tests from 1 test suite ran. (2 ms total)',
].join('\n')

test('a failing run is recorded and blocks gh pr create until a green re-run and a frame', async ($, on) => {
  mock.clock(on, { now: Date.UTC(2026, 9, 10, 12) })
  world(on, () => '')
  let output = FAILED_TESTS
  on('tool.call', () => ({ result: { stdout: output, stderr: '', interrupted: false }, text: output }))

  await $.tool.call({ tool: 'Bash', command: 'build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=Water.*' })
  const refused = await $.tool.call({ tool: 'Bash', command: 'gh pr create --title t --body "Debug run green."' })
  const why = String(refused.deny ?? refused.text)
  expect(why).toContain('olo-evidence refused')
  expect(why).toContain('Water.B')
  expect(why).toContain('renderer or shader code')

  output = FAILED_TESTS.replace('[  FAILED  ] Water.B (1 ms)', '[       OK ] Water.B (1 ms)')
  await $.tool.call({ tool: 'Bash', command: 'build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=Water.*' })
  await $.tool.call({ tool: 'Read', file_path: 'C:/repos/OloEngine-x/OloEditor/assets/tests/visual/Water_GL_Forward.png' })
  await $.tool.call({ tool: 'Bash', command: 'curl -s localhost:7391/mcp -d olo_screenshot' })

  output = 'https://github.com/o/r/pull/1'
  const allowed = await $.tool.call({ tool: 'Bash', command: 'gh pr create --title t --body "Debug run green."' })
  expect(allowed.deny).toBeUndefined()
  expect(allowed.text).toBe('https://github.com/o/r/pull/1')
})

test('ordinary commands are untouched and record nothing', async ($, on) => {
  mock.clock(on, { now: 1 })
  const store = world(on, () => '')
  on('tool.call', () => ({ result: { stdout: 'ok', stderr: '', interrupted: false }, text: 'ok' }))
  const r = await $.tool.call({ tool: 'Bash', command: 'git status' })
  expect(r.text).toBe('ok')
  expect(store.size).toBe(0)
})

test('a run that shares a log file with a later run keeps the outcome the log held for it', async ($, on) => {
  mock.clock(on, { now: Date.UTC(2026, 9, 10, 12) })
  world(on, () => '')
  // The log file both runs redirect into; the "tool" writes it when it runs.
  // Keyed the way the engine may hand a path on: either slash, any case.
  const key = (p: string) => p.replace(/\\/g, '/').toLowerCase()
  const files = new Map<string, string>()
  const seen: string[] = []
  on('fs.stat', ($, e) => {
    seen.push(e.path)
    const text = files.get(key(e.path))
    return text === undefined ? { deny: 'ENOENT' } : { value: { kind: 'file', size: text.length, mtimeMs: 0, isLink: false } }
  })
  on('fs.read', ($, e) => {
    const text = files.get(key(e.path))
    return text === undefined ? { deny: 'ENOENT' } : { value: text }
  })
  let nextLog = FAILED_TESTS
  on('tool.call', ($, e) => {
    const command = (e as { command?: string }).command ?? ''
    if (command.includes('> C:/t/run.log')) {
      files.set(key('C:/t/run.log'), nextLog)
    }
    return { result: { stdout: '', stderr: '', interrupted: false }, text: '' }
  })

  await $.tool.call({ tool: 'Bash', command: 'build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe --gtest_filter=Water.* > C:/t/run.log 2>&1' })
  nextLog = FAILED_TESTS.replace('[  FAILED  ] Water.B (1 ms)', '[       OK ] Water.B (1 ms)')
  await $.tool.call({ tool: 'Bash', command: 'build-cached/OloEngine/tests/Release/OloEngine-Tests.exe --gtest_filter=Water.* > C:/t/run.log 2>&1' })

  // The Debug run failed; the Release run's passing log must not stand in for it.
  const refused = await $.tool.call({ tool: 'Bash', command: 'gh pr create --title t --body "Debug run green." # OLO_LEDGER_INCOMPLETE' })
  const why = String(refused.deny ?? refused.text)
  expect(why).toContain('Debug')
  expect(why).toContain('Water.B')
})

test('a failure while recording never re-runs or fails the call it records', async ($, on) => {
  mock.clock(on, { now: 1 })
  on('store.get', () => ({ value: undefined }))
  on('store.set', () => {
    throw new Error('store unavailable')
  })
  on('process.run', ($, e) => {
    const argv = e.argv.join(' ')
    const out = argv.includes('--show-toplevel') ? 'C:/repos/x\n' : argv.includes('--abbrev-ref') ? 'feature/x\n' : ''
    return { value: { exitCode: 0, stdout: out, stderr: '', isStdoutTruncated: false, isStderrTruncated: false } }
  })
  let calls = 0
  on('tool.call', () => {
    calls += 1
    return { result: { stdout: 'ok', stderr: '', interrupted: false }, text: 'ok' }
  })
  const r = await $.tool.call({ tool: 'Bash', command: 'cmake --build build-cached --config Debug' })
  expect(r.text).toBe('ok')
  expect(calls).toBe(1)
})
