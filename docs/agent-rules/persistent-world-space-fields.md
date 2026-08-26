# A persistent world-space field that never fades, and every math test green

Applies to: `OloEngine/src/OloEngine/Renderer/Water/WaterDisturbanceField.h`,
`WaterDisturbanceSystem.cpp`, `OloEditor/assets/shaders/compute/WaterDisturbance_Update.comp`,
`include/WaterDisturbanceCommon.glsl` — and to any other *accumulation buffer* the
engine keeps across frames: the snow-depth clipmap, a damage map, a decal fade
buffer, a footprint field.

Written from issue #967 (*Drift*'s persistent boat wake). The toroidal addressing
itself is covered by [ddgi-probe-cascades-and-sparsity.md](ddgi-probe-cascades-and-sparsity.md)
§1–2 and [terrain-virtual-texturing.md](terrain-virtual-texturing.md); this
document is about the three things that are **specific to a field that DECAYS**,
and each of them renders a perfectly plausible picture while being wrong.

---

## 1. A multiplicative decay is unrepresentable in a normalized-integer texture below a rate threshold

This is the one that nearly shipped, and it is arithmetic, not graphics.

A field that fades exponentially runs `value *= factor` once per frame. Store it
in an 8-bit normalized texture and every write is **rounded to the nearest 1/255**.
So the decay only happens at all if one frame's step exceeds half a quantum:

```
step  = v * (1 - factor)        must exceed        0.5 / 255  ≈ 0.00196
```

For a 6-second half-life at 60 Hz, `factor = 2^(-1/360) ≈ 0.998076`, and a texel
sitting at 0.5 loses **0.00096** per frame. That is under the threshold. The value
settles onto the nearest representable level, and every step after that rounds
back to the level it is already on — so **the field never changes again**.

Measured over 600 frames (10 s, 1.67 half-lives), from 0.5:

| storage | value after 10 s | lost |
|---|---|---|
| exact | 0.157 | 69% |
| 16-bit float | 0.157 | 69% |
| R8 | 0.498 | **0.4%, all of it in the first step** |

What that looks like: a wake that renders correctly, follows the boat correctly,
wraps correctly — and is still sitting there, at full brightness, ten minutes
later. Every unit test of the decay *function* passes, because the function is
fine. The bug is in the **storage**, which no math test touches.

Note the shape of the R8 row: it is not "decays slowly", it is "moves once and
stops". An assertion written as *"the value has barely changed"* is the right one;
one written as *"the value is exactly where it started"* is wrong, because 0.5 is
not representable in R8 (0.5 × 255 = 127.5) so the first write snaps it one
quantum. That over-precise assertion is what the test was first written with, and
it failed — usefully.

Two things follow:

* **Pick the format from the decay rate, not from the value range.** A field whose
  values are in [0, 1] looks exactly like an R8 candidate, and the range is not
  the constraint. A 16-bit float's relative quantum near 0.5 is ~2⁻¹¹ ≈ 0.00049,
  so the same step is ~4 quanta and the decay proceeds.
* **The test has to model the QUANTISATION, not the formula.**
  `WaterDisturbanceFieldTest.DecayStallsUnderR8QuantisationButNotUnder16BitFloat`
  runs the recurrence through both quantisers for 600 frames and asserts R8
  *stalls* and half *decays*. It asserts first that the fixture is still in the
  stall regime (`ASSERT_LT(perFrameStep, 0.5f/255.0f)`), so a future default that
  moved the half-life down cannot turn it into a test that asserts nothing.

The same arithmetic applies to any slow accumulation in a normalized format —
melting snow, a fading damage overlay, a heat map. If the per-frame change is
smaller than half a quantum, the buffer is frozen and nothing says so.

**The engine's `ImageFormat` has no single-channel 16-bit float**, and adding one
is not a free fix: the enum's own comments record `R32UInt` having had nothing to
map to on the Vulkan side and silently returning the NULL texture handle, so the
resource simply did not exist on that backend. `RG16F` is already plumbed on both,
carries the identical mantissa, and costs one extra unused channel. Take the
plumbed format over the tidy one.

## 2. A per-frame stamp cannot draw a diverging shape — the divergence has to come from the LOOKUP

A boat's wake is a **V** that widens behind the hull. The obvious implementation
stamps the trail at the hull's current position each frame with a lateral offset,
and it cannot produce a V: at the moment a piece of water is stamped, its offset
is whatever the offset is *now*, and nothing ever goes back to widen it. You get
three parallel lines.

The fix is to invert where the age comes from. Keep a **bounded, time-stamped
history** of poses, and each frame lay the arms at the pose that is now
`kArmAgeSeconds` old, offset by an amount computed from that age. Each patch of
water gets its arm laid exactly once — when it reaches the right age — and
because successive frames pick successively older samples, the offset grows along
the trail. The V diverges, and it follows an S-turn for free, because the
historical *headings* curve.

The general shape: **when a field is written once per frame but the thing you are
drawing has extent in TIME, the time has to be in the read, not in the write.**

## 3. A persistent field's `dt` must come from the mockable clock, or none of its goldens are stable

`RenderPipeline`'s wind/snow block derives `dt` from a `static steady_clock`. That
is fine for something whose state you never assert. It is fatal for a field whose
whole visible behaviour is *how much it has decayed*: a capture decays by however
long the previous frame happened to take, so two runs of the same test differ, and
a golden of a decayed trail is flaky by construction.

Use `Time::GetTime()` against a stored previous sample (`Renderer3DData::
WaterDisturbancePrevTimeSeconds`, mirroring `CloudPrevTimeSeconds`). Then
`Time::SetMockTime` freezes the field, and a test can advance it by an exact
number of seconds — which is what lets `WaterWakeVisualEvidenceTest` capture
"the same trail, twelve seconds later" as a golden at all.

**But the field has to be TICKED, not fast-forwarded.** The same `dt` is clamped
(0.25 s here) so a breakpoint or a frame hitch cannot wipe the field in one step —
correct engine behaviour, and it means setting the mocked clock 12 s ahead and
rendering ONE frame decays the field by 0.25 s. The test that did that measured no
decay at all, which reads as a broken decay rather than as a broken test. Advance
in steps under the clamp, one render each (`DecayFor`).

And note what else moves when you advance that clock: the **wave phase**. See §4.

## 4. What the golden cannot tell you, and what to assert instead

Three of this field's failure modes produce a *different but entirely plausible*
frame, so an RMSE-vs-golden check reports them identically to a driver
difference:

| failure | what the frame shows |
|---|---|
| bare `%` on a negative lattice coordinate | a wake, mirrored about the world origin |
| dropped half-texel in the world↔texel mapping | a wake, half a texel off the hull |
| newly-exposed texels not reset on a window shift | a wake, plus a second phantom one trailing the camera |
| R8 storage (§1) | a wake that is completely correct and never fades |

So the evidence test asserts three things from the readback itself, none of which
a golden can express:

1. **there is a trail** — the on-trail band is brighter than open water beside it;
2. **it is in the right PLACE** — the bright band is where the splats were laid,
   checked by comparing the centre column against two symmetric off-trail columns.
   This is the assertion that catches all three addressing failures, because each
   of them moves the wake somewhere else while leaving something plausible on
   screen;
3. **it fades** — the same pixels are dimmer after the clock advances, which is
   the only thing that fails on §1.

Note that (2) works by comparing *within one frame*. A brightness-only check
("the wake is bright") passes on a mirrored field; a golden passes on nothing but
the exact frame it recorded. Comparing two regions of the same frame is what
separates "wrong" from "different".

### (3) must be measured as CONTRAST, not brightness — the clock moves the sea too

Advancing the clock to decay the field also advances the wave phase, so the whole
sea is different between the two captures. Measured absolutely, the decayed frame
came out **marginally brighter** than the undecayed one: the sea moved further
than the wake faded. The assertion has to be *trail minus nearby open water in the
same frame*, before and after — the part of the signal that is actually about the
field. (Measured here: contrast 11.8 → must fall below 60% of that.)

### Measure the sampling bands off a real capture; do not derive them from fractions

The first version of these assertions sampled "rows 55%–80%, centre 8% of the
width", which sounds like the middle of the frame and is not where the trail
projects: it straddled the trail's near end and ran on into foreground water, and
reported *no wake* in a frame that plainly has one. Capture once, read the PNG,
take a row/column profile, and write the measured pixel bounds into the test with
the measured values in the comment. A band derived from percentages is a guess
about the projection, and the failure it produces looks exactly like the bug the
test exists to catch.

## 5. Adding a `TEX_*` slot is also a shader edit — and it moves a hand-written literal

Covered in full in `ShaderBindingLayout.h` and `include/BindlessHeap.glsl`, but it
comes up the moment a field like this needs a sampled view: `HEAP_IMAGE_SLOT_BASE`
is derived from `TEX_SHADER_GRAPH_0`, and the GLSL twin `OLO_HEAP_IMAGE_BASE` is a
hand-written literal that does not move with it. #967 pushed it 71 → 72 (and the
mirrored copy in `BindlessHeapGpuTest.cpp` with it). It is pinned headlessly by
`BindlessShaderPipeline.HeapImageBaseMatchesTheBindingLayout` — if that fails, fix
the literal, do not relax the test.

## 6. Where this kind of field belongs, and where it does not

Not in the render graph. `WriteNewVersion` renames a physical resource and the
transient pool aliases memory between passes — a texture that must survive to the
next frame is precisely the stale-pool-read archetype in
[render-graph-transient-aliasing.md](render-graph-transient-aliasing.md).
`WaterDisturbanceSystem` is a static singleton dispatched from `RenderPipeline`,
next to the wind and snow computes, exactly like `SnowAccumulationSystem` — which
is the same shape and was already there.

Two consequences worth stating because both are easy to skip:

* **Reset it on scene entry.** The field is world-anchored, so a second Play
  session or a runtime scene switch into another watery scene inherits the
  previous one's trail *at the same world coordinates*. It then decays away over
  a few seconds, which reads as a rendering glitch rather than as stale state.
  `Scene::OnRuntimeStart` / `OnSimulationStart` call `Reset()`, alongside dropping
  the pose history — whose timestamps are against `m_SimulationTime`, which those
  same functions zero, so a retained history would carry timestamps from the
  future and the age lookup would never match.
* **The disabled state must be published, not just skipped.** `GetShaderParams()`
  returns intensity 0 for every reason the field could be unusable — not
  initialised, disabled, never yet written — and the scene publishes its settings
  *unconditionally*, including the default disabled form. Publishing only when
  enabled is what would let a scene switch inherit the previous scene's field.

## 7. One validation boundary, so the CPU and GPU halves can stay literal mirrors

`WaterDisturbance::SplatWeight` and `waterDisturbanceSplatWeight` are
expression-for-expression identical, down to using `max(x, 1e-4)` guards rather
than the `std::isfinite(x) ? x : fallback` form the rest of the engine uses for
deserialized floats. That is deliberate: non-finite inputs are rejected one layer
up, at `WaterDisturbanceSystem::SubmitSplat`.

A CPU/GPU pair that sanitises *differently* agrees on every value anyone tests and
disagrees on exactly the ones nobody does. Put the validation at the submission
boundary and keep the mirrored math free of it.
