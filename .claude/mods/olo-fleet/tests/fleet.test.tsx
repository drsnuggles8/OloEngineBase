import { expect, mock, test } from 'claude-code/testing'

import type { FleetPr, FleetSnapshot } from '../types'
import { describeHolder, parseLockStatus, parsePrs, parseWorktrees, statusLine, summaryMarkdown, transitions } from '../hooks/fleet'

const PORCELAIN = [
  'worktree C:/repos/Base',
  'HEAD 76dcd996c262de638e3cfebb228e48b90648be54',
  'branch refs/heads/master',
  '',
  'worktree C:/repos/OloEngine-dogs',
  'HEAD 6407c0bef0000000000000000000000000000000',
  'branch refs/heads/feature/dogs',
  '',
  'worktree C:/repos/OloEngine-check',
  'HEAD 10bdac65b0000000000000000000000000000000',
  'detached',
  '',
].join('\n')

const PRS = JSON.stringify({
  data: {
    repository: {
      pullRequests: {
        nodes: [
          {
            number: 1577,
            title: 'Samoyed and Bernese',
            headRefName: 'feature/dogs',
            url: 'https://github.com/o/r/pull/1577',
            isDraft: false,
            mergeable: 'MERGEABLE',
            reviewThreads: { nodes: [{ isResolved: true }, { isResolved: false }] },
            commits: {
              nodes: [
                {
                  commit: {
                    statusCheckRollup: {
                      state: 'FAILURE',
                      contexts: {
                        nodes: [
                          { __typename: 'CheckRun', name: 'build', status: 'COMPLETED', conclusion: 'FAILURE' },
                          { __typename: 'CheckRun', name: 'asan', status: 'IN_PROGRESS', conclusion: null },
                          { __typename: 'StatusContext', context: 'sonar', state: 'SUCCESS' },
                        ],
                      },
                    },
                  },
                },
              ],
            },
          },
        ],
      },
    },
  },
})

const LOCK = JSON.stringify({
  slots: [
    {
      name: 'olo-build.lock',
      held: true,
      worktree: 'C:\\repos\\OloEngine-dogs',
      command: 'cmake --build C:/repos/OloEngine-dogs/build-cached --target OloEngine-Tests --config Release --parallel 6',
      acquired: '2026-10-10T09:00:00.0000000+00:00',
    },
    { name: 'olo-build.slot1.lock', held: false, worktree: null, command: null, acquired: null },
  ],
  queue: [
    { worktree: 'C:\\repos\\OloEngine-x', command: 'cmake --build build-cached --config Debug', enqueued: '2026-10-10T09:10:00+00:00', alive: true },
    { worktree: 'C:\\repos\\OloEngine-y', command: 'cmake --build build-cached', enqueued: '2026-10-10T08:00:00+00:00', alive: false },
  ],
})

const NOW = Date.parse('2026-10-10T09:30:00Z')

test('worktree porcelain: base first, branches and detached heads', () => {
  const list = parseWorktrees(PORCELAIN)
  expect(list.map(w => [w.name, w.branch, w.isBase])).toEqual([
    ['Base', 'master', true],
    ['OloEngine-dogs', 'feature/dogs', false],
    ['OloEngine-check', null, false],
  ])
})

test('PRs: rollup state, failing and running checks, unresolved threads', () => {
  const [pr] = parsePrs(PRS)
  expect(pr).toEqual(
    expect.objectContaining({ number: 1577, branch: 'feature/dogs', checks: 'FAILURE', failing: ['build'], running: 1, unresolved: 1 }),
  )
})

test('lock status: a lone slot object is a list of one', () => {
  expect(parseLockStatus(JSON.stringify({ slots: { name: 'a', held: false }, queue: [] })).slots).toHaveLength(1)
  const holder = parseLockStatus(LOCK).slots[0]
  expect(holder !== undefined && describeHolder(holder, NOW)).toBe('OloEngine-dogs Release, 30 min')
  const fanned = { ...holder!, command: 'cmake --build C:/repos/OloEngine-1259-head/build-cached --config Release' }
  expect(describeHolder(fanned, NOW)).toBe('OloEngine-dogs (OloEngine-1259-head) Release, 30 min')
})

test('transitions: only settled changes of a PR already seen', () => {
  const pr = (checks: FleetPr['checks']): FleetPr => ({ number: 1, title: 't', branch: 'b', url: '', isDraft: false, mergeable: 'MERGEABLE', checks, failing: [], running: 0, unresolved: 0 })
  expect(transitions(undefined, [pr('SUCCESS')])).toEqual([])
  expect(transitions([pr('PENDING')], [pr('PENDING')])).toEqual([])
  expect(transitions([pr('PENDING')], [pr('SUCCESS')]).map(t => t.to)).toEqual(['SUCCESS'])
  expect(transitions([pr('SUCCESS')], [pr('PENDING')])).toEqual([])
})

test('status line names this worktree PR, the lock and red PRs', () => {
  const snap: FleetSnapshot = { takenAt: NOW, base: 'C:/repos/Base', worktrees: [], prs: parsePrs(PRS), ...parseLockStatus(LOCK), errors: [] }
  expect(statusLine(snap, 'feature/dogs')).toBe('fleet: PR #1577 RED, 1 open thread(s) · build lock 1/2 held, 1 waiting · 1 red PR(s)')
  expect(statusLine(snap, 'master')).toBe('fleet: build lock 1/2 held, 1 waiting · 1 red PR(s)')
})

test('a push refreshes the fleet, and the pane draws it on every surface', async ($, on) => {
  const clock = mock.clock(on, { now: NOW })
  const answer = (stdout: string) => ({ value: { exitCode: 0, stdout, stderr: '', isStdoutTruncated: false, isStderrTruncated: false } })
  on('process.run', ($, e) => {
    const argv = e.argv.join(' ')
    if (argv.includes('--git-common-dir')) return answer('C:/repos/Base/.git\n')
    if (argv.includes('worktree list')) return answer(PORCELAIN)
    if (argv.includes('remote get-url')) return answer('https://github.com/o/r.git\n')
    if (argv.includes('--abbrev-ref')) return answer('feature/dogs\n')
    if (argv.includes('lock-status.ps1')) return answer(LOCK)
    if (argv.includes('status --porcelain')) return answer(argv.includes('dogs') ? ' M a.cpp\n?? b.cpp\n' : '')
    if (argv.startsWith('gh api graphql')) return answer(PRS)
    return { value: { exitCode: 1, stdout: '', stderr: 'unexpected', isStdoutTruncated: false, isStderrTruncated: false } }
  })
  on('tool.call', () => ({ result: { stdout: '', stderr: '', interrupted: false }, text: '' }))

  await $.tool.call({ tool: 'Bash', command: 'git push -u origin feature/dogs' })
  await clock.advance(61_000)

  for (const surface of ['terminal', 'desktop', 'vscode'] as const) {
    const ui = await $.ui.mount({
      plugin: 'olo-fleet',
      surface,
      component: 'Pane',
      requestId: 'olo-fleet',
      props: { title: 'OloEngine fleet', isFocused: false, bodyColumns: 160, placement: 'dock', scroll: { offset: 0, bodyRows: 40 }, view: {} },
    })
    expect(await ui.find({ type: 'Text', text: /OloEngine-dogs.*2 changed.*PR #1577: CI RED/ })).toBeDefined()
    expect(await ui.find({ type: 'Text', text: /held by OloEngine-dogs/ })).toBeDefined()
    expect(await ui.find({ type: 'Text', text: /1 stale ticket/ })).toBeDefined()
    expect(await ui.find({ key: 'refresh' })).toBeDefined()
    await ui.unmount()
  }

  // Where no pane can be placed, /fleet answers in its own row, drawn as the same view.
  for (const surface of ['terminal', 'vscode'] as const) {
    const row = await $.ui.mount({
      plugin: 'olo-fleet',
      surface,
      component: 'CommandOutput',
      props: { command: 'fleet', args: '', text: '**Worktrees**', isErrored: false },
    })
    expect(await row.find({ type: 'Text', text: /PR #1577: CI RED/ })).toBeDefined()
    await row.unmount()
  }
})

test('the markdown summary lists worktrees, PRs without a worktree, the lock and the queue', () => {
  const snap: FleetSnapshot = {
    takenAt: NOW,
    base: 'C:/repos/Base',
    worktrees: parseWorktrees(PORCELAIN).map(w => ({ ...w, dirty: 0 })),
    prs: [...parsePrs(PRS), { ...parsePrs(PRS)[0]!, number: 99, branch: 'feature/elsewhere' }],
    ...parseLockStatus(LOCK),
    errors: [],
  }
  const md = summaryMarkdown(snap, NOW)
  expect(md).toContain('- OloEngine-dogs  [feature/dogs]  clean  PR #1577: CI RED')
  expect(md).toContain('(no worktree) PR #99 `feature/elsewhere`')
  expect(md).toContain('- olo-build.lock: held by OloEngine-dogs Release, 30 min')
  expect(md).toContain('- 1. OloEngine-x Debug, 20 min waiting')
  expect(md).toContain('1 stale ticket(s)')
})

test('a garbled GitHub reply still publishes the rest, and says so', async ($, on) => {
  const clock = mock.clock(on, { now: NOW })
  const answer = (stdout: string) => ({ value: { exitCode: 0, stdout, stderr: '', isStdoutTruncated: false, isStderrTruncated: false } })
  on('process.run', ($, e) => {
    const argv = e.argv.join(' ')
    if (argv.includes('--git-common-dir')) return answer('C:/repos/Base/.git\n')
    if (argv.includes('worktree list')) return answer(PORCELAIN)
    if (argv.includes('remote get-url')) return answer('https://github.com/o/r.git\n')
    if (argv.includes('--abbrev-ref')) return answer('feature/dogs\n')
    if (argv.includes('lock-status.ps1')) return answer(LOCK)
    if (argv.includes('status --porcelain')) return answer('')
    if (argv.startsWith('gh api graphql')) return answer('<html><body>Proxy authentication required</body></html>')
    return { value: { exitCode: 1, stdout: '', stderr: 'unexpected', isStdoutTruncated: false, isStderrTruncated: false } }
  })
  on('tool.call', () => ({ result: { stdout: '', stderr: '', interrupted: false }, text: '' }))

  await $.tool.call({ tool: 'Bash', command: 'git push -u origin feature/dogs' })
  await clock.advance(61_000)

  const ui = await $.ui.mount({
    plugin: 'olo-fleet',
    surface: 'vscode',
    component: 'Pane',
    requestId: 'olo-fleet',
    props: { title: 'OloEngine fleet', isFocused: false, bodyColumns: 200, placement: 'dock', scroll: { offset: 0, bodyRows: 40 }, view: {} },
  })
  expect(await ui.find({ type: 'Text', text: /held by OloEngine-dogs/ })).toBeDefined()
  expect(await ui.find({ type: 'Text', text: /not the expected JSON/ })).toBeDefined()
  await ui.unmount()
})
