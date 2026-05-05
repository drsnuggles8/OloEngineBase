#pragma once

#include "OloEngine/Renderer/ResourceHandle.h"

#include <array>

namespace OloEngine
{
    // @brief Typed per-frame blackboard for canonical RenderGraph resource handles.
    //
    // `Renderer3D::SetupFrameBlackboard()` populates this at frame start by
    // calling `RenderGraph::ImportTexture` / `ImportFramebuffer` / `ImportHistory`
    // for each live physical resource, then storing the returned typed handles here.
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
        // -----------------------------------------------------------------------
        // Scene outputs
        // -----------------------------------------------------------------------
        RGFramebufferHandle SceneColor; // HDR scene framebuffer (RGBA16F)
        RGTextureHandle SceneDepth;     // Shared depth/stencil attachment
        RGTextureHandle SceneNormals;   // World-space normals (deferred + SSAO path)

        // -----------------------------------------------------------------------
        // G-Buffer (deferred rendering path only)
        // -----------------------------------------------------------------------
        // Layout matches GBuffer::AttachmentIndex:
        //   RT0 Albedo   (RGBA8)  — albedo.rgb + metallic.a
        //   RT1 Normal   (RGBA16F)— octahedral normal + roughness + AO
        //   RT2 Emissive (RGBA16F)— emissive HDR (+ unlit flag in .a)
        //   RT3 Velocity (RG16F)  — exposed via `Velocity` below.
        RGTextureHandle GBufferAlbedo;   // RT0 — albedo + metallic
        RGTextureHandle GBufferNormal;   // RT1 — octahedral normal + roughness + AO
        RGTextureHandle GBufferEmissive; // RT2 — emissive HDR
        RGTextureHandle Velocity;        // RT3 — screen-space motion vectors

        // -----------------------------------------------------------------------
        // G-Buffer multisample companions (deferred + MSAA)
        // -----------------------------------------------------------------------
        // Populated only when `GBuffer::GetSampleCount() > 1`. These are raw
        // multisample texture IDs (`GL_TEXTURE_2D_MULTISAMPLE`), distinct from
        // the resolved single-sample copies above. Consumers — today only
        // `DeferredLightingPass` per-sample shading — prefer these typed
        // handles when MSAA is active and fall back to
        // `GBuffer::GetMSColorAttachmentID()` for headless / unit-test
        // contexts.
        RGTextureHandle GBufferAlbedoMS;   // RT0 multisample
        RGTextureHandle GBufferNormalMS;   // RT1 multisample
        RGTextureHandle GBufferEmissiveMS; // RT2 multisample
        RGTextureHandle VelocityMS;        // RT3 multisample
        RGTextureHandle SceneDepthMS;      // multisample depth/stencil

        // -----------------------------------------------------------------------
        // Ambient occlusion
        // -----------------------------------------------------------------------
        RGTextureHandle AOBuffer; // SSAO or GTAO output (depending on active technique)

        // Phase D: transient scratch resources (pool-allocated, no cross-frame persistence)
        // SSAORaw — noisy half-res AO written by SSAO pass 1 and consumed by SSAO blur pass 2.
        // Internal to SSAORenderPass; never consumed by other passes.
        RGFramebufferHandle SSAORaw; // half-res RG16F scratch; transient, not imported

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

        // Phase D Slice 4: GTAO edge scratch texture (R8, viewport-sized).
        // Written by the GTAO main dispatch (bilateral edge weights), read by the
        // denoise pass. Internal to GTAORenderPass; never consumed by other passes.
        RGTextureHandle GTAOEdge;

        // Phase D Slice 6: HZB depth pyramid scratch texture (R32F mip chain).
        // Written by HZB generation and sampled by GTAO. Internal to GTAORenderPass;
        // never consumed by other passes.
        RGTextureHandle HZBDepth;

        // Phase D Slice 5: Water refraction scratch texture (RGBA16F, viewport-sized).
        // Copied from scene color before water renders; sampled by water shaders for
        // refraction distortion. Internal to WaterRenderPass; never consumed by other passes.
        RGTextureHandle WaterRefraction;

        // -----------------------------------------------------------------------
        // Shadow maps
        // -----------------------------------------------------------------------
        RGTextureHandle ShadowMapCSM;  // Cascaded shadow map array
        RGTextureHandle ShadowMapSpot; // Spot-light shadow atlas
        // Point-light shadow cubemaps — indexed by light slot (0..3).
        // Matches UBOStructures::ShadowUBO::MAX_POINT_SHADOWS == 4.
        std::array<RGTextureHandle, 4> ShadowMapPoint{}; // Default-init: all handles invalid

        // -----------------------------------------------------------------------
        // Post-process chain outputs
        // -----------------------------------------------------------------------
        RGFramebufferHandle SSSColor;              // Output of SSS stage (or passthrough scene color)
        RGFramebufferHandle AOApplyColor;          // After AO apply (only valid when SSAO or GTAO is enabled)
        RGFramebufferHandle PostProcessColor;      // Dynamic post-chain input (typically AOApply/SSS/Scene fallback)
        RGFramebufferHandle BloomColor;            // After Bloom composite (only valid when Bloom is enabled)
        RGFramebufferHandle DOFColor;              // After depth-of-field (only valid when DOF is enabled)
        RGFramebufferHandle MotionBlurColor;       // After motion blur (only valid when motion blur is enabled)
        RGFramebufferHandle TAAColor;              // After temporal AA resolve (only valid when TAA is enabled)
        RGFramebufferHandle PrecipitationColor;    // After screen-space precipitation overlay (only valid when precipitation screen FX enabled)
        RGFramebufferHandle FogColor;              // After volumetric fog composite (only valid when fog is enabled)
        RGFramebufferHandle ChromAbColor;          // After chromatic aberration (only valid when ChromAb enabled)
        RGFramebufferHandle ColorGradingColor;     // After colour grading (only valid when ColorGrading enabled)
        RGFramebufferHandle ToneMapColor;          // After tone mapping (always valid when ToneMapPass exists)
        RGFramebufferHandle VignetteColor;         // After vignette (only valid when Vignette enabled)
        RGFramebufferHandle FXAAColor;             // Anti-aliased post-process output (only valid when FXAA enabled)
        RGFramebufferHandle SelectionOutlineColor; // PostProcess output with selection outline composited
        RGFramebufferHandle UIComposite;           // UI composited over post-processed scene
        RGFramebufferHandle Backbuffer;            // Imported external output target (default framebuffer / swapchain)

        // -----------------------------------------------------------------------
        // OIT buffers
        // -----------------------------------------------------------------------
        RGFramebufferHandle OITAccum;     // WB-OIT RGBA16F accumulation
        RGFramebufferHandle OITRevealage; // WB-OIT RG16F revealage

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
