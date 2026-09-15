# A deformation revision is not a transform history, and must not be derived like one

**When a GPU Scene record carries both a transform history and a deformation history, derive them
by different rules: the transform's previous value comes from the record's own slot, the
deformation's comes from the producer that owns the palettes.** From issue #1228, which put animated
meshes into the canonical records. Read
[gpu-scene-record-contract.md](gpu-scene-record-contract.md) for the record rules this extends and
[skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) for the producer.

Code: `Renderer/GPUScene/GPUSceneTypes.h` (`GPUSceneInstance`'s deformation lanes,
`GPUSceneAnimatedSurface`, `GPUSceneAnimatedStats`), `Animation/SkeletonData.h` (the revision pair),
`Scene/Scene.cpp` (`MakeGPUSceneAnimatedSurface` and the two animated submission sites).

## 1. Why the two histories cannot share a rule

`GPUScene.cpp`'s instance commit derives the previous transform from the slot:

```cpp
const glm::mat4& previousWorldTransform =
    slot.m_Live ? slot.m_Input.m_WorldTransform : input.m_WorldTransform;
```

That is right for a transform. "Where was this record last frame" is a question about the record,
and an instance that was not extracted last frame genuinely has no previous position, so it starts
static.

Applying the same rule to the deformation gives two wrong answers, and both are silent:

- **A surface culled for one frame.** `SkeletalDeformationSystem::AdvanceHistory` advances every
  skinned entity every frame, drawn or not, so its bone history is intact when it comes back. The
  slot rule would call it a discontinuity and throw away a velocity that was available.
- **A reset that lands after the frame's advance.** A morph surface that moved, an LOD switch, a
  teleport discovered during the tick — these all reject the history *after* `AdvanceHistory` has
  already run. The skeleton records that by holding `prev == current`. The slot rule cannot see it
  at all: last frame's record said N, this frame's says N+1, so it reports continuity across the
  exact seam the rejection exists to declare. That is a plausible wrong image — a velocity measured
  between two surfaces — reaching TAA and motion blur two subsystems from the cause.

So the deformation lanes are copied **verbatim** from the producer, and the divergence from the
transform rule immediately above them is written into the header rather than left to be rediscovered.

## 2. The revision pair, not a boolean

`SkeletonData` already had `HasBoneHistory()`. The record carries a *pair* instead:

```cpp
u32 m_DeformationRevision = 0;
u32 m_PrevDeformationRevision = 0;
```

advanced by one on the single continuous branch of `AdvanceBoneHistory` and **held equal** on every
other outcome and in `ResetBoneHistory`. Continuity is then `current == previous + 1` in u32 modular
arithmetic, which survives the ~2.2-year wrap because `previous + 1` wraps exactly when `current`
does.

A boolean would have been enough for the raster path and is not enough for a canonical record: a
consumer that built something from this surface — an acceleration structure, a cached bound — has to
ask *"has it deformed since I built?"*, and a per-frame flag cannot answer that. The pair is what
#1229's refit will read.

Two spellings of one fact is the risk, and the counter-move is not inspection:
`GPUSceneAnimated.TheRevisionPairNeverDisagreesWithTheBonePalettes` walks construction, first use,
advance, a reset *after* an advance, recovery and a bone-count change, asserting
`HasBoneHistory() == HasContinuousDeformation()` at every step.

## 3. A rigid instance must not read as an animated one with no history

Both lanes are 0 on a rigid instance, and `0 == 0` is the *discontinuous* reading. So the record
carries `GPUSceneInstanceFlagAnimated` and every consumer tests the flag before the values.

The flag is set from the caller's own statement — `GPUSceneAnimatedSurface::m_IsAnimated` — and never
inferred from the revisions being non-zero. Inference classifies a newly spawned character as rigid
for exactly one frame, which is the frame its history is most obviously absent.

## 4. What identity did NOT need

Almost nothing. A skinned mesh's vertex buffer holds a rest surface that the vertex stage deforms,
and `Scene::EvaluateMorphTargets` writes that same buffer in place, so the geometry key
`(vertex buffer, index buffer, submesh)` was already stable across the animation before this issue
existed. What was missing was a path that used it — three sites short-circuited before staging, with
a comment saying a skinned entity "is not representable in GPU Scene".

That comment was true of the *deformation*, not of the *identity*, and the distinction is the whole
issue. Expect the same shape elsewhere: a path excluded from a registry for a reason that applies to
one field of the record, not to the key.

The corollary is the split the submission sites keep: **identity is per submesh, the revision is per
entity.** One character is N records sharing one pose. Keying the pose per submesh lets two submeshes
of one character disagree about their shared palette; keying identity per entity gives every submesh
after the first the previous one's transform history, which is the defect the per-entity
previous-transform cache had before #994.

## 5. A category that stops reporting is not a category that became accurate

`GPUSceneUnsupportedCategory::Skinned` used to be a blanket count of every skinned entity, taken in
`ProcessScene3DSharedLogic` before any submission ran. Deleting it once the records took those
surfaces would leave the diagnostics unable to say anything about an animated entity that still
cannot be represented.

It now means *an animated entity that reached a submission path and produced no canonical instance*,
reported per entity at the sites that know the refusal — so it reads 0 in a healthy scene and moves
the moment one stops being representable. Per **entity**, deliberately: `ExtractGPUSceneMesh` already
ticks `NotExtractable` per refused **submesh**, and a per-submesh Skinned tick would double-count the
same event under two names.

What the records *did* take is `GPUSceneAnimatedStats`, in the shape `GPUSceneFoliageStats`
established for #1230, because "the records take everything" and "nothing was ever offered" must not
read alike. It partitions its own canonical instances into with-history and without-history, and the
evidence test asserts that partition — a third number that is neither is a producer bug, not a
property of a scene.

## 6. Do not declare a criterion met on an entry point nobody calls

Acceptance criterion 4 named concurrent command recording.
`Renderer3D::DrawAnimatedMeshParallel` and its `SubmitMeshesParallel` bracket had **no callers
anywhere in the repository** — `Model::DrawParallel` is the only live user of the bracket and
hard-codes `IsAnimated = false`.

This repo has paid for that shape once: #1226's `HasBoneHistory()` guard went onto
`Renderer3D::RenderAnimatedMeshes`, which has no callers, and looked load-bearing through two review
passes while the path that actually rendered stayed ungated
([skeletal-deformation-shared-output.md §3](skeletal-deformation-shared-output.md)).

So the link parameter was threaded through *and* driven:
`AnimatedGPUSceneConcurrentRecordingTest` is the caller. It mints links on the main thread, records
from the worker contexts, and compares the links the workers wrote into their packets against the
ones the main thread minted. A branch that dropped the parameter submits the same number of packets
and draws the same picture; only that comparison fails.

The thread-safety argument is structural and belongs in the header, not in a lock: links are
**minted** on the main thread (`ExtractGPUSceneMesh` appends to an unsynchronised per-frame vector)
and only **carried** across the boundary as a plain integer copied into the descriptor. The test
pins the invariant that makes it safe — the link table does not grow during the parallel region —
rather than trying to catch a race by repetition, which is TSan's job and not something this box can
do ([build-trees-and-windows-asan.md](build-trees-and-windows-asan.md)).

## Related

- [gpu-scene-record-contract.md](gpu-scene-record-contract.md) — the record, key and generation rules
  this extends, and the draw-link ordering in §7.
- [skeletal-deformation-shared-output.md](skeletal-deformation-shared-output.md) — the producer that
  owns the revisions, and the frame-boundary advance.
- [morph-and-lod-in-the-animated-surface.md](morph-and-lod-in-the-animated-surface.md) — what makes a
  surface a different surface, and therefore what rejects its history.
- [no-silent-fallbacks.md](no-silent-fallbacks.md) — the rule §5 is an instance of.
