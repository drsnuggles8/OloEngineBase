# The digital human: the skin epic's acceptance subjects

Issue #1222's acceptance. **Two artefacts with two different subjects**, and keeping them straight
matters — they cover different things and it is easy to credit one with the other's results.

| | subject | transport coverage | what it is for |
|---|---|---|---|
| [`Assets/Scenes/DigitalHuman.olo`](../../OloEditor/SandboxProject/Assets/Scenes/DigitalHuman.olo) | a **scanned human head** (`InfiniteScanHead`, CC BY 3.0) carrying `ReferenceHead.oloskin` | **transport version 1 only** — diffusion, and nothing above it | the **live** half — the editor, the Vulkan rows, the MCP debug views |
| [`SkinDigitalHumanEvidenceTest.cpp`](../../OloEngine/tests/Rendering/PropertyTests/SkinDigitalHumanEvidenceTest.cpp) | a **procedural multi-profile probe** (primitives), GL only | **version 5**, so the whole cumulative ladder | the **asserted** half — tones, expression, cross-talk, cost |

**The cumulative version-5 coverage belongs to the procedural probe, not to the scanned head.**
`ReferenceHead.oloskin` is authored at version 1, so #1242's thin-region transmission and #1243's
layered specular are *not enabled* on the live scene's head. Its backlight rig is there as the
place that gap is visible — the material-transmission view is black over the head — and not as
evidence that transmission works. That is tracked as #1394.

The split is not arbitrary. The tone ladder and the expression need several authored profiles and a
morph target, and both are things a static scene can display but cannot *check*; the Vulkan rows
need a real editor because the headless fixtures require a GL 4.6 context and skip without one.
Neither artefact can do the other's job.

## The slot budget is spent by one face

**A complete face fills the skin-profile slot table exactly.** The G-Buffer names a profile by a
three-bit field with the all-ones pattern reserved for "no profile", so `kMaxSkinProfileSlots` is
**7** ([`Renderer/SkinProfile.h`](../../OloEngine/src/OloEngine/Renderer/SkinProfile.h)) — and a
face needs exactly that many: `ReferenceHead`, `EyeIris`, `EyeTearLine`, `OralLip`, `OralTongue`,
`OralGum`, `OralEnamel`.

The **headless probe** is where this is asserted (four parts, four distinct slots, four distinct
parameter sets). `DigitalHuman.olo` no longer demonstrates it: it carries one profile, because the
six extra primitives that had been keeping its count at seven were removed once they stopped being
visible. Tracked as #1393.

An **eighth** profile in a scene containing this head does not fail loudly. It resolves to
`kSkinProfileSlotNone` with `SkinProfileFallbackReason::SlotBudgetFull`, logs once per handle, and
renders as **not skin** — a plausible frame with one surface quietly shading as a dielectric. A
second character, a different eye colour, or a freckled variant of this head is each that eighth.

So: before adding a profile to a scene that already has a face in it, count. If you need more than
seven, the field has to widen, and that is a G-Buffer change with a mirrored GLSL decode
([`include/PBRCommon.glsl`](../../OloEditor/assets/shaders/include/PBRCommon.glsl)), not a table
resize.

## The debug views are deferred-only

`MaterialDebugView` — `Diffuse`, `Specular`, `Transmission`, `ProfileIdentity`, `ScatteringMask` —
is branched in
[`include/DeferredLightingShared.glsl`](../../OloEditor/assets/shaders/include/DeferredLightingShared.glsl),
and `RenderPipeline.cpp` forces it to `None` on the forward paths. There is no forward equivalent
and asking for one on Forward or Forward+ silently gives you a composite.

The headless fixture refuses that combination with a named failure rather than capturing a beauty
frame and comparing it against an AOV, which is the shape of the bug it would otherwise hide.

**The skin diffusion pass stands down under every view except `Diffuse`** (#1394,
`SkinDiffusionRunsThisFrame` in `RenderPipeline.cpp`). It adds `blur(aux) - aux` into scene colour
in place, and under a debug view scene colour is the AOV rather than the composite. Before the fix,
the high-pass of the diffuse half landed on the `Transmission` view and outlined every crease of a
backlit head, even at version 1, where the term is exactly zero. The `Diffuse` view keeps the
pass, because the diffused diffuse half is what that view shows.


**History is not one of these views.** Per #1256, history separability is delivered as the three
separated reactive causes on `TemporalReactivity` (`SurfaceMotion` / `CoverageChange` /
`MaterialChange`) in `Renderer/SurfaceHistory.h`, not as a `MaterialDebugView` AOV. Ask *which*
cause dropped the history rather than reading one blended confidence number.

## Switching lighting rigs in the live scene

The scene carries all three rigs of the acceptance criterion — `Soft Key`, `Hard Side`, `Backlight`
— and only one is strong. Change a rig by editing **intensities on lights that are already placed**,
never by adding a light: the geometry, the exposure and the shadow settings then stay fixed and the
capture is comparable.

| cell | Soft Key | Hard Side | Backlight |
|---|---|---|---|
| soft (authored default) | 6 | 0.6 | 0.8 |
| hard side | 0.5 | 9 | 0.8 |
| backlight | 0.3 | 0.3 | 11 |

The backlight cell is currently a **negative** result and worth reading as one: #1242's
transmission is a thin-region term, and the scanned head's profile is at version 1, so the
material-transmission view is black over it. That is the term being disabled, not broken — #1394.
The transmission evidence that *is* positive comes from the headless probe, whose cranium carries
a version-5 profile and a thickness map thin at the silhouette.

## What the headless fixture actually asserts

Each claim is A against B on the same scene with **one authored value moved**, measured against the
fixture's own same-mode repeat floor — per raster path, because deferred is jittery here and
forward is not.

- **Four parts, four slots, four parameter sets.** The cheapest way the table can be wrong and
  still look right is two parts aliasing onto one slot, which renders teeth shading with the lips'
  mean free path.
- **The expression reaches the shading, not only the geometry.** #1243 derives its detail gain from
  `MorphTargetComponent::AppliedWeights`, so the control arm re-authors *only* the
  `ExpressionDetailGain` and requires the frame-to-frame difference to shrink. Without that arm the
  test passes on a subject whose expression only ever moved vertices.
- **The tone ladder never inverts, under any rig**, and the three tones stay tellable apart under
  all three including backlight.
- **The wider scattering kernel changes its frame more** — measured by toggling the diffusion pass
  per tone and normalising by that tone's own mean luma, which is what makes it a claim about the
  **mean free path** rather than about the albedo. Asserted under the soft and hard-side rigs only:
  the diffusion pass redistributes the *diffuse* half, and under a pure backlight that half is
  almost absent, so the measurement degenerates into noise (it inverts there, 0.0065 against
  0.0076). Two earlier formulations of this claim were wrong and both are recorded in the test —
  raw red fraction reads the albedo, and the red *shift* has an unstable sign.
- **Each view responds to its own control and not the others'.** The obvious test of a
  decomposition is that the parts sum to the whole, and it cannot be written here: the composite is
  tone-mapped, so any tolerance loose enough for the curve also accommodates a wrong decomposition.
  Cross-talk survives tone mapping — if the specular view moves when the *transmission* strength is
  re-authored, the two terms are not separated.

## What the cost numbers can and cannot tell you

`DigitalHuman_Timing.txt` records the full skin stack against a version-0 split-only control, per
raster path, each arm captured after a **discarded warm-up** so neither pays for the frame-graph
reconfigure a path switch forces. Timing the first capture after that switch is how an earlier
version of this fixture reported a uniform ~+95% that was the rebuild rather than the skin.

With the warm-up removed the measured differences come out **within noise and sometimes negative**.
The full stack cannot really be cheaper than the control, so a negative percentage means the cost
is below this fixture's noise floor at 384×384 with one head on screen — not that the stack is
free. Establishing a real per-pass budget needs GPU timer queries on a quiet box and a scene with
enough skin to dominate the frame. Do not quote these figures as a budget.

## Limits

- **The headless subject is a procedural probe, deliberately.** Its assertions are material claims
  measured against a per-rig repeat floor, and its measurement disc and hue histogram are
  calibrated to a known silhouette — swapping in real head geometry broke six of ten tests when
  tried, for reasons that had nothing to do with skin. A material does not know what mesh it is
  on, which is what makes the probe valid; it is not a picture of a person and should not be
  presented as one.
- **Neither subject can demonstrate eyes or mouth.** The scan has closed eyelids and its own lips;
  no asset in this repo has separable eye or oral geometry. An earlier revision assembled one from
  primitives and it does not work at any level of effort — #1402 records the whole dead end. So
  #1222's first acceptance criterion is **unmet**, and #1244's `Eyes.olo` and #1245's
  `OralSurfaces.olo` remain where that geometry is judged.
- **`ReferenceHead.olo` is deliberately untouched.** It is a pinned benchmark whose captures are
  golden and whose manifest is asserted; adding geometry to it would invalidate every number
  measured against it. `Eyes.olo` and `OralSurfaces.olo` each made the same call, and
  `DigitalHuman.olo` is the third scene in that additive family.
- **The live scene does not carry the tone ladder**, and now carries only one profile. Several
  tones need several authored profiles; the ladder is measured on the headless probe instead. The
  seven-slot budget finding is asserted there too (#1393) — an earlier revision kept six extra
  primitives in the live scene purely to hold the count at seven, which demonstrated nothing once
  they were no longer visible.
