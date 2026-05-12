#pragma once

#include "OloEngine/Renderer/ResourceHandle.h"

#include <array>

namespace OloEngine
{
    // @brief Typed per-frame blackboard for canonical RenderGraph resource handles.
    //
    // `RenderPipeline::PopulateBlackboard(...)` populates this at frame start
    // by declaring graph-owned transients, declaring frame-local resources
    // with explicit external backing, and importing long-lived external
    // resources or prior-frame histories, then storing the returned typed
    // handles here.
    //
    // Passes that need to declare resources by slot can read handles from the
    // blackboard (e.g. `board.SceneColor`) and pass them to `DeclareRead`/
    // `DeclareWrite` instead of using raw string constants directly.
    //
    // Frame resources are grouped into:
    //  - Scene outputs (produced by SceneRenderPass / GBuffer fill)
    //  - AO / shadow maps (produced by SSAO/GTAO/ShadowRenderPass)
    //  - Post-process chain (produced by standalone dynamic post passes)
    //  - OIT buffers (produced by WaterRenderPass / ParticleRenderPass / DecalRenderPass)
    //  - Temporal histories (imported from prior frame)
    //
    // Unset handles (IsValid() == false) mean the resource is not available in
    // the current frame — passes must guard against this when consuming optional
    // resources (e.g. AO when no AO technique is enabled).
    struct FrameBlackboard
    {
        static constexpr u32 MaxHZBMipViews = 16u;
        static constexpr u32 MaxShadowMapCascades = 4u;
        static constexpr u32 MaxShadowMapSpotLights = 4u;
        static constexpr u32 MaxShadowMapPointLights = 4u;
        static constexpr u32 MaxShadowMapCubeFaces = 6u;

        // -----------------------------------------------------------------------
        // Scene outputs
        // -----------------------------------------------------------------------
        RGFramebufferHandle SceneColor;       // HDR scene framebuffer (RGBA16F + editor/depth MRT attachments)
        RGTextureHandle SceneColorTexture;    // Live SceneColor RT0 attachment view
        RGTextureHandle SceneEntityID;        // Live SceneColor RT1 entity-ID attachment view
        RGTextureHandle SceneViewNormals;     // Live SceneColor RT2 view-space normals attachment view
        RGTextureHandle SceneDepthAttachment; // Live SceneColor depth attachment view
        RGTextureHandle SceneDepth;           // Semantic scene depth (forward snapshot texture, deferred attachment view, or deferred MSAA resolve view)
        RGTextureHandle SceneNormals;         // Semantic AO/deferred normals input (forward snapshot, deferred attachment view, or deferred MSAA resolve view)

        // -----------------------------------------------------------------------
        // G-Buffer (deferred rendering path only)
        // -----------------------------------------------------------------------
        // Layout matches GBuffer::AttachmentIndex:
        //   RT0 Albedo   (RGBA8)  — albedo.rgb + metallic.a
        //   RT1 Normal   (RGBA16F)— octahedral normal + roughness + AO
        //   RT2 Emissive (RGBA16F)— emissive HDR (+ unlit flag in .a)
        //   RT3 Velocity (RG16F)  — exposed via `Velocity` below.
        RGTextureHandle GBufferAlbedo;   // RT0 — canonical single-sample albedo + metallic view (direct attachment or MSAA resolve view)
        RGTextureHandle GBufferNormal;   // RT1 — canonical single-sample normal + roughness + AO view (direct attachment or MSAA resolve view)
        RGTextureHandle GBufferEmissive; // RT2 — canonical single-sample emissive HDR view (direct attachment or MSAA resolve view)
        RGTextureHandle Velocity;        // RT3 — screen-space motion vectors (forward snapshot, deferred attachment view, or deferred MSAA resolve view)

        // -----------------------------------------------------------------------
        // G-Buffer multisample companions (deferred + MSAA)
        // -----------------------------------------------------------------------
        // Populated only when `GBuffer::GetSampleCount() > 1`. These are
        // graph-owned multisample texture handles (`GL_TEXTURE_2D_MULTISAMPLE`)
        // published post-decal from the live GBuffer attachments. The canonical
        // single-sample handles above become explicit multisample-resolve views
        // over these sources, while consumers that need per-sample shading —
        // today only `DeferredLightingPass` — read these multisample handles
        // directly and still fall back to `GBuffer::GetMSColorAttachmentID()`
        // for headless / unit-test contexts.
        RGTextureHandle GBufferAlbedoMS;   // RT0 multisample attachment view
        RGTextureHandle GBufferNormalMS;   // RT1 multisample attachment view
        RGTextureHandle GBufferEmissiveMS; // RT2 multisample attachment view
        RGTextureHandle VelocityMS;        // RT3 multisample attachment view
        RGTextureHandle SceneDepthMS;      // multisample depth/stencil attachment view

        // -----------------------------------------------------------------------
        // Ambient occlusion
        // -----------------------------------------------------------------------
        RGTextureHandle AOBuffer; // SSAO or GTAO output (depending on active technique)

        // Phase D: transient scratch resources (pool-allocated, no cross-frame persistence)
        // SSAORaw — noisy half-res AO written by SSAO pass 1 and consumed by SSAO blur pass 2.
        // Internal to SSAORenderPass; never consumed by other passes.
        RGFramebufferHandle SSAORaw;  // half-res RG16F scratch; transient, not imported
        RGFramebufferHandle SSAOBlur; // half-res RG16F bilateral-blur scratch; copied into AOBuffer after blur

        // Phase D Slice 2: JFA ping-pong framebuffers for SelectionOutlineRenderPass.
        // Full-res RGBA32F; ping-pong alternated by the jump-flood passes.
        // Internal to SelectionOutlineRenderPass; never consumed by other passes.
        RGFramebufferHandle JFAPing; // RGBA32F full-res JFA ping buffer
        RGFramebufferHandle JFAPong; // RGBA32F full-res JFA pong buffer

        // Phase D Slice 3: Bloom mip-chain scratch framebuffers.
        // Up to 5 entries (matches BloomRenderPass::MAX_BLOOM_MIPS); each level is
        // half the size of the previous (starting at viewport/2). RGBA16F.
        // Internal to BloomRenderPass; never consumed by other passes.
        std::array<RGFramebufferHandle, 5> BloomMips{}; // RGBA16F half-res mip chain

        // GTAO denoise scratch textures (R8, viewport-sized).
        // The main GTAO dispatch writes into Ping, denoise ping-pongs Ping/Pong,
        // and the final result is copied into AOBuffer.
        RGTextureHandle GTAODenoisePing;
        RGTextureHandle GTAODenoisePong;

        // GTAO edge scratch texture (R8, viewport-sized).
        // Written by the GTAO main dispatch (bilateral edge weights), read by the
        // denoise pass. Internal to GTAORenderPass; never consumed by other passes.
        RGTextureHandle GTAOEdge;

        // Phase D Slice 6: HZB depth pyramid scratch texture (R32F mip chain).
        // Written by HZB generation and sampled by GTAO. Internal to GTAORenderPass;
        // never consumed by other passes.
        RGTextureHandle HZBDepth;
        std::array<RGTextureHandle, MaxHZBMipViews> HZBDepthMipViews{}; // Logical single-mip views over HZBDepth

        // Phase D Slice 5: Water refraction scratch texture (RGBA16F, viewport-sized).
        // Copied from scene color before water renders; sampled by water shaders for
        // refraction distortion. Internal to WaterRenderPass; never consumed by other passes.
        RGTextureHandle WaterRefraction;

        // Phase D Slice 8: Fog half-resolution scratch framebuffer.
        // RGBA16F at ceil(viewport/2). Written by Fog pass A, sampled by Fog pass B,
        // then extracted into `FogHistory` for next-frame reprojection.
        RGFramebufferHandle FogHalfRes;

        // -----------------------------------------------------------------------
        // Shadow maps
        // -----------------------------------------------------------------------
        RGTextureHandle ShadowMapCSM;                                              // Cascaded shadow map array root (frame-local transient with explicit backing when available)
        std::array<RGTextureHandle, MaxShadowMapCascades> ShadowMapCSMCascades{};  // Explicit per-cascade array-layer views over ShadowMapCSM
        RGTextureHandle ShadowMapSpot;                                             // Spot-light shadow atlas root (frame-local transient with explicit backing when available)
        std::array<RGTextureHandle, MaxShadowMapSpotLights> ShadowMapSpotLayers{}; // Explicit per-light array-layer views over ShadowMapSpot
        // Point-light shadow cubemaps — indexed by light slot (0..3).
        // Matches UBOStructures::ShadowUBO::MAX_POINT_SHADOWS == 4.
        std::array<RGTextureHandle, MaxShadowMapPointLights> ShadowMapPoint{};                                         // Point-light cubemap roots (frame-local transients with explicit backing when available)
        std::array<std::array<RGTextureHandle, MaxShadowMapCubeFaces>, MaxShadowMapPointLights> ShadowMapPointFaces{}; // Explicit per-face views over each point-light cubemap

        // -----------------------------------------------------------------------
        // Post-process chain outputs
        // -----------------------------------------------------------------------
        RGFramebufferHandle SSSColor;                 // Full-resolution SSS output when the blur stage is enabled and ready
        RGTextureHandle SSSColorTexture;              // Color attachment view of SSSColor
        RGFramebufferHandle AOApplyColor;             // After AO apply (only valid when SSAO or GTAO is enabled)
        RGTextureHandle AOApplyColorTexture;          // Color attachment view of AOApplyColor
        RGFramebufferHandle PostProcessColor;         // Alias for the latest upstream full-resolution post-chain source (AOApply, SSS, or SceneColor)
        RGTextureHandle PostProcessColorTexture;      // Color attachment view alias matching PostProcessColor
        RGFramebufferHandle BloomColor;               // After Bloom composite (only valid when Bloom is enabled)
        RGTextureHandle BloomColorTexture;            // Color attachment view of BloomColor
        RGFramebufferHandle DOFColor;                 // After depth-of-field (only valid when DOF is enabled)
        RGTextureHandle DOFColorTexture;              // Color attachment view of DOFColor
        RGFramebufferHandle MotionBlurColor;          // After motion blur (only valid when motion blur is enabled)
        RGTextureHandle MotionBlurColorTexture;       // Color attachment view of MotionBlurColor
        RGFramebufferHandle TAAColor;                 // After temporal AA resolve (only valid when TAA is enabled)
        RGTextureHandle TAAColorTexture;              // Color attachment view of TAAColor
        RGFramebufferHandle PrecipitationColor;       // After screen-space precipitation overlay (only valid when precipitation screen FX enabled)
        RGTextureHandle PrecipitationColorTexture;    // Color attachment view of PrecipitationColor
        RGFramebufferHandle FogColor;                 // After volumetric fog composite (only valid when fog is enabled)
        RGTextureHandle FogColorTexture;              // Color attachment view of FogColor
        RGFramebufferHandle ChromAbColor;             // After chromatic aberration (only valid when ChromAb enabled)
        RGTextureHandle ChromAbColorTexture;          // Color attachment view of ChromAbColor
        RGFramebufferHandle ColorGradingColor;        // After colour grading (only valid when ColorGrading enabled)
        RGTextureHandle ColorGradingColorTexture;     // Color attachment view of ColorGradingColor
        RGFramebufferHandle ToneMapColor;             // After tone mapping (always valid when ToneMapPass exists)
        RGTextureHandle ToneMapColorTexture;          // Color attachment view of ToneMapColor
        RGFramebufferHandle VignetteColor;            // After vignette (only valid when Vignette enabled)
        RGTextureHandle VignetteColorTexture;         // Color attachment view of VignetteColor
        RGFramebufferHandle FXAAColor;                // Anti-aliased post-process output (only valid when FXAA enabled)
        RGTextureHandle FXAAColorTexture;             // Color attachment view of FXAAColor
        RGFramebufferHandle SelectionOutlineColor;    // PostProcess output with selection outline composited
        RGTextureHandle SelectionOutlineColorTexture; // Color attachment view of SelectionOutlineColor
        RGFramebufferHandle UIComposite;              // UI composited over post-processed scene
        RGTextureHandle UICompositeTexture;           // UIComposite RT0 attachment view
        RGFramebufferHandle Backbuffer;               // Imported external output target (default framebuffer / swapchain)

        // -----------------------------------------------------------------------
        // OIT buffers
        // -----------------------------------------------------------------------
        RGFramebufferHandle OITBuffer;      // Shared WB-OIT MRT framebuffer
        RGTextureHandle OITAccum;           // WB-OIT RGBA16F accumulation attachment view
        RGTextureHandle OITRevealage;       // WB-OIT RG16F revealage attachment view
        RGTextureHandle OITDepthAttachment; // WB-OIT depth attachment view seeded from SceneDepthAttachment

        // -----------------------------------------------------------------------
        // Temporal histories (imported from prior frame)
        // -----------------------------------------------------------------------
        RGTextureHandle TAAHistory; // Previous TAA accumulation buffer
        RGTextureHandle FogHistory; // Previous volumetric fog integration result

        // -----------------------------------------------------------------------
        // IBL resources (global, imported once per IBL change)
        // -----------------------------------------------------------------------
        RGTextureHandle IrradianceMap; // Diffuse irradiance cube
        RGTextureHandle PrefilterMap;  // Specular pre-filter mip chain
        RGTextureHandle BrdfLut;       // BRDF integration LUT

        // Resets all handles to the default (invalid) state.
        // Called at the start of each frame before imports are registered.
        void Reset()
        {
            *this = FrameBlackboard{};
        }
    };

} // namespace OloEngine
