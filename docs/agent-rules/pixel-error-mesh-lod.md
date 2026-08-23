# Pixel-error mesh LOD (generation + selection)

The classic-mesh LOD path: `Renderer/LOD.{h,cpp}` for selection,
`MeshOptimization::BuildAutoLODChain` / `GenerateAutoLODGroup` for generation. From issue #711,
which replaced hand-authored distance thresholds with a generated per-level error measure and a
view-independent screen-size estimate.

Design reference: `Timberdoodle/src/scene/mesh_lod.hpp` (Apache-2.0) — rationale at `:13-41`,
selection at `:45-120`, generation in `src/scene/asset_processor.cpp`.

---

## 1. The projection plane faces the MESH, not the camera — and the wrong one passes every test

`EstimateProjectedPixelSize` takes the camera POSITION, the field of view and the render height.
It never touches the view matrix. That is the entire trick, and it is the one thing here that a
plausible-looking reimplementation gets wrong.

Projecting the AABB through the real view-projection — the obvious implementation — makes the
estimated size depend on **where on screen** the mesh happens to land, because a perspective
projection stretches away from the centre. Then:

- the level flips as the camera merely **sways**, which is exactly the popping the issue removed;
- geometry **off-screen or behind the camera** projects to garbage or to nothing, so shadow casters
  (and any future ray tracing) pick a level from a number that means nothing.

Neither failure shows up in a value test. `SelectLODByPixelError` is a pure function of one float,
so every test that feeds it a pixel size passes identically under both implementations. The tests
that separate them are the invariance ones — `LODGroup.PixelSizeIsInvariantToCameraOrbit` and
`AutoMeshLODScene.LODOrbitDoesNotSwitchLevels` — and they only work because they hold the
camera-to-subject **distance** fixed while changing the direction. An orbit test around a *field*
of subjects does not do that: orbiting genuinely changes each individual member's distance, so it
would fail on a correct implementation. Orbit a single subject at the orbit centre.

The same reasoning bans re-fitting an AABB around a rotated one. `EstimateProjectedPixelSize`
takes the per-axis extent as `length(modelMatrix[i]) * localSize[i]`; a refit box grows by up to
sqrt(3) at 45 degrees and would pump the level as the OBJECT spins.
`LODGroup.PixelSizeIsInvariantToObjectRotation` pins that.

## 2. The error is a RATIO, and meshoptimizer is why

`meshopt_simplify*` rescales positions to unit extent internally (`simplifier.cpp`,
`rescalePositions`) and returns `result_error` relative to that extent. Attributes get no such
treatment: `rescaleAttributes` just multiplies each attribute by its weight. So **an attribute
weight is expressed in normalized-position units**, and a normal weight has to be
`importance * (averageVertexDistance / modelExtent)` or it means a different thing on every mesh.

Get this wrong — use a raw model-space vertex distance — and nothing fails loudly. The chain still
generates, the errors are still monotone, the renderer still selects. What breaks is that the same
model authored in centimetres rather than metres produces a different chain, so one pixel-error
threshold cannot serve a scene with mixed authoring scales.
`MeshOptimization.AutoLODChainIsInvariantToModelScale` is the discriminator: the same mesh at 1x
and 100x, asserting the same chain length, triangle counts within 2% and accumulated errors within
10%.

**The ERROR assertion is the discriminating half, and the triangle counts are nearly worthless
here.** With `target_error = FLT_MAX` the simplifier collapses until it hits the triangle target, so
the counts come out the same under *any* weighting — measured within one triangle per level at a
100x weight. Weights decide **which** edges collapse, not how many. An unnormalized weight instead
shows up in the accumulated error (measured 41-72% apart at the deeper levels) and in the surviving
index buffers.

The tolerances are loose because the two runs genuinely are not bit-identical: f32 positions 100x
apart round differently, that flips the occasional quadric tie, and a one-triangle difference at
level 1 propagates down a nine-level chain. Measured drift is ~0.1% on counts and a few percent on
error. Because "within 10%" could be satisfied by accident, a companion case —
`MeshOptimization.NormalWeightMustBeNormalizedByModelExtent` — simulates the missing division and
asserts both that the chain lands outside that band **and that the index buffers differ**. Keep the
pair together, and keep the geometry half: the error gap alone is partly produced by the
`stepError / (1 + normalWeight)` renormalisation, so on its own it would be measuring the divisor
rather than the simplifier.

**The average vertex distance is recomputed from the level being simplified**, not estimated as
`sqrt(2)^level`. The estimate assumes every step really halved; the moment a step is
topology-limited it stops being true and the levels quietly stop being mutually consistent. Since
the measurement is a single pass over an index buffer that the simplifier is about to walk many
times, the estimate buys nothing worth that risk.

## 3. Two selection paths coexist, and `HasErrorData()` is the switch

A group with any level carrying `Error > 0` selects by pixel error. A group with none — every
hand-authored group, and every scene or save-game written before #711 — keeps the legacy
`MaxDistance` thresholds. This is deliberate: an authored chain's distances are a designer's
decision, and a generated chain's `MaxDistance` values are nominal (derived from the errors at a
reference 1080p / 90° camera purely so the inspector reads sensibly).

The consequence for serialization: **`Error` defaulting to 0 must stay meaningful**, because that
is what a version-gated read leaves behind. It does — 0 means "no measurement", which routes the
group to the distance path it was authored for. A sentinel like -1 would have been worse.

## 4. `LODLevel::Error` is a serialized field in four places

`LODGroupComponent` is in `kComponentsCustomSerialize`, so nothing generated guards it:

- `Scene/SceneSerializer.cpp` — both the emit and the read, with an `std::isfinite` check.
- `SaveGame/SaveGameComponentSerializer.cpp::SerializeLODLevel` — gated behind
  `HasFieldsSince(ar, 21)`. **The `AtEnd()` probe `DecalComponent` uses cannot work here**: the
  field is inside a variable-length per-level loop, so only the last element of the last component
  would ever be at the end. A version gate is the only correct shape for a field appended inside
  an array element.
- `LODLevel::operator==` — bitwise, per `cpp-coding-quality.md` §2a. Miss it and an inspector edit
  to the field records no change and cannot be undone.
- `OloEngine/tests/Serialization/NestedStructSerializerCodegenTest.cpp` mirrors the generator's
  emit for this exact struct; keep the mirror faithful.

## 4a. A generated chain is DERIVED data and is not persisted level by level

Every generated level is a **memory-only** `Mesh` asset, and a memory-only handle is dead the
moment the process exits. Writing those handles into a scene file stores a reference nothing can
ever resolve — the archetype in
[cache-stored-unresolvable-reference.md](cache-stored-unresolvable-reference.md), and its failure
only shows on the SECOND load, which no CI run performs.

The emit path tests **two independent facts**, and treats the group as derived if either holds:

| | what it means | why the other one cannot replace it |
|---|---|---|
| `m_AutoGenerated` | **provenance** — the generator made this | still true after the chain's memory-only assets have been *released*, which no handle check can see |
| any level is a memory-only asset | **persistability** — this handle is dead next process | catches a half-edited chain whose flag was already cleared |

Both directions were tried and both are wrong alone. Trusting only the flag wrote dead handles for a
chain that was hand-edited in the inspector. Trusting only the handles — which is what a reviewer
suggested and I briefly applied — made a *released* generated chain read as authored, and a test
caught it only because an unrelated suite had left an asset manager installed in the process. Keep
the union.

The panel keeps the flag honest at the source: **only the two actions that change where a level's
geometry came from touch it.** Generate LODs sets it; re-pointing a level at a real asset clears it.
Distance drags, adds and removes deliberately do not — they change nothing about persistability, and
a flag maintained on every edit is a second source of truth that can only drift.

Three consequences worth not re-deriving:

- **Regeneration can decline** — auto-LOD off, no graphics device, no asset manager, or a mesh the
  simplifier cannot reduce. The component is still added, **empty**, carrying `Enabled`, `Bias` and
  the marker. Dropping it instead loses the authored settings and erases the marker on the next
  save, silently converting the entity to "no LOD" for everyone who opens the scene afterwards. An
  empty group is inert: `SelectLODMesh` returns early and the base mesh draws at full detail.
- **Save-games never carry a generated chain either.** A save is restored *into* an
  already-loaded scene, so restoring stale handles would clobber the chain the scene load just
  rebuilt with the live one. `Serialize(FArchive&, LODGroupComponent&)` writes zero levels for a
  derived group and, on load, keeps the live levels rather than the archived ones.
- **`AssetManager::IsMemoryAsset` asserts without an asset manager**, so every call on these paths
  is guarded by `Project::HasAssetManager()` first. Headless serialization has no memory-only
  assets to find anyway.

`m_AutoGenerated` carries `OLO_SERIALIZE(Skip)` purely for the tag's *other* effect. The component's
scene serializer is hand-written, so the tag changes nothing there — but the same field scan feeds
the MCP writable-field registry, and flipping this flag from a debug tool would change what the next
save does. It is a provenance record, not a knob.

## 4b. The generated meshes have an owner, and it is `OnComponentRemoved`

Every level past LOD 0 is a memory-only `Mesh` asset in the process-global `AssetManager`, and
making load regenerate means a scene reopened ten times would strand ten full sets of CPU + GPU
buffers. `Scene::OnComponentRemoved<LODGroupComponent>` calls
`ModelImporter::ReleaseGeneratedLODAssets`, which frees exactly the handles in
`m_GeneratedLODHandles` that are *still* memory-only — a level the user re-pointed at a real asset
is not ours to remove. `LODGroupComponent` is therefore in `kComponentsCustomOnRemove`
(`tools/OloHeaderTool/main.cpp`) and in `ComponentHandlerCoverageTest`'s mirror of that set; the two
must stay in sync.

**The hook alone is not enough, and this is the part that bites.** `OnComponentRemoved` in this
engine is **not** an entt signal — `Entity::RemoveComponent<T>` calls it by hand, so
`m_Registry.destroy()` never fires it. Destroying an entity therefore has to release the chain
explicitly, which `Scene::DestroyEntity` now does alongside the same manual teardown vehicles,
physics bodies and crowd agents already needed. Grep `m_Registry.destroy() does NOT fire` in
`Scene.cpp` for the other members of that club before assuming a removal hook covers you.

`ModelImporter::DiscardGeneratedLODGroup` handles the re-import case, and it runs **before** the
animated gate, not inside it. Both halves are load-bearing: the editor's "Import Model" buttons
assign a new `m_MeshSource` onto an **existing** entity, so a chain generated for the previous mesh
is stale whatever the new mesh is — and re-importing a static model as an *animated* one takes the
"no chain for skinned meshes" skip path, which would otherwise leave that stale chain attached to a
skinned mesh forever. `ModelImporterTest.StaticToAnimatedReimportDiscardsTheGeneratedLODGroup` pins
exactly that ordering. An *authored* group still wins and is never discarded.

**Known residual: a duplicated entity shares the original's chain without owning it.** The copy
constructor deliberately does not copy `m_GeneratedLODHandles` (ownership is not duplicable), so
deleting the original releases assets the duplicate still names. That degrades gracefully — an
unresolvable handle makes `SelectLODMesh` fall back to the base mesh, so the duplicate simply
renders at full detail — and `Scene::Copy` (Play) is unaffected, since the editor scene outlives the
runtime one and keeps owning the assets. The proper fix is the shared, refcounted per-mesh chain
cache in §5.

## 4c. A zero-error level past index 0 is UNMEASURED, not free

`SelectLODByPixelError` stops the scan at any level whose `Error` is not finite and positive. This
matters because the inspector's "Add LOD Level" button appends a level with `Error = 0`, and
`pixelSize * 0` satisfies every budget — so a blank level would have been selected at *every*
distance, which reads on screen as "LOD is dead" or, once a mesh is assigned to it, a coarse mesh
drawn point-blank. There is nothing to judge an unmeasured level on, so the scan keeps the last
measured one.

## 5. What this does NOT cover

- **Multi-submesh sources are rejected.** The generator collapses everything onto material index 0,
  so a 25-submesh Sponza import gets no chain. That is the pre-existing limitation of
  `GenerateLODMesh`, inherited.
- **Skinned and morph-target sources are rejected.** Simplification drops bone weights and morph
  deltas, so a skinned LOD would render in bind pose.
- **Shadow casters always draw LOD 0.** `SubmitMeshSourceClassic` adds the caster from the
  unselected submesh, so the depth pass keeps full density. Correct but not free.
- **No cross-entity sharing.** Two entities importing the same model each generate and own their
  own chain, and a scene load regenerates one per entity. The editor's "Generate LODs" button has
  always behaved this way; import-time generation and regenerate-on-load make it more visible. A
  cache keyed by the source mesh would fix both the duplicate cook and the duplicate memory.

## 6. Auto-generation at import is guarded on the graphics device

`ModelImporter::EnsureAutoLODGroup` skips generation when
`AutoLODImportConfig::RequireGraphicsDevice` is set (the default) and
`RenderCommand::IsDeviceAvailable()` is false. The generated levels are `Mesh` assets whose GPU
buffers a headless process can never create, so generating them there is cook cost for something
nothing can draw. A test that wants the chain without a device clears the flag.

It also never touches an entity that already has a `LODGroupComponent` — an authored chain always
wins — and it drops the whole group when the chain comes back as LOD 0 alone, so a mesh the
simplifier cannot reduce does not pay a per-draw selection cost for nothing.
