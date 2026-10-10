import { expect, test } from 'claude-code/testing'
import type { On } from 'claude-code'

const ROOT = 'C:/repos/OloEngine-foo'
const OTHER = 'C:/repos/OloEngine-bar'
const SHADER = `${ROOT}/OloEditor/assets/shaders/include/PBRCommon.glsl`
const GUIDE = `${ROOT}/docs/agent-rules/glsl-shaders.md`

// A small file system beneath the plugin: these paths exist, nothing else does (two
// checkouts, each with its .git). Paths are compared as the engine may hand them on:
// either slash, any case.
function files(on: On, existing: Record<string, string>) {
  const norm = (p: unknown) => String(p).replace(/\\/g, '/').toLowerCase()
  const withCheckouts = { [`${ROOT}/.git`]: 'gitdir', [`${OTHER}/.git`]: 'gitdir', ...existing }
  const table = new Map(Object.entries(withCheckouts).map(([k, v]) => [norm(k), v]))
  const seen: string[] = []
  on('fs.exists', ($, e) => {
    seen.push(`exists ${String(e.path)}`)
    return { value: table.has(norm(e.path)) }
  })
  on('fs.stat', ($, e) => {
    const text = table.get(norm(e.path))
    return text === undefined ? { deny: 'ENOENT' } : { value: { kind: 'file', size: text.length, mtimeMs: 0, isLink: false } }
  })
  on('fs.read', ($, e) => {
    const text = table.get(norm(e.path))
    return text === undefined ? { deny: 'ENOENT' } : { value: text }
  })
  return seen
}

const ok = { result: { success: true }, text: 'ok' }

test('a shader edit is refused until the GLSL guide is read, then allowed', async ($, on) => {
  files(on, { [SHADER]: 'void main() {}', [GUIDE]: '# GLSL' })
  on('tool.call', () => ok)

  const before = await $.tool.call({ tool: 'Edit', file_path: SHADER, old_string: 'a', new_string: 'b' })
  expect(String(before.deny ?? before.text)).toContain(`read ${GUIDE}`)

  await $.tool.call({ tool: 'Read', file_path: GUIDE })
  const after = await $.tool.call({ tool: 'Edit', file_path: SHADER, old_string: 'a', new_string: 'b' })
  expect(after.text).toBe('ok')
})

test("another worktree's copy of the guide does not open this worktree's gate", async ($, on) => {
  const otherGuide = `${OTHER}/docs/agent-rules/glsl-shaders.md`
  files(on, { [SHADER]: 'void main() {}', [GUIDE]: '# GLSL', [otherGuide]: '# GLSL' })
  on('tool.call', () => ok)
  await $.tool.call({ tool: 'Read', file_path: otherGuide })
  const r = await $.tool.call({ tool: 'Edit', file_path: SHADER, old_string: 'a', new_string: 'b' })
  expect(String(r.deny ?? r.text)).toContain(`${ROOT}/docs/agent-rules/glsl-shaders.md`)
})

test('a vendor edit spelled through .. is refused', async ($, on) => {
  files(on, {})
  on('tool.call', () => ok)
  const r = await $.tool.call({ tool: 'Edit', file_path: `${ROOT}/OloEngine/src/../vendor/imgui/imgui.cpp`, old_string: 'a', new_string: 'b' })
  expect(String(r.deny ?? r.text)).toContain('FetchContent')
})

test('no gate when the guide does not exist on this branch', async ($, on) => {
  files(on, { [SHADER]: 'void main() {}' })
  on('tool.call', () => ok)
  const r = await $.tool.call({ tool: 'Edit', file_path: SHADER, old_string: 'a', new_string: 'b' })
  expect(r.text).toBe('ok')
})

test('a generated file is refused whatever was read', async ($, on) => {
  const gen = `${ROOT}/OloEngine/src/OloEngine/Scene/Generated/AllComponents.Generated.inl`
  files(on, { [gen]: '// generated' })
  on('tool.call', () => ok)
  const r = await $.tool.call({ tool: 'Edit', file_path: gen, old_string: 'a', new_string: 'b' })
  expect(String(r.deny ?? r.text)).toContain('OloHeaderTool')
})

test('a new unlisted test source gets a note after it is written', async ($, on) => {
  const testFile = `${ROOT}/OloEngine/tests/Rendering/NewThingTest.cpp`
  files(on, {
    [`${ROOT}/docs/agent-rules/testing-architecture.md`]: '# tests',
    [`${ROOT}/OloEngine/tests/CMakeLists.txt`]: 'Rendering/OldThingTest.cpp',
  })
  on('tool.call', () => ok)
  await $.tool.call({ tool: 'Read', file_path: `${ROOT}/docs/agent-rules/testing-architecture.md` })
  const r = await $.tool.call({ tool: 'Write', file_path: testFile, content: '// OLO_TEST_LAYER: L3\nTEST(A, B) {}' })
  expect(r.context?.join('\n')).toContain('not listed in OloEngine/tests/CMakeLists.txt')
})
