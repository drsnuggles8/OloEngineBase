import { expect, test } from 'claude-code/testing'

import { DOCS, gateFor, newTestFileNotes, repoRoot } from '../hooks/gates'

const R = 'C:\\repos\\OloEngine-foo'
const at = (rel: string) => `${R}\\${rel.replace(/\//g, '\\')}`

test('the repo root is found from any top-level directory, not a worktree name', () => {
  expect(repoRoot(at('OloEngine/src/OloEngine/Scene/Scene.cpp'))).toBe(R)
  expect(repoRoot('C:/repos/OloEngine-x/docs/a.md')).toBe('C:/repos/OloEngine-x')
  expect(repoRoot('C:/somewhere/else.txt')).toBeUndefined()
})

test('vendor, vcpkg and generated files are refused', () => {
  expect(gateFor(at('OloEngine/vendor/imgui/imgui.cpp'), { isNew: false }).kind).toBe('deny')
  expect(gateFor(at('build-cached/vcpkg_installed/x64/include/a.h'), { isNew: false }).kind).toBe('deny')
  expect(gateFor(at('OloEngine/src/OloEngine/Scene/Generated/AllComponents.Generated.inl'), { isNew: false }).kind).toBe('deny')
  expect(gateFor(at('OloEngine-ScriptCore/src/OloEngine/Scene/Components.Generated.cs'), { isNew: false }).kind).toBe('deny')
  expect(gateFor(at('docs/test-catalogue.renderer.md'), { isNew: false }).kind).toBe('deny')
})

test('shaders need the GLSL guide', () => {
  const gate = gateFor(at('OloEditor/assets/shaders/include/PBRCommon.glsl'), { isNew: false })
  expect(gate).toEqual(expect.objectContaining({ kind: 'requires-read', doc: DOCS.shaders }))
  expect(gateFor(at('OloEditor/assets/shaders/compute/RayTracingProbe.comp'), { isNew: true }).kind).toBe('requires-read')
})

test('a header declaring a component needs the codegen guide; other headers do not', () => {
  const header = at('OloEngine/src/OloEngine/Animation/FootIKComponent.h')
  expect(gateFor(header, { isNew: false, text: '#pragma once\nstruct FootIKComponent\n{\n};\n' })).toEqual(
    expect.objectContaining({ kind: 'requires-read', doc: DOCS.components }),
  )
  expect(gateFor(header, { isNew: false, text: 'struct FootIKComponent;\nstruct Other {};' }).kind).toBe('none')
  expect(gateFor(at('OloEngine/src/OloEngine/Core/Base.h'), { isNew: false, text: 'using u32 = unsigned;' }).kind).toBe('none')
  expect(gateFor(at('OloEngine/src/OloEngine/X/New.h'), { isNew: true, incoming: 'struct WindComponent { f32 S; };' }).kind).toBe('requires-read')
})

test('a NEW test source needs the testing guide; editing an existing one does not', () => {
  expect(gateFor(at('OloEngine/tests/Rendering/NewThingTest.cpp'), { isNew: true })).toEqual(
    expect.objectContaining({ kind: 'requires-read', doc: DOCS.tests }),
  )
  expect(gateFor(at('OloEngine/tests/Rendering/OldThingTest.cpp'), { isNew: false }).kind).toBe('none')
})

test('ordinary engine and doc edits pass', () => {
  expect(gateFor(at('OloEngine/src/OloEngine/Renderer/Renderer3D.cpp'), { isNew: false }).kind).toBe('none')
  expect(gateFor(at('docs/agent-rules/README.md'), { isNew: false }).kind).toBe('none')
})

test('a new test file: unlisted, unclassified and DISABLED_ are each reported', () => {
  const path = at('OloEngine/tests/Rendering/NewThingTest.cpp')
  const content = '#include <gtest/gtest.h>\nTEST(NewThing, DISABLED_Works) {}\n'
  const [note] = newTestFileNotes(path, content, 'set(SOURCES\n  Rendering/OldThingTest.cpp\n)', '{}')
  expect(note).toContain('not listed in OloEngine/tests/CMakeLists.txt')
  expect(note).toContain('OLO_TEST_LAYER')
  expect(note).toContain('DISABLED_')
})

test('a listed, classified new test file is quiet', () => {
  const path = at('OloEngine/tests/Rendering/NewThingTest.cpp')
  const content = '// OLO_TEST_LAYER: L3\n#include <gtest/gtest.h>\nTEST(NewThing, Works) {}\n'
  expect(newTestFileNotes(path, content, '  Rendering/NewThingTest.cpp\n', '{}')).toEqual([])
  expect(newTestFileNotes(path, '#include <gtest/gtest.h>', 'NewThingTest.cpp', '{"file_layer_map":{"NewThingTest.cpp":"L3"}}')).toEqual([])
})
