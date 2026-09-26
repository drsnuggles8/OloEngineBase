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

ClosureV2 differs in exactly five ways (PBRCommon.glsl "PBR CLOSURE V2"
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
5. **An energy-conserving diffuse weight** (issue #1479). The Lambert term is
   weighted by the Kelemen / Kulla-Conty coupling
   `(1 − E_spec(μv)) (1 − E_spec(μl)) / (1 − E_spec_avg)` instead of
   Legacy's `1 − F(v·h)`, where `E_spec` is the Fresnel-aware directional
   albedo of the whole specular lobe, read from the same generated table
   (§4). A white dielectric then reflects exactly what it receives, where
   `1 − F(v·h)` made it reflect up to 1.83× at grazing.

The v2 sampler is the existing Heitz VNDF sampler + `G2/G1` weight (#706),
reused not rewritten; the CPU side gains production-precision mirrors
(`SampleGGXVNDF`, `PdfGGXVNDF`). The diffuse lobe stays cosine-sampled
Lambert. The mixture (`SpecularLobeProbability`, clamped to [0.1, 0.9]) is
shared with Legacy; §5 says why it did not move with the diffuse weight.

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
grid and 16-entry averages row, **as identical half-packed hex words in both
files** (one grid node per u32, two IEEE-754 halfs, decoded with
`unpackHalf2x16` on both sides, so the two languages evaluate the same
quantized values; quantization ≤ 2^-12 = 2.44e-4 absolute, audited at
generation). Each node stores two moments of the single-scattering lobe,
from the same 4096 deterministic Hammersley VNDF samples:

    x: 1 − Ess(μ, r),   Ess     = E[G2/G1]
    y: Schlick(μ, r),   Schlick = E[G2/G1 · (1 − v·h)^5]

— i.e. computed *with the v2 sampler's own estimator identity*, on the
*clamped* alpha, so each row is exactly the lobe v2 samples. Schlick's F is
affine in F0, so the lobe's albedo for any F0 is
`E_ss = F0 (Ess − Schlick) + Schlick`: that is what the diffuse coupling
(§2 item 5) subtracts, and why the second moment rides in the same word
rather than in a split-sum LUT (the IBL BRDF LUT is not available to the GPU
path tracer or the CPU twin). The averages row stores both moments
cosine-averaged over μ, `(1 − E_avg, Schlick_avg)`. Loss form (not Ess) is
stored because the compensation consumes `(1 − Ess)` directly and
near-mirror rows are ~1e-5.

**The grid is node-centred on a square-root axis** (issue #1478): node `j`
sits at `(j/15)²` in both μ and roughness, so the lookup coordinate of a value
`x` is `sqrt(x) · 15` and both endpoints are nodes — nothing clamps and nothing
extrapolates. The earlier grid was cell-centred (`(j + 0.5)/16`) and clamped
a quarter cell short of μ = 0, μ = 1 and r = 1, which read the white furnace
0.9615 at r = 1, μ = 1 and 1.0247 at r = 1, μ = 0.02. The square root is
there because `1 − Ess` is not smooth on a linear μ axis: it is 0 at μ = 0
(at a grazing view every visible facet reflects above the horizon and
`G2/G1 → 1`), peaks near μ ≈ α and then falls, so at low roughness the whole
feature sits inside the first linear cell. Measured against the independent
oracle over the whole domain, linear `j/15` nodes were no better than the
cell-centred table (worst furnace error 4.2 % for μ ≥ 0.05, 10 % below it);
square-root nodes of the same 16×16 size give 0.8 % and 2.4 %. μ = 0 is
baked at μ = 1e-4, the cosine floor `GgxSmithLambda` applies, so node 0 is the
closure the engine actually evaluates there. Lookups take AUTHORED roughness;
rows 0–3 all hold the r = 0.04 lobe the clamp produces.

The packing is not cosmetic. The first cut stored a plain `const float[256]`
in the GLSL include; it passed glslc but **failed NVIDIA's GL linker at
runtime** (`error C5025: lvalue in assignment too complex`) once the lookups
were inlined at `PBR_MultiLight.glsl`'s three lighting call sites —
SPIRV-Cross materialises a dynamically-indexed constant array as a local
temporary per site, and the driver's complexity limit tripped in the big
forward shaders while single-call-site probes compiled the same array fine
(272 scalar elements). The two-moment table is 68 `uvec4` constants
(`kGgxEnergyPacked[64]` + `kGgxEnergyAvgPacked[4]`, 1088 bytes), up from 34 —
a quarter of the element count that failed, and verified by linking every
forward and deferred shader on an RTX 4090 (GL) and on Vulkan with the
lookups inlined at all call sites. A UBO or texture LUT was rejected
deliberately: the UBO namespace has exactly one slot left, and the only free
texture units (57, 63) collide with `PBR_MultiLight`'s own Vulkan vertex-pull
SSBO bindings under the single-set model (the ADR 0011 item-A2 trap).

The compensation lobe is Kulla-Conty's reciprocal form with Schlick's
average-Fresnel factor:

    f_ms = F_ms · (1 − Ess(μv))(1 − Ess(μl)) / (π (1 − E_avg))
    F_ms = F_avg² E_avg / (1 − F_avg (1 − E_avg)),  F_avg = F0 + (1 − F0)/21

At `F_avg = 1` the hemispherical cosine integral of `f_ms` is exactly
`1 − Ess(μv)`, so the white furnace closes to 1 analytically — the property
the furnace test asserts, and the reason the tables need no fudge factor.

The diffuse coupling reads the same lookups:

    E_spec   = F0 (Ess − Schlick) + Schlick + F_ms (1 − Ess)
    f_d      = albedo (1 − metallic) / π · (1 − E_spec(μv)) (1 − E_spec(μl)) / (1 − E_spec_avg)

The last term of `E_spec` is the Kulla-Conty lobe's own albedo
(`∫ f_ms cos = F_ms (1 − Ess(μv))`, exactly). The cosine integral of `f_d`
over l is exactly `albedo (1 − metallic)(1 − E_spec(μv))`, so a white
dielectric's model albedo is 1 at every angle and roughness. Both factors
are symmetric in (v, l), so v2 stays reciprocal.

## 5. Deliberate clamps, approximations and biased modes (the honest list)

* `closureV2Roughness` clamp to `[0.04, 1]` — the only roughness guard in v2;
  a true delta mirror is out of scope for a raster closure.
* `f_ms` guard: compensation returns 0 when `1 − E_avg < 1e-4` (below the
  table's resolution; also guards the division). Near-mirror lobes shed
  nothing worth compensating.
* Energy-table bilinear interpolation error is accepted, and measured by
  `BsdfIdentityOracleTest` against an f64 quadrature that shares no code with
  the engine. The white metal furnace reads within [0.9902, 1.0064] over
  roughness 0.05–1 and view cosines 0.02–1, r = 1 and grazing included (it
  was [0.9615, 1.0247] on the cell-centred grid). A white dielectric reads
  within [0.9976, 1.0106] against its model's exact 1; the worst is at
  roughness 0.05, view cosine 0.02. The lookup itself is within 0.0066 of the
  true `1 − Ess` for μ ≥ 0.05 and 0.062 below it, where the loss peak at
  μ ≈ α is narrower than the first node spacings at low roughness; the
  averages row is within 2.8e-3 everywhere.
* **Sphere-AREA lights shade v2 materials through the Legacy
  representative-point evaluator** — Karis's normalization rescales D in a way
  with no v2 derivation yet. Documented at the dispatch site.
* **IBL/ambient stays split-sum with no multi-scatter term** in this slice;
  compensation applies to punctual direct lighting and the reference tracer.
* The diffuse lobe is still SAMPLED cosine-weighted, and the lobe mixture
  (`SpecularLobeProbability`) still compares F0 with the diffuse albedo. Both
  are variance choices — the one-sample estimator is unbiased for any value in
  (0, 1) — so the coupling did not force a change, and the mixture's measure
  is the normal-incidence reflectance, where the coupling's weight is ~1
  (`E_spec ≈ F0` head-on). Making it follow `E_spec(μv)` would move variance at
  grazing only, and would add a view dependence to a density that ReSTIR PT's
  shift charts re-evaluate at shifted vertices.
* Where a shading normal faces away from the viewer (N·V ≤ 0, e.g. a normal
  map at a silhouette), NdotV clamps to 0 and the coupling reads
  `E_spec(0)` — about 0.94 for a smooth dielectric — so the diffuse there is
  ~6 %, where `1 − F(v·h)` kept it near full. The MaterialLab probe's grazing
  capture shows this as a sharp N·V = 0 line, because it shades a hemisphere
  facing +z under a view vector 65° off it; real geometry meets it only in a
  silhouette sliver.
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
* **v2 is not frozen.** Until it becomes the engine default, a defect in v2
  is fixed in place — no ClosureV3, no compatibility flag, no frozen copy of
  the old v2 — and the repo's own content (its scenes and goldens) is
  rebased in the same change; #1478 and #1479 were the first such changes.
  After it becomes the default the same holds, under the repo's
  no-legacy-for-its-own-sake rule: the engine has no external users, so a
  look change is migrated, not versioned around.
* Regenerating the energy tables (e.g. to widen the grid) is a generator run
  emitting both files plus the recompute test keeping them honest. The
  generator is `tools/OloGgxEnergyTableGen` (issue #998) — build the target,
  run it from the repository root, and it overwrites `GgxEnergyTables.h` and
  `include/PBRClosureV2Energy.glsl` in place:

      cmake --build <build-dir> --target OloGgxEnergyTableGen
      <build-dir>/tools/OloGgxEnergyTableGen/<Config>/OloGgxEnergyTableGen[.exe] \
          --grid 16 --samples 4096 --avg-points 64 --avg-samples 2048

  The `<Config>` segment exists only under multi-config generators (both
  trees here are, so it is `Debug/` or `Release/`); drop it for a
  single-config tree, and drop `.exe` off Windows.

  Those defaults are what the committed tables were baked with (both
  moments, on the node-centred square-root grid of §4), so a clean
  `git diff` after a default run is the reproduction proof and `--check` is
  the same comparison without writing — a *byte-reproduction* check, valid
  only for the toolchain that baked the tables, never a substitute for the
  recompute pin. Raising the sample counts is a flag, not an edit; changing
  `--grid` is not purely a flag, because `ClosureV2Test`'s twin-drift pin
  hardcodes the table size, the packed-array lengths and the word counts and
  has to move in the same change; the grid must also be a multiple of 4 (four
  nodes per `uvec4`). The tool calls the engine's own
  `SampleGGXVNDFTangent` / `GgxSmithLambda` / `FresnelSchlick` /
  `ClosureV2Roughness` out of
  `ReferenceBRDF.h` rather than re-implementing them, which is why it is a
  C++ target and not a script: generator-vs-engine estimator drift is
  structurally impossible, not merely tested for. Until #998 this bullet was
  aspirational — the tables were marked GENERATED with no generator checked
  in, and the recipe lived only as prose in the header comment.
* Flipping any default — engine-wide, per-project, or per-import — is a new
  decision with a golden rebake attached, not a follow-on cleanup.
