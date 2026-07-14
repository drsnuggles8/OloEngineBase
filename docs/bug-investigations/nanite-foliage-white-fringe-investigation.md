# Nanite (#629) — Sponza foliage white-fringe investigation

> Branch: `feature/nanite-virtualized-geometry-629`
> Symptom: Sponza's potted vines render with a bright white "lacework" — a
> silvery skeleton along the stems and a white rim on the leaf silhouettes —
> **only** through the virtualized-geometry path. Everything else in the frame
> (columns, curtains, floor, brick) is correct.
> Status: **RESOLVED.** Root cause in §2; fix and verification in §2.3.
> Every fact below is measured, not inferred. The point of this document is that
> the measurements were expensive and should not have to be repeated — and that
> the *plausible* explanations, several of which survived a long time, were all
> wrong.

## TL;DR

`getNormalFromMap` (`PBRCommon.glsl`) built its tangent frame from screen-space
UV derivatives. On a **UV-degenerate triangle** — corners sharing a texcoord, so
`dFdx(uv) == dFdy(uv) == 0` — the tangent collapsed to the zero vector,
`normalize(vec3(0))` returned **NaN**, and the NaN propagated through the TBN into
the G-Buffer normal and out into deferred lighting as a blown-out white pixel.
Sponza's mesh ships **314** such triangles; the cluster LOD DAG carries them into
LOD 0 and simplification creates more. **100% of the 1008 white pixels had a NaN
G-Buffer normal.**

The fix guards the degenerate tangent/bitangent and falls back to the geometric
normal. `1008 → 0` white pixels; the classic control is unchanged.

This was a **latent bug in the shared PBR code, not in the virtualized-geometry
path**. The classic path writes NaN normals too (~989 in the same frame, on the
red curtain). The virtual path merely landed them on near-black foliage, where
any specular blows out to white — which is why it looked like a Nanite bug.

## 0. How to reproduce and measure

Scenes:

* `OloEditor/SandboxProject/Assets/Scenes/VirtualGeometrySponza.olo` — Sponza as
  a `VirtualMeshComponent`. Requires the **Deferred** path (`VirtualGeometryPass`
  self-disables on Forward / Forward+).
* `OloEditor/SandboxProject/Assets/Scenes/VirtualGeometrySponzaControl.olo` — the
  **clean A/B control**: byte-identical scene (same sun, same IBL, same post
  settings, same camera) with only `VirtualMeshComponent` swapped for
  `ModelComponent`.

> **Do not use `SponzaDeferred.olo` as the control.** It is confounded: it has
> four extra point lights, a DamagedHelmet in frame, and a different component.
> It happens to agree at the pixels checked here, but that is luck, not design —
> the control scene above exists precisely so the only variable is the geometry
> path.

Camera used for every number in this document:

```
position [-4.5, 1.2, 2.6]   target [-1.2, 0.9, 1.4]   path: deferred
```

**Metric.** "Near-white pixel count" = pixels with `r, g, b` all `> 190` in the
vine region `x ∈ [500, 1000]` of the **native 1411×942** capture of
`FXAAColorTexture` (`olo_render_capture_target {maxWidth: 4096, forceFrame: true}`).
Do not measure on `olo_screenshot` output — it is downscaled to 1024 wide, and
the fringe is ~1px, so a downscaled sample lands off it.

| Configuration | near-white |
| --- | --- |
| Virtual (baseline) | **1008** |
| Classic control | **0** |

## 1. The mechanism (measured)

`olo_render_probe_pixel` at the brightest fringe texel, (882, 228):

| channel | fringe pixel | neighbouring leaf body |
| --- | --- | --- |
| finalColor | **0.859** (white) | 0.345 |
| albedo | (0.020, 0.016, 0.016) — near black | (0.051, 0.051, 0.020) |
| metallic | 0.0 | 0.0 |
| **roughness** | **0.007** | **0.749** |
| emissive | 0 | 0 |
| ao | 1.0 | 1.0 |

A **mirror-smooth, near-black surface** under a strong sun + IBL is a pure
specular highlight — i.e. white. That is the entire mechanism. The white is not
emissive, not a missing alpha test, and not a blown-out albedo; it is
`roughness → 0`.

Roughness comes from the metallic-roughness texture's green channel. It reads
~0 because the sample lands **outside the leaf** in the MR atlas, where the
texture is black.

## 2. The cause: NaN G-Buffer normals from UV-degenerate triangles

### 2.1 A wrong turn worth recording

The roughness reads ~0 because **the whole G-Buffer normal is NaN**, and a NaN
normal makes the deferred lighting resolve to white regardless of what the other
channels say. But before that was found, a plausible and completely wrong theory
was pursued for a long time, so it is written down here to stop the next person
walking the same path.

Writing `v_TexCoord` into the G-Buffer albedo in both `VirtualMeshGBuffer.glsl`
and `PBR_GBuffer.glsl` and probing identical pixels appeared to show **different
UVs at the same depth**:

| pixel | depth (virtual / control) | UV virtual | UV control | |
| --- | --- | --- | --- | --- |
| (760, 350) | 2.4730 / 2.4730 | (0.839, 0.596) | (0.839, 0.596) | match |
| (700, 300) | 2.4456 / 2.4408 | (0.326, 0.369) | (0.639, 0.667) | "wrong" |
| (882, 228) | 2.6228 / 2.6148 | (0.098, 0.314) | (0.020, 0.333) | "wrong" |

Combined with "all non-foliage geometry probes identical depths to four decimals",
this looked exactly like a **position-weld** corrupting UVs: right vertex, wrong
texcoord. `meshopt_generatePositionRemap` ("same position ⇒ same index") does
collapse UV-seam duplicates, and Sponza's leaf quads *do* share corner positions
with different UVs. The story was coherent. It was also false.

**It was disproved by going and checking the data instead of reasoning about it.**
A probe over the real `.omesh` blobs showed the DAG's LOD-0 triangles are an exact
multiset match of the source submesh triangles **including UVs** (262,267 triangles,
**0** mismatches). No position-weld leakage anywhere. The differing UV readings were
real but *misread*: those pixels show a **different visible fragment** — a
UV-degenerate sliver winning the depth race in the virtual path — not a corrupted
vertex. Two paths can disagree about *which* fragment is on top without either
one's vertex data being wrong.

**Lesson: "the depths match, so it's the same surface" is not sound.** Depths can
match to 3 decimals and still be different fragments.

### 2.2 The actual root cause

`OloEditor/assets/shaders/include/PBRCommon.glsl` (pre-fix, ~:284), inside
`getNormalFromMap`, which builds a tangent frame from screen-space derivatives:

```glsl
vec3 T = normalize(Q1 * st2.t - Q2 * st1.t);   // normalize(vec3(0)) == NaN
vec3 B = -normalize(cross(N, T));
```

On a **UV-degenerate triangle** — one whose three corners share a texcoord, so the
interpolated UV is constant and `dFdx(v_TexCoord) == dFdy(v_TexCoord) == 0` — that
tangent collapses to the zero vector. `normalize(vec3(0))` is NaN. The NaN poisons
the TBN, the returned normal, the octahedral encoding written into
`GBufferNormal`, and finally the deferred lighting, which resolves it as a
blown-out white pixel.

Sponza's source mesh ships **314** such triangles (zero UV area, non-zero 3D area);
the DAG carries them into LOD 0 verbatim and simplification produces more (1321
DAG-wide).

Evidence:

* G-Buffer probe at a white pixel (604, 32): albedo, roughness (0.2217), AO and
  depth are **bit-identical** between virtual and control. Only the normal differs
  — control `(-0.944, -0.120, 0.308)`, virtual **NaN**.
* Shader instrumentation over MCP: `|v_Normal| ≈ 1` (the interpolated normal is
  fine), `isnan(N) == 1` after `getNormalFromMap`, and
  `dFdx(v_TexCoord) == dFdy(v_TexCoord) == 0` at exactly those pixels (non-zero at
  healthy ones).
* **100% of the 1008 white pixels have a NaN G-Buffer normal.**

### 2.3 Fix and verification

Guard the degenerate tangent and bitangent and fall back to the geometric normal —
with no UV gradient there is no tangent frame to rotate the tangent-space normal
by, so the geometric normal is the only meaningful answer. The guard is written
`!(x > eps)` rather than `x < eps` so a **NaN** derivative also takes the fallback.

Pinned by `PbrNormalMapTest.DegenerateUvGradientFallsBackToGeometricNormal`
(`OloEngine/tests/Rendering/PropertyTests/PbrPropertyTests.cpp`, with the probe
shader `OloEditor/assets/shaders/tests/PbrDegenerateUvProbe.glsl`). The test
**fails on the pre-fix shader** — 3136/3136 pixels NaN — and passes after.

| configuration | white px in vine region |
| --- | --- |
| virtual, pre-fix | **1008** |
| virtual, post-fix | **0** |
| classic control | 0 (unchanged — no regression) |

Full suite: 4593 passed, 0 failed.

### 2.4 This was never a Nanite bug

`PBRCommon.glsl` is shared. The **classic path writes NaN normals too** — ~989 in
the same frame, on the red curtain — where the "r,g,b all > 190" metric never
counted them and where a NaN on a mid-tone albedo is far less visible. The virtual
path simply landed its NaNs on **near-black foliage**, where any specular blows out
to white. That is the whole reason it presented as a virtualized-geometry defect.

The virtual path had 25,987 NaN-normal pixels region-wide vs. the classic's 989.
That gap is *presumed* to come from the extra UV-degenerate triangles created by
simplification plus different depth-race winners (`GL_LESS` + nondeterministic
cluster compaction order, vs. the classic depth-prepass `GL_LEQUAL` order), but
that decomposition was **not proven**.

## 3. Eliminated — each with a measurement, not an argument

| Hypothesis | How it was killed |
| --- | --- |
| Compute **software rasterizer** | `olo_virtual_geometry_set {swRasterMode: "disabled"}` → **1008**, identical to `auto`. The fringe is entirely hardware-rasterized. |
| **Normal-cone backface cull** rejecting two-sided leaf clusters | Disabled the cone test outright in `VirtualClusterCull.comp` → **1008**, no change. (Plausible-sounding and wrong.) |
| **Hi-Z occlusion cull** dropping visible clusters | Disabled → **1523**, i.e. *worse*. So the white pixels are geometry being **drawn**, not geometry going missing. |
| **Texture mip level** (coarse mip bleeding black MR into leaf edges) | `textureQueryLod` written into the G-Buffer: **both paths sample LOD 0.0**. |
| **Material data** (wrong alpha mode / cutoff / wrong texture bound) | `olo_material_get` on the plant submesh: `alphaMode: Mask`, `alphaCutoff: 0.5`, `metallicRoughness` map id bound, `useMetallicRoughnessMap: true`, `twoSided: true`, `roughnessFactor: 1.0`. All correct. |
| **Back-face normals** un-flipped on two-sided geometry | Adding `if (!gl_FrontFacing) N = -N;` → **1012**, no change. Discarding back faces entirely removes only 23% (1008 → 776), so most of the white is on **front** faces. |
| **DAG LOD / simplification** | Forcing `ErrorThresholdPixels = 0.05` (LOD 0, the original triangles) → **identical wrong UVs**. The corruption is in the **base cluster geometry**, not in the simplified levels. |
| Geometry duplication (the 25× mesh-cache bug) | Real bug, fixed separately (see `Model::CreateCombinedMeshSource`); the fringe survived it. Not this. |
| Emissive channel | Probed: **0** at the fringe. |
| Scene lighting differing between paths | Both scenes carry an identical sun (direction, colour, intensity 6). |
| **Corrupt UVs in the cooked DAG** (the theory that survived longest — see §2.1) | Probed the real `.omesh` blobs: LOD-0 triangles are an exact multiset match of the source **including UVs** (262,267 tris, 0 mismatches). The cook is clean. |

Note how much of this table is *plausible mechanisms that were not the bug*. Every
one of them would have been a reasonable thing to "fix" on inspection alone. The
only reason none of them shipped is that each was made to produce a number first.

### A methodology failure worth recording

At one point `olo_entity_set_field` was called with `id` instead of `entity`. The
call **failed**, the failure was easy to skim past, and several subsequent
experiments were run believing the LOD had been forced to 0 when it had not —
which produced a wrong "DAG ruled out" conclusion that had to be retracted.

Two consequences, both now fixed:

* `olo_entity_set_field` now reads the value **back out of the live component**
  after the write and reports `changed` / `clamped` explicitly, so a caller can
  verify a write landed instead of trusting that the call returned.
* `VirtualMeshComponent` was not even in the field registry (9 components were).
  The registry is now **generated** from OloHeaderTool's component scan (89
  components / 712 fields).

**Rule for the next person: never accept a negative result from an experiment
whose setup you did not verify took effect.**

## 4. Follow-ups left open

* **Confirm the classic path's ~989 NaN normals are gone.** The fix is in shared
  code so they should be, and the classic control's final colour is byte-identical
  before/after (hence regression-free), but the `isnan` diagnostic was not re-run
  against `PBR_GBuffer` post-fix.
* **Consider rejecting UV-degenerate triangles at import**, not just surviving them
  in the shader. 314 of them ship in Sponza; they carry no texture information and
  contribute nothing but a fallback normal. Dropping them in `MeshOptimization`
  (alongside the existing degenerate-triangle handling) would be strictly cheaper
  than paying the guard per-fragment forever.
* **The simplifier creates more of them** (1321 DAG-wide vs 314 in the source).
  Worth checking whether `meshopt_simplifyWithAttributes` can be told to avoid
  collapses that zero a triangle's UV area.

### A trap that did NOT apply here — but will, next time

The cooked DAG is **cached in the `.omesh`** under `OloEditor/SandboxProject/Cache/`,
and `VirtualMeshRegistry::RegisterMeshSource` takes the `HasVirtualMeshBlob()` fast
path. After **any builder change**, delete the Sponza cache entry or you will
re-test the old cook, see no change, and conclude your fix did nothing. (It did not
bite this investigation only because the cook turned out to be innocent.)

## 5. Tooling that made this tractable

All of it landed in #607 during this investigation; without it the above would
have been days of shader-patching and editor restarts:

* `olo_render_probe_pixel` — per-pixel **numeric** G-Buffer / depth / final-colour
  readout. The question "which channel is anomalous at this pixel" has a one-number
  answer; an image is the wrong output format for it.
* `olo_virtual_geometry_set` / `_stats` — the SW-raster mode and debug views existed
  in `VirtualMeshRegistry` and were reachable from nowhere. Toggling the software
  rasterizer off is a one-call experiment.
* **Pass-owned shader hot-reload** — `VirtualMeshGBuffer.glsl` and the
  `VirtualCluster*.comp` compute shaders reload by name over MCP. Five hypotheses
  were tested and killed in minutes; each would previously have cost a full editor
  restart (~2 min + scene reload).
* `olo_material_get` — the *resolved* `PODMaterialData` actually uploaded for a
  draw. "Is the plant arriving as Mask with cutoff 0.5?" is a data question, not a
  pixel question.

## 6. Related engine bugs surfaced along the way

* **`Model::CreateCombinedMeshSource` duplicated geometry N× on a warm `.omesh`
  load** — every `Mesh` is a submesh *view* into one already-combined source, but
  the concatenation loop copied each mesh's *entire* source. Sponza loaded as
  4,812,300 verts / 6.5M tris instead of 192,492 / 262k, which stalled the DAG cook
  indefinitely. Fixed.
* **The classic `MeshComponent` path never consults a submesh's imported material**
  (`Scene.cpp:~7281` resolves `MaterialComponent → engine default` only). Only
  `VirtualMeshComponent` does the full `override → imported → default` precedence.
  Surfaced by `olo_material_get`; **still open** — worth its own issue.
