# Water System — Future Improvements

This document tracks potential enhancements beyond the current Gerstner-wave
ocean implementation. Items are grouped by subsystem and roughly ordered by
impact vs. effort within each section.

---

## 1. FFT Ocean Simulation (High Impact / High Effort)

The current system sums 8 Gerstner waves (2 primary + 6 detail octaves with
domain warping). This produces convincing results for a moderate number of
waves, but cannot match the spectral realism of thousands of superimposed
frequency components.

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

### 1.2 GPU Compute Shader Implementation

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

### 1.3 Cascaded FFT

A single 512² grid covers one wavelength range. Real oceans need waves from
centimeters to hundreds of meters. Solution: 3–4 cascaded FFT grids at
different scales (e.g. 4m, 64m, 512m, 4096m tiles).

Each cascade uses a different band of the Phillips spectrum. During rendering,
all cascades are sampled and summed in the vertex/tessellation shader.

**Reference**: Dupuy & Bruneton, *"Real-Time Animation and Rendering of Ocean
Whitecaps"* (SIGGRAPH Asia 2012) — describes a multi-cascade approach with
correct filtering.

### 1.4 JONSWAP Spectrum

The Phillips spectrum is the simplest; real-world measurements show a sharper
peak. The JONSWAP (Joint North Sea Wave Project) spectrum adds a peak
enhancement factor $\gamma$ (typically 3.3) and spectral width parameters:

$$S(\omega) = \frac{\alpha g^2}{\omega^5} \exp\!\left[-\frac{5}{4}\left(\frac{\omega_p}{\omega}\right)^4\right] \gamma^{\exp\!\left[-\frac{(\omega - \omega_p)^2}{2\sigma^2\omega_p^2}\right]}$$

This gives more energy near the spectral peak, producing the characteristic
dominant swell with suppressed high-frequency tail — closer to the look of
Atlantic / Pacific seas.

---

## 2. Foam & Whitecap Generation (Medium Impact / Medium Effort)

### 2.1 Jacobian-Based Foam

The FFT pipeline naturally provides the Jacobian determinant of the horizontal
displacement. Areas where $J < 0$ (surface folding) correspond to breaking
waves. Store the Jacobian in a texture, then:

- Accumulate foam over time where $J < \epsilon$ (with exponential decay).
- Modulate foam brightness by $\max(0, -J)$ for varying intensity.
- Far more physically plausible than height/angle thresholds.

### 2.2 Foam Advection

Currently foam is a static function of wave height. Realistic foam drifts with
the surface current:

- Store a foam density texture.
- Advect it each frame using the displacement field's velocity.
- Add foam where waves break; decay exponentially over several seconds.
- Can run as a simple compute shader pass.

### 2.3 Bubble / Spray Particles

Wave crests that fold or exceed a steepness threshold can emit GPU particles:

- Compute shader updates particle positions (ballistic trajectory + wind drag).
- Render as point sprites or billboards with soft-particle depth blending.
- Provides dramatic visual feedback for stormy seas.

---

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
[`ProceduralSky.h`](../OloEngine/src/OloEngine/Renderer/ProceduralSky.h))
bakes a Preetham 1999 analytic daylight sky into a cubemap via
[`ProceduralSky.glsl`](../OloEditor/assets/shaders/ProceduralSky.glsl) and
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

### 3.3 Volumetric Light Shafts (God Rays)

Underwater god rays through the water surface, rendered as a screen-space
radial blur from the sun position. Relatively cheap post-process effect that
dramatically improves the underwater look.

### 3.4 Improved Subsurface Scattering

The current SSS model uses a simple $(\mathbf{V} \cdot -\mathbf{L})^4$ term.
Improvements:

- **Thickness-aware SSS**: use the distance between front and back face depth
  to estimate water thickness at each pixel.
- **Wavelength-dependent absorption**: red light is absorbed fastest, then
  green, then blue — producing the characteristic teal-to-deep-blue gradient
  that varies with viewing angle and depth.

---

## 4. Tessellation & LOD (Medium Impact / Medium Effort)

### 4.1 Projected Grid

Instead of tessellating a world-space plane, project a screen-space grid onto
the water plane. Benefits:

- Uniform pixel density across the entire visible ocean.
- No wasted vertices behind the camera or beyond the horizon.
- Screen resolution determines vertex count, not world size.

**Reference**: Claes Johanson, *"Real-time water rendering — Introducing the
projected grid concept"* (2004).

### 4.2 Adaptive Tessellation with Displacement Map Roughness

Currently tessellation factor is purely distance-based. Better: also consider
the displacement map's local gradient magnitude. Flat areas get minimal
tessellation; rough areas (wave crests) get maximum.

### 4.3 GPU Tessellation with Hull Shader Culling — **shipped**

Frustum culling in the TCS lands in
[`Water.glsl`](../OloEditor/assets/shaders/Water.glsl) — patches whose
displacement-inflated AABB lies entirely outside any of the six view-frustum
planes are skipped by setting `gl_TessLevelOuter[*]` to 0. The displacement
margin is derived in-shader from the per-frame wave parameters so wave crests
at the edges of off-screen patches don't pop into view. CPU mirror tests in
[`WaterRenderingTest.cpp`](../OloEngine/tests/Rendering/WaterRenderingTest.cpp)
pin the math against the actual Gerstner amplitudes.

Back-face culling for water is **not** added: the water plane is double-sided
(camera goes underwater) so a back-face reject would punch holes when looking
up through the surface. Frustum culling is the cleaner standalone win.

---

## 5. Interaction & Physics (High Impact / High Effort)

### 5.1 Buoyancy System — **shipped (CPU)**

A `BuoyancyComponent` ([`Components.h`](../OloEngine/src/OloEngine/Scene/Components.h))
on any dynamic `Rigidbody3DComponent` makes it float on a `WaterComponent`
surface. [`BuoyancySystem`](../OloEngine/src/OloEngine/Physics3D/BuoyancySystem.cpp)
runs each physics tick (before `JoltScene::Simulate`) and:

- Samples the wave height at the eight corner probes of a configurable
  buoyancy box via [`WaterSurface`](../OloEngine/src/OloEngine/Renderer/WaterSurface.h)
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

Still open:

- **FFT displacement source** — once the §1 FFT pipeline lands, `WaterSurface`
  should sample its displacement texture (GPU readback or a shared CPU copy)
  instead of summing Gerstner octaves on the CPU.
- **GPU readback for many objects** — the current path is CPU per-probe
  (cheap for tens of floaters). A crowd of hundreds wants a batched GPU height
  query.
- **Submerged-volume from the real collider** — probes are derived from a
  user box (`m_ProbeExtents`), not the actual convex/mesh collider shape.

### 5.2 Wake / Kelvin Wake Pattern

Objects moving through water should produce a V-shaped wake (Kelvin wake):

- Render a signed-distance wake pattern into a separate displacement texture.
- Blend with the ocean displacement.
- Decay over distance behind the object.
- The wake half-angle is always ~19.47° regardless of speed (Kelvin's result).

### 5.3 Shore Wave Deformation

Waves approaching a shallow shore should:

- Slow down (shoaling) — phase speed decreases with depth.
- Increase in amplitude and steepness.
- Refract toward the shore (Snell's law for water waves).
- Eventually break — detect via Jacobian or steepness threshold.

Requires a depth map of the seabed and modifying wave parameters per-vertex
based on local depth.

### 5.4 Ripple Injection

Allow gameplay events (explosions, character wading, rain) to inject
disturbances into the water surface:

- Maintain a heightfield texture for interactive ripples.
- Update via a wave-equation compute shader each frame.
- Blend with the ocean displacement additively.

---

## 6. Performance (Variable Impact / Medium Effort)

### 6.1 Planar Reflections

SSR has artifacts (missing data outside screen). For important water surfaces,
render a mirrored scene pass below the water plane into a reflection texture.
Use oblique near-plane clipping to avoid rendering underwater geometry.

Expensive (extra draw calls) but produces perfect reflections. Can be
resolution-scaled (half or quarter res) and updated at reduced frequency.

### 6.2 Reflection Probe Blending

When planar reflections are too expensive, blend between multiple reflection
probes based on the camera/water position. Update probes asynchronously.

### 6.3 Normal Map Detail Tiling

The current normal perturbation uses scrolling normal maps. To avoid tiling
artifacts at large scales, use detail-preserving tiling:

- Blend between the normal map at different UV offsets using a hash-based
  random offset per tile.
- Or use a hex-tiling approach (Hextiling by Morten Mikkelsen).

### 6.4 Compute Shader Wave Evaluation

Move the Gerstner wave evaluation from vertex shader to a compute shader that
writes to a displacement texture. Benefits:

- Decouple wave resolution from mesh resolution.
- Share the displacement texture between rendering, physics queries, and foam
  generation.
- Easier transition path to FFT later (same output texture, different
  generation method).

---

## 7. Visual Polish (Low–Medium Impact / Low Effort)

### 7.1 Caustics

Project animated caustic patterns onto underwater geometry:

- Use a tileable caustic texture animated via scrolling or procedural noise.
- Project onto surfaces using the water surface normal + light direction.
- Modulate intensity by water depth and light angle.

### 7.2 Underwater Rendering

When the camera goes below the water surface:

- Switch to underwater fog (exponential, tinted blue-green).
- Apply chromatic distortion to simulate light refraction.
- Render the water surface from below with inverted normals.
- Add floating particle effects (dust, plankton).

### 7.3 Rain Impact

During rain, add small circular ripple impacts on the water surface:

- Can be purely in the normal map (no displacement needed).
- Random positions, brief lifetime with expanding ring pattern.
- Cheap visual enhancement for weather systems.

### 7.4 Foam Trail Persistence

For objects dragging through water, leave a persistent foam trail that
gradually fades. Store in an advected foam density texture.

---

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
