# A compute kernel that writes level 0 leaves the mip chain holding the old picture

*Postmortem of issue #716 (GPU-resident terrain painting and erosion). Two bugs, one
invariant, and the second bug was introduced by the fix for the first.*

## The shape of it

`Texture2D::SetData` and the mip-aware branch of `TerrainData::UploadRegionToGPU` rebuild
the mip chain as a **side effect**. Nothing says so at the call site; you get it by going
through the upload path. Every producer in the engine did, so the invariant was invisible
and unwritten.

A GPU-resident authoring path does not go through the upload path. It writes level 0 with
`imageStore` from a compute kernel, or with `glCopyImageSubData` from a snapshot. Levels
1..N keep whatever they last held, and **no API reports an error** — you have a texture
whose base level is correct and whose coarse levels are a photograph of the past.

## Why it was invisible here, and why that is the general case

Terrain geometry is not shaded from the heightmap, it is *positioned* from it. The
tessellation-evaluation stage picks a mip from the triangle footprint
(`include/TerrainHeightSampling.glsl`) and `textureLod`s it to place the vertex. So a stale
chain means:

- terrain near the camera is correct — fine footprint, low mip, level 0;
- terrain in the distance keeps its **pre-edit shape**;
- as the camera closes, the mip index drops and the surface *snaps* to the edit.

That is correct everywhere the artist is looking and wrong everywhere they are not. A human
tested the editor by hand and reported it working; the defect sat in the same build. It is
not a thing you notice by sculpting — you notice it by sculpting and then *walking away*.

None of the obvious tests see it either. A CPU/GPU parity test compares height fields, and
the field is right. An undo test compares texels, and the texels are right. A visual
evidence test compares frames from a close camera, and the frames are right.

## The invariant

> **Every writer of level 0 owes the mip chain.** Not every writer of the texture — every
> writer of level 0 specifically, because that is who invalidates levels 1..N.

Issue #716 created *four* such writers, and they were found one at a time:

| writer | how it was found |
|---|---|
| sculpt compute dispatch | code review |
| paint compute dispatch | same review, same reasoning |
| erosion compute dispatch | same review; it had *regressed* an existing `UploadToGPU()` that rebuilt the chain implicitly |
| `TerrainTextureUndoStack::Restore` (a `glCopyImageSubData`) | **the visual evidence test, one round after the other three were fixed** |

The fourth is the interesting one. Before mip regeneration existed anywhere, the visual test
passed: every level was uniformly stale, so before / after-stroke / after-undo all agreed.
Fixing the first three made the chain fresh after a stroke and left `Restore` as the only
level-0-only writer, so undo restored the near field and left the stroke in the distance —
arguably worse than not undoing. **Fixing one instance of this bug exposes the others**,
because a consistent lie looks like consistency.

If you add a fifth writer, it owes the chain too.

## What to do

`Texture2D::RegenerateMips()` exists for exactly this (issue #716). It is a no-op on a
single-level texture, so calling it is always safe.

- Call it after the dispatch, not per iteration. Coarse levels are only read when the frame
  is drawn, so a batch (`TerrainErosion::ApplyIterations`) refreshes once at the end;
  rebuilding a 1024² chain N times a frame is pure waste.
- On the GL path it is `glGenerateTextureMipmap` **plus the min-filter follow-up** — until a
  chain exists the sampler is left on a non-mipped filter, and leaving it there gives you a
  correct chain that is never sampled.
- Do not reach for "just call `UploadToGPU()` at settle" instead. It works only because the
  CPU mirror happens to have been synced first; the ordering is load-bearing, invisible, and
  one refactor from pushing a stale mirror over the GPU copy.

## The adjacent trap: `Ref<T>` propagates constness

Three separate signatures in #716 took `const Ref<Texture2D>&` / `const Ref<TerrainChunkManager>&`
and then could not call a non-const member on the pointee, because `Ref<T>`'s `operator->`
propagates the constness of the handle to the referent. `RegenerateMips()` is non-const, so
every helper that forwards a texture to it must take the `Ref` **by value** (one refcount
bump). The panel already documents this on `OnVoxelUpdate`; it recurred twice more in one
change, which is how you know it is a shape and not an accident.

## Related

- [terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md) — the descent that selects the
  mip, and why a gating flag no scene sets is a feature with zero coverage.
- [cpu-gpu-surface-parity.md](cpu-gpu-surface-parity.md) — the sibling failure: the two
  halves agree on the function and disagree about which space its arguments are in.
- [terrain-virtual-texturing.md](terrain-virtual-texturing.md) — every defect is a wrong
  address; a stale mip is a wrong address in the LOD dimension.
