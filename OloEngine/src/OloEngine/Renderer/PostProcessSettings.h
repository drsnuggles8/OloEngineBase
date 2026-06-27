#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace OloEngine
{
    // Tonemap operator constants (match PBRCommon.glsl defines)
    enum class TonemapOperator : i32
    {
        None = 0,
        Reinhard = 1,
        ACES = 2,
        Uncharted2 = 3
    };

    // AO technique selector
    enum class AOTechnique : i32
    {
        None = 0,
        SSAO = 1,
        GTAO = 2
    };

    struct PostProcessSettings
    {
        // Tone mapping (always active)
        TonemapOperator Tonemap = TonemapOperator::Reinhard;
        f32 Exposure = 1.0f;
        f32 Gamma = 2.2f;

        // Automatic exposure / eye adaptation (histogram metering).
        // When enabled, a compute pass meters the HDR scene luminance each frame
        // and drives the exposure multiplier instead of the manual Exposure above
        // (the eye adapting between bright exteriors and dark interiors). The math
        // lives in Renderer/AutoExposure.h and is pinned by AutoExposureMathTest.
        bool AutoExposureEnabled = false;
        f32 AutoExposureMinLogLuminance = -8.0f; // histogram lower bound, log2 luminance
        f32 AutoExposureMaxLogLuminance = 3.5f;  // histogram upper bound, log2 luminance
        f32 AutoExposureSpeedUp = 3.0f;          // adaptation rate when brightening (per second)
        f32 AutoExposureSpeedDown = 1.0f;        // adaptation rate when darkening (per second)
        f32 AutoExposureCompensation = 0.0f;     // EV bias; +1 doubles the resulting brightness
        f32 AutoExposureMinExposure = 0.05f;     // hard clamp on the metered exposure multiplier
        f32 AutoExposureMaxExposure = 16.0f;     // hard clamp on the metered exposure multiplier

        // Bloom
        bool BloomEnabled = false;
        f32 BloomThreshold = 1.0f;
        f32 BloomIntensity = 0.5f;
        i32 BloomIterations = 5;

        // Vignette
        bool VignetteEnabled = false;
        f32 VignetteIntensity = 0.3f;
        f32 VignetteSmoothness = 0.15f;

        // Chromatic Aberration
        bool ChromaticAberrationEnabled = false;
        f32 ChromaticAberrationIntensity = 0.005f;

        // FXAA
        bool FXAAEnabled = false;

        // Depth of Field
        bool DOFEnabled = false;
        f32 DOFFocusDistance = 10.0f;
        f32 DOFFocusRange = 5.0f;
        f32 DOFBokehRadius = 3.0f;

        // Motion Blur
        bool MotionBlurEnabled = false;
        f32 MotionBlurStrength = 0.5f;
        i32 MotionBlurSamples = 8;

        // Temporal Anti-Aliasing (TAA)
        // Velocity-reprojected temporal accumulation with 3x3 neighborhood
        // colour clamping. In Deferred mode consumes G-Buffer RT3 velocity;
        // in Forward / Forward+ reconstructs camera-motion velocity from
        // depth. Does not currently inject projection jitter (sub-pixel AA
        // quality is reduced; ghost-elimination + temporal-aliasing smoothing
        // still function).
        bool TAAEnabled = false;
        f32 TAAFeedback = 0.9f;   // History blend weight (0..1); higher = smoother, slower response
        f32 TAASharpness = 0.25f; // Post-TAA sharpen amount to offset blur (0 = off)

        // Color Grading
        bool ColorGradingEnabled = false;

        // AO technique (None/SSAO/GTAO)
        AOTechnique ActiveAOTechnique = AOTechnique::SSAO;

        // SSAO
        bool SSAOEnabled = false;
        f32 SSAORadius = 0.5f;
        f32 SSAOBias = 0.025f;
        f32 SSAOIntensity = 1.0f;
        i32 SSAOSamples = 32;
        bool SSAODebugView = false;

        // GTAO
        bool GTAOEnabled = false;
        f32 GTAORadius = 0.5f;             // World-space AO radius
        f32 GTAOPower = 2.2f;              // AO contrast curve
        f32 GTAOFalloffRange = 0.615f;     // Relative falloff distance
        f32 GTAOSampleDistribution = 2.0f; // Sample distance distribution power
        f32 GTAOThinCompensation = 0.0f;   // Thin occluder compensation
        f32 GTAODepthMipOffset = 3.3f;     // HZB mip selection offset
        bool GTAODenoiseEnabled = true;
        i32 GTAODenoisePasses = 4;  // Bilateral blur pass count
        f32 GTAODenoiseBeta = 1.2f; // Edge sensitivity
        bool GTAODebugView = false;

        // Screen-Space Reflections (SSR)
        // Deferred-only: reflects the lit scene color off opaque G-Buffer
        // surfaces via a view-space ray march against scene depth. The reflection
        // is composited with a replace/mix blend (lerp toward the reflected
        // colour by reflectance x confidence, NOT additive — adding double-counts
        // the IBL already in the base colour) into a fresh SSRColor target
        // inserted between AOApply and Bloom. The math is pinned by
        // ScreenSpaceReflectionMathTest and the frame is checked by
        // SSRVisualEvidenceTest. A min-depth HZB acceleration is the planned
        // follow-up (the existing HZB pyramid stores max depth for GTAO
        // occlusion, unsuited to front-to-back SSR) — tracked in GitHub issue
        // drsnuggles8/OloEngineBase#284.
        bool SSREnabled = false;
        f32 SSRMaxDistance = 40.0f;   // Max ray length in world/view units before the ray is abandoned
        f32 SSRThickness = 0.8f;      // View-space depth tolerance for accepting a hit (thin-surface guard)
        f32 SSRStride = 0.25f;        // Initial view-space marching step length
        i32 SSRMaxSteps = 64;         // Maximum linear march iterations
        i32 SSRBinarySearchSteps = 6; // Binary-search refinement iterations after a crossing is found
        f32 SSRIntensity = 1.0f;      // Overall reflection strength multiplier
        f32 SSRMaxRoughness = 0.6f;   // Surfaces rougher than this receive no SSR (fade-out cutoff)
        f32 SSREdgeFade = 0.1f;       // Screen-border fade width in UV (0..0.5); larger = softer edges
        bool SSRDebugView = false;    // Show the raw reflection buffer instead of the composite

        // Screen-Space Global Illumination (SSGI)
        // Deferred-only: one-bounce indirect *diffuse* lighting. For each opaque
        // pixel it casts a cosine-weighted hemisphere of short rays around the
        // surface normal, marches each against scene depth, and on a hit samples
        // the lit scene colour as incoming indirect radiance — so a saturated
        // wall bleeds its colour onto a neutral floor. The averaged radiance is
        // tinted by the receiver albedo and ADDED to the lit colour (unlike SSR,
        // which is a replace/mix — indirect diffuse is extra bounced light, not a
        // mirror substitution) into a fresh SSGIColor target inserted between
        // AOApply and SSR. The math is pinned by ScreenSpaceGIMathTest and the
        // frame is checked by SSGIVisualEvidenceTest. Screen-space only: light
        // from off-screen or behind surfaces is unknown and contributes nothing,
        // so rays that leave the screen fade out.
        bool SSGIEnabled = false;
        f32 SSGIIntensity = 1.0f;   // Overall indirect-diffuse strength multiplier
        f32 SSGIMaxDistance = 8.0f; // Max ray length in view/world units (GI is local — keep short)
        f32 SSGIThickness = 0.5f;   // View-space depth tolerance for accepting a hit (thin-surface guard)
        f32 SSGIStride = 0.25f;     // Initial view-space marching step length
        i32 SSGIMaxSteps = 24;      // Maximum linear march iterations per ray
        i32 SSGIRayCount = 8;       // Cosine-weighted hemisphere rays per pixel
        f32 SSGIEdgeFade = 0.1f;    // Screen-border fade width in UV (0..0.5); larger = softer edges
        bool SSGIDebugView = false; // Show the indirect-diffuse buffer in isolation instead of the composite

        // Screen-Space Contact Shadows (SSCS)
        // Deferred-only: short-range per-pixel hard shadows for the primary
        // directional light. For each lit opaque pixel it reconstructs the
        // view-space position from depth and marches a single ray TOWARD the sun
        // against scene depth; if a nearby occluder crosses the ray within a thin
        // thickness window the pixel is darkened. This grounds dynamic geometry
        // that the coarse shadow map misses (an object's base where it touches a
        // surface). The shadow factor MULTIPLIES the lit colour (occlusion of
        // direct light — unlike SSGI's add or SSR's replace/mix) into a fresh
        // ContactShadowColor target inserted between SSR and Bloom. Screen-space
        // only: occluders off-screen or behind surfaces are unknown, so rays that
        // leave the screen fade out. The math is pinned by ContactShadowMathTest
        // and the frame is checked by ContactShadowVisualEvidenceTest.
        bool ContactShadowEnabled = false;
        f32 ContactShadowIntensity = 1.0f;   // Strength of the darkening at full occlusion (0 = none, 1 = black)
        f32 ContactShadowMaxDistance = 1.0f; // Max ray length toward the light in view/world units (keep short — contact range)
        f32 ContactShadowThickness = 0.3f;   // View-space depth tolerance for accepting an occluder (thin-surface guard)
        f32 ContactShadowStride = 0.04f;     // View-space marching step length
        i32 ContactShadowMaxSteps = 24;      // Maximum linear march iterations along the ray
        f32 ContactShadowBias = 0.02f;       // Depth-proportional start offset along the normal (self-intersection guard)
        f32 ContactShadowEdgeFade = 0.1f;    // Screen-border fade width in UV (0..0.5); larger = softer edges
        bool ContactShadowDebugView = false; // Show the shadow factor as greyscale instead of the composite

        bool operator==(const PostProcessSettings&) const = default;
    };

    // Upper bounds for the SSR step counts. These MUST match the runtime UBO
    // upload clamp in RenderPipeline.cpp and the HARD_MAX_* loop caps in
    // PostProcess_SSR.glsl — otherwise a persisted/edited value above the
    // runtime cap is saved but silently ignored when rendering. Shared here so
    // the sanitizer and the upload path use a single source of truth.
    inline constexpr i32 kSSRMaxSteps = 256;
    inline constexpr i32 kSSRMaxBinarySearchSteps = 32;

    // Clamp SSR parameters to finite, sane ranges. Call after loading settings
    // from disk (scene YAML / save-game), per the CLAUDE.md rule that floats read
    // from external data are validated with std::isfinite. The shader also clamps
    // at use-time, but persisted/edited settings should never carry NaN/Inf.
    inline void SanitizeSSR(PostProcessSettings& s) noexcept
    {
        const auto finite = [](f32 v, f32 fallback) noexcept
        { return std::isfinite(v) ? v : fallback; };

        s.SSRMaxDistance = std::clamp(finite(s.SSRMaxDistance, 40.0f), 0.1f, 10000.0f);
        s.SSRThickness = std::clamp(finite(s.SSRThickness, 0.8f), 0.001f, 1000.0f);
        s.SSRStride = std::clamp(finite(s.SSRStride, 0.25f), 0.001f, 100.0f);
        s.SSRMaxSteps = std::clamp(s.SSRMaxSteps, 1, kSSRMaxSteps);
        s.SSRBinarySearchSteps = std::clamp(s.SSRBinarySearchSteps, 0, kSSRMaxBinarySearchSteps);
        s.SSRIntensity = std::clamp(finite(s.SSRIntensity, 1.0f), 0.0f, 16.0f);
        s.SSRMaxRoughness = std::clamp(finite(s.SSRMaxRoughness, 0.6f), 0.0f, 1.0f);
        s.SSREdgeFade = std::clamp(finite(s.SSREdgeFade, 0.1f), 0.0f, 0.5f);
    }

    // Upper bounds for the SSGI ray-march counts. These MUST match the runtime
    // UBO upload clamp in RenderPipeline.cpp and the HARD_MAX_* loop caps in
    // PostProcess_SSGI.glsl — a persisted/edited value above the runtime cap is
    // otherwise saved but silently ignored when rendering. Single source of
    // truth shared by the sanitizer and the upload path.
    inline constexpr i32 kSSGIMaxSteps = 64;
    inline constexpr i32 kSSGIMaxRays = 32;

    // Clamp SSGI parameters to finite, sane ranges. Call after loading settings
    // from disk (scene YAML / save-game), per the CLAUDE.md rule that floats read
    // from external data are validated with std::isfinite. The shader also clamps
    // at use-time, but persisted/edited settings should never carry NaN/Inf.
    inline void SanitizeSSGI(PostProcessSettings& s) noexcept
    {
        const auto finite = [](f32 v, f32 fallback) noexcept
        { return std::isfinite(v) ? v : fallback; };

        s.SSGIIntensity = std::clamp(finite(s.SSGIIntensity, 1.0f), 0.0f, 16.0f);
        s.SSGIMaxDistance = std::clamp(finite(s.SSGIMaxDistance, 8.0f), 0.1f, 10000.0f);
        s.SSGIThickness = std::clamp(finite(s.SSGIThickness, 0.5f), 0.001f, 1000.0f);
        s.SSGIStride = std::clamp(finite(s.SSGIStride, 0.25f), 0.001f, 100.0f);
        s.SSGIMaxSteps = std::clamp(s.SSGIMaxSteps, 1, kSSGIMaxSteps);
        s.SSGIRayCount = std::clamp(s.SSGIRayCount, 1, kSSGIMaxRays);
        s.SSGIEdgeFade = std::clamp(finite(s.SSGIEdgeFade, 0.1f), 0.0f, 0.5f);
    }

    // Upper bound for the contact-shadow march step count. MUST match the runtime
    // UBO upload clamp in RenderPipeline.cpp and the HARD_MAX_STEPS loop cap in
    // PostProcess_ContactShadow.glsl — a persisted/edited value above the runtime
    // cap is otherwise saved but silently ignored when rendering. Single source of
    // truth shared by the sanitizer and the upload path.
    inline constexpr i32 kContactShadowMaxSteps = 128;

    // Clamp contact-shadow parameters to finite, sane ranges. Call after loading
    // settings from disk (scene YAML / save-game), per the CLAUDE.md rule that
    // floats read from external data are validated with std::isfinite. The shader
    // also clamps at use-time, but persisted/edited settings should never carry
    // NaN/Inf.
    inline void SanitizeContactShadow(PostProcessSettings& s) noexcept
    {
        const auto finite = [](f32 v, f32 fallback) noexcept
        { return std::isfinite(v) ? v : fallback; };

        s.ContactShadowIntensity = std::clamp(finite(s.ContactShadowIntensity, 1.0f), 0.0f, 1.0f);
        s.ContactShadowMaxDistance = std::clamp(finite(s.ContactShadowMaxDistance, 1.0f), 0.01f, 1000.0f);
        s.ContactShadowThickness = std::clamp(finite(s.ContactShadowThickness, 0.3f), 0.001f, 1000.0f);
        s.ContactShadowStride = std::clamp(finite(s.ContactShadowStride, 0.04f), 0.001f, 100.0f);
        s.ContactShadowMaxSteps = std::clamp(s.ContactShadowMaxSteps, 1, kContactShadowMaxSteps);
        s.ContactShadowBias = std::clamp(finite(s.ContactShadowBias, 0.02f), 0.0f, 1.0f);
        s.ContactShadowEdgeFade = std::clamp(finite(s.ContactShadowEdgeFade, 0.1f), 0.0f, 0.5f);
    }

    // Clamp the auto-exposure parameters to a finite, ordered, sane range.
    // Call after loading settings from disk (scene YAML / save-game), per the
    // CLAUDE.md rule that floats read from external data are validated with
    // std::isfinite. The renderer also defends against bad values at use-time,
    // but persisted/edited settings should never carry NaN/Inf or min>max.
    inline void SanitizeAutoExposure(PostProcessSettings& s) noexcept
    {
        const auto finite = [](f32 v, f32 fallback) noexcept
        { return std::isfinite(v) ? v : fallback; };

        s.AutoExposureMinLogLuminance = finite(s.AutoExposureMinLogLuminance, -8.0f);
        s.AutoExposureMaxLogLuminance = finite(s.AutoExposureMaxLogLuminance, 3.5f);
        s.AutoExposureSpeedUp = std::max(0.0f, finite(s.AutoExposureSpeedUp, 3.0f));
        s.AutoExposureSpeedDown = std::max(0.0f, finite(s.AutoExposureSpeedDown, 1.0f));
        s.AutoExposureCompensation = std::clamp(finite(s.AutoExposureCompensation, 0.0f), -16.0f, 16.0f);
        s.AutoExposureMinExposure = std::max(0.0f, finite(s.AutoExposureMinExposure, 0.05f));
        s.AutoExposureMaxExposure = std::max(0.0f, finite(s.AutoExposureMaxExposure, 16.0f));

        if (s.AutoExposureMinLogLuminance > s.AutoExposureMaxLogLuminance)
            std::swap(s.AutoExposureMinLogLuminance, s.AutoExposureMaxLogLuminance);
        if (s.AutoExposureMinExposure > s.AutoExposureMaxExposure)
            std::swap(s.AutoExposureMinExposure, s.AutoExposureMaxExposure);
    }

    // GPU-side UBO layout for post-process parameters (std140, binding 7)
    struct PostProcessUBOData
    {
        i32 TonemapOperator = 1; // Reinhard
        f32 Exposure = 1.0f;
        f32 Gamma = 2.2f;
        f32 BloomThreshold = 1.0f;

        f32 BloomIntensity = 0.5f;
        f32 VignetteIntensity = 0.3f;
        f32 VignetteSmoothness = 0.15f;
        f32 ChromaticAberrationIntensity = 0.005f;

        f32 DOFFocusDistance = 10.0f;
        f32 DOFFocusRange = 5.0f;
        f32 DOFBokehRadius = 3.0f;
        f32 MotionBlurStrength = 0.5f;

        i32 MotionBlurSamples = 8;
        f32 InverseScreenWidth = 0.0f;
        f32 InverseScreenHeight = 0.0f;
        f32 _padding0 = 0.0f;

        // Per-pass volatile data (re-uploaded before each effect)
        f32 TexelSizeX = 0.0f;
        f32 TexelSizeY = 0.0f;
        f32 CameraNear = 0.1f;
        f32 CameraFar = 1000.0f;

        static constexpr u32 GetSize()
        {
            return sizeof(PostProcessUBOData);
        }
    };

    // GPU-side UBO layout for motion blur matrices (std140, binding 8)
    struct MotionBlurUBOData
    {
        glm::mat4 InverseViewProjection = glm::mat4(1.0f);
        glm::mat4 PrevViewProjection = glm::mat4(1.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(MotionBlurUBOData);
        }
    };

    // GPU-side UBO layout for motion-blur per-pass flags (std140, binding 42).
    // Set by MotionBlurRenderPass each frame from whether a per-pixel velocity
    // texture resolved for this path. x = hasVelocityTexture (0/1): when 1 the
    // shader reads full camera+object motion from the velocity buffer on
    // geometry pixels; when 0 every pixel falls back to camera-only
    // reconstruction from depth.
    struct MotionBlurParamsUBOData
    {
        glm::vec4 Params = glm::vec4(0.0f); // x = hasVelocityTexture, yzw reserved

        static constexpr u32 GetSize()
        {
            return sizeof(MotionBlurParamsUBOData);
        }
    };

    // GPU-side UBO layout for TAA (std140, binding 32)
    struct TAAUBOData
    {
        // xyzw = feedback, sharpness, hasVelocityTexture (0/1), pad
        glm::vec4 FeedbackSharpnessHasVelocity = glm::vec4(0.9f, 0.25f, 0.0f, 0.0f);
        // xyzw = 1/width, 1/height, pad, pad
        glm::vec4 TexelSize = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(TAAUBOData);
        }
    };

    // GPU-side UBO layout for Dynamic Resolution Scaling parameters (std140, binding 33).
    // RenderScaleBounds.xy = (renderWidth / physicalWidth, renderHeight / physicalHeight).
    // Screen-space passes that read DRS-scaled framebuffers clamp their UV to this range
    // so they never sample uninitialised texels beyond the rendered region.
    // Both components are 1.0 when DRS is inactive (scale == 1.0).
    struct DRSUBOData
    {
        glm::vec2 RenderScaleBounds = glm::vec2(1.0f);
        glm::vec2 _pad = glm::vec2(0.0f); // pad to 16 bytes (std140 vec2 alignment)

        static constexpr u32 GetSize()
        {
            return sizeof(DRSUBOData);
        }
    };

    // GPU-side UBO layout for SSAO parameters (std140, binding 9)
    struct SSAOUBOData
    {
        f32 Radius = 0.5f;
        f32 Bias = 0.025f;
        f32 Intensity = 1.0f;
        i32 Samples = 32;

        i32 ScreenWidth = 0;
        i32 ScreenHeight = 0;
        i32 DebugView = 0;
        f32 _pad1 = 0.0f;

        glm::mat4 Projection = glm::mat4(1.0f);
        glm::mat4 InverseProjection = glm::mat4(1.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SSAOUBOData);
        }
    };

    // GPU-side UBO layout for screen-space reflections (std140, binding 38).
    // All math the PostProcess_SSR.glsl ray march needs: camera matrices to
    // reconstruct/project view-space positions, plus the ray-march parameters.
    // Mirrored on the CPU by ScreenSpaceReflectionMathTest so the contract is
    // pinned without a GL context.
    struct SSRUBOData
    {
        glm::mat4 Projection = glm::mat4(1.0f);
        glm::mat4 InverseProjection = glm::mat4(1.0f);
        glm::mat4 View = glm::mat4(1.0f);

        // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
        glm::vec4 RayParams = glm::vec4(64.0f, 40.0f, 0.8f, 0.25f);
        // x = Intensity, y = MaxRoughness, z = EdgeFade (UV), w = BinarySearchSteps
        glm::vec4 ShadeParams = glm::vec4(1.0f, 0.6f, 0.1f, 6.0f);
        // x = width, y = height, z = 1/width, w = 1/height
        glm::vec4 ScreenParams = glm::vec4(0.0f);
        // x = DebugView (0/1), yzw = pad
        glm::vec4 Flags = glm::vec4(0.0f);
        // Min-depth HZB acceleration (#284). x = HZB UVFactor.x, y = HZB
        // UVFactor.y (hzbUV = screenUV * UVFactor), z = HZB mip count, w =
        // UseHiZ (0 = pure linear march, 1 = hierarchical-Z skip). The HZB
        // texture itself is bound separately at TEX_SSR_HZB.
        glm::vec4 HZBParams = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SSRUBOData);
        }
    };

    static_assert(sizeof(SSRUBOData) % 16 == 0, "SSRUBOData must be 16-byte aligned for std140");
    static_assert(sizeof(SSRUBOData) == 272, "SSRUBOData std140 size drifted — update PostProcess_SSR.glsl layout");

    // GPU-side UBO layout for screen-space global illumination (std140, binding 40).
    // All math the PostProcess_SSGI.glsl hemisphere gather needs: camera matrices
    // to reconstruct/project view-space positions, plus the ray-march + shading
    // parameters. Mirrored on the CPU by ScreenSpaceGIMathTest so the contract is
    // pinned without a GL context.
    struct SSGIUBOData
    {
        glm::mat4 Projection = glm::mat4(1.0f);
        glm::mat4 InverseProjection = glm::mat4(1.0f);
        glm::mat4 View = glm::mat4(1.0f);

        // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
        glm::vec4 RayParams = glm::vec4(24.0f, 8.0f, 0.5f, 0.25f);
        // x = Intensity, y = RayCount, z = EdgeFade (UV), w = unused
        glm::vec4 ShadeParams = glm::vec4(1.0f, 8.0f, 0.1f, 0.0f);
        // x = width, y = height, z = 1/width, w = 1/height
        glm::vec4 ScreenParams = glm::vec4(0.0f);
        // x = DebugView (0/1), yzw = pad
        glm::vec4 Flags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SSGIUBOData);
        }
    };

    static_assert(sizeof(SSGIUBOData) % 16 == 0, "SSGIUBOData must be 16-byte aligned for std140");
    static_assert(sizeof(SSGIUBOData) == 256, "SSGIUBOData std140 size drifted — update PostProcess_SSGI.glsl layout");

    // GPU-side UBO layout for screen-space contact shadows (std140, binding 41).
    // All math the PostProcess_ContactShadow.glsl ray march needs: camera matrices
    // to reconstruct/project view-space positions, the world-space toward-light
    // direction, plus the ray-march + shading parameters. Mirrored on the CPU by
    // ContactShadowMathTest so the contract is pinned without a GL context.
    struct ContactShadowUBOData
    {
        glm::mat4 Projection = glm::mat4(1.0f);
        glm::mat4 InverseProjection = glm::mat4(1.0f);
        glm::mat4 View = glm::mat4(1.0f);

        // xyz = world-space direction TOWARD the light (normalized, = -lightTravelDir),
        // w = HasDirectionalLight (0/1). When 0 the pass passes the colour through.
        glm::vec4 LightDirection = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        // x = MaxSteps, y = MaxDistance (view units), z = Thickness, w = Stride (view units)
        glm::vec4 RayParams = glm::vec4(24.0f, 1.0f, 0.3f, 0.04f);
        // x = Intensity, y = EdgeFade (UV), z = Bias (depth-proportional), w = unused
        glm::vec4 ShadeParams = glm::vec4(1.0f, 0.1f, 0.02f, 0.0f);
        // x = width, y = height, z = 1/width, w = 1/height
        glm::vec4 ScreenParams = glm::vec4(0.0f);
        // x = DebugView (0/1), yzw = pad
        glm::vec4 Flags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(ContactShadowUBOData);
        }
    };

    static_assert(sizeof(ContactShadowUBOData) % 16 == 0, "ContactShadowUBOData must be 16-byte aligned for std140");
    static_assert(sizeof(ContactShadowUBOData) == 272, "ContactShadowUBOData std140 size drifted — update PostProcess_ContactShadow.glsl layout");
    // Snow rendering settings (scene-level, separate from PostProcess)
    struct SnowSettings
    {
        bool Enabled = false;

        // Coverage parameters
        f32 HeightStart = -5.0f; // World-Y where snow starts appearing
        f32 HeightFull = 5.0f;   // World-Y where snow reaches full coverage
        f32 SlopeStart = 0.7f;   // Normal.y threshold where snow starts reducing
        f32 SlopeFull = 0.3f;    // Normal.y threshold where snow is gone

        // Snow material
        glm::vec3 Albedo = glm::vec3(0.92f, 0.93f, 0.98f);
        f32 Roughness = 0.65f;

        // SSS
        glm::vec3 SSSColor = glm::vec3(0.4f, 0.6f, 0.9f);
        f32 SSSIntensity = 0.6f;

        // Sparkle
        f32 SparkleIntensity = 0.8f;
        f32 SparkleDensity = 80.0f;
        f32 SparkleScale = 1.0f;

        // Normal perturbation
        f32 NormalPerturbStrength = 0.25f;

        // Wind drift (snow coverage responds to wind direction)
        f32 WindDriftFactor = 0.0f; // 0 = no wind effect, 1 = full wind-driven accumulation bias

        // SSS blur pass
        bool SSSBlurEnabled = false;
        f32 SSSBlurRadius = 2.0f;
        f32 SSSBlurFalloff = 1.0f;

        bool operator==(const SnowSettings&) const = default;
    };

    // GPU-side UBO layout for snow parameters (std140, binding 13)
    struct SnowUBOData
    {
        // vec4(HeightStart, HeightFull, SlopeStart, SlopeFull)
        glm::vec4 CoverageParams = glm::vec4(-5.0f, 5.0f, 0.7f, 0.3f);
        // vec4(Albedo.rgb, Roughness)
        glm::vec4 AlbedoAndRoughness = glm::vec4(0.92f, 0.93f, 0.98f, 0.65f);
        // vec4(SSSColor.rgb, SSSIntensity)
        glm::vec4 SSSColorAndIntensity = glm::vec4(0.4f, 0.6f, 0.9f, 0.6f);
        // vec4(SparkleIntensity, SparkleDensity, SparkleScale, NormalPerturbStrength)
        glm::vec4 SparkleParams = glm::vec4(0.8f, 80.0f, 1.0f, 0.25f);
        // vec4(Enabled, WindDriftFactor, pad, pad)
        glm::vec4 Flags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SnowUBOData);
        }
    };

    // GPU-side UBO layout for SSS blur parameters (std140, binding 14)
    struct SSSUBOData
    {
        // vec4(BlurRadius, BlurFalloff, ScreenWidth, ScreenHeight)
        glm::vec4 BlurParams = glm::vec4(2.0f, 1.0f, 0.0f, 0.0f);
        // vec4(Enabled, pad, pad, pad)
        glm::vec4 Flags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SSSUBOData);
        }
    };

    // Snow accumulation & deformation settings (scene-level)
    struct SnowAccumulationSettings
    {
        bool Enabled = false;

        // Accumulation
        f32 AccumulationRate = 0.02f; // Meters of snow per second when snowing
        f32 MaxDepth = 0.5f;          // Maximum snow depth (meters)
        f32 MeltRate = 0.005f;        // Meters of snow lost per second (temperature-driven)
        f32 RestorationRate = 0.01f;  // How fast deformed snow fills back in (m/s)

        // Displacement
        f32 DisplacementScale = 1.0f; // Multiplier for vertex displacement from snow depth

        // Clipmap
        u32 ClipmapResolution = 2048; // Texels per axis for the snow depth texture
        f32 ClipmapExtent = 128.0f;   // World-space side length of innermost clipmap ring (meters)
        u32 NumClipmapRings = 3;      // Number of clipmap LOD rings

        // Physics
        f32 SnowDensity = 0.3f; // Density factor for compaction (0 = powder, 1 = packed ice)

        bool operator==(const SnowAccumulationSettings&) const = default;
    };

    // GPU-side UBO layout for snow accumulation (std140, binding 16)
    // Contains clipmap matrices + accumulation parameters
    struct SnowAccumulationUBOData
    {
        static constexpr u32 MAX_CLIPMAP_RINGS = 3;

        // Orthographic view-projection for each clipmap ring (top-down)
        glm::mat4 ClipmapViewProj[MAX_CLIPMAP_RINGS] = { glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) };
        // vec4(centerX, centerZ, extent, invExtent) per ring
        glm::vec4 ClipmapCenterAndExtent[MAX_CLIPMAP_RINGS] = { glm::vec4(0.0f), glm::vec4(0.0f), glm::vec4(0.0f) };
        // vec4(AccumulationRate, MaxDepth, MeltRate, RestorationRate)
        glm::vec4 AccumulationParams = glm::vec4(0.02f, 0.5f, 0.005f, 0.01f);
        // vec4(DisplacementScale, SnowDensity, Enabled, NumRings)
        glm::vec4 DisplacementParams = glm::vec4(1.0f, 0.3f, 0.0f, 3.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(SnowAccumulationUBOData);
        }
    };

    // Snow ejecta particle settings (scene-level)
    struct SnowEjectaSettings
    {
        bool Enabled = false;                                  // Whether snow deformers emit ejecta particles
        u32 ParticlesPerDeform = 8;                            // Particles emitted per deformer stamp per frame
        f32 EjectaSpeed = 2.5f;                                // Base outward velocity (m/s)
        f32 SpeedVariance = 0.8f;                              // Random speed variation factor (0–1)
        f32 UpwardBias = 0.6f;                                 // Fraction of velocity directed upward vs outward
        f32 LifetimeMin = 0.4f;                                // Minimum particle lifetime (seconds)
        f32 LifetimeMax = 1.2f;                                // Maximum particle lifetime (seconds)
        f32 InitialSize = 0.04f;                               // Starting particle size (meters)
        f32 SizeVariance = 0.02f;                              // Random size variation (meters)
        f32 GravityScale = 0.3f;                               // Gravity multiplier (snow falls slowly)
        f32 DragCoefficient = 2.0f;                            // Air drag for quick deceleration
        glm::vec4 Color = glm::vec4(0.95f, 0.97f, 1.0f, 0.7f); // RGBA snow puff color
        f32 VelocityThreshold = 0.1f;                          // Min deformer speed to emit (m/s)
        u32 MaxParticles = 8192;                               // Max GPU particles in the ejecta pool

        // --- Advanced Simulation (modify with care) ---
        f32 WindInfluence = 0.5f;     // How strongly wind affects snow puffs (0–1)
        f32 NoiseStrength = 0.3f;     // Turbulence intensity for organic motion
        f32 NoiseFrequency = 2.0f;    // Spatial frequency of turbulence noise
        f32 GroundY = 0.0f;           // Ground plane Y for collision
        f32 CollisionBounce = 0.0f;   // Bounce factor on ground hit (0 = no bounce)
        f32 CollisionFriction = 1.0f; // Friction on ground contact (1 = full stop)

        bool operator==(const SnowEjectaSettings&) const = default;
    };

    // Fog mode constants (match FogCommon.glsl defines)
    enum class FogMode : i32
    {
        Linear = 0,
        Exponential = 1,
        ExponentialSquared = 2
    };

    // Fog & atmospheric scattering settings (scene-level)
    struct FogSettings
    {
        bool Enabled = false;
        FogMode Mode = FogMode::ExponentialSquared;

        // Base fog
        glm::vec3 Color = glm::vec3(0.5f, 0.6f, 0.7f);
        f32 Density = 0.02f; // For Exponential / Exponential² modes
        f32 Start = 10.0f;   // For Linear mode (near plane)
        f32 End = 300.0f;    // For Linear mode (far plane)

        // Height fog
        f32 HeightFalloff = 0.1f; // Density decay rate above reference height
        f32 HeightOffset = 0.0f;  // Sea-level reference for height fog

        // Opacity
        f32 MaxOpacity = 1.0f; // Clamp fog factor (0 = no fog, 1 = fully opaque at distance)

        // Atmospheric scattering
        bool EnableScattering = false;
        f32 RayleighStrength = 1.0f;
        f32 MieStrength = 0.005f;
        f32 MieDirectionality = 0.76f;                            // Henyey-Greenstein g (0 = isotropic, ~1 = full forward scatter)
        glm::vec3 RayleighColor = glm::vec3(0.27f, 0.51f, 0.83f); // Sky blue
        f32 SunIntensity = 22.0f;

        // Volumetric ray-marching
        bool EnableVolumetric = false;
        i32 VolumetricSamples = 32;        // Ray-march steps (16-64)
        f32 AbsorptionCoefficient = 0.02f; // Beer-Lambert absorption (separate from scatter density)

        // Noise / turbulence (modulates fog density spatially)
        bool EnableNoise = false;
        f32 NoiseScale = 0.01f;    // 3D noise spatial frequency (world-space)
        f32 NoiseSpeed = 0.1f;     // Animation speed (units/sec)
        f32 NoiseIntensity = 0.3f; // Noise modulation strength (0 = uniform, 1 = full variation)

        // Volumetric light shafts (god rays through shadow map)
        bool EnableLightShafts = false;
        f32 LightShaftIntensity = 1.0f; // In-scattering boost for lit volume samples

        bool operator==(const FogSettings&) const = default;
    };

    // GPU-side UBO layout for fog parameters (std140, binding 17)
    struct FogUBOData
    {
        // vec4(Color.rgb, Density)
        glm::vec4 ColorAndDensity = glm::vec4(0.5f, 0.6f, 0.7f, 0.02f);
        // vec4(Start, End, HeightFalloff, HeightOffset)
        glm::vec4 DistanceParams = glm::vec4(10.0f, 300.0f, 0.1f, 0.0f);
        // vec4(RayleighStrength, MieStrength, MieG, SunIntensity)
        glm::vec4 ScatterParams = glm::vec4(1.0f, 0.005f, 0.76f, 22.0f);
        // vec4(RayleighColor.rgb, MaxOpacity)
        glm::vec4 RayleighColorAndMaxOpacity = glm::vec4(0.27f, 0.51f, 0.83f, 1.0f);
        // vec4(SunDirection.xyz, fogFrameIndex) — w = temporal jitter frame counter
        glm::vec4 SunDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
        // vec4(Enabled, Mode, ScatteringEnabled, VolumetricEnabled)
        glm::vec4 Flags = glm::vec4(0.0f);
        // vec4(NoiseScale, NoiseSpeed, NoiseIntensity, Time)
        glm::vec4 NoiseParams = glm::vec4(0.01f, 0.1f, 0.3f, 0.0f);
        // vec4(VolumetricSamples, AbsorptionCoeff, LightShaftIntensity, LightShaftsEnabled)
        glm::vec4 VolumetricParams = glm::vec4(32.0f, 0.02f, 1.0f, 0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(FogUBOData);
        }
    };

    // Precipitation type constants
    enum class PrecipitationType : i32
    {
        Snow = 0,
        Rain = 1,
        Hail = 2,
        Sleet = 3
    };

    /// Maximum number of simultaneous lens impacts for screen-space precipitation.
    /// Shared between ScreenSpacePrecipitation and PrecipitationScreenUBOData.
    inline constexpr u32 kMaxLensImpacts = 16;

    // Precipitation intensity presets (map to parameter sets)
    enum class PrecipitationIntensity : i32
    {
        None = 0,
        Light = 1,
        Moderate = 2,
        Heavy = 3,
        Blizzard = 4
    };

    // Scene-global precipitation settings (camera-relative, multi-layer)
    struct PrecipitationSettings
    {
        bool Enabled = false;
        PrecipitationType Type = PrecipitationType::Snow;
        f32 Intensity = 0.5f;       // 0–1 continuous
        f32 TransitionSpeed = 1.0f; // Interpolation speed toward target

        // Emission
        u32 BaseEmissionRate = 4000; // Particles/sec at intensity=1
        u32 MaxParticlesNearField = 100000;
        u32 MaxParticlesFarField = 200000;

        // Near-field layer (detailed, close to camera)
        glm::vec3 NearFieldExtent = glm::vec3(15.0f, 20.0f, 15.0f);
        f32 NearFieldParticleSize = 0.04f;
        f32 NearFieldSizeVariance = 0.02f;
        f32 NearFieldSpeedMin = 0.8f;
        f32 NearFieldSpeedMax = 2.0f;
        f32 NearFieldLifetime = 10.0f;

        // Far-field layer (atmospheric, larger volume)
        glm::vec3 FarFieldExtent = glm::vec3(60.0f, 30.0f, 60.0f);
        f32 FarFieldParticleSize = 0.025f;
        f32 FarFieldSpeedMin = 0.5f;
        f32 FarFieldSpeedMax = 1.5f;
        f32 FarFieldLifetime = 15.0f;
        f32 FarFieldAlphaMultiplier = 0.5f;

        // Physics
        f32 GravityScale = 0.8f;
        f32 WindInfluence = 1.0f;
        f32 DragCoefficient = 0.3f;
        f32 TurbulenceStrength = 0.8f;
        f32 TurbulenceFrequency = 0.5f;

        // Ground interaction
        bool GroundCollisionEnabled = true;
        f32 GroundY = 0.0f;
        f32 CollisionBounce = 0.0f;   // 0=stick/splash, >0=bounce (hail)
        f32 CollisionFriction = 1.0f; // Friction on ground contact
        bool FeedAccumulation = true;
        f32 AccumulationFeedRate = 0.0001f;

        // Screen-space effects
        bool ScreenStreaksEnabled = true;
        f32 ScreenStreakIntensity = 0.3f;
        f32 ScreenStreakLength = 0.15f;
        bool LensImpactsEnabled = true;
        f32 LensImpactRate = 3.0f; // Impacts/sec at intensity=1
        f32 LensImpactLifetime = 1.5f;
        f32 LensImpactSize = 0.03f;

        // LOD / performance budget
        f32 LODNearDistance = 30.0f;
        f32 LODFarDistance = 120.0f;
        f32 FrameBudgetMs = 1.0f;

        // Visual
        glm::vec4 ParticleColor = glm::vec4(0.95f, 0.97f, 1.0f, 0.85f);
        f32 ColorVariance = 0.05f;
        f32 RotationSpeed = 30.0f; // deg/s

        /// @brief Return sensible default parameters for the given precipitation type.
        ///        The caller can further tweak individual fields after applying defaults.
        [[nodiscard]] static PrecipitationSettings GetDefaultsForType(PrecipitationType type)
        {
            PrecipitationSettings s;
            s.Enabled = true;
            s.Type = type;
            switch (type)
            {
                case PrecipitationType::Snow:
                    // Already the struct defaults — snow is the baseline
                    break;
                case PrecipitationType::Rain:
                    s.BaseEmissionRate = 8000;
                    s.NearFieldParticleSize = 0.02f;
                    s.NearFieldSizeVariance = 0.005f;
                    s.NearFieldSpeedMin = 5.0f;
                    s.NearFieldSpeedMax = 10.0f;
                    s.NearFieldLifetime = 4.0f;
                    s.FarFieldParticleSize = 0.015f;
                    s.FarFieldSpeedMin = 4.0f;
                    s.FarFieldSpeedMax = 8.0f;
                    s.FarFieldLifetime = 6.0f;
                    s.FarFieldAlphaMultiplier = 0.35f;
                    s.GravityScale = 2.5f;
                    s.WindInfluence = 0.6f;
                    s.DragCoefficient = 0.05f;
                    s.TurbulenceStrength = 0.1f;
                    s.TurbulenceFrequency = 0.3f;
                    s.CollisionBounce = 0.0f;
                    s.CollisionFriction = 1.0f;
                    s.FeedAccumulation = false;
                    s.ScreenStreakIntensity = 0.5f;
                    s.ScreenStreakLength = 0.35f;
                    s.LensImpactRate = 5.0f;
                    s.LensImpactLifetime = 1.0f;
                    s.LensImpactSize = 0.025f;
                    s.ParticleColor = glm::vec4(0.7f, 0.75f, 0.85f, 0.45f);
                    s.ColorVariance = 0.08f;
                    s.RotationSpeed = 0.0f;
                    break;
                case PrecipitationType::Hail:
                    s.BaseEmissionRate = 2000;
                    s.NearFieldParticleSize = 0.05f;
                    s.NearFieldSizeVariance = 0.02f;
                    s.NearFieldSpeedMin = 8.0f;
                    s.NearFieldSpeedMax = 15.0f;
                    s.NearFieldLifetime = 3.0f;
                    s.FarFieldParticleSize = 0.03f;
                    s.FarFieldSpeedMin = 6.0f;
                    s.FarFieldSpeedMax = 12.0f;
                    s.FarFieldLifetime = 5.0f;
                    s.FarFieldAlphaMultiplier = 0.4f;
                    s.GravityScale = 3.0f;
                    s.WindInfluence = 0.3f;
                    s.DragCoefficient = 0.02f;
                    s.TurbulenceStrength = 0.05f;
                    s.TurbulenceFrequency = 0.2f;
                    s.CollisionBounce = 0.35f;
                    s.CollisionFriction = 0.6f;
                    s.FeedAccumulation = false;
                    s.ScreenStreakIntensity = 0.15f;
                    s.ScreenStreakLength = 0.05f;
                    s.LensImpactRate = 2.0f;
                    s.LensImpactLifetime = 2.5f;
                    s.LensImpactSize = 0.06f;
                    s.ParticleColor = glm::vec4(0.9f, 0.92f, 0.95f, 0.9f);
                    s.ColorVariance = 0.03f;
                    s.RotationSpeed = 5.0f;
                    break;
                case PrecipitationType::Sleet:
                    s.BaseEmissionRate = 5000;
                    s.NearFieldParticleSize = 0.03f;
                    s.NearFieldSizeVariance = 0.01f;
                    s.NearFieldSpeedMin = 3.0f;
                    s.NearFieldSpeedMax = 6.0f;
                    s.NearFieldLifetime = 6.0f;
                    s.FarFieldParticleSize = 0.02f;
                    s.FarFieldSpeedMin = 2.0f;
                    s.FarFieldSpeedMax = 5.0f;
                    s.FarFieldLifetime = 9.0f;
                    s.FarFieldAlphaMultiplier = 0.4f;
                    s.GravityScale = 1.5f;
                    s.WindInfluence = 0.8f;
                    s.DragCoefficient = 0.15f;
                    s.TurbulenceStrength = 0.3f;
                    s.TurbulenceFrequency = 0.4f;
                    s.CollisionBounce = 0.05f;
                    s.CollisionFriction = 0.9f;
                    s.FeedAccumulation = true;
                    s.AccumulationFeedRate = 0.00005f;
                    s.ScreenStreakIntensity = 0.35f;
                    s.ScreenStreakLength = 0.2f;
                    s.LensImpactRate = 4.0f;
                    s.LensImpactLifetime = 1.2f;
                    s.LensImpactSize = 0.03f;
                    s.ParticleColor = glm::vec4(0.82f, 0.85f, 0.9f, 0.6f);
                    s.ColorVariance = 0.06f;
                    s.RotationSpeed = 10.0f;
                    break;
                default:
                    // Unknown enum value — preserve Snow/baseline defaults
                    OLO_CORE_WARN("GetDefaultsForType: unknown PrecipitationType {}", static_cast<i32>(type));
                    break;
            }
            return s;
        }

        bool operator==(const PrecipitationSettings&) const = default;
    };

    // GPU-side UBO layout for precipitation parameters (std140, binding 18)
    struct PrecipitationUBOData
    {
        // vec4(Intensity, WindInfluence, StreakIntensity, StreakLength)
        glm::vec4 IntensityAndScreenFX = glm::vec4(0.5f, 1.0f, 0.3f, 0.15f);
        // vec4(LensImpactRate, LensImpactLifetime, LensImpactSize, Enabled)
        glm::vec4 LensParams = glm::vec4(3.0f, 1.5f, 0.03f, 0.0f);
        // vec4(ScreenWindDirX, ScreenWindDirY, Time, StreaksEnabled)
        glm::vec4 ScreenWindAndTime = glm::vec4(0.0f);
        // vec4(ParticleColor rgba)
        glm::vec4 ParticleColor = glm::vec4(0.95f, 0.97f, 1.0f, 0.85f);
        // vec4(Type, 0, 0, 0) — x: 0=Snow, 1=Rain, 2=Hail, 3=Sleet
        glm::vec4 TypeParams = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(PrecipitationUBOData);
        }
    };

    // GPU-side UBO layout for precipitation screen-space effects (std140, binding 19)
    struct PrecipitationScreenUBOData
    {
        // vec4(DirX, DirY, Intensity, Length)
        glm::vec4 StreakParams = glm::vec4(0.0f);
        // LensImpacts[16] — each element has 2 vec4 (PositionAndSize, TimeParams)
        struct LensImpactGPU
        {
            glm::vec4 PositionAndSize{ 0.0f };
            glm::vec4 TimeParams{ 0.0f };
        } LensImpacts[kMaxLensImpacts]{};

        static constexpr u32 GetSize()
        {
            return sizeof(PrecipitationScreenUBOData);
        }
    };

    // Wind simulation settings (scene-level, separate from PostProcess)
    struct WindSettings
    {
        bool Enabled = false;

        // Base wind
        glm::vec3 Direction = glm::vec3(1.0f, 0.0f, 0.0f); // Normalized wind direction
        f32 Speed = 5.0f;                                  // Wind speed in m/s

        // Gust modulation
        f32 GustStrength = 0.3f;  // 0–1 amplitude of gust modulation
        f32 GustFrequency = 0.5f; // Hz — how rapidly gusts oscillate

        // Turbulence (noise-driven spatial variation)
        f32 TurbulenceIntensity = 0.5f; // Strength of turbulent fluctuations
        f32 TurbulenceScale = 0.1f;     // Spatial frequency of turbulence noise

        // 3D wind field grid
        f32 GridWorldSize = 200.0f; // Side length of cube centered on camera (meters)
        u32 GridResolution = 128;   // Voxels per axis (128³)

        bool operator==(const WindSettings&) const = default;
    };

    // GPU-side UBO layout for wind parameters (std140, binding 15)
    struct WindUBOData
    {
        // vec4(Direction.xyz, Speed)
        glm::vec4 DirectionAndSpeed = glm::vec4(1.0f, 0.0f, 0.0f, 5.0f);
        // vec4(GustStrength, GustFrequency, TurbulenceIntensity, TurbulenceScale)
        glm::vec4 GustAndTurbulence = glm::vec4(0.3f, 0.5f, 0.5f, 0.1f);
        // vec4(GridMin.xyz, GridWorldSize)
        glm::vec4 GridMinAndSize = glm::vec4(0.0f);
        // vec4(Time, Enabled, GridResolution, PrevTime)
        // PrevTime is used by consumers (e.g. Foliage_Instance.glsl) to re-evaluate
        // the analytical wind at `t - dt` for per-fragment velocity reprojection.
        glm::vec4 TimeAndFlags = glm::vec4(0.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(WindUBOData);
        }
    };

    // Underwater rendering state. Populated each frame by the scene when the
    // camera sits inside a water volume (WATER_FUTURE_IMPROVEMENTS.md §7.2).
    // `Active == false` short-circuits the underwater fog pass; the pass
    // itself decides whether to skip or just pass through the input texture
    // unchanged so render-graph wiring stays stable.
    struct UnderwaterFogState
    {
        bool Active = false; // Camera is below an enabled water surface
        glm::vec3 FogColor = glm::vec3(0.05f, 0.15f, 0.25f);
        f32 Density = 0.08f;      // Per-metre exponential absorption coefficient
        f32 WaterSurfaceY = 0.0f; // World-Y of the dominant water plane (debug / depth-fade tuning)

        // Float members go through Math::BitwiseEqual rather than a defaulted
        // operator== so the comparison doesn't rely on `==` over floats (per the
        // project's float-comparison rule); bitwise equality is exactly the
        // "did this state change" semantics this struct needs.
        bool operator==(const UnderwaterFogState& other) const
        {
            return Active == other.Active &&
                   Math::BitwiseEqual(FogColor, other.FogColor) &&
                   Math::BitwiseEqual(Density, other.Density) &&
                   Math::BitwiseEqual(WaterSurfaceY, other.WaterSurfaceY);
        }
    };

    // GPU-side UBO layout for underwater fog (std140, binding 36).
    // Drives the per-pixel water-volume fog in the tone-map pass: it fogs the
    // portion of each pixel's view ray that passes below the water plane, so
    // the waterline is handled per-pixel (underwater half fogged, above-water
    // half clear) rather than as a full-screen toggle.
    struct UnderwaterFogUBOData
    {
        // vec4(FogColor.rgb, Density)
        glm::vec4 ColorAndDensity = glm::vec4(0.05f, 0.15f, 0.25f, 0.08f);
        // vec4(Active, WaterSurfaceY, Time, pad). Active is a float for
        // straightforward branch-free GLSL evaluation; tested as > 0.5. Time
        // drives the refraction wobble + caustic animation (§7.1/§7.2).
        glm::vec4 Flags = glm::vec4(0.0f);
        // vec4(CameraPos.xyz, pad) — ray origin for the underwater-segment test.
        glm::vec4 CameraPos = glm::vec4(0.0f);
        // Submerged refraction distortion (§7.2): vec4(Strength, Scale, Speed,
        // ChromaticStrength). Strength is in UV units; 0 disables. Mirrored by
        // UnderwaterCaustics::RefractionOffset.
        glm::vec4 RefractionParams = glm::vec4(0.0f);
        // Caustics (§7.1): vec4(Intensity, Scale, Speed, MaxDepth). Intensity 0
        // disables. Mirrored by UnderwaterCaustics::CausticPattern / CausticDepthFade.
        glm::vec4 CausticParams = glm::vec4(0.0f);
        // vec4(CausticTint.rgb, SunOverhead). SunOverhead is max(-sunDir.y, 0) —
        // precomputed so the tone-map pass needs no light UBO; fades caustics as
        // the sun drops toward the horizon.
        glm::vec4 CausticColorAndSun = glm::vec4(0.7f, 0.85f, 1.0f, 0.0f);
        // Volumetric light shafts / god rays (§3.3): vec4(Intensity, Decay,
        // Density, Weight). Occlusion radial blur toward the sun; the normalised
        // openness in [0,1] is scaled by Weight then Intensity. Intensity 0
        // disables. The decay normaliser is mirrored by UnderwaterCaustics::GodRayDecaySum.
        glm::vec4 GodRayParams = glm::vec4(0.0f);
        // vec4(SampleCount, SunScreenU, SunScreenV, SunInFront). The sun's
        // screen-space vanishing point is projected on the CPU each frame (so the
        // shader needs no extra matrix); SunInFront > 0.5 means the sun is on
        // screen / in front of the camera — god rays are skipped otherwise.
        glm::vec4 GodRaySun = glm::vec4(48.0f, 0.5f, 0.5f, 0.0f);
        // vec4(GodRayTint.rgb, pad) — warm sun colour multiplied into the shafts.
        glm::vec4 GodRayColor = glm::vec4(1.0f, 0.95f, 0.8f, 0.0f);
        // vec4(DappleFloor, SunFalloff, pad, pad). DappleFloor = trough darkness of
        // the surface-wave shimmer (0 = full extinction, 1 = no dapple); SunFalloff =
        // Gaussian tightness of the sun source (higher = narrower shafts).
        glm::vec4 GodRayShape = glm::vec4(0.35f, 16.0f, 0.0f, 0.0f);
        // Reconstructs world position from depth (NDC → world). Avoids a
        // per-fragment matrix inverse in the shader.
        glm::mat4 InverseViewProjection = glm::mat4(1.0f);

        static constexpr u32 GetSize()
        {
            return sizeof(UnderwaterFogUBOData);
        }
    };

    static_assert(sizeof(UnderwaterFogUBOData) % 16 == 0, "UnderwaterFogUBOData must be 16-byte aligned for std140");
    static_assert(sizeof(UnderwaterFogUBOData) == 224, "UnderwaterFogUBOData unexpected size — update GLSL layout");

    // Local fog volume shape type (matches FogVolumeShape enum in Components.h)
    static constexpr i32 FOG_VOLUME_SHAPE_BOX = 0;
    static constexpr i32 FOG_VOLUME_SHAPE_SPHERE = 1;
    static constexpr i32 FOG_VOLUME_SHAPE_CYLINDER = 2;

    // GPU-side per-volume data (std140 aligned)
    struct FogVolumeData
    {
        glm::mat4 WorldToLocal = glm::mat4(1.0f);                       // Inverse transform for local-space conversion
        glm::vec4 ColorAndDensity = glm::vec4(0.6f, 0.65f, 0.7f, 0.5f); // rgb = color, a = density
        glm::vec4 ShapeAndFalloff = glm::vec4(0.0f, 1.0f, 1.0f, 0.0f);  // x = shape, y = falloff, z = blendWeight, w = pad
        glm::vec4 Extents = glm::vec4(5.0f, 5.0f, 5.0f, 0.0f);          // xyz = half-extents/radius, w = pad

        static constexpr u32 GetSize()
        {
            return sizeof(FogVolumeData);
        }
    };

    // GPU-side UBO layout for fog volumes (std140, binding 20)
    struct FogVolumesUBOData
    {
        static constexpr u32 MAX_FOG_VOLUMES = 16;

        FogVolumeData Volumes[MAX_FOG_VOLUMES];
        glm::ivec4 VolumeCount = glm::ivec4(0); // x = active count, yzw = reserved

        static constexpr u32 GetSize()
        {
            return sizeof(FogVolumesUBOData);
        }
    };

    // Compile-time layout guards — must match GLSL std140 declaration in FogVolumeCommon.glsl
    static_assert(std::is_standard_layout_v<FogVolumeData>, "FogVolumeData must be standard layout for GPU upload");
    static_assert(std::is_standard_layout_v<FogVolumesUBOData>, "FogVolumesUBOData must be standard layout for GPU upload");
    static_assert(sizeof(FogVolumeData) == 112, "FogVolumeData unexpected size — update GLSL layout");
    static_assert(sizeof(FogVolumeData) % 16 == 0, "FogVolumeData size must be 16-byte aligned for std140");
    static_assert(sizeof(FogVolumesUBOData) % 16 == 0, "FogVolumesUBOData size must be 16-byte aligned for std140");
    static_assert(sizeof(FogVolumesUBOData) == FogVolumeData::GetSize() * FogVolumesUBOData::MAX_FOG_VOLUMES + sizeof(glm::ivec4),
                  "FogVolumesUBOData unexpected size — update GLSL layout");
    static_assert(offsetof(FogVolumesUBOData, VolumeCount) == FogVolumeData::GetSize() * FogVolumesUBOData::MAX_FOG_VOLUMES,
                  "FogVolumesUBOData::VolumeCount offset mismatch — packing/alignment drift");
} // namespace OloEngine
