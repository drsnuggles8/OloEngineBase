# Skin diffusion

Screen-space subsurface scattering over the separated diffuse half of a skin material, with the
specular half recombined untouched. Issue #1241; the profile asset and the split it consumes are
[skin material profiles](skin-material-profiles.md) (#1241's blocker, #1231).

## Turning it on

Two switches, and they answer different questions.

| Switch | Question | Where |
|---|---|---|
| `SkinProfile::EvaluationModel` | Is THIS profile diffused? | The `.oloskin` file — `EvaluationModel: 1` |
| `SkinDiffusionSettings::Enabled` | Does the PASS run at all? | Post Process panel → **Skin Diffusion** |

A profile stays at version 0 until an author moves it, and no renderer setting overrides that. That
is [ADR 0024](../adr/0024-material-kind-is-not-the-closure-version.md)'s rule: correcting or
extending a transport must not silently restate every authored head.

`ReferenceHead.oloskin` is authored at version 1 and is the worked example.

## What the pass does

Every lit pass that can shade skin writes a second render target beside scene colour — the scene
framebuffer's attachment 4 — holding the **diffuse half** of a skin pixel's lighting in `.rgb` and
the **identity of its profile** in `.a`. The diffusion pass blurs that target with the profile's
kernel, horizontally then vertically, and **adds the difference** back into scene colour.

Adding `blur(diffuse) − diffuse` rather than replacing the image is what makes the feature fail
safe: scene colour already holds the sharp composite, so a frame in which the pass is culled,
disabled, or not yet run is exactly the #1231 frame — not a head missing its diffuse lighting. It
is also why nothing downstream has to be rewired to find the result.

The specular half is never handed over, never read by the pass, and cannot be disturbed by it.
That is the whole argument for #1231's split, and the reason this issue's title is about sharp
specular.

## Choosing the diffusion model

The first acceptance criterion asks for a reference comparison before any production tuning.
`experiments/skin-diffusion-reference/compare_profiles.py` is that comparison: a Monte Carlo random
walk in a semi-infinite isotropically-scattering medium gives the ground-truth radial distribution
of light re-emerging around a pencil beam, and three analytic profiles are scored against it under
the parameterisation a `.oloskin` actually authors — transport albedo plus mean free path.

Relative RMS of the radial energy distribution, 4 M photons per channel, reference head parameters,
**lower is better**:

| channel | A | mfp (mm) | Burley | single Gaussian | six-Gaussian (d'Eon) |
|---|---|---|---|---|---|
| red | 0.85 | 1.55 | **0.257** | 0.888 | 0.482 |
| green | 0.55 | 0.80 | **0.144** | 0.857 | 0.551 |
| blue | 0.45 | 0.55 | **0.100** | 0.843 | 0.596 |

Burley wins on every channel, and it wins **unfitted** — `d = mfp / s(A)` straight from Christensen
& Burley's albedo fit, with no free parameter — while both Gaussian forms were given a scale fitted
to the reference. It is also the only one of the three whose CDF is analytic, which is what lets the
tap weights be exact integrals rather than point samples.

**Burley, then.** `SkinBurleyShapeFromAlbedo` / `SkinBurleyProfile` /
`SkinBurleyCdf` in [SkinDiffusion.h](../../OloEngine/src/OloEngine/Renderer/SkinDiffusion.h).

## Choosing the 1D kernel — the non-obvious half

The blur is separable: one 1D kernel applied along x, then along y. The obvious 1D kernel is the
radial profile itself, and that is what most separable subsurface implementations use. It is
measurably wrong.

A two-pass separable blur reproduces the true 2D response of a **straight edge** only if its 1D
kernel is the profile's **line spread function** — the projection of the 2D profile onto one axis.
A lit face is mostly straight edges: a shadow terminator, an N·L falloff, the shaded side of a nose.

Maximum error against a true 2D convolution of the profile, 17 taps:

| channel | d (mm) | px/mm | terminator: radial | terminator: **LSF** | line: radial | line: **LSF** |
|---|---|---|---|---|---|---|
| red | 1.549 | 2.0 | 0.1244 | **0.0258** | 0.5954 | **0.2992** |
| red | 1.549 | 8.0 | 0.1450 | **0.0518** | 1.5374 | **1.1993** |
| green | 0.568 | 2.0 | 0.0984 | **0.0127** | 0.5121 | **0.0294** |
| green | 0.568 | 8.0 | 0.1234 | **0.0396** | 0.8105 | **0.4220** |
| blue | 0.324 | 2.0 | 0.1049 | **0.0082** | 0.4382 | **0.0341** |
| blue | 0.324 | 8.0 | 0.1138 | **0.0359** | 0.5628 | **0.2632** |

*terminator* is the maximum absolute error on a 0→1 step, in a frame whose radiance is 0..1.
*line* is the same on a one-pixel bright line, relative to the true peak. 8 px/mm is a head filling
a 1080p frame in portrait framing; 2 px/mm is the same head at conversational distance.

The line spread function is three to twelve times better on the terminator, across every channel and
framing, **for the same tap count** — it costs nothing at runtime, because both are just numbers in
a table the CPU builds.

It is computed as a **strip fraction** rather than pointwise, because the pointwise line spread
function is singular: R(r) has a 1/r pole and its line integral a logarithmic one, so sampling near
x = 0 measures the quadrature step rather than the profile. In polar form the pole cancels against
the Jacobian and the strip's energy comes out of a bounded 1D integral —
`SkinBurleyStripFraction`, checked against a direct 2D integration in `SkinDiffusionTest`.

## Quality tiers

Taps per axis. The pass is separable, so a tier costs 2N fetches per skin pixel, not N².

| Tier | Taps | Fetches/px | terminator @ 2 px/mm | terminator @ 8 px/mm |
|---|---|---|---|---|
| Low | 9 | 18 | 0.0735 | 0.1295 |
| **Medium** (default) | 17 | 34 | 0.0258 | 0.0518 |
| High | 25 | 50 | 0.0159 | 0.0338 |

Red channel, the widest, so the worst case. Low is visibly banded on a wide radius; the step from
Medium to High is a third of the error for half again the fetches.

## The unit chain

```
ScatterRadiusMM     authored, MILLIMETRES
  -> d              Burley scaling, MILLIMETRES:  d = mfp / s(A)
  -> supportMM      the widest channel's 99.5% support, MILLIMETRES
  -> world units    supportMM * 0.001          (one world unit is one metre)
  -> pixels         world * (0.5 * viewportHeight * P[1][1]) / viewDepth
```

**`ThicknessScale` is not in that chain.** It converts a *material's* authored thickness (metres,
per glTF `KHR_materials_volume`) into the millimetre space the radii already live in: it is the
transmission knob, not a second opinion about how long a millimetre is. Folding it in would make a
profile that exaggerates transmission quietly shrink its own blur, and its lower bound is 0, so the
reciprocal that would need is not even defined. `SkinDiffusionTest` asserts this directly.

The radius follows depth, resolution and field of view because all three are arguments to
`SkinDiffusionRadiusPixels`, and the test asserts each as a **ratio** under a change of that one
input — a test that asserted a number would pass with any of the three dropped.

## Bounded artifacts

| Situation | What happens | Why that |
|---|---|---|
| Face against the near plane | Radius clamps at 64 px | The surface is no longer locally flat over the kernel's footprint, and the cost would grow without bound. The failure reads as a slightly firm close-up. |
| Radius below half a texel | Pass skips the pixel | The kernel cannot express anything a bilinear fetch has not already done. |
| Tap off the screen | Tap rejected | Clamping to the border would smear the edge texel across the whole kernel. |
| Tap on a different profile | Tap rejected | Exact integer test on the profile identity. Two heads with different profiles cannot exchange light. |
| Tap across a depth discontinuity | Tap rejected | Threshold is a multiple of the kernel's own **world-space** support, so it means the same thing on a close-up and at distance. |
| Any rejection | Energy goes to the **centre** tap | A filter that renormalises by the weights it actually used turns every silhouette into a brightness change. |
| Transparent surface in front | That pixel is not diffused | Transparents overwrite the hand-off attachment with zero. Conservative: the head behind reads sharp rather than bleeding through glass. |
| Snow coverage on a skin material | That pixel is not diffused | Snow *replaces* the shaded colour, so its diffuse half is no longer in scene colour to be swapped out. |

## What it costs

One full-resolution RGBA16F attachment on the scene framebuffer, allocated every frame whether or
not the scene has skin in it (≈16 MB at 1080p), plus one full-resolution RGBA16F scratch target
declared only when the pass can run.

The attachment is part of the scene framebuffer because the alternative — switching draw buffers per
draw, keyed on material kind — is a framebuffer state change per draw. Every shader that renders
into that framebuffer **writes** the attachment, with zero where it is not skin: an MRT output a
shader leaves alone is undefined, not zero, and the diffusion would blur the garbage into scene
colour.

Both fullscreen draws are skipped entirely when no authored profile in the frame asks to be
diffused, which is the normal state of every scene without a version-1 skin profile in it.

## Explicit limitations

This is screen-space real-time diffusion, not a claim of volumetric transport.

- **Albedo detail is blurred with the light.** The hand-off carries the lit diffuse term, which
  includes the albedo map. Pore-scale albedo variation inside a 1.5 mm radius is softened. That is
  closer to right than not — melanin and haemoglobin variation lives *inside* the scattering layer —
  but it is not the pre/post-scatter texturing split a film pipeline would use, and it is the first
  thing to add if a head reads as soft.
- **Only what is on screen scatters.** Light that would have entered the surface just outside the
  frame, or behind a nearer surface, is rejected rather than estimated.
- **A one-pixel feature is not reconstructed.** At 8 px/mm the innermost tap spans more than a
  texel, so a delta-function input loses its peak (the *line* column above). Real input — diffuse
  irradiance — is smooth at that scale.
- **No transmission.** Light through an ear lit from behind is not this feature; it is what
  `ThicknessScale` is being carried for.

## Where the code is

| Piece | File |
|---|---|
| All of the maths | `OloEngine/src/OloEngine/Renderer/SkinDiffusion.{h,cpp}` |
| The pass | `OloEngine/src/OloEngine/Renderer/Passes/SkinDiffusionPass.{h,cpp}` |
| The blur | `OloEditor/assets/shaders/SkinDiffusion.glsl` |
| The hand-off encoding | `OloEditor/assets/shaders/include/SkinDiffusionCommon.glsl` |
| The reference comparison | `experiments/skin-diffusion-reference/compare_profiles.py` |

The shader knows no physics: it is a weighted sum along one axis with a bilateral guard, handed a
table of (offset, per-channel weight) built on the CPU. A diffusion profile evaluated in GLSL is a
thing no test can look at, and the number this feature was most likely to get wrong is a unit.
