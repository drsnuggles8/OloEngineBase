# Groom strand visibility: sub-pixel coverage (#1246)

Read before touching `OloEngine/src/OloEngine/Groom/Groom{Visibility,Coverage,StrandMesh,StrandRequest}.*`, `Renderer/Passes/GroomRenderPass.*` or `OloEditor/assets/shaders/GroomStrand.glsl` and its include.

## Rules

1. **Take `abs(projection[1][1])` before using it as a pixels-per-world-unit scale.** Vulkan clip space has +Y down, so the uploaded `[1][1]` is negative there; without `abs()` the strand pass produced a negative half width, `oloGroomWidenedAlpha` clamped to 0, and every fragment was discarded with no log, no VUID, and a recorded draw. Invisible on OpenGL. `Renderer3D.h` and `RenderPipeline.cpp` already use the magnitude. [Details](groom-strand-visibility-details.md#projection11-is-negative-on-vulkan--take-abs-before-using-it-as-a-scale)
2. **Bisect a "pass runs, no pixels" fault in two runs.** Replace `gl_Position` with a fixed quad and the output with a constant colour (appearing clears pass, pipeline, scope, depth, draw: the fault is data); then encode one pulled attribute in red and one UBO lane in green. Read pixels with a decoder, not by eye (tone-mapped). [Details](groom-strand-visibility-details.md#projection11-is-negative-on-vulkan--take-abs-before-using-it-as-a-scale)
3. **Design for below one pixel.** A hair projects to 0.003–0.04 px of half width at any usable framing; sub-pixel behaviour is the entire feature. Measured table: [the analysis](../analysis/groom-strand-visibility-1246.md). [Details](groom-strand-visibility-details.md#the-rules)
4. **Keep the one-pixel width floor with compensating alpha** (`oloGroomWidenedAlpha`). Rasterised honestly, a sub-pixel strand sparkles under motion and vanishes at distance; every composition mode needs this step first. [Details](groom-strand-visibility-details.md#the-rules)
5. **Model square-ended bands, never capsules.** `GroomStrandMesh` emits one quad per segment with no end caps; a round-cap model adds ~0.79 px² per end regardless of thinness (20% of silhouette on a 17 500-segment case). [Details](groom-strand-visibility-details.md#the-rules)
6. **Give all four corners of a ribbon quad the same tangent:** `GroomStrandVertex::Other` is `Position + (P1 − P0)` for every corner. Pinned by `GroomStrandMesh.AllFourCornersCarryTheSameTangentSoTheQuadIsNotABowtie`. [Details](groom-strand-visibility-details.md#the-rules)
7. **Do not use alpha-to-coverage as "the obvious hair AA".** It cannot express coverage below 1/(2·samples) (8 samples need α ≥ 0.0625), then equals a hard cutoff at 553 MiB for ×8 at 1080p, and overlapping strands claim the same samples. [Details](groom-strand-visibility-details.md#the-rules)
8. **Refuse a stochastic mode that nothing converges.** `SelectGroomComposition` reports `TemporalResolveUnavailable` and drops to the opaque tier. Read `TemporalUpscalePolicy::ShouldRunEngineTAA(data.EngineTAAWanted, ...) || data.TemporalUpscaleActive`, not `PostProcessSettings::TAAEnabled` (FSR2 forces engine TAA off but is itself a resolve). A refused coat is bald, so a stochastic groom scene requests its own resolve: [groom-scene-temporal-resolve.md](groom-scene-temporal-resolve.md). [Details](groom-strand-visibility-details.md#the-rules)
9. **Budget a cooked groom with a stride, never a prefix.** The cook makes groups contiguous, so a prefix gives a bald flank (same as `GroomPreview` subsampling). [Details](groom-strand-visibility-details.md#the-rules)
10. **Widths on disk are diameters; halve exactly once,** in `GroomStrandMesh`, where object-space width becomes vertex radius. [Details](groom-strand-visibility-details.md#the-rules)
11. **Write all five scene MRT attachments from the strand pass,** including `o_SkinDiffuse = vec4(0.0)`; an unwritten MRT output is undefined and attachment 4 is blurred into scene colour by `SkinDiffusion.glsl`. See [glsl-shaders.md §4](glsl-shaders.md). [Details](groom-strand-visibility-details.md#the-rules)
12. **Keep `GroomPreview` (debug lines, "did it import") and `GroomRenderPass` (production geometry) separate,** with separate component fields, strand budgets and inspector sections; do not delete the preview. [Details](groom-strand-visibility-details.md#the-rules)
13. **Keep `GroomStrandCommon.glsl` and `GroomCoverage.h` bit-identical twins:** width projection, one-pixel floor, widened alpha and stochastic hash. The hash uses only 32-bit unsigned ops and a 24-bit final shift-scale (2⁻²⁴ grid). `GroomSegmentIdentity` lives in `GroomVisibility.h` and must not be re-derived. [Details](groom-strand-visibility-details.md#the-cpu-model-and-the-shader-must-agree-bit-for-bit)
14. **Measure accuracy, not spread.** Assert `|area − 1|`, not `max(area) − min(area)`; the hard cutoff reproduced 0.0% of the coat at every resolution, so its spread was zero and it scored as the stablest. When a discriminating assertion fails with suspiciously equal numbers (0.13601656493276718 vs itself), the equality is the finding: assert it. [Details](groom-strand-visibility-details.md#when-you-measure-a-composition-approach-measure-accuracy-not-spread)
15. **Treat strand PNGs as evidence, not SSIM goldens.** `GroomStrandVisualEvidenceTest.cpp` writes `OloEditor/assets/tests/visual/GroomStrand_GL_<Path>*.png`; the selected mode's single frame is deliberately noise, and `--olo-golden-rebase` does nothing. [Details](groom-strand-visibility-details.md#where-the-evidence-lives)

## Where the evidence lives

- `OloEngine/tests/Rendering/PropertyTests/GroomCoveragePropertyTests.cpp` (CI re-runs the comparison; every analysis claim is an assertion).
- `OloEngine/tests/Groom/GroomCompositionSelectionTest.cpp` (seam, see [technique-selection-seams.md](technique-selection-seams.md)).
- Live scene: `OloEditor/SandboxProject/Assets/Scenes/GroomStrandCoat.olo`.

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[groom-strand-visibility-details.md](groom-strand-visibility-details.md).
