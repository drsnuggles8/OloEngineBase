# `Scene::Copy()` must carry every scene-level settings member, not just entities

Short rule for anyone adding a new scene-level (non-ECS) settings struct —
`WindSettings`, `FogSettings`, `PostProcessSettings`, and friends — or wiring
a gameplay system that reads one of them via `Scene::GetXSettings()`.

## The trap (issue #460, wind-coupling slice)

`Scene::Copy()` (`Scene.cpp`) is what `EditorLayer::OnScenePlay()` /
`OnSceneSimulate()` calls to build the runtime scene from the authored editor
scene. Before this fix it copied entities (`CopyComponent(AllComponents{},
...)`), the viewport size, and `m_StreamingSettings` — and **nothing else**.
`m_PostProcessSettings`, `m_FogSettings`, `m_WindSettings`, `m_SnowSettings`,
`m_SnowAccumulationSettings`, `m_SnowEjectaSettings`, and
`m_PrecipitationSettings` were left at their default-constructed values on
the new (`Ref<Scene>::Create()`) copy, silently discarding whatever the
authored scene had.

**Even the fix that added this doc initially missed one:** the first pass
copied six of the seven — `m_SnowSettings` was overlooked (easy to conflate
with the similarly-named `m_SnowAccumulationSettings`/`m_SnowEjectaSettings`
it sits next to in `Scene.h`) and was only caught by a subsequent
`/code-review` pass, not by any test. This is the exact hand-maintained-list
failure mode the "Guard" section below warns about, demonstrated within the
same PR that introduced the warning.

`ClothWindSystem::OnUpdate` reads `scene->GetWindSettings()` once per physics
tick (the same pattern `BuoyancySystem` uses for its own scene-level data).
A scene authored with `WindSettings.Enabled: true` therefore worked
perfectly in every **headless** test (`FunctionalTest` builds one `Scene` and
calls `OnPhysics3DStart()`/`OnUpdateRuntime()` directly — it never goes
through `Scene::Copy()`) but did *nothing* in the actual editor: the instant
Play mode started, the runtime scene's `WindSettings` reset to
`Enabled: false`, and wind silently stopped applying. This is why "4200/4200
tests green" was not sufficient evidence the feature worked — it only proved
the *headless* path worked. **Live-editor verification (via the run-oloengine
MCP tools) is what caught this; the automated suite structurally cannot.**

Rendering-side effects of these settings mostly survived by accident:
`EditorLayer::LoadEditorSceneFile` separately copies the freshly-deserialized
scene's settings into the `Renderer3D` **global singleton** once, at scene
*load* time (not at Play-*start* time) — `Renderer3D::GetWindSettings() =
newScene->GetWindSettings();` and five siblings. Anything that reads the
Renderer3D global (the GPU wind-field dispatch in `RenderPipeline.cpp`, for
example) kept working across a Play/Stop cycle because that singleton is
untouched by `Scene::Copy()`. Only code that reads the **Scene's own** copy
of a settings struct during gameplay — as any new scene-level system should,
mirroring `BuoyancySystem`/`ClothWindSystem` — hits this gap.

## The rule

**Every scene-level settings member added to `Scene.h` must be copied in
`Scene::Copy()`, not just registered as a `Get/SetXSettings()` accessor
pair.** When you add the next settings struct, add its copy line right next to
the other seven — there is no reflection/enumeration doing this for you (unlike
the ECS component tuple, which OloHeaderTool generates; scene-level settings
are hand-maintained). If you're building a new system that reads
`Scene::GetXSettings()` from gameplay code, don't assume "it works in my
FunctionalTest" is sufficient proof — that harness never exercises
`Scene::Copy()`. Verify in the actual running editor (Play mode), not just
the headless suite.

## Guard

No automated guard exists for this today — `Scene::Copy()`'s completeness
isn't covered by a test, and the seven settings structs it now copies were
fixed in two passes (six, then `m_SnowSettings`) rather than one at a time
with individual coverage. A reasonable follow-up: a test that authors
non-default values on every scene-level settings struct, runs it through
`Scene::Copy()`, and asserts the copy matches the source field-for-field
(structural, so the next settings struct fails loudly if its copy line is
forgotten).
