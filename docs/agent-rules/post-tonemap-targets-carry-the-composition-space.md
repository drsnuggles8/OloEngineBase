# A pass after the tone map works in the composition space, which exceeds 1.0 in HDR

Rules for any pass that runs after `ToneMapPass` (sharpening, vignette, FXAA, the selection
outline, `UIComposite`, colour-vision adaptation, `FinalRenderPass`, and anything inserted
among them). The spaces are defined in
[DisplayOutput.h](../../OloEngine/src/OloEngine/Renderer/DisplayOutput.h) (issue #1353).

1. **Declare the pass's display-resolution output with `postToneMapFormat`**
   (`RenderPipeline.cpp`, `PopulateBlackboard`), never a literal `RGBA8UNorm`. It is RGBA8 in
   SDR, where the chain is bit-identical to before #1353, and RGBA16F when
   `FrameGraphDeclarationConfig::HdrPostChain` is set by an HDR output mode. An RGBA8 target in
   the HDR chain clips every highlight to UI white, and every SDR test stays green.
2. **Never clamp a post-tone-map value to 1.0.** The composition space is gamma-encoded and
   relative to UI white. 1.0 is UI white, and a highlight on an HDR display is 2, 5 or 10.
   Floor at 0 when a `pow` needs it (`max(x, vec3(0.0))`). In SDR the RGBA8 store clamps on
   its own, so removing the ceiling changes no SDR pixel.
3. **Decode and re-encode with `PostProcessSettings::Gamma`**, the value the tone map encoded
   with. The display encode decodes with the same value (`DisplayOutput::GetCompositionGamma`).
4. **Do not encode for the display in a pass.** On Vulkan in an HDR mode, "the backbuffer" is
   the context's RGBA16F composition image. `VulkanContext::RecordDisplayEncode` converts it to
   HDR10 PQ or scRGB after the UI overlay. A pass that writes PQ itself is encoded twice.

## What stayed green

`PostProcess_ColorBlind.glsl` re-encoded with `pow(clamp(adapted, 0, 1), 1/gamma)`. With
HDR output on, every pixel above UI white came out at exactly UI white whenever a colour-vision
mode was active. The colour-blind tests stayed green because their targets are RGBA8, where
the clamp and the store agree. Found while building #1353. The shader now floors at 0 only.
