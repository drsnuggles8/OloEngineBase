# A visual-evidence scene with only a dynamic mesh + light can render unlit black

Short rule for anyone adding a new `RendererAttachedTest`-based visual-evidence
scene (screenshot PNG artifact) that is sparser than the usual "ground + a few
props + subject" layout.

## The trap (issue #460, wind-coupling slice)

A `ClothWindScene` test (camera + one `DirectionalLightComponent` + a single
hanging `ClothComponent`, no ground/props) rendered the cloth as an almost
solid near-black silhouette, regardless of:

- the material's `BaseColorFactor` (tried a muted amber and a saturated
  orange — both read as near-black),
- the light's intensity (tried 2.0 and 3.0), or
- the light's direction (tried the reference scene's overhead-biased angle
  and a more forward-facing one).

Neither the light tweak alone nor a `MeshComponent` ground plane alone fixed
it — screenshot A/B testing showed **both together** were required: the same
scene with the improved light *and* a plain static ground plane (no collider,
purely a rendering prop — the cloth in this test never reaches the floor)
rendered the cloth correctly lit and coloured. Reverting either one on its own
reproduced the near-black result.

This was **not** investigated down to the exact renderer mechanism (shadow
cascade fitting off scene bounds, an auto-exposure/tonemap response to a
near-empty frame, or something else) — treat this as a confirmed, reproducible
symptom + fix, not a root-cause diagnosis. If you get to the bottom of the
actual mechanism, replace this note with the real explanation and consider
whether the renderer should handle a sparse scene more gracefully.

## The rule

**When authoring a `RendererAttachedTest` visual-evidence scene with only one
or two dynamic subjects and no other geometry, don't assume copying the
reference scene's exact light settings is enough — add at least one static
`MeshComponent` (a plain ground plane is enough; it doesn't need a collider
unless the test also depends on physical collision) and check the actual PNG
before trusting a passing contrast/colour assertion.** A luminance-spread or
colour-channel-dominance assertion can pass on a scene that is technically
non-flat but still reads as "black cloth on black background" to a human
reviewer — these checks prove *some* contrast exists, not that the subject is
well-lit. Per this repo's `CLAUDE.md` rule ("rendering changes MUST be
visually verified"), actually open the PNG.

## Guard

No automated guard — this is a rendering-quality issue that numeric pixel
assertions did not catch (they still passed against the near-black frame).
The only guard is: read the PNG artifact after any new visual-evidence test
passes, the same way you would for a changed shader or render pass.
