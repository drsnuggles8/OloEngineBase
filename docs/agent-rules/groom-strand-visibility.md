# Groom strand visibility: sub-pixel coverage (#1246)

Read before touching `OloEngine/src/OloEngine/Groom/Groom{Visibility,Coverage,StrandMesh,StrandRequest}.*`,
`Renderer/Passes/GroomRenderPass.*` or `OloEditor/assets/shaders/GroomStrand.glsl` and its include.

## The rules

1. **Design for below one pixel, because that is where hair lives.** A human hair is 17–180 µm. At
   any framing a person would actually use it projects to **0.003–0.04 px of half width** — a strand
   only reaches one pixel of *width* at about 6 cm from the camera at 720p, which is inside the
   skull. "How does this behave sub-pixel" is not an edge case to handle after the main path works;
   it is the entire feature. Measured table: [the analysis](../analysis/groom-strand-visibility-1246.md).

2. **The one-pixel width floor with compensating alpha is the whole mechanism.** A strand narrower
   than a pixel is rasterised at one pixel and its alpha is scaled by how much it was widened
   (`oloGroomWidenedAlpha`). Rasterised honestly instead, a quarter-pixel strand produces a fragment
   only where it happens to cross a pixel centre — so a coat of them sparkles under motion and
   vanishes at distance. Every composition mode is a different answer to "now turn that alpha back
   into coverage"; none of them works without this step first.

3. **Model the shape the GPU actually draws — square-ended bands, never capsules.** `GroomStrandMesh`
   emits one quad per segment and the vertex shader widens it sideways: there are no end caps. A
   round-capped model adds a half-disc of the *rasterised* half width at each end, which is ~0.79 px²
   **regardless of how thin the strand is**, because the rasterised half width has a half-pixel
   floor. On a 17 500-segment sub-pixel case that spurious area was 20 % of the whole silhouette —
   the CPU model reported coverage for a shape nothing ever drew, and the selected mode's silhouette
   read 105–120 % instead of 99–100 %.

4. **All four corners of a ribbon quad must derive the same tangent.** `GroomStrandVertex::Other` is
   `Position + (P1 − P0)` for every corner, not "the other end of my segment". The obvious encoding
   makes the two corners at P1 see the tangent reversed, so they widen the opposite way and every
   quad is an hourglass — which still renders as hair, just thinner, with a pinch at every joint.
   Pinned by `GroomStrandMesh.AllFourCornersCarryTheSameTangentSoTheQuadIsNotABowtie`.

5. **Alpha-to-coverage cannot express coverage below 1/(2·samples).** The mask is `round(α × S)`
   bits, so at 8 samples a fragment needs α ≥ 0.0625 — a half width of 0.031 px — to claim a single
   sample. Below that it produces output *bit-identical* to a hard alpha cutoff while costing a
   multisample target (553 MiB at 1080p for ×8). Its mask is also a deterministic function of α, so
   two overlapping strands at the same α claim the *same* samples and dense coats systematically
   under-cover. Do not reach for it as "the obvious hair AA" without re-reading those numbers.

6. **A stochastic mode with nothing to converge it is a silent fallback — refuse it.** A hashed
   alpha test's single frame is *worse* than a hard cutoff (0.119 vs 0.073 mean coverage error); it
   only wins once a temporal resolve averages it (0.055 over eight frames). `SelectGroomComposition`
   therefore reports `TemporalResolveUnavailable` and drops to the opaque tier rather than shipping
   noise. Read `TemporalUpscalePolicy::ShouldRunEngineTAA(...) || data.TemporalUpscaleActive`, not
   `PostProcessSettings::TAAEnabled` — FSR2 forces engine TAA off while it runs and is itself a
   resolve, so the naive read reports "no resolve" on exactly the frames that have the best one.

7. **A budget over a cooked groom is a STRIDE, never a prefix.** The cook sorts curves so each group
   is contiguous, so "the first N strands" is one side of the animal. The visible result is a bald
   flank, which reads as a broken import rather than as a budget. Same rule, same reason, as
   `GroomPreview`'s subsampling.

8. **Widths on disk are DIAMETERS; halve them exactly once.** `GroomStrandMesh` does it, at the one
   point where object-space width becomes a vertex radius. A second halving downstream produces a
   coat exactly half as thick as authored, which looks like a plausible groom.

9. **The strand pass writes all five scene MRT attachments**, including `o_SkinDiffuse = vec4(0.0)`.
   An MRT output a shader leaves alone is undefined, not zero, and attachment 4 is blurred into
   scene colour by `SkinDiffusion.glsl`. See [glsl-shaders.md §4](glsl-shaders.md).

10. **The diagnostic preview and the production path are different things and must stay visibly
    separate.** `GroomPreview` draws debug lines to answer "did this import correctly";
    `GroomRenderPass` draws geometry to answer "what does this coat look like". They have separate
    component fields, separate strand budgets and separate sections in the inspector. Do not fold
    one into the other, and do not delete the preview — a groom with no shading yet is exactly when
    the debug lines are the only usable view.

## `projection[1][1]` is NEGATIVE on Vulkan — take `abs()` before using it as a scale

Any shader that derives a pixels-per-world-unit scale from the projection matrix must write
`abs(projection[1][1])`. Vulkan's clip space has +Y downwards, so the engine uploads a projection
whose `[1][1]` is negative there; the sign is a clip-space convention and the MAGNITUDE is what a
scale is asking for. The engine already does this elsewhere — `Renderer3D.h` stores
`|cull projection[1][1]|`, `RenderPipeline.cpp` takes `std::abs` of it.

**The failure is invisible on OpenGL and silent on Vulkan**, which is why it gets its own heading.
Without the `abs()` the strand pass produced a negative half width, `oloGroomWidenedAlpha` clamped
it to 0, and the alpha test discarded every fragment. What that looks like from the outside:

- the pass runs and logs its stats (thousands of segments "drawn"),
- the render-graph node is present and declares its resources,
- the draw is RECORDED, not dropped — `PrepareDrawCommon` warns on every early-out and none fire,
- the vertex pull and the UBO both carry correct data,
- 0 errors, 0 VUIDs, 0 validation messages,
- and not one pixel changes.

**The bisect that finds this class of bug in two runs**, rather than by reading code: shaders are
runtime assets, so each iteration is an editor restart, not a rebuild.

1. Replace `gl_Position` with a fixed on-screen quad and the fragment output with an unconditional
   colour. If it appears, the pass, pipeline, render scope, depth state and draw are all fine and
   the problem is the DATA.
2. Keep the fixed quad and encode one PULLED attribute in red and one UBO lane in green. Non-zero
   in both clears the vertex pull and the uniform buffer, leaving only the arithmetic between them.

Read the pixels with a decoder rather than by eye — the frame is tone-mapped, so "looks green" and
"red is zero" are different claims.

## When you measure a composition approach, measure ACCURACY, not spread

The trap, because it nearly shipped: the first version of the resolution-stability test asserted
that the hard cutoff's silhouette area *varied more across resolutions* than the selected mode's.
It failed — because the cutoff reproduced **0.0 % of the coat at every resolution**, so its spread
was exactly zero and it scored as the more stable mode.

The cutoff *is* resolution-stable. Stably at nothing. A spread comparison scores "loses the whole
coat, consistently" as the better result, which is how a measurement ends up flattering the very
approach it was built to reject. Assert `|area − 1|`, not `max(area) − min(area)`.

The same shape appears once more in this file's history: the sub-pixel case originally asserted that
8× alpha-to-coverage *beat* the cutoff, and it failed with `0.13601656493276718 vs
0.13601656493276718`. That equality was the finding (rule 5), and the fix was to assert the equality
rather than to loosen the comparison. **When a discriminating assertion fails with suspiciously
equal numbers, the numbers are telling you something about the technique.**

## Where the evidence lives

- **The decision and its numbers:** [docs/analysis/groom-strand-visibility-1246.md](../analysis/groom-strand-visibility-1246.md).
- **The comparison itself, re-run by CI:**
  `OloEngine/tests/Rendering/PropertyTests/GroomCoveragePropertyTests.cpp`. Every claim the analysis
  makes is one of its assertions, so the evidence fails loudly rather than ageing quietly.
- **The seam, one reason at a time:** `OloEngine/tests/Groom/GroomCompositionSelectionTest.cpp`,
  in the shape `ShadowTechniqueSelectionTest` established — see
  [technique-selection-seams.md](technique-selection-seams.md).
- **The pixels:** `OloEngine/tests/Rendering/PropertyTests/GroomStrandVisualEvidenceTest.cpp` writes
  `OloEditor/assets/tests/visual/GroomStrand_GL_<Path>*.png`. These are **evidence, not SSIM
  goldens** — the selected mode's single frame is deliberately noise, so a committed per-pixel
  baseline would be a flake generator. `--olo-golden-rebase` does nothing here.
- **The live scene:** `OloEditor/SandboxProject/Assets/Scenes/GroomStrandCoat.olo`. Its own header
  explains why it is a new scene rather than an edit to `GroomReference.olo` (which is also #1293's
  repro) or to `Benchmark/AnimalLongCoat.olo` (which is a no-groom baseline that must not improve).

## The CPU model and the shader must agree, bit for bit

`GroomStrandCommon.glsl` and `GroomCoverage.h` are twins: the width projection, the one-pixel floor,
the widened alpha and the stochastic hash all exist in both. The hash is written entirely in 32-bit
unsigned operations whose wraparound is defined identically in C++ and GLSL, and its final
shift-and-scale uses 24 bits so both sides land on the same 2⁻²⁴ grid.

This matters more than it looks: the coverage error every capture is judged against is computed on
the CPU. A shader that widened strands differently, or hashed differently, would make every measured
number a measurement of something that is not on screen. `GroomSegmentIdentity` lives in
`GroomVisibility.h` for the same reason — three places need it and none of them may re-derive it.
