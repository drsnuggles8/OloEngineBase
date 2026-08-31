# Water that aliases into hard-edged flats — the normal is the bug, and the suite stays green

Applies to: `OloEditor/assets/shaders/Water.glsl`,
`OloEditor/assets/shaders/include/WaterCommon.glsl`,
`OloEngine/src/OloEngine/Renderer/WaterSurface.cpp`, and any surface that
displaces geometry and derives its own normal.

Written from issue #943 (*Drift*'s wake and sea surface). Four independent defects
produced one symptom — hard-edged, faceted, aliased water. Every one of them was
invisible to the test suite, and two of them had shipped for months.

---

## 1. A derived normal must carry every factor its displacement carries

The Gerstner ladder is eight octaves. Each one displaced the vertex by
`amplitude * weight`, and then computed its normal contribution **without** those
factors:

```glsl
displaced += gerstnerWave(...) * waveAmplitude * 0.55 * meshW;
gerstnerWaveNormal(position, dir0, waveDir0.z, ...);   // <- amplitude dropped
```

So the normals described a sea with waves the geometry did not have. Turning the
amplitude down flattened the *shape* while the *shading* kept its full-strength
creases, which is exactly what "hard-edged flats" looks like: facets lit as though
steep, on geometry that is nearly flat.

**The rule:** every scale factor applied to a displacement must be applied to the
normal derived from it, in the same expression, ideally on the adjacent line. A
displacement and its normal are one change, never two.

### Why nothing caught it

The CPU mirror in `WaterSurface.cpp` — the one `BuoyancySystem` samples — computes
**displacement only**. It has no normal at all. So no C++ math test could have
caught this, and none would catch a re-break either. The pin for it
(`WaterGerstnerNormalScaleTest`) is deliberately a **text** test that reads the GLSL
and asserts each octave's normal call carries the same factors as its displacement
call. A test that cannot fail on the bug it names is worse than no test.

Negative-control it: point the test at the unfixed shader and confirm it flags all
eight octaves. An unfalsified pin is a decoration.

## 2. `fwidth` in a non-quad-uniform branch is undefined

Derivative functions need all four fragments in the quad to reach the same code.
Sparkle and foam noise were evaluated **inside** `if (hasNoiseMap)` /
`if (hasFoamTexture)` — conditions that come from texture samples and are therefore
not quad-uniform. Any `fwidth` of a value computed in there is undefined behaviour,
and on this hardware it produced garbage footprints, which fed the wrong mip and
the wrong AA width.

**The rule:** hoist every value you intend to take a derivative of **out** of any
branch whose condition is not quad-uniform. Compute unconditionally, select after.

## 3. Sub-pixel detail must be DROPPED, not filtered

This is what actually fixed #943.

A normal map whose texels are smaller than a pixel cannot be filtered into
correctness — mip selection blurs its *colour*, but the shading still swings across
the pixel and moirés into bands and hard flats. The fix is to stop asking for the
detail once the frame can no longer resolve it:

```glsl
float detailFootprint = max(length(fwidth(uv0)), length(fwidth(uv1)));
float detailBlend = 0.6 * (1.0 - smoothstep(0.03, 0.12, detailFootprint));
vec3 normal = safeNormalize(mix(gerstnerNormal, normalMapWorld, detailBlend), gerstnerNormal);
```

Accept the consequence honestly: **far water becomes flatter**, because the detail
that was making it busy was never resolvable in the first place. It was aliasing,
not detail. If distant water now looks too plain, the answer is longer-wavelength
*geometry*, not putting the sub-pixel normal map back.

Reflections need the same treatment — `textureGrad(u_EnvironmentMap, reflectDir,
dFdx(reflectDir), dFdy(reflectDir))` rather than an implicit-LOD `texture()`, so the
cubemap fetch is selected off the real screen-space footprint.

## 4. The MESH is the sea-state ceiling, not the shading

After fixing (1) I asserted that Drift's sea-state anchors "had been calibrated
against the shading bug" and raised them from `{0.05, 0.12, 0.22}` to
`{0.22, 0.45, 0.70}`. **My own later measurement disproved it.** Crests facet from
roughly 0.25 upward — clean at 0.22, visibly faceted at 0.32 — because *vertex
spacing* runs out before the shading does. The shipped values were already at the
limit.

Fixing a shading bug does not buy geometric headroom. Before re-tuning any amplitude
after a shading fix, **measure the faceting threshold** and write the measured number
next to the constants, or the next person raises them again.

A storm can be made to read as a storm without taller waves: `foamCoverage`
(whitecaps) plus a darker water colour did it here.

## 5. Generating mips has a blast radius through every implicit-LOD sampler

Adding `GenerateMipmaps()` to the sky cubemap bake — needed so (3)'s `textureGrad`
has anything to select — silently changed the **IBL convolutions**, because
`IBLPrefilter.glsl` and `IrradianceConvolution.glsl` sampled the environment with
implicit-LOD `texture()`. With mips present they began selecting off the derivative
of the sample vector and blurring every procedural-sky scene.

**The rule:** when a texture gains mips, grep every consumer for implicit-LOD
sampling and pin the ones that meant level 0 to `textureLod(..., 0.0)`. They were
relying on "there is only one level" as an invariant, and you just broke it.

## 6. A wall clock keeps animating a paused scene

The shared animation time was `Time::GetTime()`, so pausing stopped the simulation
but not the waves or the foliage. The fix is a scene-owned clock that accumulates
only while unpaused — with two details worth copying:

- **Seed it from the wall clock, not from zero.** The golden evidence tests freeze
  time with `Time::SetMockTime`; under a frozen clock every delta is zero, so the
  value stays at the seed and the captures keep their exact wave phase. Seeding at
  zero silently rephases every water golden.
- **Clamp the delta.** A stop/start, an asset load or a stalled editor frame
  otherwise hands the surface the whole gap at once and jumps the phase visibly.

## 7. A SPECTRUM IS A DENSITY, and a second grid makes its bin spacing load-bearing

From issue #969 (the band-limited three-cascade FFT ocean). Same shape as
everything above: every test green, the sea gone.

Tessendorf's construction sets the amplitude of each frequency bin to
`|h0(k)| = sqrt(Phi(k))` with no bin-area factor. That is sound with **one**
grid, because the missing constant is absorbed into the amplitude scale nobody
measures in absolute terms. It stops being sound the moment a second grid exists:
`Phi` is a spectral **density**, so a bin stands for `Phi(k) * dk^2` of energy,
and `dk = 2*pi/L` differs per cascade. The fine cascade's tile was 4.43x smaller,
so its k-lattice was 4.43x **coarser** and each of its bins stood for 19.6x more
of the spectrum than a mid-band bin — while carrying the same amplitude.

The failure is worth stating precisely, because its signature is what makes it
invisible:

| quantity | single cascade | three cascades (unfixed) |
|---|---|---|
| summed height RMS | 0.167 m | 0.170 m — **preserved** |
| summed **slope** RMS | 0.0425 | 0.0262 — **38% lost** |

The amplitude normalisation is defined on height, so it dutifully held height
constant while the energy drained out of the short waves. **Height is what the
tests asserted; slope is what the eye reads.** The captures showed a
mirror-smooth foreground you could see the sky reflected in — a sea that had
stopped being water — and not one assertion moved.

**Two rules come out of it.**

*Weight each grid by its own bin spacing.* Multiply cascade `i`'s spectrum by
`dk_i = 2*pi/L_i` before the shared normalisation. Only the ratio between bands
matters, which is also why a one-cascade field is bit-identical: a single
constant factor divides straight back out through the normalisation.

*When a change redistributes a signal across scales, assert on the DERIVATIVE,
not just the value.* A test on summed height cannot tell "the same sea, arranged
differently" from "a flat sea with a long swell under it". `OceanCascadeTest
.DiagBandEnergyAndSlopeAtDriftSettings` reports per-band height RMS **and**
summed slope RMS, and fails if the preset costs more than 30% of the slope the
author tuned. This generalises past water: any change that moves energy between
frequencies — an LOD scheme, a noise octave rebalance, a filter — needs its
acceptance written on the quantity that survives the redistribution, and the
value usually is not it.

The diagnostic path was cheap once started, and is the one to copy: the captures
said "flatter", so the question became *which* statistic had moved. Measuring
per-band RMS **and** slope at the scene's own authored settings localised it to
the fine band in one run — no bisecting, no instrumenting the shader.

---

## The through-line

Five defects now, one symptom, zero test failures. Each was found by *looking at
pixels* under a deliberately chosen condition — a specific distance, a grazing
angle, a top-down view — and none by running the suite. §7 was found the same way
a year of water work later, in a subsystem with a parity test that was passing
*exactly*, on a quantity that was not the one that mattered. `CLAUDE.md` requires visual
verification for rendering changes for exactly this reason: the math tests prove the
formula, and every bug here was a formula that was individually defensible and
wrong in company.
