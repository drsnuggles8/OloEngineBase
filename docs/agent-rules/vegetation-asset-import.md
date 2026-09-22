# Importing scanned vegetation

Rules for bringing a scanned plant into this repository, and for measuring whether it worked.
The tool is [tools/vegetation-import/](../../tools/vegetation-import/README.md); this file is the
part that generalizes beyond it.

## 1. A texture resolution tier does not shrink the mesh

Poly Haven (and most scan libraries) publish "1k / 2k / 4k / 8k". **That selects textures only.**
The geometry buffer is byte-identical at every tier — `pine_tree_01.bin` is 949 MB whichever you
ask for, and the *1k* package's own include list points at the **8k** directory's `.bin`.

Budget a scan by asking the files API for real sizes, never by assuming the tier means anything
about geometry. Choosing "1k" to keep a download small is a mistake that only shows up as a
download that is three orders of magnitude bigger than expected.

## 2. Modelled foliage cannot be decimated — bake it into cards

A scanned conifer models every needle. `pine_tree_01` carries 6.8 M triangles of them, and **91.7%
of that surface passes its own alpha test**, because the alpha map trims a needle EDGE rather than
cutting a shape out of a card. Two consequences:

- **Any collapse-based simplification destroys it.** Merging vertices across a thin needle strip
  produces a sliver, not a needle. Quadric error metrics do not help; the error is small and the
  result is mush.
- **The "it already has alpha, so it is card-based" inference is wrong.** Check coverage over the
  surface before believing it (rule 3).

The workable treatment is to rebuild the canopy from cross-cards and **bake the card texture out of
the scan**: project a cluster of real needles onto its own dominant plane and record what is there.
The transparency in the result is then real geometry coverage — a texel is clear because the scan
has no surface at that point — rather than a painted guess.

Solid parts (trunk, bark) are the opposite: they are closed surfaces and decimate fine. A plant
therefore needs **two opposite treatments in one import**, and which part gets which is authored,
not detected.

### Corollary: a grid cluster key with a UV term has a floor

Seam-aware vertex clustering keys on `(cell, coarse uv)` so that two vertices on opposite sides of a
UV wrap do not merge. That is correct, and it means **no cell size, however large, can merge two
vertices in different UV buckets**. A budget search then hits a floor and silently returns a mesh
well over budget — `pine_tree_01`'s bark stalled at 11,354 triangles against a budget of 3,500,
because each branch carries its own UV island. Coarsen the bucket count when the floor is above
budget, and report which count was used.

## 3. Measure the surface, not the sheet

**Averaging alpha over a whole texture answers a question nobody asked.** A UV atlas has unused
space, and on a scanned plant that space is usually *opaque*. `pine_tree_01`'s twig sheet reads
**69.5%** measured flat; the canopy that actually samples it reads **54.4%**. The flat figure
describes a canopy that does not exist.

Sample at points distributed over the mesh's own triangles, weighted by triangle area. This is the
same failure as measuring a shader effect over a whole frame when it occupies 3% of it, and it cuts
both ways: the number can be far too high (unused opaque atlas space) or far too low (a plant that
fills a fraction of its bounding square).

Two-sided assertions beat floors. A coverage floor alone passes a solid opaque rectangle — which is
exactly what pointing a billboard at a source UV atlas produces.

The engine applies this rule itself at layer load
([FoliageAlphaCoverage.h](../../OloEngine/src/OloEngine/Terrain/Foliage/FoliageAlphaCoverage.h),
issue #1399). It logs one `FoliageRenderer: layer '…'` warning when a texture passes an implausible
share of its surface at the layer's `AlphaCutoff`, with a band per role: card 2–90%, authored-mesh
part and impostor bake at least 30%. The editor's foliage inspector shows the figure under the
Alpha Cutoff slider. Read that warning before hunting a see-through plant in the renderer.

## 4. glTF and OBJ disagree about which way v runs

glTF puts UV (0,0) at the **top-left**, v increasing downward. Wavefront OBJ puts it at the
bottom-left, v increasing upward. Convert exactly once, in one place, and say where.

This is not a subtle shading difference. `pine_tree_01`'s twig atlas has needle strips in the lower
half and pine cones in the upper, so a flipped v bakes **brown cones onto every foliage card**. The
tell is that the result looks entirely plausible — a dead or autumnal tree — and every geometric
check passes. It was caught by looking at the baked card, not by any assertion.

## 5. Name a layer for what it draws

A foliage layer whose albedo is a picture of a different species is the defect
[#1398](https://github.com/drsnuggles8/OloEngineBase/issues/1398) records: one 512×512 grass cutout,
passing 17.9% of its texels at the authored cutoff, textured every species in the repository
including the pines. The symptoms read as renderer bugs — see-through trees, needles that are
streaks, a stippled far field — and none of them are.

So when a species is unavailable, **substitute explicitly and rename**. Poly Haven has no palm under
CC0; the palm layers became broadleaf layers named `Broadleaf`, and a `Lichen` layer that was
0.3–0.7 m tall with wind on became `Ferns`. Shipping a "Palms" layer drawing something else is the
bug, not the fix.

The same rule governs the far LOD rung. `AlbedoPath` is what the flat card samples, so it must be a
**billboard of the plant**, not the plant's source atlas.

## 6. The foliage authoring frame is load-bearing and only warned about

`FoliageRenderer` assumes every plant mesh is **base at the origin, unit height**, and rescales
nothing. A mesh authored at any other size is drawn at the wrong size in the near mesh *and* the far
impostor, consistently — so nothing looks inconsistent, it is just all wrong. The engine logs a
warning and carries on.

Normalize on import and assert it in a test
([VegetationAssetContractTest.cpp](../../OloEngine/tests/Rendering/VegetationAssetContractTest.cpp)).
Record the plant's real-world height alongside, because that is what a layer's `MinHeight`/
`MaxHeight` should reflect.

Related: the flat card quad is `x ∈ [-0.5, 0.5], y ∈ [0, 1]`, so a plant wider than it is tall
cannot be drawn at true width on the card rung. Squeeze the billboard rather than cropping it —
`fern_02` spreads 2.31× its height, and a straight projection cut 57% of its fronds off — and say
so in the asset's README.

## 7. Real geometry changes the workload, so bound the near rung

`MeshViewDistance` defaults to 30 m. Thirty metres of dense ground cover is a different workload
entirely: a 9-per-m² grass layer puts on the order of 25,000 tufts of real geometry inside that
radius. Set it per layer when a species goes from card to mesh, and expect benchmark numbers for any
scene you touch to move — that is a real result, not a regression.

## 8. Provenance is part of the asset

Follow `OloEditor/assets/models/InfiniteScanHead/`: a `LICENSE.md` naming the licence and its URL,
the original author, the exact download URLs, and **every modification made**. For a derived plant
the modification list is not a formality — what ships is not the scan, and somebody comparing the
two is otherwise entitled to think the import broke.

CC0 requires no attribution. Record it anyway: it costs nothing, and a benchmark manifest that
declares an asset `LicenseVerified: in-repo-file` is making a claim that needs the file to exist.
When a scene's assets change, re-run `tools/benchmark/reference_assets.py --write-hashes` — a
manifest still naming retired assets keeps passing while its provenance claim is false.
