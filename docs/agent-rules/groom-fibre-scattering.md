# Groom fibre scattering (#1247)

Read before touching `OloEngine/src/OloEngine/Groom/GroomFibreScattering.*`,
`OloEditor/assets/shaders/include/GroomFibreCommon.glsl`, the fibre half of
`OloEditor/assets/shaders/GroomStrand.glsl`, or `GroomFibreComponent`.

## The rules

1. **A fibre is not a surface, and the cosine is not N·L.** The BCSDF is normalised over the sphere
   *without* a cosine — the longitudinal falloff is already inside the `M` lobes. What multiplies it
   per light is the fibre's **projected width**, `sqrt(1 − (T·L)²)`, which belongs to the geometry
   and not to the scattering function. Both mistakes are invisible at normal incidence: putting the
   cosine inside the model makes the furnace test fail and looks like a broken model, and leaving it
   out entirely makes a strand lit end-on as bright as one lit broadside.

2. **Only the azimuth DIFFERENCE is needed, so never introduce a binormal.** The far-field response
   depends on `T·V`, `T·L` and the angle between their perpendicular components. A screen-facing
   ribbon has no meaningful binormal; a model that wanted one would have to invent it per frame, and
   it would swim as the camera turned. `v_ViewNormal` in `GroomStrand.glsl` is a fabrication written
   for SSAO and is **not** what the lighting reads.

3. **Averaging N point samples over the fibre width is REJECTED — widen each node.** The obvious
   far-field implementation reproduces the lobe as N spikes at every order a fragment shader can
   afford: 54–93 % RMS from the truth at N = 4, with an azimuthal total variation up to 7.4× the
   reference's, which bands visibly as the strand tangent turns. `NodeWidenedScale` widens each
   node's logistic to cover its own slice of `h`. Both rules stay callable, and
   `GroomFibreQuadratureTest` re-runs the rejection every build. Numbers:
   [the analysis](../analysis/groom-fibre-scattering-1247.md) tables 1 and 2.

4. **Energy conservation is in the ATTENUATIONS, not in the quadrature.** `A_0 = f`,
   `A_1 = (1−f)²T`, `A_2 = A_1 T f`, `A_3 = A_2 T f / (1 − T f)` sum to exactly one when `T = 1`,
   whatever the Fresnel term is. Truncating the tail instead of folding it into that closed form
   makes the identity approximate and the furnace test a tolerance argument. Lowering the quadrature
   order therefore cannot change the fibre's albedo — it only blurs the lobes.

5. **Sampling the environment around the fibre's normal circle is REJECTED.** It spans 0.00 to 11.25
   of the true integral, and the zeros are real: at a smooth fibre's grazing angles the scattering
   cone has tilted off the perpendicular plane, the circle misses it, and the coat's ambient goes to
   exactly zero. Use `GroomFibreAmbientResponse` — the mean attenuation times `cos θ_o` — which is
   free, lands in [0.64, 1.11] of the truth, and uses the same attenuations the direct lighting
   uses, so the two cannot drift apart.

6. **The longitudinal term does not depend on `h`. Hoist it.** Inside the quadrature loop it costs N
   fourteen-term Bessel series per lobe instead of one, which is the difference between a four-tap rule
   being affordable in a fragment shader and not. Both the C++ and the GLSL are written this way and
   must stay that way together, because the parity test compares values and a restructure that
   changes the summation order moves the last bits.

7. **The lobe width ratios are derived, never authored.** `v_TT = v_R/4` and `v_TRT = 4 v_R` are
   Marschner's measured ratios. Exposing them would let an author break the measured relationship
   between the two highlights, which is the thing that makes hair read as hair.

8. **Dark versus pale is about WHICH LOBE, not about brightness.** A pale fibre backlit is carried by
   transmission at ~180× its surface reflection; a dark one has absorbed that transmission away. Any
   change that makes pale hair paler by scaling the whole response is wrong, and the test that
   catches it renders the TT lobe ALONE for both pigments under one backlight — not the sum, and not
   a ratio of ratios (rule 13).

9. **R is uncoloured, always.** It never enters the fibre, so it carries the light's colour. Tinting
   it with the pigment is the most common way to get dark hair wrong, and it looks plausible.

10. **The component's ABSENCE is #1246's picture.** A groom with no `GroomFibreComponent` renders the
    neutral root-to-tip ramp the visibility slice shipped. That is what keeps every capture #1246
    committed meaning what it meant, and it is why the material is a separate component rather than
    fields on `GroomComponent` (which is also pinned at 48 bytes with a whole-object memcmp).

11. **Derive `GroomFibreParams` on the CPU, upload the derived values.** The fits involve a `pow`, a
    `log` and a `sin`; deriving them per fragment would pay for them per fragment *and* put them
    between the two sides of a twin whose contract is that they agree on values.

12. **Chiang's published colour inversion is for an ASSEMBLY, and this slice has no assembly.** Used
    directly it renders a coat authored at 0.1 as 0.76, because the absorption it asks for assumes
    neighbouring fibres will do the darkening and there are none until #1248. `BaseColor` bisects
    against this renderer's own albedo instead. Keep `GroomFibreSigmaAFromColor` callable — it
    becomes correct when the density transport lands — but do not wire it back into the mode without
    re-running `ChiangsAssemblyFitIsRejectedForASingleFibre`.

13. **Never compare two tone-mapped means that sit on different parts of the curve.** The
    pale-versus-dark criterion was first written as a ratio of mean coat luminance backlit against
    the same ratio frontal, and it measured *backwards* (2.86 against 3.25) — the pale coat was up on
    the compressive shoulder while the dark one was on the near-linear part. Compare a single lobe
    through a debug view, or compare at matched brightness. The analysis records the numbers.

## The CPU model and the shader must agree

`GroomFibreCommon.glsl` and `GroomFibreScattering.{h,cpp}` are twins. The quadrature rule was chosen
on the CPU, the energy check runs on the CPU, and every number in the analysis comes from the CPU —
so a shader that scattered differently would make all of it a description of something that is not
on screen, and nothing else in the suite could tell, because a plausible hair shader and a correct
one produce the same *kind* of picture.

`GroomFibreGpuParityTest` renders `tests/GroomFibreParityProbe.glsl` over a 64×64 angle grid and
compares every texel. Unlike #1246's integer hash this is floating point with transcendentals, so
the bar is a **2×10⁻³ relative tolerance** rather than exact equality — tight enough to catch a
dropped term or a wrong constant, loose enough to survive a different `exp` implementation. Mirror
the **order of operations**, not just the formula: a mathematically equal rearrangement moves the
last bits, and a tolerance that has to absorb that stops being able to catch a real divergence.

## Things that will bite

- **A probe channel carrying a SUM of two lobes read back as exactly zero**, on this machine's
  driver, while a constant in the same channel read back fine and each lobe alone read its correct
  value. No cause was established. `GroomFibreParityProbe.glsl` now writes one lobe per channel,
  which is better coverage anyway — a lobe that goes missing is invisible inside a sum another lobe
  dominates. If you add a probe that sums anything, measure it before trusting it.
- **Bisect with a constant before rewriting anything.** The zero above cost three speculative
  rewrites of `GroomFibreCommon.glsl` — removing array indexing, then out-parameters — on untested
  hypotheses, when writing `3.0` into the suspect channel settled it in one run.
  [groom-strand-visibility.md](groom-strand-visibility.md) already documents that recipe.

- **`MSAASampleCount` lives on `DeferredSettings`, not on `RendererSettings`.** MSAA is a G-Buffer
  setting on the deferred path only; the strand pass runs after the resolve. Verify that rather than
  assuming it — `TheMaterialSurvivesMsaa` is the capture that does.
- **The multi-light UBO needs no binding call from the pass.** A `UniformBuffer` claims its binding
  point at construction, and `Renderer3D::UploadMultiLightUBO` refills it once a frame — on GL
  through `glBindBufferBase`, on Vulkan through `VulkanBindingState`'s mirror. Declaring the block is
  the whole integration.
- **The irradiance cube DOES need an explicit bind**, from `Renderer3D::GetGlobalIrradianceMapHandle()`
  with `RHI::NullSamplerKind::Cube`. Inheriting whatever a previous draw left on `TEX_USER_0` makes a
  coat's ambient depend on draw order, and a 2D null sampler on a `samplerCube` declaration is
  undefined behaviour rather than a black read.
- **Camera-relative rendering (#429).** The groom pass's model matrices must go through
  `MakeModelRelative`. `u_ViewProjection` and the light positions are both render-relative, so an
  absolute model matrix draws the coat offset from its body *and* lights it from a shifted
  direction — invisible near the world origin, which is where every test scene sits. This was a
  latent bug in #1246's pass and is fixed in #1247's branch as a separate commit.

## Where the evidence lives

- **The decision and its numbers:** [docs/analysis/groom-fibre-scattering-1247.md](../analysis/groom-fibre-scattering-1247.md).
- **The comparison, re-run by CI:** `OloEngine/tests/Rendering/PropertyTests/GroomFibrePropertyTests.cpp`.
- **The twin:** `OloEngine/tests/Rendering/PropertyTests/GroomFibreGpuParityTest.cpp`.
- **The pixels:** `OloEngine/tests/Rendering/PropertyTests/GroomFibreVisualEvidenceTest.cpp` writes
  `OloEditor/assets/tests/visual/GroomFibre_GL_*.png`. Evidence, not SSIM goldens — the composition
  tier underneath is stochastic by design, so a committed per-pixel baseline would be a flake
  generator.
- **The save-game cell:** `OloEngine/tests/SaveGame/GroomFibreMaterialSaveLoadTest.cpp`.
