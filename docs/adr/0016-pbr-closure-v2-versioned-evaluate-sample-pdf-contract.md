# PBR closure v2 — a versioned Evaluate/Sample/Pdf contract, specified as pinned twin implementations

Issue #975. The renderer's material closure gains an explicit version
(`PBRModel { Legacy, ClosureV2 }`) and a three-function contract
(`Evaluate` / `Sample` / `Pdf`) that raster lighting, the CPU reference path
tracer (#709), and any future GPU path tracer / ReSTIR pass share. This ADR
records the decisions the issue asked to have recorded — where the version
lives, what v2 is, how the one-specification problem is solved, and every
deliberate clamp and approximation. It is the contract for all later transport
estimators.

---

## 1. The version is PER-MATERIAL, and the default is the constructor default

`PBRModel : u8 { Legacy = 0, ClosureV2 = 1 }` (`Renderer/PBRModel.h`) is a
field on `Material`, copying the `MaterialType` precedent exactly. It travels:

    Material::m_PBRModel
      -> PODMaterialData::pbrModel            (command packet + its == cache key)
      -> PBRMaterialUBO::PBRModel             (the former Pad2 lane; 144 B unchanged)
      -> u_PBRModel in PBRMaterialProperties  (all 8 GLSL declaration sites)
      -> forward: passed into calculateLightContribution / fplusEvaluateTileLights
      -> deferred: bit 1 of the G-Buffer RT2 alpha "MaterialFlags" lane
                   (bit 0 stays the unlit flag; Legacy still writes a = 0.0,
                   so existing G-Buffer bytes are unchanged)
      -> ReferenceMaterial::Model             (ReferenceSceneBuilder copies it)

Serialization: scene YAML writes `PBRModel` only when non-Legacy (existing
scenes stay byte-identical) and REJECTS an out-of-range index back to Legacy —
a discriminated value must never saturate to a different valid model. Save-games
append it version-gated (`kSaveGameFormatVersion` 24→25). The numbering is on
disk: append, never renumber.

There is deliberately NO global/scene default knob in this slice. "The project
default is Legacy" is the constructor default; flipping the engine default is a
later, deliberate decision that per the issue happens only after parity and
visual review. A scene-level default would also have to survive `Scene::Copy`
and both serializer sites — cost with no current consumer.

## 2. What ClosureV2 is (and Legacy stays, bit for bit)

Legacy is the shipped Cook-Torrance closure, frozen: Schlick-GGX `k=(r+1)²/8`
geometry, `max(denom, 1e-4)`-clamped GGX, `kD = 1 - F(H)` Lambert, no energy
compensation. Every existing scene, golden and test keeps it.

ClosureV2 differs in exactly four ways (PBRCommon.glsl "PBR CLOSURE V2"
section; C++ twins in `ReferenceBRDF.h`):

1. **One geometry term** — the height-correlated Smith *visibility* form
   (`visibilitySmithGGXCorrelated`, #904's corrected `alpha = roughness²`).
   Schlick-GGX stays reachable only through Legacy.
2. **Near-mirror handling clamps ALPHA, not the denominator** —
   `closureV2Roughness` clamps perceptual roughness to
   `[MIN_ROUGHNESS = 0.04, 1]`, so `alpha ≥ 0.0016` and the *unclamped* NDF
   (`distributionGGXUnclamped`) is finite everywhere. The lobe narrows and
   brightens toward a mirror as authored roughness → 0 instead of collapsing
   toward black.
3. **Kulla-Conty multiple-scattering energy compensation**, driven by generated
   Ess/E_avg tables (§4). Reciprocal by construction; closes the white furnace
   exactly at F_avg = 1.
4. **One D for Evaluate, Sample and Pdf.** Because the guard moved from the
   denominator to alpha, the evaluated D *is* the sampled D *is* the density's
   D. Legacy structurally cannot have this (its evaluation-side clamp must
   never enter a density — see `DistributionGGXSamplingDensity`'s notes); v2's
   consistency is by construction, which is the property path tracing and
   ReSTIR actually depend on.

The v2 sampler is the existing Heitz VNDF sampler + `G2/G1` weight (#706),
reused not rewritten; the CPU side gains production-precision mirrors
(`SampleGGXVNDF`, `PdfGGXVNDF`). The diffuse lobe stays cosine-sampled
Lambert. The mixture (`SpecularLobeProbability`, clamped to [0.1, 0.9]) is
shared with Legacy.

## 3. One authoritative specification — pinned twins, not codegen

The issue offered two strategies: generate C++ + GLSL from one description, or
keep explicit twin implementations pinned by cross-language parity tests plus a
formula ledger. **We chose the second**, because it is the mechanism the repo
already trusts and polices (`ReferenceBRDFGpuParityTest`, THE ALPHA LEDGER,
the #904/#926 history), and the issue says to extend rather than replace it.
The contract's homes:

* GLSL — `PBRCommon.glsl` "PBR CLOSURE V2": `closureV2Evaluate`,
  `closureV2SampleBRDF`, `closureV2Pdf`.
* C++ — `Renderer/PathTracing/PBRClosureBSDF.h`: `BSDF::Evaluate/Sample/Pdf`,
  dispatching on `ReferenceMaterial::Model`. The Legacy branch is the former
  PathTracer-internal code moved verbatim (the bit-identical render hashes pin
  that move); the integrator consumes the contract through it.
* The pins — the GPU parity probe extended over the v2 functions, the headless
  ClosureV2 contract/consistency tests, and the energy-table recompute test.

The ONE genuinely generated artifact is the energy table pair (§4), where a
single generation procedure emits both languages' constants.

## 4. The energy tables

`GgxEnergyTables.h` / `include/PBRClosureV2Energy.glsl` hold the same 16×16
`1 − Ess(mu, r)` grid and 16-entry `1 − E_avg(r)` row, **as identical
half-packed hex words in both files** (two IEEE-754 halfs per u32, decoded
with `unpackHalf2x16` on both sides, so the two languages evaluate the same
quantized values; quantization ≤ 2.3e-4 absolute, audited at generation).
`Ess = E[G2/G1]` over 4096 deterministic Hammersley VNDF samples — i.e. the
table is computed *with the v2 sampler's own estimator identity*, on the
*clamped* alpha, so each row is exactly the albedo of the lobe v2 samples.
Loss form (not Ess) is stored because the compensation consumes `(1 − Ess)`
directly and near-mirror rows are ~1e-5.

The packing is not cosmetic. The first cut stored a plain `const float[256]`
in the GLSL include; it passed glslc but **failed NVIDIA's GL linker at
runtime** (`error C5025: lvalue in assignment too complex`) once the lookups
were inlined at `PBR_MultiLight.glsl`'s three lighting call sites —
SPIRV-Cross materialises a dynamically-indexed constant array as a local
temporary per site, and the driver's complexity limit tripped in the big
forward shaders while single-call-site probes compiled the same array fine.
34 `uvec4` constants sit an order of magnitude below that cliff. A UBO or
texture LUT was rejected deliberately: the UBO namespace has exactly one slot
left, and the only free texture units (57, 63) collide with `PBR_MultiLight`'s
own Vulkan vertex-pull SSBO bindings under the single-set model (the ADR 0011
item-A2 trap).

The compensation lobe is Kulla-Conty's reciprocal form with Schlick's
average-Fresnel factor:

    f_ms = F_ms · (1 − Ess(μv))(1 − Ess(μl)) / (π (1 − E_avg))
    F_ms = F_avg² E_avg / (1 − F_avg (1 − E_avg)),  F_avg = F0 + (1 − F0)/21

At `F_avg = 1` the hemispherical cosine integral of `f_ms` is exactly
`1 − Ess(μv)`, so the white furnace closes to 1 analytically — the property
the furnace test asserts, and the reason the tables need no fudge factor.

## 5. Deliberate clamps, approximations and biased modes (the honest list)

* `closureV2Roughness` clamp to `[0.04, 1]` — the only roughness guard in v2;
  a true delta mirror is out of scope for a raster closure.
* `f_ms` guard: compensation returns 0 when `1 − E_avg < 1e-4` (below the
  table's resolution; also guards the division). Near-mirror lobes shed
  nothing worth compensating.
* Energy-table bilinear interpolation error (16×16 grid, ≲1% mid-table) is
  accepted; the furnace tolerance covers it.
* **Sphere-AREA lights shade v2 materials through the Legacy
  representative-point evaluator** — Karis's normalization rescales D in a way
  with no v2 derivation yet. Documented at the dispatch site.
* **IBL/ambient stays split-sum with no multi-scatter term** in this slice;
  compensation applies to punctual direct lighting and the reference tracer.
* The diffuse split keeps `(1 − F(H)) (1 − metallic)` Lambert. F at the half
  vector is symmetric in wo/wi, so v2 is reciprocal (asserted by test); the
  split is an energy heuristic, not a measured coupled term.
* v2's `Pdf` reports 0 below the horizon (matching the Legacy convention);
  `Sample` may produce a below-horizon direction with `Value = 0`, which the
  one-sample estimator scores as a zero-contribution draw.
* Terrain, voxel, water, foliage, particles and snow keep their existing
  closures/paths regardless of model — per the issue's out-of-scope list.
* ~~**The G-Buffer flags lane is not average-safe.**~~ **Fixed in #996, and no
  longer a limitation.** It was: in the resolved-MSAA deferred mode (MSAA > 1
  with per-sample lighting off — two non-default, session-local toggles) the
  resolve averaged samples, and an averaged bitfield decodes wrongly at
  silhouettes. `GBuffer::Resolve()` now keeps the average blit for RT2's
  emissive RGB (which is what leaves a Legacy-only frame byte-identical) and
  then runs `GBufferFlagsResolve.glsl`, which overwrites the alpha channel
  alone with `texelFetch` of a **single sample** — the first lit one, so a pixel
  a real surface covers is never read as unlit — a value some sample wrote, and
  an exactly-defined fetch on both backends and every vendor, unlike the
  averaged encoding whose half-way cases included an implementation-defined
  `round()` tie-break. The same change removed the lane's single-bit model
  ceiling: it carries the whole `PBRModel` index shifted past the unlit bit and
  the decode is a plain `>> 1`, so appending a model can no longer truncate to
  Legacy on the deferred path while Forward shades it correctly. The layout
  still has exactly one executable home (`oloEncodeGBufferPbrFlags`) and one
  decode site (`DeferredLightingShared.glsl`); the only remaining bound is
  exact integer representation in RGBA16F, asserted at `kPBRModelGBufferLaneMax`
  in `PBRModel.h`.
* **Asset previews render their own closure regardless of model** —
  `MaterialPreview.glsl` is a standalone closure copy that predates the
  dispatch and does not include PBRCommon; the `MaterialAsset` lane and the
  imported-material binary codec carry no `PBRModel` field (importers cannot
  author one). Each site carries a comment pointing here.

## 6. Consequences

* A future GPU path tracer / ReSTIR pass consumes `closureV2Sample/Pdf`
  directly; the density its MIS uses is the same function the CPU integrator
  divides by, which is the entire point.
* Changing any v2 formula is a TWO-file edit (GLSL + C++ twin) — same law as
  the Legacy BRDF, enforced by the same parity probe.
* Regenerating the energy tables (e.g. to widen the grid) is a generator run
  emitting both files plus the recompute test keeping them honest.
* Flipping any default — engine-wide, per-project, or per-import — is a new
  decision with a golden rebake attached, not a follow-on cleanup.
