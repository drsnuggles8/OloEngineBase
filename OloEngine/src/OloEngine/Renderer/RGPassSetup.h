#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include <functional>
#include <string_view>

namespace OloEngine
{
    // ========================================================================
    // Phase C — Pass parameter structures and callback types
    //
    // Instead of mutating pass state through setters after construction,
    // passes now capture per-frame settings into immutable parameter structs
    // at setup time. This makes data flow explicit and enables proper
    // frame graph compilation.
    //
    // Setup callback: declares resources and captures parameters
    //   typedef std::function<void(RGBuilder&, const PassParams&)> PassSetupFn;
    //
    // Execute callback: receives resolved resources and parameters
    //   typedef std::function<void(RGCommandContext&, const PassParams&)> PassExecuteFn;
    //
    // ========================================================================

    // Callback signatures for graph-native passes
    typedef std::function<void(RGBuilder&)> PassSetupFn;
    typedef std::function<void(RGCommandContext&)> PassExecuteFn;

    // ========================================================================
    // SSAO/GTAO parameter structs (Phase C pilot passes)
    // ========================================================================

    struct SSAORenderPassSetup
    {
        // Per-frame SSAO settings captured at setup time
        f32 Radius = 0.5f;
        f32 Bias = 0.025f;
        f32 Intensity = 1.0f;
        i32 Samples = 32;
        bool DebugView = false;

        // Input resources
        RGTextureHandle SceneDepth;
        RGTextureHandle SceneNormals;

        // Output resource
        RGTextureHandle& OutAOTexture;
    };

    struct GTAORenderPassSetup
    {
        f32 Radius = 0.5f;
        f32 Power = 2.2f;
        f32 FalloffRange = 0.615f;
        f32 SampleDistribution = 2.0f;
        f32 ThinCompensation = 0.0f;
        f32 DepthMipOffset = 3.3f;
        bool DenoiseEnabled = true;
        i32 DenoisePasses = 4;
        f32 DenoiseBeta = 1.2f;
        bool DebugView = false;

        RGTextureHandle SceneDepth;
        RGTextureHandle SceneNormals;

        RGTextureHandle& OutAOTexture;
    };

    struct PostProcessPassSetup
    {
        // Per-frame post-process settings
        i32 TonemapOperator = 0;
        f32 Exposure = 1.0f;
        f32 Gamma = 2.2f;
        f32 BloomThreshold = 1.0f;
        f32 BloomIntensity = 0.04f;
        f32 VignetteIntensity = 0.0f;
        f32 VignetteSmoothness = 1.0f;
        bool FXAAEnabled = false;
        bool DOFEnabled = false;
        bool MotionBlurEnabled = false;
        bool ChromaticAberrationEnabled = false;
        bool FogEnabled = false;

        // Input resources
        RGFramebufferHandle InputColor;
        RGTextureHandle SceneDepth;
        RGTextureHandle Velocity;
        RGTextureHandle AOBuffer;
        RGTextureHandle ShadowMapCSM;
        RGTextureHandle IrradianceMap;
        RGTextureHandle PrefilterMap;

        // Temporal histories (previous frame)
        RGTextureHandle TAAHistoryPrev;
        RGTextureHandle FogHistoryPrev;

        // Output resources
        RGFramebufferHandle& OutColor;
        RGTextureHandle& OutTAAHistoryCurrent;
        RGTextureHandle& OutFogHistoryCurrent;
    };

    struct ShadowRenderPassSetup
    {
        // Directional light CSM parameters
        i32 CascadeCount = 4;
        f32 ShadowDistance = 100.0f;
        f32 ShadowBias = 0.0005f;

        // Point light shadow parameters
        i32 PointShadowCount = 8;

        // Output resources
        RGTextureHandle& OutShadowMapCSM;
        RGTextureHandle& OutShadowMapSpot;
    };

    struct OITResolvePassSetup
    {
        bool OITEnabled = false;

        // Input resources
        RGFramebufferHandle InputColor;
        RGFramebufferHandle OITAccum;
        RGFramebufferHandle OITRevealage;

        // Output
        RGFramebufferHandle& OutColor;
    };

} // namespace OloEngine
