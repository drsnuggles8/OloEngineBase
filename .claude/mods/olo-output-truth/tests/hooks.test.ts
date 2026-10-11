import { expect, mock, test } from 'claude-code/testing'

// The test's own tool.call hook sits beneath the plugin and plays the engine: it
// answers each call with the text a real run would have printed.
const ZERO = [
  'Note: Google Test filter = NoSuchSuite.*',
  '[==========] Running 0 tests from 0 test suites.',
  '[==========] 0 tests from 0 test suites ran. (0 ms total)',
  '[  PASSED  ] 0 tests.',
].join('\n')

test('a Bash run whose filter matched nothing gets a context note', async ($, on) => {
  mock.clock(on, { now: 1_000 })
  on('tool.call', () => ({ result: { stdout: ZERO, stderr: '', interrupted: false }, text: ZERO }))
  const ran = await $.tool.call({ tool: 'Bash', command: 'OloEngine-Tests.exe --gtest_filter=NoSuchSuite.*' })
  expect(ran.deny).toBeUndefined()
  expect(ran.context?.join('\n')).toContain('selected NO tests')
})

test('a clean run is passed through untouched', async ($, on) => {
  mock.clock(on, { now: 1_000 })
  on('tool.call', () => ({ result: { stdout: 'ok', stderr: '', interrupted: false }, text: 'ok' }))
  const ran = await $.tool.call({ tool: 'Bash', command: 'echo ok' })
  expect(ran.context).toBeUndefined()
})

test('the same finding is reported once per window, then again after it', async ($, on) => {
  const clock = mock.clock(on, { now: 5_000 })
  on('tool.call', () => ({ result: { stdout: ZERO, stderr: '', interrupted: false }, text: ZERO.replace('NoSuchSuite', 'Again') }))
  const first = await $.tool.call({ tool: 'Bash', command: 'run' })
  const second = await $.tool.call({ tool: 'Bash', command: 'run' })
  await clock.advance(11 * 60 * 1000)
  const third = await $.tool.call({ tool: 'Bash', command: 'run' })
  expect(first.context).toBeDefined()
  expect(second.context).toBeUndefined()
  expect(third.context).toBeDefined()
})
