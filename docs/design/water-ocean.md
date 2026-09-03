# Water & Ocean — Design Record

**What this is:** the design record for the water/ocean system as built — the
FFT ocean and its cascades, the spectrum models, buoyancy, caustics, underwater
rendering and planar reflections. Roughly 57 code comments cite its sections by
number (`§1`, `§1.2`, `§5.1`, `§7.2`, …), so the section numbering is a stable
interface: **do not renumber sections**, and add new ones at the end.

**What this is NOT, any more:** a backlog. It used to be
`water-ocean.md` and carried unshipped ideas alongside the
shipped design, which meant the shipped half rotted (items stayed listed as
"future" years after they landed) and the unshipped half was invisible to the
issue tracker and the picker. The remaining open work now lives in GitHub:

| Was | Now |
|---|---|
| §5.3 shore wave deformation | [#1033](https://github.com/drsnuggles8/OloEngineBase/issues/1033) |
| §2.2 foam advection, §2.3 spray particles, §7.3 rain impact | [#1034](https://github.com/drsnuggles8/OloEngineBase/issues/1034) |
| §4.1 projected grid, §4.2 adaptive tessellation, §6.3 hex tiling, §6.4 compute Gerstner | [#1035](https://github.com/drsnuggles8/OloEngineBase/issues/1035) |

§7.4 (foam trail persistence) is **shipped** — the wake disturbance field from
issue #967 is exactly that, so it is not in the table.

---

## 1. FFT Ocean Simulation (High Impact / High Effort)

The current system sums 8 Gerstner waves (2 primary + 6 detail octaves with
domain warping). This produces convincing results for a moderate number of
waves, but cannot match the spectral realism of thousands of superimposed
frequency components.

### Status — **partially shipped (single cascade, toggleable)**

A Tessendorf spectral ocean is wired end to end behind a `WaterComponent`
toggle (`m_UseFFT`); the Gerstner path stays the default for comparison /
fallback. Shipped:

- ✅ **Phillips spectrum + dispersion + inverse-FFT math (CPU reference).**
  [`OceanSpectrum`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanSpectrum.h)
  builds the frequency-domain heightfield $\tilde{h}_0(\mathbf{k})$, time-evolves
  it with $\omega=\sqrt{g\lVert k\rVert}$ (the $\tilde h_0(k)e^{i\omega t} +
  \tilde h_0^*(-k)e^{-i\omega t}$ Hermitian construction), and inverse-FFTs to a
  spatial tile of height + choppy horizontal displacement + analytic normals +
  Jacobian (folding → foam). A small radix-2 Cooley-Tukey FFT lives in
  [`OceanFFT`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanFFT.h). Pinned by
  `OceanFFTSpectrumTest` (FFT round-trip / Parseval, spectrum shape, Hermitian
  reality, determinism, Jacobian folding).
- ✅ **Runtime field provider.**
  [`OceanFFTField`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanFFTField.h)
  evaluates the field each tick and uploads it to two `RGBA32F` textures
  (displacement `dx,h,dz` + foam; normal + Jacobian) the water shader samples,
  RMS-normalised so `m_FFTAmplitude` maps to a predictable metre-scale wave
  height. A CPU copy is retained for physics/buoyancy sampling with no readback.
- ✅ **Shader integration.** [`Water.glsl`](../../OloEditor/assets/shaders/Water.glsl)
  samples the FFT displacement/normal textures in the vertex + tessellation-eval
  stages (and Jacobian foam in the fragment stage) when `u_FFTParams.x > 0.5`,
  instead of summing Gerstner waves. New textures at bindings 50/51; `FFTParams`
  appended to the `WaterParams` UBO (binding 23). Visual evidence:
  `OceanFFT_<Overview|GrazingAcross|Submerged|TopDown>.png` +
  `OceanFFT_Toggle{On,Off}.png` (`OceanFFTVisualEvidenceTest`).
- ✅ **Editor / scene / save-game / Lua wiring** for the FFT params on
  `WaterComponent`.
- ✅ **GPU compute butterfly port (§1.2)** — the default field producer.
  [`OceanFFTGpu`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanFFTGpu.h)
  generates the field entirely on the GPU behind the same two-texture
  interface (§6.4's transition path): `Ocean_SpectrumEvolve.comp` time-evolves
  the CPU-generated h0(k) into 8 complex spectra packed two-per-texel in a
  4-layer RGBA32F image array, `Ocean_FFTButterfly.comp` runs 2·log₂N
  radix-2 inverse-FFT passes (ping-pong arrays + precomputed twiddle/index
  LUT whose stage 0 absorbs the bit-reversal), and `Ocean_Assemble.comp`
  packs the displacement/derivatives textures — `Water.glsl` untouched.
  Toggleable per-component (`m_FFTUseGpuCompute`, editor/scene/save-game/Lua
  wired) with automatic CPU fallback when compute is unavailable. While the
  GPU owns the rendered field, physics' `SampleHeight` reads a band-limited
  ≤64² CPU proxy (`ExtractBandLimitedH0` — same wave vectors and phases, so
  it tracks the rendered surface, not a statistical re-roll). Pinned by
  `OceanFFTGpuContractTest` (GPU-vs-CPU butterfly + full-field + end-to-end
  texture comparison) and `OceanFFTVisualEvidenceTest::
  GpuComputeToggleLeavesSurfaceUnchanged` (full-pipeline frame RMSE).

- ✅ **JONSWAP spectrum (§1.4)** — a fetch-limited sharper-peak alternative to
  Phillips, selectable per-component (`m_FFTSpectrumType` + `m_FFTJonswapGamma`
  / `m_FFTJonswapFetch`, editor/scene/save-game wired). Because the base
  heightfield $\tilde h_0(\mathbf k)$ is CPU-generated (the GPU only evolves it),
  the new spectrum lives entirely in
  [`OceanSpectrum`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanSpectrum.h)
  (`JonswapSpectrum` + a `SpectrumEnergy` dispatch `GenerateH0` routes through)
  — no shader change. Defaults to Phillips so existing scenes are unchanged.
  Pinned by `OceanFFTSpectrumTest` (γ peak enhancement, fetch→peak-frequency
  shift, dispatch routing, metre-scale field) + a `ComponentRoundTrip` YAML test.

- ✅ **Band-limited three-cascade preset (§1.3)** — opt-in per water surface
  (`m_FFTCascades`), shipped by issue #969. See §1.3 below.

**Buoyancy sampling from the FFT field** (§5.1) is also **shipped** —
`BuoyancySystem` reads `OceanFFTField`'s band-limited CPU proxy via
`WaterSurface::SampleHeightFFT` when a tile uses the FFT ocean, falling back to
the Gerstner sum otherwise; since #969 that proxy is the SUM of the enabled
cascades.

### 1.1 Tessendorf FFT Pipeline

**Reference**: Jerry Tessendorf, *"Simulating Ocean Water"* (SIGGRAPH 2001
course notes).

The industry-standard approach:

1. **Phillips spectrum** — statistical model of ocean wave energy as a function
   of wind speed, direction, and fetch distance. Generates an initial frequency-
   domain heightfield $\tilde{h}_0(\mathbf{k})$ where $\mathbf{k}$ is the 2D
   wave vector.

2. **Time evolution** — each frequency component oscillates according to the
   deep-water dispersion relation:
   $$\omega(\mathbf{k}) = \sqrt{g \|\mathbf{k}\|}$$
   giving $\tilde{h}(\mathbf{k}, t) = \tilde{h}_0(\mathbf{k}) e^{i\omega t} + \tilde{h}_0^*(-\mathbf{k}) e^{-i\omega t}$

3. **Inverse FFT** — a 2D IFFT converts the frequency-domain heightfield back
   to spatial-domain displacement (height + horizontal "choppiness" via
   separate x/z displacement maps).

4. **Normal map derivation** — spatial derivatives from the FFT output yield an
   analytical normal map for per-pixel lighting (no finite differences needed).

### 1.2 GPU Compute Shader Implementation — **shipped**

The FFT runs on the GPU: `Renderer/Ocean/OceanFFTGpu.{h,cpp}` with
`Ocean_SpectrumEvolve.comp` / `Ocean_FFTButterfly.comp` / `Ocean_Assemble.comp`.
The CPU `OceanFFT` remains as the reference the tests pin against.

For OloEngine (OpenGL 4.6 / compute shaders):

- **Spectrum texture** (512×512 or 1024×1024, `RGBA32F`): store
  $\tilde{h}_0(\mathbf{k})$ as complex pairs, regenerated when wind parameters
  change.
- **Time-evolution pass** (compute): multiply each texel by the phase factor
  $e^{\pm i \omega t}$, producing the animated spectrum.
- **Butterfly FFT passes** (compute): radix-2 Cooley-Tukey, $\log_2 N$ passes
  per dimension. Use ping-pong SSBOs or image load/store.
- **Output textures**: displacement map (RGB16F — dx, height, dz), normal map
  (RG16F or RGB8), folding/Jacobian map (R16F — for foam generation).

### 1.3 Cascaded FFT — **shipped** (issue #969)

A single grid covers one wavelength range. Real oceans need waves from
centimetres to hundreds of metres, and the one tile a single-cascade field
settles on is also the distance at which the whole sea visibly repeats — which
is what #969 opened with, as "weak near-to-horizon coherence" in Drift captures.

**Reference**: Dupuy & Bruneton, *"Real-Time Animation and Rendering of Ocean
Whitecaps"* (SIGGRAPH Asia 2012).

What shipped is a **fixed three-band preset**, not an N-cascade system — the
issue's non-goal is an artist knob surface before the preset is visually proven.
A water surface opts in with `m_FFTCascades = 3` (default 1 = the pre-#969
field, unchanged); everything else derives from the patch size and resolution
the scene already authors. The full design, and the reasoning behind each
constant, lives at the top of
[`Ocean/OceanCascades.h`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanCascades.h)
rather than being duplicated here. The four load-bearing decisions:

| decision | what it buys |
|---|---|
| The authored patch size becomes the **mid** band; broad and fine tiles derive either side of it | an author keeps the wave scale they tuned, and the preset adds an octave in each direction |
| Band boundaries sit at the **next tile's fundamental** ($2\pi/L_{i+1}$), as half-open ranges | no gap and no double-counted energy at the handoff — every wave vector belongs to exactly one band |
| Tile ratios (broad $L_0 = 6.29\,L$, fine $L_2 = L/4.43$) are deliberately **non-commensurate**, and the mid band's sampling domain is **rotated** (with its wind counter-rotated to match) | the three lattices share neither a period nor an axis, so there is no repetition for the eye to lock onto |
| Per-band resolutions are derived from each band's **shortest wavelength** | the bounded bands need 64 where the fine band needs the authored resolution |

The three fields are **layers of one `Texture2DArray` pair**, so the preset costs
the same two engine texture slots (`TEX_WATER_FFT_DISPLACEMENT` /
`_DERIVATIVES`) the single-cascade field did, and the single-cascade path is a
one-layer array through the same code. The sum itself is written once per side:
`OceanFFTField::SampleCascades` on the CPU (which is what buoyancy floats on)
and `include/OceanCascadeCommon.glsl::sampleOceanCascades` on the GPU, included
by the vertex, tessellation-evaluation and fragment stages rather than copied
into each.

**Consequence worth knowing before changing the classifier:** every chain runs
at the array resolution rather than at its own derived one, because a texture
array's layers must share a size. That costs GPU time and nothing else — a
band-limited spectrum on a larger grid is the same spectrum with the extra bins
zero, and its inverse FFT is the exact band-limited reconstruction of the
smaller one. That equivalence is pinned by
`OceanCascadeTest.DerivedResolutionReproducesTheArrayResolutionField`, not
asserted in a comment.

Pinned by `OceanCascadeTest` (band partition, tile non-commensurability,
resolution derivation, the single-cascade fallback against an independent
re-derivation of the old pipeline, summed slope vs summed height, and a text
test that all three water stages share one sum), `OceanFFTGpuContractTest`
(every layer produced and distinct on the GPU, CPU/texture summed parity, array
lifetime across the opt-in), `WaterSurfaceSamplerTest` (buoyancy reads the sum)
and `OceanCascadeVisualEvidenceTest` (near/mid/horizon A/B captures).

### 1.4 JONSWAP Spectrum — **shipped**

The Phillips spectrum is the simplest; real-world measurements show a sharper
peak. The JONSWAP (Joint North Sea Wave Project) spectrum adds a peak
enhancement factor $\gamma$ (typically 3.3) and spectral width parameters:

$$S(\omega) = \frac{\alpha g^2}{\omega^5} \exp\!\left[-\frac{5}{4}\left(\frac{\omega_p}{\omega}\right)^4\right] \gamma^{\exp\!\left[-\frac{(\omega - \omega_p)^2}{2\sigma^2\omega_p^2}\right]}$$

This gives more energy near the spectral peak, producing the characteristic
dominant swell with suppressed high-frequency tail — closer to the look of
Atlantic / Pacific seas.

`JonswapSpectrum`
([`OceanSpectrum.cpp`](../../OloEngine/src/OloEngine/Renderer/Ocean/OceanSpectrum.cpp))
evaluates this 1-D frequency spectrum at $\omega=\sqrt{g\lVert k\rVert}$, derives
the peak frequency $\omega_p = 22\,(g^2/(VF))^{1/3}$ from wind speed $V$
(`m_FFTWindSpeed`) and fetch $F$ (`m_FFTJonswapFetch`), uses $\sigma=0.07/0.09$
below/above $\omega_p$, then maps $S(\omega)$ to a 2-D wave-vector density via
the polar Jacobian $\Psi(\mathbf k)=S(\omega)\,(d\omega/dk)/k$. The directional
term and small-wave suppression are shared with Phillips so the wind controls
behave identically. The equilibrium constant $\alpha$ folds into `m_FFTAmplitude`
because `OceanFFTField` RMS-normalises the field — only the *shape* differs.
Selected per-component via `m_FFTSpectrumType` (`SpectrumType::{Phillips,
JONSWAP}`); a `SpectrumEnergy` dispatch is what `GenerateH0` calls, so the choice
flows through both the CPU and GPU-compute producers (the GPU only time-evolves
the already-built $\tilde h_0$, so no shader changes were needed). Defaults to
Phillips. Pinned by `OceanFFTSpectrumTest`.

---

## 2. Foam & Whitecap Generation (Medium Impact / Medium Effort)

### 2.1 Jacobian-Based Foam

The FFT pipeline naturally provides the Jacobian determinant of the horizontal
displacement. Areas where $J < 0$ (surface folding) correspond to breaking
waves. Store the Jacobian in a texture, then:

- Accumulate foam over time where $J < \epsilon$ (with exponential decay).
- Modulate foam brightness by $\max(0, -J)$ for varying intensity.
- Far more physically plausible than height/angle thresholds.

## 3. Lighting & Shading (Medium Impact / Low–Medium Effort)

### 3.1 Proper sRGB Albedo Sampling — **shipped**

`TextureSpecification::SRGB` (and a corresponding `srgb` parameter on
`Texture2D::Create(path)`) now selects `GL_SRGB8` / `GL_SRGB8_ALPHA8` for
colour textures, letting the GPU do the sRGB→linear conversion that the
PBR shaders already assumed. `Model::LoadMaterialTextures` and
`AnimatedModel::LoadMaterialTextures` tag `aiTextureType_DIFFUSE` /
`BASE_COLOR` / `EMISSIVE` as sRGB; normal / metallic-roughness / AO /
height maps stay linear. The asset-pipeline drag-drop path goes through
`TextureSerializer::IsLikelyColorTextureByName` for the same decision.
Pinned by `SRGBTextureSupportTest.cpp`.

### 3.2 Atmospheric Scattering / Sky Integration — **Preetham shipped**

`ProceduralSkyComponent` (see
[`ProceduralSky.h`](../../OloEngine/src/OloEngine/Renderer/ProceduralSky.h))
bakes a Preetham 1999 analytic daylight sky into a cubemap via
[`ProceduralSky.glsl`](../../OloEditor/assets/shaders/ProceduralSky.glsl) and
feeds it through the existing `EnvironmentMap` IBL pipeline. Because the
output is the same cubemap + irradiance / prefilter / BRDF set that the
file-based environment map produces, water reflections, IBL ambient, and
the skybox all consume it with no shader changes. Sun direction can track
the scene's directional light (`m_LinkSunToDirectionalLight`) for a
time-of-day controller, and a representative sun disk is baked in. The
Preetham math is pinned by `ProceduralSkyMathTest.cpp` and the GPU bake by
`ProceduralSkyBakeTest.cpp`.

Still open:

- **Bruneton / Hosek-Wilkie precomputed scattering** — Preetham degrades
  near the horizon and at sunset (the sun-area glow is approximate). A
  precomputed multiple-scattering model gives a far better twilight and
  aerial-perspective result, at the cost of a heavier precompute.
- **Aerial perspective / haze over distant water** — needs the scattering
  transmittance LUT a Bruneton model would provide; not derivable from the
  Preetham analytic form alone.
- **Night sky / sub-horizon sun** — Preetham is undefined below the
  horizon (currently clamped a few degrees above), so dusk-to-night needs a
  separate model or a blend.

### 3.3 Volumetric Light Shafts (God Rays) — **shipped**

Underwater god rays ship as a post-process: `PostProcessSettings::GodRayParams`,
with the decay normaliser mirrored by `UnderwaterCaustics::GodRayDecaySum`, and a
`GodRayUnderwater.olo` sandbox scene.

Underwater god rays through the water surface, rendered as a screen-space
radial blur from the sun position. Relatively cheap post-process effect that
dramatically improves the underwater look.

### 3.4 Improved Subsurface Scattering — **partially shipped**

A wave-height-driven SSS term exists (`u_SSSColor.rgb`, `u_FoamParams2.z`
= `sssIntensity` in `Water.glsl`). The deeper model below — thickness-aware,
view-dependent back-scatter — is not built.

The current SSS model uses a simple $(\mathbf{V} \cdot -\mathbf{L})^4$ term.
Improvements:

- **Thickness-aware SSS**: use the distance between front and back face depth
  to estimate water thickness at each pixel.
- **Wavelength-dependent absorption**: red light is absorbed fastest, then
  green, then blue — producing the characteristic teal-to-deep-blue gradient
  that varies with viewing angle and depth.

---

## 4. Tessellation & LOD (Medium Impact / Medium Effort)

### 4.3 GPU Tessellation with Hull Shader Culling — **shipped**

Frustum culling in the TCS lands in
[`Water.glsl`](../../OloEditor/assets/shaders/Water.glsl) — patches whose
displacement-inflated AABB lies entirely outside any of the six view-frustum
planes are skipped by setting `gl_TessLevelOuter[*]` to 0. The displacement
margin is derived in-shader from the per-frame wave parameters so wave crests
at the edges of off-screen patches don't pop into view. CPU mirror tests in
[`WaterRenderingTest.cpp`](../../OloEngine/tests/Rendering/WaterRenderingTest.cpp)
pin the math against the actual Gerstner amplitudes.

Back-face culling for water is **not** added: the water plane is double-sided
(camera goes underwater) so a back-face reject would punch holes when looking
up through the surface. Frustum culling is the cleaner standalone win.

---

## 5. Interaction & Physics (High Impact / High Effort)

### 5.1 Buoyancy System — **shipped (CPU)**

A `BuoyancyComponent` ([`Components.h`](../../OloEngine/src/OloEngine/Scene/Components.h))
on any dynamic `Rigidbody3DComponent` makes it float on a `WaterComponent`
surface. [`BuoyancySystem`](../../OloEngine/src/OloEngine/Physics3D/BuoyancySystem.cpp)
runs each physics tick (before `JoltScene::Simulate`) and:

- Samples the wave height at the eight corner probes of a configurable
  buoyancy box via [`WaterSurface`](../../OloEngine/src/OloEngine/Renderer/WaterSurface.h)
  — a **1:1 CPU mirror of `WaterCommon.glsl :: sumGerstnerWaves`**, so a body
  tracks the *rendered* crest (it reads `Time::GetTime()`, the same clock the
  water shader is fed). `WaterSurface::SampleHeight` inverts the horizontal
  Gerstner shift with a short fixed-point iteration so the height belongs to the
  column above the query.
- Applies an upward Archimedes force per submerged probe (acting at the corner,
  so asymmetric submersion yields a self-righting / wave-tilting torque), ramped
  smoothly across the waterline.
- Damps bobbing / rocking with submerged, mass-scaled linear + angular drag.

The CPU wave math is pinned by `WaterSurfaceSamplerTest` (L1) and the
emergent physics behaviour (settles at the waterline, tracks the plane height,
dense bodies sink, tracks a frozen wave surface) by `WaterBuoyancyTest`
(Functional). Editor UI, Lua bindings, scene + save-game serialization are all
wired.

**FFT displacement source — shipped.** When a `WaterComponent` renders the
Tessendorf FFT ocean (`m_UseFFT`), `BuoyancySystem` samples that surface instead
of the Gerstner approximation, so a floater tracks the ocean that's actually
rendered. It reads `OceanFFTField`'s retained **band-limited CPU proxy** via
`WaterSurface::SampleHeightFFT`, which maps the proxy exactly the way
`Water.glsl`'s FFT vertex path does (`planeHeight + disp.y * heightScale`); the
proxy already inverts the choppy horizontal shift, so the height belongs to the
column above the probe. No GPU readback — the proxy is the same one the renderer
maintains each frame. The switch is purely runtime-derived (no new serialized
field): a tile uses the FFT path only when `m_UseFFT` **and** its `m_OceanField`
proxy has been evaluated; otherwise it falls back to Gerstner (so headless
physics with no render pass, and all non-FFT water, are unchanged). Pinned by
`WaterSurfaceSamplerTest`'s FFT cases (the proxy-mapping contract) and
`WaterBuoyancyTest::RestsAtTheFFTSurfaceHeight` (a body resting on a displaced
FFT column with Gerstner disabled — proving the branch fired).

Still open:

- **GPU readback for many objects** — the current path is CPU per-probe
  (cheap for tens of floaters). A crowd of hundreds wants a batched GPU height
  query.
- **Submerged-volume from the real collider** — probes are derived from a
  user box (`m_ProbeExtents`), not the actual convex/mesh collider shape.

### 5.2 Wake / Kelvin Wake Pattern — **shipped (issue #967)**

Delivered as a world-anchored decaying disturbance field: `Renderer/Water/WaterWake.h`,
`WaterWakeSystem`, `Physics3D/BoatWakeSystem`, sampled in `Water.glsl` via
`sampleWaterDisturbance()` with its own long distance fade so a chase camera does not
delete the trail.

<!-- original proposal retained below for the derivation -->

Objects moving through water should produce a V-shaped wake (Kelvin wake):

- Render a signed-distance wake pattern into a separate displacement texture.
- Blend with the ocean displacement.
- Decay over distance behind the object.
- The wake half-angle is always ~19.47° regardless of speed (Kelvin's result).

### 5.3 Shore Wave Deformation — **shipped (issue #1033)**

Waves shoal, refract and break against a seabed depth field. The rule first: the depth comes
from the terrain the scene already has, never from a separately authored texture.

- **The field.** `Renderer/Water/WaterShoreDepthSystem` resamples every `TerrainComponent`'s
  height field into ONE 512x512 RGBA16F window covering the water tile — `r` = water depth in
  metres, `gb` = the depth gradient. Baked when its inputs change and not per frame (the sea
  floor does not move), retained on the CPU so buoyancy reads the same field with no readback.
- **The transform.** `Renderer/Water/WaterShoreDepth.h` is the contract and the whole of the
  maths; `include/WaterShoreCommon.glsl` is its GPU twin. Frequency is what is conserved, so the
  local wavenumber comes from inverting `w^2 = g k tanh(k h)` and the phase speed is `w/k` —
  the time term of every wave's phase is therefore depth-independent, which is why a camera
  crossing a slope sees no shimmer. Amplitude follows Green's law, direction follows Snell's law
  against the depth gradient, and both reduce to the identity in deep water.
- **Breaking reuses the Jacobian**, it does not add a second signal. `gerstnerWaveNormal`'s
  accumulated tangent/binormal ARE the off-identity entries of the horizontal displacement map's
  Jacobian (see `waterGerstnerJacobian`), so the fold detection the FFT foam reads from a texture
  is available analytically here for nothing. The depth adds the LIMIT — amplitude capped at
  `breakerIndex * h` — which is what makes the surf zone decay toward the waterline instead of
  growing into a wall of water.
- **Both displacement chains, structurally.** The transform lives inside
  `sumGerstnerWavesShore` in `WaterCommon.glsl`, which the vertex and tess-eval stages share and
  which `Water.glsl` and `Water_Depth.glsl` both include — so the colour pass and the
  surface-depth capture cannot disagree about the shape.
- **The FFT path gets the depth limit only.** A tiled spectrum has no per-train heading to turn,
  so refraction is not representable there; the crest height is still clamped to the breaker
  limit, because a metre-tall crest in 20 cm of water is the artefact that actually shows.

Known limitation, stated so it is not rediscovered as a bug: this is ray theory evaluated
pointwise rather than integrated along a ray, so crest SPACING, heading, height and breaking are
right and individual crest POSITIONS drift from an eikonal solution by the accumulated phase.

Contracts: `WaterShoreWaveTest.cpp` (dispersion, Green's-law dip, Snell, the breaker limit, the
bake, and the GLSL constant mirror). Evidence:
`WaterShoreVisualEvidenceTest.cpp` -> `assets/tests/visual/WaterShore_*.png`, from
shoreline-grazing angles — a top-down shot cannot show whether waves turn.

### 5.4 Ripple Injection — **shipped (issue #967)**

`Renderer/Water/WaterDisturbanceField.{h}` + `WaterDisturbanceSystem` maintain the
injectable field (UBO binding 63, `WaterDisturbance_Update.comp`); `Water.glsl`
samples it through `sampleWaterDisturbance()`. This is the mechanism a rain-impact
effect should reuse — see [#1034](https://github.com/drsnuggles8/OloEngineBase/issues/1034).

> Related (issue #630): the engine now has a real particle fluid — the PBF
> solver under `OloEngine/src/OloEngine/Fluid/` (`FluidComponent`) — which
> covers localized liquid volumes (pools, dam breaks, pouring) with two-way
> Jolt coupling. Ripple injection below remains open for the *ocean surface*
> heightfield itself; small splash volumes can instead place a fluid domain.

Allow gameplay events (explosions, character wading, rain) to inject
disturbances into the water surface:

- Maintain a heightfield texture for interactive ripples.
- Update via a wave-equation compute shader each frame.
- Blend with the ocean displacement additively.

---

## 6. Performance (Variable Impact / Medium Effort)

### 6.1 Planar Reflections — ✅ implemented (first slice)

SSR has artifacts (missing data outside screen). For important water surfaces,
render a mirrored scene pass below the water plane into a reflection texture.
Use oblique near-plane clipping to avoid rendering underwater geometry.

Expensive (extra draw calls) but produces perfect reflections. Can be
resolution-scaled (half or quarter res) and updated at reduced frequency.

**Done:** `PlanarReflectionRenderPass`
(`Renderer/Passes/PlanarReflectionRenderPass.{h,cpp}`) runs after `ScenePass`
and before `WaterPass`. It swaps the shared `CameraUBO` to a camera mirrored
across the water plane (Householder reflection + Lengyel oblique near-clip — the
pure math is `Renderer/PlanarReflection.{h,cpp}`, pinned by
`PlanarReflectionMathTest`), flips front-face winding, and **re-executes
`ScenePass`'s already-batched opaque command bucket** into an owned RGBA16F+depth
target — so the reflection is shaded by the exact same PBR/lighting/shadow/IBL
path as the main view for free. The mirror VP + enable/intensity/distortion ride
a binding-43 UBO (`UBO_PLANAR_REFLECTION`); `Water.glsl` samples the result
(`TEX_WATER_PLANAR_REFLECTION` = slot 52) projectively and blends it over the
cubemap/SSR fallback by Fresnel × edge-fade. Enabled per surface via
`WaterComponent::m_PlanarReflectionsEnabled`.

First-slice limitations (future work): **forward / forward+ path only** — the
deferred opaque bucket writes a G-Buffer, not lit colour, so a single-target
replay can't capture it (deferred would need its own mini lighting resolve);
**one global reflection plane per frame** (the largest reflective surface), so
multiple water heights aren't independently reflected; full-resolution + every
frame (no resolution-scale / reduced-frequency update yet). Forward+ reflections
re-use the main view's tiled light culling, so reflected lighting is approximate.

### 6.2 Reflection Probe Blending — **shipped (issue #705)**

Distance-impostor reflection probes with a cluster-culled lookup:
`Renderer/ReflectionProbeArray.{h,cpp}`, `ReflectionProbeDistanceField.h`, and
`include/ReflectionProbes.glsl`.

When planar reflections are too expensive, blend between multiple reflection
probes based on the camera/water position. Update probes asynchronously.

## 7. Visual Polish (Low–Medium Impact / Low Effort)

### 7.1 Caustics — **shipped (`feature/underwater-caustics-refraction`)**

Animated caustic light is projected onto submerged geometry by the tone-map
underwater stage (gated on the camera being below the surface, so it costs
nothing above water):

- ✅ **Procedural pattern (no texture asset).** A two-octave web of wavy ridge
  lines (the union of two drifting sine fields) sampled at the fragment's
  world-space XZ, animated by the wave clock. Mirrored on the CPU in
  [`UnderwaterCaustics.h`](../../OloEngine/src/OloEngine/Renderer/UnderwaterCaustics.h)
  (`CausticPattern`) and pinned by `WaterRenderingTest`.
- ✅ **Projected onto upward-facing surfaces.** The geometric normal is
  reconstructed in-pass from screen-space derivatives of the depth-reconstructed
  world position (`dFdx`/`dFdy`); caustics fade by `max(normal.y, 0)`.
- ✅ **Modulated by depth and light angle.** Faded by depth below the per-pixel
  water surface (`CausticDepthFade`, → 0 by `m_CausticsMaxDepth`) and by the
  sun's overhead factor (`max(-sunDir.y, 0)`), then added to the surface radiance
  *before* the underwater absorption so distant caustics fade into the fog.
  Driven by `WaterComponent` (`m_CausticsIntensity` / `m_CausticsScale` /
  `m_CausticsSpeed` / `m_CausticsMaxDepth` / `m_CausticsColor`; serialized,
  save-game + Lua + editor-UI wired), uploaded to UBO binding 37
  (`UnderwaterFogUBOData`) from
  [`Scene.cpp`](../../OloEngine/src/OloEngine/Scene/Scene.cpp) ~L4357, applied in
  [`PostProcess_ToneMap.glsl`](../../OloEditor/assets/shaders/PostProcess_ToneMap.glsl)
  (`underwaterCausticPattern`). Visual evidence: `UnderwaterFx_Caustics_On.png`
  vs `UnderwaterFx_Caustics_Off.png` (`UnderwaterCausticsVisualTest`).

### 7.2 Underwater Rendering — **partially shipped (PR #259)**

When the camera goes below the water surface:

- ✅ **Switch to underwater fog (exponential, tinted blue-green) — shipped (PR #259).**
  Per-pixel, *wave-aware* Beer–Lambert absorption: the tone-map pass fogs the
  portion of each view ray that lies below the water plane (underwater half
  fogged, above-water half clear) using a depth-only re-render of the nearest
  wavy water surface rather than a flat plane. Driven by `WaterComponent`'s
  `m_UnderwaterFogColor` / `m_UnderwaterFogDensity` (serialized, save-game + Lua
  wired), uploaded to UBO binding 37 (`UnderwaterFogUBOData`) from
  `Scene::OnUpdateRuntime` ([`Scene.cpp`](../../OloEngine/src/OloEngine/Scene/Scene.cpp) ~L4357),
  applied in [`PostProcess_ToneMap.glsl`](../../OloEditor/assets/shaders/PostProcess_ToneMap.glsl)
  (`applyUnderwaterFog`). The math is mirrored on the CPU in
  [`UnderwaterFog.h`](../../OloEngine/src/OloEngine/Renderer/UnderwaterFog.h) and
  pinned by `UnderwaterFogMathTest` / `WaterRenderingTest`.
- ✅ **Apply chromatic distortion to simulate light refraction — shipped
  (`feature/underwater-caustics-refraction`).** When submerged, the tone-map pass
  wobbles the scene-colour sample UV with two phase-shifted trig layers scrolled
  by the wave clock, and splits the R/G/B channels by a fraction of that offset
  for chromatic refraction (the global `ChromaticAberration` post-effect is
  unrelated and stays decoupled from submersion). Hard-capped to 0.1 UV so a bad
  param can't tear the image apart. Driven by `WaterComponent`
  (`m_UnderwaterRefractionStrength` / `m_UnderwaterRefractionScale` /
  `m_UnderwaterRefractionSpeed` / `m_UnderwaterChromaticStrength`), mirrored on
  the CPU in
  [`UnderwaterCaustics.h`](../../OloEngine/src/OloEngine/Renderer/UnderwaterCaustics.h)
  (`RefractionOffset`) and applied in
  [`PostProcess_ToneMap.glsl`](../../OloEditor/assets/shaders/PostProcess_ToneMap.glsl)
  (`underwaterRefractionOffset`). Visual evidence: `UnderwaterFx_Refraction_On.png`
  vs `UnderwaterFx_Refraction_Off.png` (`UnderwaterCausticsVisualTest`).
- ✅ **Render the water surface from below with inverted normals — shipped (PR #259).**
  [`Water.glsl`](../../OloEditor/assets/shaders/Water.glsl) branches on
  `gl_FrontFacing` (~L666–L755): the underside gets a cheap, stable tinted
  shading path with a soft cubemap rim at grazing angles, and the grazing-angle
  "see-through" artefact was fixed. Visual evidence: `Water_Submerged.png`,
  `Water_WaterlineStraddle.png`.
- ❌ **Add floating particle effects (dust, plankton) — not yet.** No underwater
  particle path; would tie into the existing `Particle` subsystem gated on
  `UnderwaterFogState::Active`.

## References

| Author(s) | Title | Year | Notes |
|---|---|---|---|
| Tessendorf, J. | *Simulating Ocean Water* | 2001 | Foundational FFT ocean paper |
| Finch, M. | *Effective Water Simulation from Physical Models* (GPU Gems Ch.1) | 2004 | Gerstner waves in vertex shaders |
| Johanson, C. | *Projected Grid Concept for Real-Time Water Rendering* | 2004 | Screen-space water mesh |
| Dupuy, J. & Bruneton, E. | *Real-Time Animation and Rendering of Ocean Whitecaps* | 2012 | Multi-cascade FFT + foam |
| Bruneton, E. et al. | *An Improved Ocean White Cap Model* | 2020 | Updated foam Jacobian model |
| Mikkelsen, M. | *Practical Real-Time Hex-Tiling* | 2022 | Tiling-artifact-free texturing |
| Catlike Coding | *Waves Tutorial* | 2018 | Beginner-friendly Gerstner waves |
| NVIDIA | GPU Gems 1–3 (various water chapters) | 2004–2007 | GPU water techniques |
| Flügge, F. | *Realtime GPGPU FFT Ocean Water Simulation* (thesis) | 2017 | Practical GPU FFT implementation |
| Horvath, C. | *Empirical Directional Wave Spectra for Computer Graphics* | 2015 | Unified spectra for ocean rendering |
