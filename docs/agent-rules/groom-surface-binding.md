# Groom surface binding

Issue #1249. Attaching a cooked groom to an animated body, so the coat follows skeletal bending and
facial morphs instead of standing in its bind pose while the character moves.

## The rules

**A binding addresses triangles by INDEX, so a topology mismatch must refuse.** Never rebind, never
re-snap to the nearest point, never fall back to "close enough". A binding built for one body and
attached to another with the same vertex count produces a coat that looks authored and is wrong — a
plausible wrong image, which is the worst failure this subsystem can produce. `CheckCompatibility`
returns the first reason it will not attach; the caller reports it and draws the coat at rest.

**Build the frame in exactly one place.** `MakeGroomSurfaceFrame` constructs the orthonormal basis
for both the bind-time record and the per-frame deformed evaluation. A second construction that
disagrees about handedness, or about which edge becomes the tangent, is a constant rotation applied
to every strand: a coat that sits on the body, points the wrong way, and looks deliberate.

**Carry each strand rigidly from its root's frame.** Do not skin a groom's points against the body.
A rigid transfer preserves every strand's length exactly whatever the pose does, which is what
"without gross coat collapse" means; per-point skinning pinches long strands into the surface
wherever the nearest bone disagrees with the root's.

**Store the rest frame in the binding; never recompute it from the live mesh.** Morph targets are
applied on the CPU straight into `MeshSource`'s vertex array, so the "rest" positions a runtime
consumer can read are the *morphed* ones. A binding that derived its reference frame from them drifts
with every expression — the coat slides across the face and nothing says so.

**Split the topology check from the bind-pose check.** Vertex count, index count, index hash, bone
count and skeleton identity are morph-invariant and are checked on every attach. The bind-pose
position hash is not: it would mismatch the instant a face expressed. It is a build-time and
editor-time check, and `GroomBindingTargetSignature::MatchesTopology` is deliberately the smaller set.

**Derive previous-frame positions from the previous POSE, never from a carried-forward buffer.** A
buffer of last frame's positions is a buffer that, after an LOD switch or a teleport, holds positions
from a different surface with nothing in it saying so. Deriving them from
`SkeletonData::m_PrevFinalBoneMatrices` means a discontinuity is expressible: hold `prev == current`
and the frame emits *exactly* zero motion.

**Both history gates have to hold.** The caller's (a teleport it detected, an LOD switch, a morph
state that moved) and the skeleton's own `HasBoneHistory()`. Either one alone leaves a discontinuity
uncovered, and the symptom is a coat-length smear under TAA rather than an error.

**Attribute every reset.** `GroomHistoryResetCause` exists because "the history was dropped" is not
something anyone can act on. A refusal maps to a cause (`GroomHistoryCauseFor`), so a body that has
not finished loading does not report itself as a changed binding.

**Two versions, and they refuse differently.** `OloGroomBindingFormat::CurrentVersion` moves when the
BYTES move; `kGroomBinderVersion` moves when the binder's OUTPUT moves for unchanged input. A file
can be perfectly readable and still be refused at attach, and the messages say different things on
purpose — one is "this file is corrupt", the other is "rebuild the binding".

**A new component does not need a save-game format version.** Save components are keyed by an FNV
hash of the type name, so a save written before the component existed simply does not carry the key.
Spending a `kSaveGameFormatVersion` number on one collides with whatever another branch is appending
to an *existing* component, for no benefit.

## What is deliberately not here

Simulation. #1249 stops at deformation and interpolation; guide simulation is #1250. Every strand is
rigid with respect to its root by design, not as a placeholder.

A second body-skinning system. The deformation evaluates #1226's shared output at three vertices per
strand root — a 40 000-strand coat touches 120 000 vertex evaluations against a 20 000-vertex body,
and only the vertices the roots actually sit on. The raster path materialises no deformed vertex
buffer to read (`skeletal-deformation-shared-output.md`), and the one place that does —
`RayTracing::DeformedSurfaceCache` — is Vulkan-RT-only by construction.

## The stories

### The vertex grew and the Vulkan pull did not

`GroomStrandVertex` was twelve floats, and `GroomStrand.glsl` pulls it on Vulkan as a flat float
array with a hard-coded stride — there is no vertex input state on that backend at all (ADR 0011 §5).
A groom bound to a body deforms per strand, so last frame's position of a given point is not
recoverable from any model matrix: the *body's* pose moved, the groom's transform did not. That made
a per-vertex `PrevPosition` unavoidable, which moved the stride from 12 to 16.

The rule that came out of it: when the vertex grows, the shader's stride, the GL attribute layout and
the test that pins `sizeof` all move in the same commit, and the new floats are APPENDED so every
existing pull offset still lands where it did. An unbound groom writes `PrevPosition == Position`, so
its velocity is bit-for-bit what it was before the field existed — which is what keeps every
committed #1246 capture comparable with the one beside it.

### The tie that was resolved by cell size

Two triangles at exactly the same distance from a root is the common case, not a corner one: it
happens on every shared edge of every closed mesh. The first binder resolved it with `<=` while
walking a uniform grid outward, so which triangle won depended on the grid's cell size — a
performance knob — and the same pair bound twice at two resolutions produced different files.

The fix is a strict `<` against the running best, with every cell's triangle list built by one
ordered pass so it is sorted by index. The tie then resolves to the lowest triangle index, which is a
property of the data. `GroomBindingBuilder.AGridResolutionChangeDoesNotChangeTheResult` and
`.ATieBetweenTwoTrianglesGoesToTheLowerIndex` are what hold it there.

### 56 bytes, not 48

`GroomRootBinding` was written with a `static_assert(sizeof(...) == 48)` and a header comment saying
so, on arithmetic done in someone's head. It is 56 — `u32 + u32 + vec3 + f32 + vec3 + f32 + quat`.
The build caught it in one line, which is the point of asserting the size of anything that goes to
disk as a block: the alternative is a stride that is wrong only on the read path, in a file that
already shipped.

## Where to look

| File | What it owns |
|---|---|
| `Groom/GroomBinding.h` | the asset, the signatures, the refusal reasons |
| `Groom/GroomSurfaceFrame.h` | the ONE frame construction, and the strided surface view |
| `Groom/GroomBindingBuilder.{h,cpp}` | the bind, and the determinism contract |
| `Groom/GroomDeformation.{h,cpp}` | the per-frame evaluation, current and previous |
| `Groom/GroomBindingCooker.{h,cpp}` | the named cook of a groom/body pair |
| `Scene/Scene.cpp::DeformGroomAgainstSurface` | resolving the body, deciding history, publishing |
| `Renderer/Passes/GroomRenderPass.cpp` | the per-entity dynamic geometry a bound groom needs |
