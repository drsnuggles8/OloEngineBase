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

---

## The through-line

Four defects, one symptom, zero test failures. Each was found by *looking at pixels*
under a deliberately chosen condition — a specific distance, a grazing angle, a
top-down view — and none by running the suite. `CLAUDE.md` requires visual
verification for rendering changes for exactly this reason: the math tests prove the
formula, and every bug here was a formula that was individually defensible and
wrong in company.
