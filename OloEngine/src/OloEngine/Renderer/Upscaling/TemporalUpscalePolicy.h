#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PostProcessSettings.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine::TemporalUpscalePolicy
{
    // @brief The pure decisions and conversions behind FSR2 integration (#684).
    //
    // These live here, as free functions over plain values, for one reason: every
    // one of them is a rule whose failure mode is a PLAUSIBLE IMAGE. A wrong
    // motion-vector scale does not crash or draw black, it draws trails; a jitter
    // converted against the display width instead of the render width does not
    // error, it just quietly stops reconstructing. None of that is visible to a
    // contract test that can only reach the code through a live GL context — so
    // the rules are separated from the GL and pinned by FSR2PolicyTest.
    //
    // RenderPipeline and FSR2RenderPass CALL these rather than restating them.
    // That is the point: the bug this file exists to prevent is two sites
    // deriving the same number slightly differently.

    // Everything the "does FSR2 run this frame" question depends on.
    struct ActivationInputs
    {
        UpscaleMode Mode = UpscaleMode::Off;
        UpscalerTechnique Technique = UpscalerTechnique::Spatial;
        // The pass exists AND its backend reports itself usable.
        bool BackendAvailable = false;
        // Sample count of the scene band. > 1 means the frame is MSAA-resolved.
        u32 SceneSampleCount = 1u;
    };

    // MSAA is not merely untested with FSR2 — it is incompatible. The upscaler
    // reconstructs sub-pixel detail from per-pixel depth and motion vectors, and a
    // resolve has already averaged both across samples, so every disocclusion test
    // it makes is answered with a blend of two surfaces. The result is a soft,
    // crawling image rather than an error, which is exactly why this is a guard
    // and not a note in the docs.
    [[nodiscard]] constexpr bool IsMSAAResolved(u32 sceneSampleCount) noexcept
    {
        return sceneSampleCount > 1u;
    }

    // The single authoritative answer. Note it does NOT fall back to native: when
    // the temporal path is unavailable the caller keeps the render scale and uses
    // the spatial upscaler, so the user's performance choice still holds and the
    // temporal path resumes by itself once the obstruction clears.
    [[nodiscard]] constexpr bool ShouldRunTemporalUpscale(const ActivationInputs& in) noexcept
    {
        return in.Mode != UpscaleMode::Off &&
               in.Technique == UpscalerTechnique::Temporal &&
               in.BackendAvailable &&
               !IsMSAAResolved(in.SceneSampleCount);
    }

    // True when the FSR1 spatial upscaler owns the frame instead. Exactly one of
    // this and ShouldRunTemporalUpscale is true whenever upscaling is on, and both
    // are false when it is off — the property the graph relies on to declare
    // exactly one display-res colour target.
    [[nodiscard]] constexpr bool ShouldRunSpatialUpscale(const ActivationInputs& in) noexcept
    {
        return in.Mode != UpscaleMode::Off && !ShouldRunTemporalUpscale(in);
    }

    // ---- What FSR2 SUPPRESSES, and why these are functions rather than two
    // ---- expressions written twice.
    //
    // A pass being disabled and its output resource being declared are TWO
    // decisions, made in two different functions (ConfigurePassesForFrame and
    // PopulateBlackboard). They must agree. If a resource is declared while the
    // pass that writes it is off, the render graph still routes consumers to it
    // — and since reachability is read->writer, the consumer that selected the
    // unwritten resource severs the chain and EVERYTHING upstream is culled. The
    // frame goes black, with no error, from a one-line change to an enable
    // condition.
    //
    // That is not hypothetical: it is exactly what happened when FSR2 first
    // disabled the late sharpen pass without also un-declaring UpscalerColor.
    // UICompositePass selected UpscalerColor, nothing wrote it, and the entire
    // post chain back to ScenePass was culled. So both sites call these.

    // Engine TAA. FSR2 is itself a temporal resolve fed by the same jitter and
    // motion vectors; running both would have TAA resolve an already-resolved,
    // already-upscaled image whose velocity describes the pre-upscale frame. The
    // user's setting is not mutated — it simply does not take effect this frame.
    [[nodiscard]] constexpr bool ShouldRunEngineTAA(bool taaEnabled, bool temporalUpscaleActive) noexcept
    {
        return taaEnabled && !temporalUpscaleActive;
    }

    // The late post-tonemap sharpen pass (UpscalerRenderPass), which serves CAS
    // on the native path and RCAS on the FSR1 path. FSR2 runs its own RCAS on HDR
    // before tone mapping, so sharpening again here is a second pass over the
    // same edges and rings on high-contrast silhouettes — but an explicit CAS
    // request is still honoured.
    [[nodiscard]] constexpr bool ShouldRunLateSharpen(bool casEnabled, UpscaleMode mode, bool temporalUpscaleActive) noexcept
    {
        return casEnabled || (mode != UpscaleMode::Off && !temporalUpscaleActive);
    }

    // The render-resolution the scene band is sized to. Must reproduce
    // RenderPipeline's scene-band sizing EXACTLY — floor, then clamp to at least
    // one texel — because FSR2 is told this is the region the inputs occupy and a
    // one-texel disagreement offsets every reprojection.
    [[nodiscard]] inline u32 RenderExtentFromDisplay(u32 displayExtent, f32 renderScale) noexcept
    {
        const f32 scale = std::clamp(std::isfinite(renderScale) ? renderScale : 1.0f, 0.25f, 1.0f);
        return std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(displayExtent) * scale)));
    }

    // Turn FSR2's jitter offset (render-resolution pixels, centred on zero) into
    // the NDC offset baked into the projection matrix.
    //
    // The divisor is the RENDER extent, not the display extent. The scene is
    // rendered into a viewport of exactly renderWidth x renderHeight, so NDC's 2
    // units span THAT many pixels. Dividing by the display extent instead makes
    // the jitter shrink in proportion to the render scale — sub-pixel coverage
    // collapses toward zero, FSR2 accumulates frames that all sampled the same
    // geometric point, and the output degrades to a blurry bilinear upscale that
    // still looks like a working temporal upscaler.
    [[nodiscard]] inline glm::vec2 JitterPixelsToNDC(glm::vec2 jitterPixels, u32 renderWidth, u32 renderHeight) noexcept
    {
        if (renderWidth == 0u || renderHeight == 0u)
            return glm::vec2(0.0f);
        return glm::vec2(jitterPixels.x * (2.0f / static_cast<f32>(renderWidth)),
                         jitterPixels.y * (2.0f / static_cast<f32>(renderHeight)));
    }

    // The `jitterOffset` handed to FSR2's dispatch description, given the jitter
    // the pipeline baked into the PROJECTION.
    //
    // THE TWO DIFFER BY A SIGN ON BOTH AXES, and getting it wrong does not break
    // the frame in any way a still image shows. Adding +jitterNdc to the
    // projection's z-column shifts the rendered IMAGE by -jitterNdc; FSR2's
    // jitterOffset has to describe the offset the image actually carries, so it
    // is the negation of what the projection was given. Hand FSR2 the
    // un-negated value and it un-jitters the wrong way — DOUBLING the offset
    // instead of cancelling it — and the accumulated image never converges. On
    // screen that is the whole picture swimming every frame, which is how it was
    // reported; every still capture of it looks perfectly fine, and so does a
    // comparison of two SETTLED captures, which is why the original test set
    // missed it entirely.
    //
    // Measured on the #684 evidence scene, worst frame-to-frame mean|luma diff|
    // across consecutive frames on a static camera (native control: 0.000):
    //
    //     jitter handed to FSR2      worst mean|d|
    //     -------------------------  -------------
    //     as-is                          2.275
    //     negate X only                  2.120
    //     negate Y only                  1.636
    //     negate BOTH                    0.296   <- this
    //
    // Each single-axis flip recovering about half is what identifies it as a
    // sign error on both axes rather than a Y-convention quirk.
    [[nodiscard]] inline glm::vec2 UpscalerJitterFromProjectionJitter(glm::vec2 projectionJitterPixels) noexcept
    {
        return -projectionJitterPixels;
    }

    // The `motionVectorScale` handed to FSR2's dispatch description.
    //
    // Derived, not guessed. Two facts from FSR2's own sources pin it:
    //
    //   1. `ffx_fsr2.cpp` stores `constants.motionVectorScale = params
    //      .motionVectorScale / renderSize`. The value we pass is therefore NOT
    //      itself the shader's multiplier — it is that multiplier scaled UP by the
    //      render extent.
    //   2. `ffx_fsr2_callbacks_glsl2.h::LoadInputMotionVector` multiplies the
    //      sampled texel by that constant and names the result
    //      `fUvMotionVector`, which the reprojection then ADDS to the current UV.
    //      So the shader wants a UV-space vector pointing from the current pixel
    //      to where that surface was in the PREVIOUS frame.
    //
    // The engine's G-Buffer shaders write `o_Velocity = (ndcCurr - ndcPrev) * 0.5`
    // — already UV-space, but CURRENT minus PREVIOUS, the opposite direction. So
    // the shader-side multiplier must be exactly -1, and passing that through
    // fact (1) means passing `-renderExtent` here.
    //
    // Y needs no extra flip, and this is the part worth not re-deriving under
    // pressure: FSR2 works entirely in the texel/UV space of the textures we hand
    // it, and in OpenGL texel (0,0) is the BOTTOM-left, so the UV its reprojection
    // builds from `(iPxPos + 0.5) / renderSize` runs +Y up — the same orientation
    // the engine's NDC-derived velocity is written in. (This is exactly where a
    // port from a D3D/Vulkan sample, whose UV runs +Y down, would need the flip we
    // do not.)
    //
    // A wrong sign on either axis produces a fully plausible still image with
    // trailing edges in motion, so it is stated once, here, and checked by
    // FSR2VisualEvidenceTest.ConvergesAfterCameraMotion rather than by eye.
    [[nodiscard]] inline glm::vec2 MotionVectorScale(u32 renderWidth, u32 renderHeight) noexcept
    {
        return glm::vec2(-static_cast<f32>(renderWidth), -static_cast<f32>(renderHeight));
    }
} // namespace OloEngine::TemporalUpscalePolicy
