# Deform a skinned vertex in one place, and advance its history at the frame boundary

Two rules, from issue #1226.

1. **Every pass that needs a skinned vertex gets it from
   `OloEditor/assets/shaders/include/SkeletalDeformation.glsl`.** Do not write linear-blend skinning
   into a pass. If a new pass needs a deformed vertex, call `OloDeformSkinnedVertex()` or
   `OloDeformSkinnedPosition()` and add the file to `kSkinnedConsumers` in
   `SkeletalDeformationContractTest.cpp`.
2. **Advance previous-pose history once per FRAME, for every skinned entity, whether or not it
   animated** — `SkeletalDeformationSystem::AdvanceHistory`, called from Scene's frame entry points.
   Never from inside an animation update, and never from the gameplay schedule.

Both rules exist because the code broke them, and in both cases every test stayed green.

## What stayed green

Before #1226: the full suite passed, `ShaderCompilation.AllProductionShadersCompileUnderVulkanTarget`
passed, the visual RMSE goldens passed at their baseline of zero failures, and the editor rendered an
animated fox that looked correct. Both defects below were live the whole time.

## 1. Seven copies of the skinning math, and three of them had drifted

The same linear-blend skinning was written out in seven shaders:

```
DepthPrepass_Skinned.glsl     DepthPrepass_MaskSkinned.glsl
PBR_GBuffer_Skinned.glsl      PBR_MultiLight_Skinned.glsl
ShadowDepthSkinned.glsl       VSM_DepthSkinned.glsl        VSM_DepthLocalSkinned.glsl
```

The colour and depth group guarded the accumulation two ways:

```glsl
if (totalWeight > 0.001) { for (i) if (boneID >= 0 && boneID < 100) bone += palette[boneID] * w[i]; }
else                     { bone = mat4(1.0); }   // unweighted vertex passes through
```

The three shadow-path copies had neither guard — they blended four palette entries unconditionally:

```glsl
mat4 boneTransform  = u_BoneMatrices[a_BoneIndices[0]] * a_BoneWeights[0];
boneTransform      += u_BoneMatrices[a_BoneIndices[1]] * a_BoneWeights[1];   // ... and 2, 3
```

So the two groups produced different geometry for exactly two vertex classes:

- **A vertex with no bone influence.** Four zero weights accumulate the **zero matrix**, which maps
  the vertex onto the model origin. The colour pass drew it at its rest position; all three shadow
  passes dragged it to the pivot. A mesh with any rigid, unweighted vertices casts a shadow smeared
  towards its own origin — read as a shadow-bias or a cascade problem, not as a skinning problem.
- **A bone ID at or past the palette.** An out-of-range uniform read: undefined on both backends, and
  on Vulkan without robust buffer access a device-fault risk rather than a wrong pixel.

**Why no test caught it, and why a numeric test would not have.** Both classes are absent from a
well-formed procedural test mesh — `MeshPrimitives::CreateMultiBoneAnimatedCube()` weights every
vertex to sum exactly 1.0 — and present in real imports. Sampling positions out of each pass and
comparing them proves the copies agree *for the vertices sampled, on the driver that ran*; it says
nothing about the next edit, and it would have sampled precisely the vertices where the copies
agreed.

**The counter-move is structural, not numeric.** One producer, and a test that reads the shader
sources as text and fails if any consumer includes the producer without calling it, declares a bone
palette of its own, or contains a palette-times-weight accumulation. `SkeletalDeformationContract.*`
in `OloEngine/tests/Rendering/SkeletalDeformationContractTest.cpp`. It also pins `OLO_MAX_BONES`
against `UBOStructures::AnimationConstants::MAX_BONES`, because the shader's bounds test is only a
bounds test while it bounds the buffer the C++ side actually uploads.

Two details of the producer's shape are load-bearing:

- **`OloDeformedSurface::Position` is a `vec4`, and the w is not forced to 1.** The skin matrix's
  bottom row is the weighted sum of the palette's bottom rows, so w is the total influence weight.
  Every consumer has always multiplied that vec4 straight into its own model matrix; preserving it
  is what keeps the colour pass and the depth prepass bit-identical under `invariant gl_Position`.
  "Fixing" it to 1.0 is a behaviour change to six passes for no measured benefit.
- **`OloTotalBoneWeight` sums the four components explicitly instead of `dot(w, vec4(1.0))`.** A dot
  product is free to contract into fused multiply-adds; the explicit sum is not. The depth-prepass
  contract is a promise between two separately compiled programs, and `invariant` cannot rescue a
  difference the front end introduced.

- **The previous-pose palette (binding 31) is opt-in via `OLO_DEFORM_WANT_PREV`.** Only the two
  colour passes emit velocity, and a declared-but-unbound descriptor is a Vulkan validation error
  per pipeline.

A side benefit worth knowing: with one producer, an A/B that removes skinning from *every* pass at
once is a one-line edit to a runtime asset — `OloSkinMatrix` returns `mat4(1.0)` — reloadable
without a rebuild. That is the empty-shader arm a GPU bracket measurement needs. Before the
unification it was seven coordinated edits, which is why nobody had ever measured it.

## 2. History advanced only for entities that were animating, and only inside the tick

`SkeletonData::m_PrevFinalBoneMatrices` is the previous-pose half of the deformation output; the
skinned colour shaders subtract it from the current pose to emit per-pixel motion. It used to be
rotated at the top of `AnimationSystem::Update` and `AnimationGraphSystem::Update` — both of which
Scene calls only for an entity whose `m_IsPlaying` is set.

Pause a character and the rotation stopped with `prev` and `current` one frame apart. The shaders
then emitted that last frame's bone delta, unchanged, **every frame for the length of the pause**. A
character standing perfectly still smears under TAA and motion blur. Nothing asserts, nothing logs,
and the symptom appears in the temporal filter, several subsystems from the cause.

The fix has two halves and both are necessary:

- **Advance for every skinned entity, not only the animating ones.** A paused skeleton then advances
  into `prev == current` and emits exactly zero motion, with no special case anywhere.
- **Advance at the frame boundary, not in the gameplay schedule.** `Scene::OnUpdateRuntime` gates the
  whole tick on `if (!m_IsPaused || m_StepFrames-- > 0)`, so a history pass registered as a scheduler
  system stops running while paused and freezes the very delta it exists to prevent. This was the
  first attempt, and the scheduler's declared-dependency ordering made it look correct.

  The frame boundary is also the right semantics under a fixed-tick clock: whether the frame ran no
  ticks or several, `prev` is the pose the last *rendered* frame drew and `current` is the pose this
  one draws — which is what a motion vector means. It matches `Renderer3D::GetAndRecordPrevTransform`,
  whose per-entity transform history rotates in `RenderPipeline::PrepareFrame`, once per frame.

**A discontinuity is a separate concept from a pause, and must be explicit.** After a skeleton swap,
a bone-count change, a teleport or a play-mode transition, the previous palette is not a pose this
skeleton was ever in, and a velocity measured across it is a plausible wrong image.
`SkeletonData::ResetBoneHistory()` holds `prev == current` so the next frame emits zero motion, and
`SkeletalDeformationSystem` counts every reset by cause. `HasBoneHistory()` is what separates "no
motion because nothing moved" from "no motion because the history was thrown away" — the counters
are on `olo_skeletal_deformation_stats` and in the editor's Statistics panel, because the four issues
building on this contract (#1227, #1228, #1229) should not each re-derive it.

## Related

- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) — the same archetype where the mirror is a
  CPU/GPU pair rather than N shaders.
- [glsl-shaders.md §8a](glsl-shaders.md) — a shared include is only proven by compiling its largest
  consumer; `PBR_MultiLight_Skinned.glsl` is the largest consumer of this one, and it has failed to
  *link* at runtime before while `glslc` stayed clean.
