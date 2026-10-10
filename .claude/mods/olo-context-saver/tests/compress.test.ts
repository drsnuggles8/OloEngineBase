import { expect, mock, test } from 'claude-code/testing'

import { compressLog, MIN_CHARS } from '../hooks/compress'

const progress = (n: number) =>
  Array.from({ length: n }, (_, i) => `[${i + 1}/${n}] Building CXX object OloEngine/CMakeFiles/OloEngine.dir/src/OloEngine/Renderer/File${i}.cpp.obj`)

const passingTests = (n: number) =>
  Array.from({ length: n }, (_, i) => [`[ RUN      ] Suite.Case${i}`, `[       OK ] Suite.Case${i} (${i} ms)`]).flat()

test('short output is left alone', () => {
  expect(compressLog('[1/2] Building x\n[2/2] Linking y')).toBeUndefined()
})

test('ninja progress collapses, warnings between runs stay verbatim and in place', () => {
  const log = [...progress(150), 'C:/x/Foo.cpp(12,3): warning: unused variable \'a\' [-Wunused-variable]', ...progress(150), 'ninja: build stopped: subcommand failed.'].join('\n')
  expect(log.length).toBeGreaterThan(MIN_CHARS)
  const out = compressLog(log)
  expect(out?.progress).toBe(296)
  const lines = out?.text.split('\n') ?? []
  expect(lines).toContain("C:/x/Foo.cpp(12,3): warning: unused variable 'a' [-Wunused-variable]")
  expect(lines).toContain('ninja: build stopped: subcommand failed.')
  expect(lines.indexOf("C:/x/Foo.cpp(12,3): warning: unused variable 'a' [-Wunused-variable]")).toBe(3)
})

test('passing tests collapse; failures, their output and summaries stay', () => {
  const log = [
    'Note: Google Test filter = Suite.*',
    '[==========] Running 202 tests from 1 test suite.',
    '[----------] 202 tests from Suite',
    ...passingTests(100),
    '[ RUN      ] Suite.Broken',
    'x.cpp(10): error: Expected equality of these values',
    '[  FAILED  ] Suite.Broken (3 ms)',
    '[ RUN      ] Suite.Chatty',
    'some output the test printed',
    '[       OK ] Suite.Chatty (1 ms)',
    ...passingTests(100).map(l => l.replace('Case', 'Late')),
    '[----------] 202 tests from Suite (900 ms total)',
    '[==========] 202 tests from 1 test suite ran. (900 ms total)',
    '[  PASSED  ] 201 tests.',
    '[  FAILED  ] 1 test, listed below:',
    '[  FAILED  ] Suite.Broken',
  ].join('\n')
  const out = compressLog(log)
  expect(out?.passing).toBe(200)
  for (const kept of [
    '[==========] Running 202 tests from 1 test suite.',
    'x.cpp(10): error: Expected equality of these values',
    '[  FAILED  ] Suite.Broken (3 ms)',
    'some output the test printed',
    '[       OK ] Suite.Chatty (1 ms)',
    '[  PASSED  ] 201 tests.',
    '[  FAILED  ] Suite.Broken',
  ]) {
    expect(out?.text).toContain(kept)
  }
  expect(out?.text).not.toContain('Suite.Case5 ')
})

test('a Read listing with line numbers is recognised', () => {
  const log = progress(300).map((l, i) => `${String(i + 1).padStart(6)}\t${l}`).join('\n')
  expect(compressLog(log)?.progress).toBe(298)
})

test('a big log with nothing to collapse is left alone', () => {
  const log = Array.from({ length: 400 }, (_, i) => `C:/x/F${i}.cpp(1): warning: something worth reading number ${i}`).join('\n')
  expect(compressLog(log)).toBeUndefined()
})

test('the stored tool-result row is rewritten; other tools and doors are not', async ($, on) => {
  mock.session(on)
  const big = progress(400).join('\n')
  const row = (tool: string, door: 'tool-result' | 'note', uuid: string) =>
    $.session.append({
      door,
      origin: { kind: 'tool', tool },
      uuid,
      message: { type: 'user', role: 'user', content: [{ type: 'tool_result', tool_use_id: `t-${uuid}`, content: big }] },
    } as Parameters<typeof $.session.append>[0])

  const bash = JSON.stringify(await row('Bash', 'tool-result', 'a'))
  expect(bash).toContain('ninja progress lines')
  expect(bash.length).toBeLessThan(big.length / 4)

  const grep = JSON.stringify(await row('Grep', 'tool-result', 'b'))
  expect(grep).not.toContain('ninja progress lines')
})
