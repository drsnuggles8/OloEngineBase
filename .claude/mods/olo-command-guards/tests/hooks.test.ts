import { expect, mock, test } from 'claude-code/testing'

// The test's tool.call hook plays the engine: any call that reaches it "ran".
const ran = { result: { stdout: 'ok', stderr: '', interrupted: false }, text: 'ok' }

test('a push to master is refused before it runs', async ($, on) => {
  let reached = false
  on('tool.call', () => {
    reached = true
    return ran
  })
  const r = await $.tool.call({ tool: 'Bash', command: 'git push origin master' })
  expect(reached).toBe(false)
  expect(r.isError === true || r.deny !== undefined).toBe(true)
  expect(String(r.deny ?? r.text)).toContain('Pushing to master is gated')
})

test('an approved gated action runs and is logged', async ($, on) => {
  mock.clock(on, { now: Date.UTC(2026, 9, 10) })
  const written: unknown[] = []
  // The test's own store: empty on read, recording each write.
  on('store.get', () => ({ value: undefined }))
  on('store.set', ($, e) => {
    written.push(e)
    return { value: undefined }
  })
  on('tool.call', () => ran)
  const r = await $.tool.call({ tool: 'Bash', command: 'gh issue close 1528 --reason completed # OLO_USER_APPROVED' })
  expect(r.deny).toBeUndefined()
  expect(r.text).toBe('ok')
  expect(JSON.stringify(written)).toContain('approved-gated-actions')
  expect(JSON.stringify(written)).toContain('issue-close')
})

test('an ordinary command passes straight through', async ($, on) => {
  on('tool.call', () => ran)
  const r = await $.tool.call({ tool: 'PowerShell', command: 'git status' })
  expect(r.text).toBe('ok')
})
