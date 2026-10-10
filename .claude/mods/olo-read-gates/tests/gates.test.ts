import { expect, test } from 'claude-code/testing'

import { ancestors, DOCS, gateFor, guessRoot, newTestFileNotes, relativeTo, resolvePath } from '../hooks/gates'

const R = 'C:/repos/OloEngine-foo'
const at = (rel: string) => `C:\\repos\\OloEngine-foo\\${rel.replace(/\//g, '\\')}`
const gate = (rel: string, opts: { isNew: boolean; text?: string; incoming?: string }) => gateFor(at(rel), R, opts)

test('paths resolve their . and .. segments, keep their case, and know their ancestors', () => {
  expect(resolvePath('C:\\repos\\X\\OloEngine\\src\\..\\vendor\\a.cpp')).toBe('C:/repos/X/OloEngine/vendor/a.cpp')
  expect(resolvePath('C:/a/./b/../c')).toBe('C:/a/c')
  expect(relativeTo('C:/repos/X/OloEngine/src/../vendor/a.cpp', 'c:\\repos\\x')).toBe('oloengine/vendor/a.cpp')
  expect(relativeTo('C:/elsewhere/a.cpp', 'C:/repos/X')).toBeUndefined()
  expect(ancestors('C:/repos/X/docs/a.md')).toEqual(['C:/repos/X/docs', 'C:/repos/X', 'C:/repos'])
})

test('the fallback root is the directory above the first top-level directory', () => {
  expect(guessRoot(at('OloEngine/src/OloEngine/Scene/Scene.cpp'))).toBe(R)
  expect(guessRoot('C:/somewhere/else.txt')).toBeUndefined()
})

test('vendor, vcpkg and generated files are refused, also through .. and in a nested checkout', () => {
  expect(gate('OloEngine/vendor/imgui/imgui.cpp', { isNew: false }).kind).toBe('deny')
  expect(gate('OloEngine/src/../vendor/imgui/imgui.cpp', { isNew: false }).kind).toBe('deny')
  expect(gate('build-cached/vcpkg_installed/x64/include/a.h', { isNew: false }).kind).toBe('deny')
  expect(gate('OloEngine/src/OloEngine/Scene/Generated/AllComponents.Generated.inl', { isNew: false }).kind).toBe('deny')
  expect(gate('OloEngine-ScriptCore/src/OloEngine/Scene/Components.Generated.cs', { isNew: false }).kind).toBe('deny')
  expect(gate('docs/test-catalogue.renderer.md', { isNew: false }).kind).toBe('deny')
  // A worktree nested under .claude/worktrees/ is its own checkout, found by its .git (register).
  const nested = `${R}/.claude/worktrees/feature`
  expect(gateFor(`${nested}/OloEngine/vendor/x.cpp`, nested, { isNew: false }).kind).toBe('deny')
})

test('shaders need the GLSL guide', () => {
  expect(gate('OloEditor/assets/shaders/include/PBRCommon.glsl', { isNew: false })).toEqual(
    expect.objectContaining({ kind: 'requires-read', doc: DOCS.shaders }),
  )
  expect(gate('OloEditor/assets/shaders/compute/RayTracingProbe.comp', { isNew: true }).kind).toBe('requires-read')
})

test('a header declaring a component needs the codegen guide; other headers do not', () => {
  expect(gate('OloEngine/src/OloEngine/Animation/FootIKComponent.h', { isNew: false, text: '#pragma once\nstruct FootIKComponent\n{\n};\n' })).toEqual(
    expect.objectContaining({ kind: 'requires-read', doc: DOCS.components }),
  )
  expect(gate('OloEngine/src/OloEngine/Animation/FootIKComponent.h', { isNew: false, text: 'struct FootIKComponent;\nstruct Other {};' }).kind).toBe('none')
  expect(gate('OloEngine/src/OloEngine/Core/Base.h', { isNew: false, text: 'using u32 = unsigned;' }).kind).toBe('none')
  expect(gate('OloEngine/src/OloEngine/X/New.h', { isNew: true, incoming: 'struct WindComponent { f32 S; };' }).kind).toBe('requires-read')
})

test('a NEW test source needs the testing guide; editing an existing one does not', () => {
  expect(gate('OloEngine/tests/Rendering/NewThingTest.cpp', { isNew: true })).toEqual(expect.objectContaining({ kind: 'requires-read', doc: DOCS.tests }))
  expect(gate('OloEngine/tests/Rendering/OldThingTest.cpp', { isNew: false }).kind).toBe('none')
})

test('ordinary engine and doc edits pass', () => {
  expect(gate('OloEngine/src/OloEngine/Renderer/Renderer3D.cpp', { isNew: false }).kind).toBe('none')
  expect(gate('docs/agent-rules/README.md', { isNew: false }).kind).toBe('none')
})

const NEW_TEST = at('OloEngine/tests/Rendering/NewThingTest.cpp')

test('a new test file: unlisted, unclassified and DISABLED_ are each reported', () => {
  const content = '#include <gtest/gtest.h>\nTEST(NewThing, DISABLED_Works) {}\n'
  const [note] = newTestFileNotes(NEW_TEST, R, content, 'set(SOURCES\n  Rendering/OldThingTest.cpp\n)', '{"file_layer_map":{}}')
  expect(note).toContain('not listed in OloEngine/tests/CMakeLists.txt')
  expect(note).toContain('OLO_TEST_LAYER')
  expect(note).toContain('DISABLED_')
})

test('an entry counts only as the whole source path or catalogue key, not a substring', () => {
  const unclassified = '#include <gtest/gtest.h>'
  const [note] = newTestFileNotes(NEW_TEST, R, unclassified, '\tRendering/OldNewThingTest.cpp\n', '{"file_layer_map":{"OloEngine/tests/Rendering/OldNewThingTest.cpp":"L3"}}')
  expect(note).toContain('not listed in OloEngine/tests/CMakeLists.txt')
  expect(note).toContain('no `// OLO_TEST_LAYER')
})

test('a listed, classified new test file is quiet', () => {
  const classified = '// OLO_TEST_LAYER: L3\n#include <gtest/gtest.h>\nTEST(NewThing, Works) {}\n'
  expect(newTestFileNotes(NEW_TEST, R, classified, '\t\tRendering/NewThingTest.cpp\n', '{}')).toEqual([])
  expect(
    newTestFileNotes(NEW_TEST, R, '#include <gtest/gtest.h>', '\t\tRendering/NewThingTest.cpp\n', '{"file_layer_map":{"OloEngine/tests/Rendering/NewThingTest.cpp":"L3"}}'),
  ).toEqual([])
})
