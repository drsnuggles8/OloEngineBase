# The digital human: running skin, eyes and mouth on one subject

Issue #1222's acceptance, and the only place in this repo where the five skin transport versions,
the ocular surface and the oral surfaces run **on the same subject at the same time**. Each child
issue ships its own fixture and each of those isolates one feature on a probe built to show it;
this page is about what the six do together, which is a different question and has its own failure
modes.

Two artefacts:

| | what it is | what it is for |
|---|---|---|
| [`Assets/Scenes/DigitalHuman.olo`](../../OloEditor/SandboxProject/Assets/Scenes/DigitalHuman.olo) | a head assembled from primitives carrying **seven** registered `.oloskin` profiles | the **live** half — the editor, the Vulkan rows, the MCP debug views |
| [`SkinDigitalHumanEvidenceTest.cpp`](../../OloEngine/tests/Rendering/PropertyTests/SkinDigitalHumanEvidenceTest.cpp) | an L8 headless fixture, GL only | the **asserted** half — tones, expression, cross-talk, cost |

The split is not arbitrary. The tone ladder and the expression need several authored profiles and a
morph target, and both are things a static scene can display but cannot *check*; the Vulkan rows
need a real editor because the headless fixtures require a GL 4.6 context and skip without one.
Neither artefact can do the other's job.

## The slot budget is spent by one face

**A complete face fills the skin-profile slot table exactly.** The G-Buffer names a profile by a
three-bit field with the all-ones pattern reserved for "no profile", so `kMaxSkinProfileSlots` is
**7** ([`Renderer/SkinProfile.h`](../../OloEngine/src/OloEngine/Renderer/SkinProfile.h)), and
`DigitalHuman.olo` names seven: `ReferenceHead`, `EyeIris`, `EyeTearLine`, `OralLip`, `OralTongue`,
`OralGum`, `OralEnamel`.

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

The **ears** are why the backlight cell exists. #1242's transmission is a thin-region term, and
they are the only thin region on the subject (1.5 mm against the cheek's 6 mm). A head with no thin
region, backlit, is just a dark head.

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

## Limits

- **The subject is a procedural stand-in, not a scanned head.** A scanned AAA head remains a genuine
  external dependency — see [benchmark-reference-fixtures.md](benchmark-reference-fixtures.md). Every
  claim these features support is a claim about the *material*, and a material does not know what
  mesh it is on, so dropping a licensed head in changes the subject and nothing else.
- **`ReferenceHead.olo` is deliberately untouched.** It is a pinned benchmark whose captures are
  golden and whose manifest is asserted; adding geometry to it would invalidate every number
  measured against it. `Eyes.olo` and `OralSurfaces.olo` each made the same call, and
  `DigitalHuman.olo` is the third scene in that additive family.
- **The live scene does not carry the tone ladder.** Several tones need several authored profiles,
  and the slot budget above is already spent. The ladder is measured headlessly instead.
