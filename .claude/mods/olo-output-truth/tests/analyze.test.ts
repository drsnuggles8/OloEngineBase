import { expect, test } from 'claude-code/testing'

import { analyze, analyzeBuild, analyzeTests, normalize, parseTaskNotification } from '../hooks/analyze'

// The shape build-lock.ps1 left in a log when ninja failed (memory:
// build-lock-wrapper-swallows-ninja-exit-code).
const FAILED_BUILD = [
  '[build-lock] acquired slot 0 (pid=4242)',
  '[812/2055] Building CXX object OloEngine/CMakeFiles/OloEngine.dir/src/OloEngine/Renderer/Foo.cpp.obj',
  'FAILED: OloEngine/CMakeFiles/OloEngine.dir/src/OloEngine/Renderer/Foo.cpp.obj',
  'C:/repos/x/OloEngine/src/OloEngine/Renderer/Foo.cpp(42,5): error: use of undeclared identifier \'Bar\'',
  '1 error generated.',
  'ninja: build stopped: subcommand failed.',
  '[build-lock] released (pid=4242)',
  '',
  '[exited with code 0]',
].join('\n')

const CLEAN_BUILD = [
  '[build-lock] acquired slot 0 (pid=4242)',
  '[2055/2055] Linking CXX executable OloEngine\\tests\\OloEngine-Tests.exe',
  '[build-lock] released (pid=4242)',
].join('\n')

const GTEST_PASS = [
  'Note: Google Test filter = MotionVectorMath.*',
  '[==========] Running 2 tests from 1 test suite.',
  '[ RUN      ] MotionVectorMath.A',
  '[       OK ] MotionVectorMath.A (1 ms)',
  '[ RUN      ] MotionVectorMath.B',
  '[       OK ] MotionVectorMath.B (0 ms)',
  '[==========] 2 tests from 1 test suite ran. (2 ms total)',
  '[  PASSED  ] 2 tests.',
].join('\n')

const GTEST_ZERO = [
  'Note: Google Test filter = *SubmeshMaterialParity*',
  '[==========] Running 0 tests from 0 test suites.',
  '[==========] 0 tests from 0 test suites ran. (0 ms total)',
  '[  PASSED  ] 0 tests.',
].join('\n')

const GTEST_FAIL = [
  '[==========] Running 3 tests from 1 test suite.',
  '[ RUN      ] Suite.One',
  '[       OK ] Suite.One (1 ms)',
  '[ RUN      ] Suite.Two',
  'x.cpp(10): error: Expected equality',
  '[  FAILED  ] Suite.Two (3 ms)',
  '[ RUN      ] Param/Suite.Three/0',
  '[  FAILED  ] Param/Suite.Three/0, where GetParam() = 4 (2 ms)',
  '[==========] 3 tests from 1 test suite ran. (9 ms total)',
  '[  PASSED  ] 1 test.',
  '[  FAILED  ] 2 tests, listed below:',
  '[  FAILED  ] Suite.Two',
  '[  FAILED  ] Param/Suite.Three/0, where GetParam() = 4',
].join('\n')

const GTEST_DIED = [
  '[==========] Running 100 tests from 9 test suites.',
  '[ RUN      ] Skin.A',
  '[       OK ] Skin.A (40 ms)',
  '[ RUN      ] TransparentBlendOrderVisualEvidence.Draws',
].join('\n')

const GTEST_ALL_SKIPPED = [
  '[==========] Running 2 tests from 1 test suite.',
  '[ RUN      ] WaterVisualEvidence.A',
  '[  SKIPPED ] WaterVisualEvidence.A (0 ms)',
  '[ RUN      ] WaterVisualEvidence.B',
  '[  SKIPPED ] WaterVisualEvidence.B (0 ms)',
  '[==========] 2 tests from 1 test suite ran. (0 ms total)',
  '[  PASSED  ] 0 tests.',
  '[  SKIPPED ] 2 tests, listed below:',
].join('\n')

test('a failed build behind the exit-0 wrapper is reported with its first error', () => {
  const [finding, ...rest] = analyzeBuild(FAILED_BUILD)
  expect(rest).toHaveLength(0)
  expect(finding?.severity).toBe('error')
  expect(finding?.message).toContain('BUILD FAILED')
  expect(finding?.message).toContain('exit code 0 reported for it is NOT evidence')
  expect(finding?.message).toContain("use of undeclared identifier 'Bar'")
})

test('a clean build and an ordinary grep result say nothing', () => {
  expect(analyze(CLEAN_BUILD)).toHaveLength(0)
  expect(analyze('3\n')).toHaveLength(0)
  expect(analyze('src/Foo.cpp:12: // an error: in a comment is not a build')).toHaveLength(0)
})

test('MSBuild failures are recognised', () => {
  const findings = analyzeBuild('  Foo.cpp(3): error C2065: x undeclared\nBuild FAILED.\n    1 Error(s)\n')
  expect(findings[0]?.message).toContain('error C2065')
})

test('a passing gtest run says nothing', () => {
  expect(analyzeTests(GTEST_PASS)).toHaveLength(0)
})

test('a filter that selects nothing is an error naming the filter', () => {
  const [finding] = analyzeTests(GTEST_ZERO)
  expect(finding?.severity).toBe('error')
  expect(finding?.message).toContain('*SubmeshMaterialParity*')
  expect(finding?.message).toContain('CMakeLists.txt')
})

test('failures are counted by test, the summary list not double-counted, params kept', () => {
  const failed = analyzeTests(GTEST_FAIL).find(f => f.id.startsWith('gtest:failed'))
  expect(failed?.message).toContain('2 test(s) FAILED')
  expect(failed?.message).toContain('Suite.Two')
  expect(failed?.message).toContain('Param/Suite.Three/0')
})

test('a log that ends inside a test is reported as not passed', () => {
  const died = analyzeTests(GTEST_DIED).find(f => f.id.startsWith('gtest:incomplete'))
  expect(died?.message).toContain('TransparentBlendOrderVisualEvidence.Draws')
})

test('an all-skipped run is a warning', () => {
  const skipped = analyzeTests(GTEST_ALL_SKIPPED)
  expect(skipped).toHaveLength(1)
  expect(skipped[0]?.severity).toBe('warn')
})

test('sharded runs are analysed per shard', () => {
  const findings = analyzeTests(`${GTEST_PASS}\n${GTEST_ZERO}`)
  expect(findings.map(f => f.id.split(':')[1])).toEqual(['zero'])
})

test('ctest with no matching tests is an error', () => {
  expect(analyzeTests('Test project C:/x\nNo tests were found!!!\n')[0]?.id).toBe('ctest:none')
})

test('a ctest summary with failures is an error naming them', () => {
  const log = '80% tests passed, 1 tests failed out of 5\n\nThe following tests FAILED:\n\t  3 - Water.Foam (Failed)\n'
  expect(analyzeTests(log)[0]?.message).toContain('Water.Foam')
  expect(analyzeTests('100% tests passed, 0 tests failed out of 5\n')).toEqual([])
})

test('a run that stops between tests, without its final summary, is not passed', () => {
  const stopped = ['[==========] Running 2 tests from 1 test suite.', '[ RUN      ] A.B', '[       OK ] A.B (1 ms)'].join('\n')
  const [finding] = analyzeTests(stopped)
  expect(finding?.id).toContain('gtest:unfinished')
  expect(finding?.message).toContain('1 of 2 declared')
})

test('ANSI colour sequences do not hide a failure', () => {
  const coloured = [
    '\u001b[0;32m[==========] \u001b[mRunning 1 test from 1 test suite.',
    '\u001b[0;32m[ RUN      ] \u001b[mA.B',
    '\u001b[0;31m[  FAILED  ] \u001b[mA.B (1 ms)',
    '\u001b[0;32m[==========] \u001b[m1 test from 1 test suite ran. (1 ms total)',
  ].join('\n')
  expect(analyzeTests(normalize(coloured)).map(f => f.id.split(':')[1])).toEqual(['failed'])
})

test('Read-tool listings and NUL-laden logs normalise to plain lines', () => {
  const listing = GTEST_ZERO.split('\n').map((l, i) => `${String(i + 1).padStart(6)}\t${l}`).join('\n')
  expect(analyzeTests(normalize(listing, true))[0]?.id).toContain('gtest:zero')
  expect(normalize('a\u0000b\r\nc')).toBe('ab\nc')
})

test('task notifications: shell commands are read, agents are not', () => {
  const shell = parseTaskNotification(
    '<task-notification><output-file>C:\\t\\b.output</output-file><status>completed</status><summary>Background command "Build" completed (exit code 0)</summary></task-notification>',
  )
  expect(shell).toEqual({ outputFile: 'C:\\t\\b.output', summary: 'Background command "Build" completed (exit code 0)', isShellCommand: true })
  const agent = parseTaskNotification(
    '<task-notification><output-file>C:\\t\\a.output</output-file><summary>Agent "Audit" finished</summary></task-notification>',
  )
  expect(agent?.isShellCommand).toBe(false)
  expect(parseTaskNotification('plain text')).toBeUndefined()
})
