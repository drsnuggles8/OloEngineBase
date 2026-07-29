# Cluster-LOD DAG simplification (VirtualMeshBuilder)

Hard-won specifics about `Renderer/VirtualGeometry/VirtualMeshBuilder.cpp` and the meshoptimizer
simplifier underneath it. From issues #629, #651 and #685.

---

## 1. A terminal group's boundary must stay locked for the WHOLE build

**The bug archetype.** The builder locks vertices shared between partitions before simplifying each
group — that is what makes LOD cuts watertight. That lock is recomputed per level from the
**pending** cluster set. A group that goes *terminal* (simplification stuck ⇒ `FLT_MAX` error) has
its clusters emitted and removed from `pending` — so from the next level on, the lock pass cannot
see the boundary that group shares with its still-simplifying neighbours. Those neighbours then
simplify away from a boundary that is **pinned forever** (a terminal group is selected at every
threshold and never refined away), and the cut cracks along that seam.

**Why it hides.** It only manifests when a terminal group is adjacent to a group that *does*
simplify. If every group simplifies (the common case) or every group is stuck (a tiny mesh), the
mix never occurs. It also only breaks the **coarse** cuts — fine cuts still select both sides at
the same level — so a test that samples only a couple of thresholds sails past it.

**The fix shape.** A build-lifetime frozen-position set, marked *before* `EmitGroup` consumes the
member geometry, OR-ed into the per-level locks (`FreezeTerminalGroupBoundary`). The general rule:
**a constraint created by a terminal/absorbing state has to outlive the iteration that created
it** — recomputing it per level from the live set is structurally wrong, not just incomplete.

**Testing it needs a fixture sanity assert.** A test for this is vacuous unless the DAG really
contains *both* terminal and simplified groups; assert that before asserting watertightness, or a
future config change silently turns the test into a no-op.

---

## 2. `meshopt_SimplifyPermissive` vs. position-welding: same cure, different cost and fidelity

meshoptimizer classifies a position carrying **more than two attribute wedges** as `Kind_Locked`
and never collapses it (`simplifier.cpp`, `classifyVertices`: *"more than one vertex maps to this
one; we don't have classification available"*). That is exactly what Assimp produces for any source
without normals — `aiProcess_GenNormals` splits every shared vertex so each face gets its own flat
normal, and `JoinIdenticalVertices` cannot re-merge them. Result: nothing collapses, every group
goes terminal, the DAG flattens to one level, and virtual geometry renders full source density
forever in the main view *and* every shadow cascade. Silent — the DAG passes every structural
validation. That was #651.

Two cures exist and they are **not** equivalent:

| | position weld (pre-#685) | `meshopt_SimplifyPermissive` (#685) |
|---|---|---|
| un-sticks the soup | yes | yes |
| UV seams on simplified levels | **welded shut** — canonical vertex's UV wins for the whole position | preserved, via `meshopt_SimplifyVertex_Protect` |
| simplifier working set | canonical positions only | every wedge |
| cook cost on real assets | — | neutral (+4.3 % Sponza, parity on the 7.2 M dragon) |

On the real #651 asset the permissive path carries the whole build alone: 7.2 M triangles →
17 DAG levels, 118 k clusters, path census `7205 permissive / 0 welded / 0 sloppy / 0 stuck`. The
fallback rungs are insurance, not the working path.

Measured on this repo (Debug, RTX 4090 box — see `VirtualMeshRealAssetCookTest`):

| asset | weld (before) | permissive (after) | delta |
|---|---|---|---|
| Sponza — 262 k tris, 25 submeshes, mostly welded | 1090.8 ms (n=5) | 1137.3 ms (n=5) | **+4.3 %** |
| xyzrgb_dragon — 7.2 M tris, the real #651 shape | 113–135 s (n=4) | 113–115 s (n=4) | **parity, within noise** |
| flat-normal grid — 131 k tris, 393 k split verts | 885.2 ms (n=5) | 1429.7 ms (n=5) | +61.5 % — **see below** |

**Do not trust the synthetic row, and be careful inventing mechanisms from it.** A uniform,
fully-split grid (every triangle owning its three vertices, 3× vertex blowup, trivial topology) is
*not* what Assimp's `GenNormals` produces on a real scan, and it is the only fixture that shows a
large regression. On the actual 7.2 M-triangle dragon — the wedge-heavy asset this whole issue
lineage is about — permissive and weld cook in the same time. The tempting story that "the weld was
secretly a 3× working-set optimisation" was built on the synthetic number and does not survive the
real asset. Cook cost is roughly neutral; the trade you are actually making is fidelity, not speed.

**Benchmarking discipline this cost us, twice:**
- **Match your n.** A single-sample A/B on Sponza reported +10 % (real figure +4.3 %), and a
  single-sample A/B on the dragon reported −19.5 % (real figure: parity). Both were one run landing
  in a distribution tail.
- **Distributions differ per side.** Sponza's baseline is tight (±0.7 %), the dragon's baseline
  swings ±10 % while its permissive side is ±1 %. Comparing a median against a single sample from a
  differently-shaped distribution is meaningless.
- **Discard the first run.** The dragon's first baseline sample (135 s vs a 110–125 s body) was a
  cold-file-cache/warmup outlier.
- **Validate the proxy before trusting it.** If a synthetic stand-in and the real asset disagree by
  60 points, the stand-in is wrong, not the asset.

**Protect bits mirror `clodMesh::attribute_protect_mask`.** Mark a vertex whose *protected*
attributes differ from its canonical same-position representative. Protect **UVs only** — a normal
wedge is what flat shading produces everywhere, and decimated geometry wants smoothed normals.
Compare the attributes **bit-exactly**: this is an identity question ("did the importer store the
same UV here"), not a proximity one, and `!=` would flag every NaN UV as a seam since `NaN != NaN`.

**Permissive cannot break group boundaries.** In `classifyVertices` the permissive promotion runs
*before* the `vertex_lock & meshopt_SimplifyVertex_Lock` pass, so explicit locks always win. Safe
to combine.

---

## 3. `meshopt_simplifyWithUpdate` is unusable while one vertex array serves every LOD level

It is the newest and highest-appearance-quality entry point, and it is a trap here: it updates
vertex **positions and attributes in place** for an optimal fit. A `VirtualMesh` keeps a single
vertex array shared by every level (simplification only ever removes indices, never adds
vertices), so an in-place update would rewrite the LOD-0 geometry under the leaf clusters and move
locked group boundaries. Using it needs per-level vertex copies and a new blob format.
`clusterlod.h` does not use it either.

---

## 4. `meshopt_simplifySloppy` does not preserve topology — guard it or skip it

Sloppy merges vertices into grid cells. It un-sticks anything, but it can fold two surface sheets
onto one edge or punch an interior hole, either of which breaks the watertight-cut invariant that
`clodBuild` never promises and `VirtualMeshBuilderTest` does. If you take it as a fallback rung,
validate the result before accepting it: still edge-manifold, and **exactly** the border edges the
group started with (a new border edge is a hole; a missing one means the shared boundary moved).
It has no absolute-error mode, so scale its relative error by `meshopt_simplifyScale` over the
deindexed subset, and amplify (reference default ×2) for appearance degradation the quadric never
saw. Locked vertices survive it — `computeVertexIds` gives each a unique id so it never merges.

---

## 5. Measuring a builder change

- **The cook is not reachable from a plain editor run.** `VirtualGeometrySponza.olo`'s
  `VirtualMeshComponent` handle does not resolve in a clean checkout, so the editor loads the scene
  and settles without registering a virtual mesh or writing any `.omesh`. Drive
  `VirtualMeshBuilder::BuildSet` directly instead (see `VirtualMeshRealAssetCookTest`).
- **Instrument on both sides of the A/B.** Timing added *by* the change cannot measure the
  baseline. Stash the change **selectively**, leaving the instrumentation applied to both.
- **Stale registry entries lie.** `AssetRegistry.oar` lists `Models/Stanford/xyzrgb_dragon.ply`
  (the #651 asset) but the file is not committed; a benchmark keyed on it silently SKIPs. Check the
  file exists, and prefer a procedural stand-in for the pathological shape.
- **An editor run rewrites `AssetRegistry.oar`** as a side effect — revert it, and any `.oloproj`
  you patched to change `StartScene`.
- Bumping `kVirtualMeshBuilderVersion` is **not** optional when geometry changes, and any new
  `VirtualMeshBuildConfig` field must be mixed into `CurrentCookFingerprint()` — the two together
  are what reject a stale `.omesh`.
