# Procedural skinned mesh primitives (issue #592)

`MeshPrimitives::CreateAnimatedCube()` / `CreateMultiBoneAnimatedCube()` build
a `MeshSource` + `Skeleton` entirely in code — no external asset — for
animation/skinning tests. Two non-obvious points from porting them to the
current `MeshSource` bone-influence system:

## `MeshSource::Build()` needs a live GL context

`MeshSource::Build()` unconditionally creates GPU buffers
(`VertexBuffer::Create` / `IndexBuffer::Create`), even for the CPU-only
`BoneInfluence`/`Skeleton` data a skinning-correctness test actually cares
about. Any test that calls a `MeshPrimitives::Create*()` factory therefore
needs `OLO_ENSURE_GPU_OR_SKIP()` (`RenderPropertyTest.h`) — it cannot live on
the headless `Functional` axis (ADR 0002) even though its assertions are pure
math. Classify such a test `L1` (property/behavioural), not `Functional` or
`unit`.

## Give a multi-bone test skeleton a shared bind-pose origin so a bone rotation pivots at the seam

For a skeleton built purely for a test (not an imported rig), setting every
bone's `m_LocalTransforms[i]` to `glm::mat4(1.0f)` at bind time (mirroring
`AnimationFixtures.h`'s `MakeTwoBoneSkeleton()`) places every bone's bind-pose
global transform at the same world origin. Because a bone's *local* transform
composes as `T * R * S`, later mutating only the rotation component (leaving
`T` at the identity's zero translation) rotates that bone's whole subtree
**around that shared origin** — which is exactly the mesh seam if you
partition vertex weights around `y == 0`. This gives a deterministic,
zero-setup "rotate the child bone, verify it bends at the seam" test without
having to reason about an offset pivot. If bones instead need distinct
bind-pose positions (mirroring a real rig), remember the pivot for a later
rotation is wherever `T` is baked into that bone's local transform at bind
time — not the origin.

## `MeshSource::GetSkeleton()` needed a mutable overload

`MeshSource` only exposed `const Skeleton* GetSkeleton() const` — sufficient
for read-only import consumers, but a test that wants to pose a
procedurally-built skeleton (mutate `m_LocalTransforms`, recompute
`m_GlobalTransforms`/`m_FinalBoneMatrices`) needs write access. Added a
`Skeleton* GetSkeleton()` non-const overload alongside it, matching the
existing `GetVertices()`/`GetIndices()`/`GetSubmeshes()` const+non-const
pattern on the same class.
