# ADR 0024 — Material kind is not the closure version

Status: accepted (issue #1231)
Date: 2026-09-14

## Decision

A material carries **three independent selectors**, not one:

| Selector | Question it answers | Where it lives | Numbering |
|---|---|---|---|
| `MaterialKind` | *What is this surface?* | `Renderer/MaterialKind.h` | append-only, fixed 2-bit G-Buffer field |
| `PBRModel` | *Which version of the closure evaluates it?* | `Renderer/PBRModel.h` (#975) | append-only, open-ended G-Buffer field |
| `SkinEvaluationModel` | *Which version of the skin transport evaluated its profile?* | `Renderer/SkinProfile.h` | append-only, per-asset |

They are separate fields in the material, separate keys in scene YAML and save-games,
separate properties in Lua and separate controls in the inspector. None of them may be
derived from another.

## Why

The engine has already paid for conflating these once. Before #975 the PBR closure was
whatever the shader happened to do, so correcting the GGX denominator would have restated
every scene and every golden in the repo at once; `PBRModel` exists so a corrected BRDF is
an opt-in per material. Issue #1231 asks for skin, and the cheap move is to add
`PBRModel::Skin` — one enum, one branch, done.

That is wrong in a way that only shows up later:

- **A kind and an algorithm version have different lifetimes.** A surface is skin forever.
  The maths that shades it will be corrected repeatedly — #1241's diffusion is already
  scheduled. On a single axis, shipping the corrected diffusion would mean either changing
  what every skin material *is*, or freezing the maths forever.
- **They are not mutually exclusive.** A skin material shades with a closure version:
  Legacy today, ClosureV2 when its author opts in, whatever comes next after that. One
  axis forces a row per combination — `SkinLegacy`, `SkinClosureV2`, … — which is the
  combinatorial explosion `RenderingPath` deliberately avoided when Virtual Shadow Maps
  landed on `ShadowSettings` instead of becoming a path (see ADR 0020's neighbours and
  `Renderer/Shadow/ShadowTechnique.h`).
- **The profile is a third thing again.** The parameters are authored in an asset shared by
  many materials; the version they were authored *against* belongs with them, not with the
  materials that name them. Putting it on the material would make "which transport does
  this profile assume?" a question with as many answers as there are materials.

## What this costs, concretely

The deferred path identifies a surface per pixel through the G-Buffer RT2 alpha flags lane.
Carrying the kind (2 bits) and the skin profile slot (3 bits) there lowered
`kPBRModelGBufferLaneMax` from **1023 to 31** — the lane is RGBA16F alpha, half is exact to
2048, and the model index now shifts up by 64 rather than by 2.

That is the price of the decision and it is paid deliberately. 31 closure models is roughly
fifteen times the current headroom, the static_assert in `PBRModel.h` is where a 32nd one is
rejected loudly, and the fix at that point is the one it always was: a wider lane or a
dedicated integer attachment. The alternative — packing the kind into the model field —
would have saved the bits by recreating the exact conflation this ADR exists to prevent.

The two new fields sit **below** the model in the lane, not above it, so the model keeps
its open-ended encoding: appending a closure model is still a one-line change in one file,
with no width and no mask anywhere in the transport.

## Consequences

- Adding a material kind is: one enumerator, `kMaterialKindCount`, the GLSL
  `OLO_MATERIAL_KIND_*` mirror, and the inspector combo (whose `static_assert` fails if you
  forget it). A fifth kind needs the lane re-encoded, and `MaterialKind.h` says so.
- Adding a skin transport version is: one enumerator in `SkinEvaluationModel`. No material
  changes kind, no scene is restated, no golden moves.
- Every deserializer treats all three as **discriminated values**: an out-of-range index
  rejects to the default and says so, rather than saturating onto a valid neighbour.
  A file naming a kind this build does not have is a file from the future, not a rounding
  problem.
- `GPUSceneMaterial` does **not** carry the kind, matching how #970's transmission block was
  handled: the per-draw material UBO is still the source for these fields on the GPU-driven
  path, and giving them a second home would create two records that can disagree.

## Alternatives rejected

**`PBRModel::Skin`.** Covered above: wrong lifetime, not orthogonal, and it would have made
`evaluatePBRClosure`'s version dispatch answer two questions.

**Scene alpha as the scattering channel.** The snow overlay already writes its SSS mask into
scene-colour alpha for `SSSRenderPass`. Issue #1231 explicitly rules out overloading it, and
it is right to: two kinds writing one channel means whichever runs last wins, silently.

> **Note (issue #1451).** Snow's mask later moved into skin's own hand-off lane (scene
> attachment 4, `.a`). That lane carries skin slots as `(s + 1) / 8` and snow as `-weight`.
> This is not the channel rejected above, for two reasons. First, each pixel has exactly one
> writer for this quantity: the surface shader decides, per pixel, whether it hands over skin
> or snow. Second, the value range is partitioned, so the decision can be read from the value
> itself: each consumer decodes its own range and reads the other as "not mine"
> (`include/SnowDiffusionCommon.glsl`). Scene alpha had neither property. Every opaque writer
> set it to 1 and every blended writer to its blend alpha, so its value depended on draw order.
> On the deferred path the snow weight crosses the G-Buffer in RT3.a, the #1256 "material
> profile" channel, because a snow blend is a position along a material profile axis.

**A seventh G-Buffer render target.** It would have cost no model headroom and is where this
goes eventually. Rejected for now because every one of the ~15 G-Buffer writers must write
every attachment (an unwritten MRT output is undefined), several of those shaders are owned
by branches in flight, and #1231 does not yet need a per-pixel channel — the scattering mask
it exposes is derived from the kind and the metallic value both paths already have.
