# Transparent draw order

Applies to: `OloEngine/src/OloEngine/Renderer/Commands/DrawKey.h`,
`Commands/CommandBucket.cpp`, and every call site that builds a sort key.

## The rules

1. **A conventional alpha-blended draw sorts depth-first.** `DrawKey`'s low 56 bits are laid out
   *per render mode*: `Opaque` / `Additive` / `Subtractive` are state-major (shader, material,
   depth); `Transparent` is depth-major (depth, shader, material). Inverting the depth value is not
   enough on its own — an inverted depth in the least significant field orders draws only *within*
   one shader+material bucket.
2. **Put the discriminator above the payload, not in a second sort.** `RenderMode` occupies bits
   [57:56], above both payloads, so opaque and transparent keys can never interleave in a raw
   64-bit compare. That is what lets one field-agnostic LSB radix sort produce two different field
   orders in the same eight passes. Adding a comparator would have been correct and would not have
   survived `CommandBucket`'s radix sort.
3. **A mode-dependent layout needs a re-packing `SetRenderMode`.** Changing the mode changes what
   the payload bits mean. `DrawKey::SetRenderMode` reads the three fields under the old
   interpretation, clears the payload and writes them back under the new one — otherwise the
   mutators are order-independent only by accident.
4. **Auto-batching may only group blended draws that share a complete sort key.**
   `InstanceGroupKey` carries the full raw `DrawKey` for `Transparent` draws and zero for every
   other mode. A batch takes the *first* member's key and nulls the rest, so grouping draws at
   different depths silently collapses them to one depth; grouping draws that differ only in
   material re-orders them against a third draw that should have sorted between. Draws with an
   identical key were already an unordered tie run, so collapsing *those* changes nothing — and the
   instanced draw lays them out in submission order, which is exactly what the stable radix sort
   would have produced.
5. **Additive and subtractive are order-independent by construction — say so, don't test it as if
   it were in doubt.** `dst + s1 + s2` and `dst - s1 - s2` commute. Only source-over does not.
6. **OIT correctness is a different claim.** The weighted-blended OIT targets are written by the
   decal, particle and groom passes under `RendererSettings::OITEnabled`. `SceneRenderPass` has no
   OIT route at all, so a blended mesh is *always* a conventional alpha draw. A green
   `OITPropertyTests` says nothing about it.
7. **State the limitation instead of implying it away.** The depth is one quantised value per
   *draw*, from the object's origin or bounding-sphere centre in view space. Two transparent meshes
   that **intersect** blend in whole-object order at every pixel, including the pixels where the
   other mesh is in front. Object-level sorting cannot fix that and does not claim to.

## The failure

`CreateTransparent` did this:

```cpp
key.SetShaderID(shaderID);
key.SetMaterialID(materialID);
key.SetDepth(0xFFFFFF - depth); // Invert depth for back-to-front sorting
```

with `SHADER` at bit 40, `MATERIAL` at bit 24 and `DEPTH` at bit 0, and `operator<` comparing the
raw key. The comment is accurate about *what the inversion does* and silent about where the field
sits, so the code reads as correct: back-to-front is right there in the name. It is right for a
single material and wrong for two.

The failure mode is quiet. The blend result is always *plausible* — two translucent surfaces still
look like two translucent surfaces — and it only diverges when overlapping transparents carry
different materials. With one glass pane in the scene, nothing is wrong. The sort key has been on
every draw in the engine since the command queue was written.

Red 50% in front of blue 50% over black is `(0.5, 0, 0.25)`. Reversed it is `(0.25, 0, 0.5)`. Both
are a purple.

## What the tests have to do

A key-level assertion is not the claim. `Rendering/TransparentDepthOrderingTest.cpp` drives the
real `CommandBucket` radix sort and then composites the resulting sequence with the source-over
operator, so it asserts the framebuffer claim; it opens with a negative control proving the
compositor is order-sensitive at all, because without one every ordering assertion could pass on a
compositor that ignored order.

The pixel counterpart
(`Rendering/PropertyTests/TransparentBlendOrderVisualEvidenceTest.cpp`) cannot assert
`(0.5, 0, 0.25)` — the frame is tone-mapped and sRGB-encoded. It captures a **pair** instead: red
in front, then blue in front, and asserts the dominance *swaps*. A single capture proves nothing,
because the pre-fix order is decided by two material-ID hashes that do not change when the quads
swap places: both captures came out with the same quad on top, and one of them happened to be
right. It also asserts the two materials hash to different sort IDs as a precondition — two
untextured PBR materials hash to the same ID, and the whole defect is unreachable in that scene.

## Where else a grouping step can undo it

Audited for #1327, all of these are safe *because* of rule 4 or because they never see a blended
`DrawMesh`:

| Stream | Why it is safe |
|---|---|
| `CommandBucket::BatchCommands` | Blended draws group only on a complete key (rule 4). |
| `CommandBucket::TryMergeCommands` | Has no callers; it is dead code, not a live path. |
| Parallel submission (`SubmitPacketParallel` + `MergeThreadLocalCommands`) | Merge order changes, then the sort runs; only fully equal keys are affected, and those are already a tie. |
| `ExecuteParallel` | Splits the *sorted* array into contiguous ranges recorded in item order; order across items is preserved. |
| Depth prepass | `CommandDispatch` makes a blended draw a no-op during the prepass (colour and depth masked off). |
| `ForwardOverlayPass` | Its own bucket, no batching; the grid is a `DrawInfiniteGrid`, the skybox sorts last by `ViewLayerType::Skybox`. |
| Decals / water / particles / groom | Not `CommandType::DrawMesh`, so the batcher never considers them; decals and particles route to OIT when it is on. |

One thing the audit found and did **not** fix: on `RenderingPath::Deferred` a blended classic mesh is
sent to `PBRGBufferShader` rather than rerouted to `ForwardOverlayPass`, so it blends into the
G-Buffer's channels and shades to pure black (issue #1404). That is a shading defect, not an
ordering one — it reproduces unchanged on master — so the deferred cell of the pixel test is a
tripwire on the defect rather than a skip, and it fails the day #1404 is fixed.
