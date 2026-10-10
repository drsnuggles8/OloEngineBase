import { expect, mock, test } from 'claude-code/testing'

import { matchTraps, TRAPS, type TrapContext } from '../hooks/traps'

const shell = (command: string, text: string, tool = 'Bash'): TrapContext => ({ tool, command, text, isError: false })
const ids = (c: TrapContext) => matchTraps(c).map(t => t.id)

// Each trap: one real-looking output that must match.
const POSITIVE: [string, TrapContext][] = [
  ['heredoc-backslash', shell("python - <<'PY'\nopen('a.cpp','w').write(s)\nPY", "<stdin>:3: SyntaxWarning: invalid escape sequence '\\.'")],
  ['newline-in-string-literal', shell('cmake --build build', "Foo.cpp(12): error C2001: newline in string literal")],
  ['edit-eperm-during-build', { tool: 'Edit', command: '', text: "EUNKNOWN: ftruncate (atomic write failed first: EPERM: rename 'Components.h.tmp.1')", isError: true }],
  ['mcp-port-excluded', shell('pwsh driver.ps1 -Action attach', '[MCP] Failed to bind 127.0.0.1:34527 — is the port already in use?')],
  ['unmerged-index', shell('git status --short', 'UU src/Merged.cpp\n M b.cpp\n')],
  ['worktree-add-powershell', shell('git -C $BASE worktree add "$WT" -b feature/x --no-track origin/master', 'fatal: invalid reference: origin/master', 'PowerShell')],
  ['gh-pr-checks-json', shell('gh pr checks 1577 --json name,bucket', 'unknown flag: --json')],
  ['no-jq', shell('gh pr view 1 --json x | jq .x', 'bash: jq: command not found')],
  ['gh-projects-classic', shell('gh issue view 1', 'GraphQL: Projects (classic) is being deprecated in favor of the new Projects experience')],
  ['gh-unknown-json-field', shell('gh issue view 1 --json stateReason', 'Unknown JSON field: "stateReason"')],
  ['worktree-dir-busy', shell('git worktree remove C:/repos/OloEngine-x', "error: failed to delete 'C:/repos/OloEngine-x': Permission denied")],
  ['editor-holds-link-output', shell('cmake --build build-cached', "lld-link: error: failed to write output 'C:\\repos\\x\\bin\\Debug\\OloEditor\\OloEditor.exe': permission denied")],
  ['llvmgold-noise', shell('cmake --preset linux', '/usr/bin/ld: /opt/llvm/lib/LLVMgold.so: cannot open shared object file')],
  ['far-near-macro', shell('cmake --build build', 'Test.cpp(40): error: expected unqualified-id\n    const f64 far = MeasureCell(a);')],
  ['tmp-path-mismatch', shell("python - <<'PY'\nopen('/tmp/x')\nPY", "FileNotFoundError: [Errno 2] No such file or directory: '/tmp/x'")],
]

test('every trap has a positive case, and each case matches its trap', () => {
  expect(POSITIVE.map(([id]) => id).sort()).toEqual(TRAPS.map(t => t.id).sort())
  for (const [id, c] of POSITIVE) {
    expect({ id, matched: ids(c).includes(id) }).toEqual({ id, matched: true })
  }
})

test('ordinary output matches nothing', () => {
  const ordinary: TrapContext[] = [
    shell('git status', 'On branch master\nnothing to commit, working tree clean'),
    shell('git status --short', ' M a.cpp\n?? b.cpp'),
    shell('cmake --build build-cached', '[2055/2055] Linking CXX executable OloEngine-Tests.exe'),
    shell("python - <<'PY'\nprint(1)\nPY", '1'),
    shell('gh pr view 1577 --json title', '{"title":"x"}'),
    { tool: 'Read', command: '', text: '   1\tconst f32 nearClip = 0.1f;', isError: false },
    shell('git worktree remove C:/repos/x', ''),
  ]
  for (const c of ordinary) {
    expect({ text: c.text, ids: ids(c) }).toEqual({ text: c.text, ids: [] })
  }
})

test('a merge still in progress is not the lost-marker trap', () => {
  expect(ids(shell('git status', 'You have unmerged paths.\n\nUU src/Merged.cpp'))).toEqual([])
})

test('the advice reaches the model once per window', async ($, on) => {
  const clock = mock.clock(on, { now: 1_000 })
  on('tool.call', () => ({ result: { stdout: '', stderr: '', interrupted: false }, text: 'bash: jq: command not found' }))
  const first = await $.tool.call({ tool: 'Bash', command: 'x | jq .' })
  const second = await $.tool.call({ tool: 'Bash', command: 'x | jq .' })
  await clock.advance(21 * 60 * 1000)
  const third = await $.tool.call({ tool: 'Bash', command: 'x | jq .' })
  expect(first.context?.join('')).toContain('memory: no-jq-on-this-box.md')
  expect(second.context).toBeUndefined()
  expect(third.context).toBeDefined()
})
