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
