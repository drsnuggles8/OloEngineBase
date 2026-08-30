# A surface two subsystems must agree on, and the three ways they agree on the wrong thing

Applies to: `OloEngine/src/OloEngine/Renderer/Water/WaterWake.h`,
`WaterWakeSystem.{h,cpp}`, `OloEditor/assets/shaders/include/WaterWakeCommon.glsl`,
`OloEngine/src/OloEngine/Physics3D/WaterProbe.cpp` — and to any other case where
a shader displaces geometry that gameplay code also has to sample: a deforming
terrain, a soft-body surface, a heightfield the AI paths over.

Written from issue #968 (*Drift*'s boat-shaped Kelvin wake), the follow-on to
#967's foam field. [persistent-world-space-fields.md](persistent-world-space-fields.md)
covers the foam field itself; this document is about the part where the CPU and
the GPU have to produce the *same surface*, and it is a different problem with
different failure modes.

---

## 1. A raster cannot be parity-tested. Choose the representation for that.

The obvious way to give physics the boat's wake was to let it read #967's
disturbance field — the wake is already in there. It is a GPU-written, decaying
RG16F texture, so "already in there" means a readback: a pipeline stall, a frame
of lag, and a number physics can only get *after* the renderer has run. Worse, it
makes the acceptance criterion untestable. "Do the CPU and the GPU agree?" has no
answer when one side's answer is a texel somebody has to copy back and the other
side's is arithmetic.

So the wake **shape** is not the wake **foam**. It is an analytic function of a
small record — four hulls, eight historical poses each, 80 `vec4` — that the CPU
builds once per tick, and that BOTH sides evaluate:

```
BoatWakeSystem (physics tick)
   |
   +--> WaterDisturbanceSystem::SubmitSplat   -> GPU raster  -> foam    (#967)
   +--> WaterWakeSystem::SubmitHull           -> vec4 record -> shape   (#968)
                                                    |
                          +-------------------------+-------------------+
                          |                                             |
              WaterUBO -> WaterWakeCommon.glsl              WaterProbe::SampleSurfaceY
              (vertex + tess-eval stages)                   -> WaterWake::Evaluate
```

Both consumers are handed the same bytes and asked the same question, so parity
is a property of the design rather than a thing to be synchronised. That is what
`WaterWakeParityTest` can assert at all, and it is worth paying for: the record
is 1.6 KB of UBO against a readback's stall.

**The general rule: if two subsystems must agree about a value, do not make one
of them the only place it exists.** Pick the representation that both can
evaluate, even when one of them already has a perfectly good copy.

## 2. The two halves can agree on the FUNCTION and disagree about WHERE

This is the one the parity test cannot catch, because the parity test feeds both
evaluators the same point by construction.

The water shader displaces a mesh vertex. Gerstner (and the FFT's choppiness)
move a vertex **horizontally** as well as vertically — by up to the wave
amplitude, which on a working sea is most of a metre. So a vertex authored at
base XZ `B` ends up at `B + d`. Evaluate the wake at `B` and the ridge is
anchored to the vertex's *authored* position; the vertex then slides out from
under it.

The CPU side has no such ambiguity and never did: `WaterSurface::SampleHeight`
already inverts the horizontal shift with a fixed-point iteration, specifically
so it can answer "how high is the water in the column **above** this world XZ".
That is column space — final world position.

Two correct functions, evaluated in two different spaces. On a flat calm sea they
are identical and every test passes. On a choppy one the rendered ridge sits up
to a metre from where buoyancy thinks it is, and the symptom is a boat that rides
its own wake slightly wrong, which reads as a physics tuning problem.

The fix is one line and its direction is not obvious from either side alone: the
shader must evaluate at the **displaced** vertex's absolute world XZ.

```glsl
vec2 wakeXZ = displacedPos.xz + u_RenderOrigin.xz;   // column space, matches the CPU
```

There is a residual circularity — the footprint suppression scales the very
displacement that placed the evaluation point — and it is second order: the mask
is metres wide and the shift is sub-metre. Note it, do not iterate it.

**Generalised: a CPU/GPU mirror needs its INPUT SPACE pinned as explicitly as its
arithmetic.** Write down which space the shared function's arguments are in
(here: absolute world XZ of the final surface point, not the base vertex, not
camera-relative) at the top of the shared header, because it is the one part of
the contract that a test comparing the two functions at the same point cannot
check.

## 3. A probe shader that COPIES the production GLSL tests the copy

`OloEditor/assets/shaders/tests/` had a working convention before this: lift the
production function into a test-only compute shader and say so in a comment.
`ShaderUnit_Fog.glsl` carries `// Duplicated verbatim from
FogCommon.glsl::computeDistanceFog — changes there must be reflected here or the
test fails`, which is honest and is still exactly backwards. The test does not
fail when the two drift; it passes, about the copy, while the shipped shader does
something else.

`ShaderUnit_WaterWake.glsl` instead `#include`s `WaterWakeCommon.glsl` — the same
text the water vertex and tess-eval stages run. Making that possible is the only
reason the evaluator takes its records through a hook:

```glsl
vec4 waterWakeFetch(int index);   // prototype in the shared file
```

The water stages define it over `u_WakeHulls[]` in the WaterParams UBO; the probe
defines it over an SSBO. One walk over the records, two buffers. Without the
hook, each consumer would write its own loop and the probe would be testing its
own loop — which is the same defect as the copy, one level up.

**If a test needs a production shader function, give the function a seam rather
than the test a copy.** A GLSL function prototype is a cheap seam and it costs
the production path nothing.

## 4. The Kelvin half-angle is a RATIO, and a spread RATE is speed-dependent

#967 laid its foam arms at `halfBeam + 1.6 m/s * age`. That is a lateral
*velocity*, and dividing it by the along-track speed gives an angle that depends
on the throttle: about 15 degrees at 6 m/s, 5 degrees at 18. The comment in the
code said "roughly 15 degrees, against the ~19.5 of a real Kelvin wake", so the
discrepancy was known and recorded — as a constant offset, when it was actually a
constant *ratio* error that vanished at one speed and grew without bound.

Nothing failed. The wake looked right at cruising speed, which is where it was
tuned and screenshotted. What it actually did was **narrow visibly as the boat
accelerated**, which reads as perspective, not as a bug.

The real result is speed-independent — `asin(1/3) = 19.47°` — because the lateral
offset is proportional to the distance run, not the time elapsed:

```
offset(age) = halfBeam + tan(19.47 deg) * |speed| * age
                         \____________/   \___________/
                          a pure ratio     the along-track run
```

So the test has to be written as a ratio too. `WaterWakeShapeTest
.ArmRidgeSitsAtTheKelvinHalfAngleAndIsSpeedIndependent` sweeps 4, 8 and 16 m/s
and asserts the same angle at each — a single-speed test passes on the old
formula, which is how the old formula survived a review that already knew the
number was wrong.

**When a physical constant is a ratio, the test must vary the denominator.** An
assertion at one operating point cannot tell a wrong constant from a wrong
*kind* of constant.

## 5. Two consumers of one pose history must sample the SAME poses

The foam arms and the height ridge are one wake. They are laid from one loop over
the pose history, at one set of ages, through one offset function
(`BoatWakeSystem::ArmOffset`, which delegates to `WaterWake::ArmOffset`).

That is not tidiness. Compute them in two passes and they will be laid from
different samples of the same curve the first time somebody changes an age range,
and the failure is *foam beside the ridge instead of on it* — which looks like a
shader offset, a UV problem, or a half-texel bug, and is none of those. This is
why #968 widened #967's age range and sample count rather than adding a second,
longer range for the height: one polyline, two renderings of it.

The cost is real and worth stating: the arm-splat budget per boat went from 6 to
14 against the field's 96-splat cap, which still fits four boats plus hull and
propeller wash.

## 5b. A hand-posed evidence camera photographs whatever it is pointed at

The five acceptance captures for #968 were built the way the #967 ones were: an
eye position plus a yaw and a pitch, copied from a test whose subject sat
somewhere else. Four of the five pointed **away** from the boat and the fifth
pointed at the **empty sky**. Every one of them rendered a completely plausible
picture of an ocean, the goldens were happily rebased, and roughly two hours went
into instrumenting the UBO, the shader, the evaluator and the record — all of
which were correct the whole time — before anyone pointed the camera at the boat.

Two things follow, and the second is the one worth keeping.

**Frame the subject, do not pose the camera.** `EditorCamera::Focus(point,
distance, yaw, pitch)` orbits a focal point, so the subject is in shot by
construction whatever the yaw convention turns out to be. `SetPose(eye, yaw,
pitch)` requires you to have got the convention right, and getting it wrong is
invisible — it produces a good-looking frame of the wrong place.

**The A/B assertion is what caught it, and a golden never could.** "The wake
changes the frame at all" failed with `maxDiff = 0` from the first run; an RMSE
check against a rebased golden passes trivially, because the golden is whatever
the camera saw. Any evidence test whose subject is a *change* should assert on
the difference between two renders that differ only in the thing under test —
that assertion is the one that knows whether the subject was in shot.

The diagnostic path is also worth stating, because it was cheap once started:
bisect between the CPU and the pixels by making each intermediate visible in
turn — is the uniform arriving (`displacedPos.y += u_WakeShapeParams.y * 2.0`),
does the array read (`u_WakeHulls[2].z`), does the hook work, does the evaluator
fire at a FIXED point known to be inside the wake, does it fire at the vertex's
own position? The first four passed and the fifth did not, which localises the
fault to the evaluation POSITION in one run. A probe whose value is identical in
both halves of the A/B tells you nothing, though — the first `+= 2.0` probe was
wasted for exactly that reason.

## 6. Where the switches live decides whether the tests can see them

#968 ships a visual-only mode — the wake shapes what you see and nothing that
floats — because the two failure modes differ in kind. A visual wake that is
wrong looks wrong; a physical wake that is wrong launches the boat.

The first version put that switch on `WaterWakeSettings`, alongside the height
scale, published by `Scene::ProcessScene3DSharedLogic`. That function is on the
**render** path. A headless scene tick never runs it, so the gate would have been
permanently shut in every functional test — and each of those tests would have
passed, by agreeing that nothing happened.

It lives on `WaterProbe::Volume` instead, read straight off the `WaterComponent`
the body is floating over, which is also where `m_FFTHeightScale` already was for
the same reason. The render half keeps its own settings; the physics half never
depends on the renderer having run.

**Before putting a gameplay switch in a renderer-published settings struct, ask
which paths publish it.** A switch a headless tick cannot open is a feature a
headless test cannot cover, and the resulting green is the most convincing kind
of wrong.

## 7. The build wrapper's exit code is not the build's exit code

Incidental to the feature and worth the line: `build-lock.ps1` returned 0 for a
build that failed with 15 `static_assert` errors. The failure was real (`WaterUBO
unexpected size` — the size assert in `ShaderBindingLayout.h` is a *second* place
the block's size is pinned, in addition to `WaterRenderingTest`), and it was
reported as success twice before the log was read.

Grep the log for `error:` and check the `[n/total]` progress line. See the memory
note `build-result-verification`; this is that note happening again, in a
different wrapper.
