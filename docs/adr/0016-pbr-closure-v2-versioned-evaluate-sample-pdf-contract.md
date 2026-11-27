# PBR closure and the Evaluate/Sample/Pdf contract

**Decision, updated by #1497.** ClosureV2 is the engine's standard PBR closure. This
supersedes #975's requirement to preserve Legacy pixels and offer a per-material selector.
The CPU and GLSL implementations still share the contract and the generated energy tables.

## 1. Material data and migration

Every standard PBR material uses the same closure. Existing scene YAML loads through the
normal material reader. The retired `PBRModel` key has no effect and disappears on save.
The imported-material codec already stored the material inputs. Save-game format 54
removes the model byte from materials and rejects format 53 at the file header.

The per-draw UBO keeps padding at byte 92 and the GPU scene record keeps padding at
byte 56 to preserve the offsets of later fields. In the deferred flags lane, bit 0
marks unlit, bits 1–2 identify the material kind and bits 3–6 carry the profile slot.
The fixed-function average resolve applies to emissive RGB. Flags resolve from one lit
sample when available. The kind and profile keep their own meanings under MSAA.

## 2. ClosureV2

1. Evaluate the GGX single-scatter lobe with height-correlated Smith visibility and
   Schlick Fresnel at the half vector. The caller supplies the incident cosine.
2. Clamp perceptual roughness to [0.04, 1] before forming `alpha = roughness²`.
   The vector form of D uses the squared cross product to preserve a narrow highlight.
   The specular sampler and its PDF use this same alpha.
3. Restore single-scatter losses with the reciprocal Kulla–Conty term (§4).
4. Couple Lambert diffuse to the directional albedo of the full specular lobe. For a
   white dielectric the cosine integral of the combined closure equals one.
5. Sample a mixture of diffuse cosine and GGX visible normals. The integrator divides
   by the mixture density and uses that same density for MIS.

## 3. Implementation and parity

`PBRCommon.glsl` owns the shader implementation. The CPU reference uses
`ReferenceBRDF.h` through `BSDF::Evaluate`, `BSDF::Sample` and `BSDF::Pdf`.
`ClosureV2GpuParityTest` and `ClosureV2SampleGpuParityTest` compare compiled shader
outputs to the CPU twins. The independent BSDF oracles cover energy, reciprocity,
sampling distributions and the mixture density. The energy table is generated once
for both languages.

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
baked at μ = 1e-4, the cosine floor `GgxSmithLambda` applies (the estimator's
`G2/G1` needs a finite Λ(μv)), and the lookup places it at coordinate 0; the
limit it stands in for is `1 − Ess → 0`, and the loss at 1e-4 is 0.012 at the
smoothest rows and below 1e-3 from roughness 0.2 up — inside the table's own
grazing resolution. Lookups take AUTHORED roughness;
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
* **Sphere-area lights** use the light centre for diffuse and multiple scattering and
  the direction to the Karis representative point for single scattering. The GGX peak is
  `1 / (π α²)`. Broadening its apparent width to `α′ = min(1, α + radius / (2 distance))`
  therefore scales peak height by `(α / α′)²`. Apply that factor to single scattering
  with the original material visibility. Multiple scattering is broad and has no such
  peak. At zero radius the representative direction becomes the centre direction and
  the peak ratio is one.

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
* The PDF returns zero below the horizon. A rejected draw counts as a zero contribution.
* The forward, Forward+ and deferred paths use this closure for meshes, terrain, voxels
  and the reflected lobe of foliage. Skin's layered surface and oral coat retain their
  profile parameters. Material thumbnails use the same evaluator with their own light rig.
  Water, fibre and unlit particles have separate shading models.

## 6. Consequences

* The GPU path tracer and ReSTIR PT use the same finite-width VNDF reflection and cosine
  mixture as the CPU reference. Correct a closure formula in both languages, verify the
  estimator and inspect affected golden images in the same change.

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
