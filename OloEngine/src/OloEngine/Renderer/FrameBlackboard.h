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
    //  - Post-process chain (produced by PostProcessRenderPass)
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
        RGFramebufferHandle AOApplyColor;          // Phase F slice 24 — after AO apply (only valid when SSAO or GTAO is enabled)
        RGFramebufferHandle PostProcessColor;      // Output of PostProcess passthrough (Phase F slice 24: AO apply moved to AOApplyPass)
        RGFramebufferHandle BloomColor;            // Phase F slice 23 — after Bloom composite (only valid when Bloom is enabled)
        RGFramebufferHandle DOFColor;              // Phase F slice 22 — after depth-of-field (only valid when DOF is enabled)
        RGFramebufferHandle MotionBlurColor;       // Phase F slice 21 — after motion blur (only valid when motion blur is enabled)
        RGFramebufferHandle TAAColor;              // Phase F slice 19 — after temporal AA resolve (only valid when TAA is enabled)
        RGFramebufferHandle PrecipitationColor;    // Phase F slice 20 — after screen-space precipitation overlay (only valid when precipitation screen FX enabled)
        RGFramebufferHandle FogColor;              // Phase F slice 18 — after volumetric fog composite (only valid when fog is enabled)
        RGFramebufferHandle ChromAbColor;          // Phase F slice 17 — after chromatic aberration (only valid when ChromAb enabled)
        RGFramebufferHandle ColorGradingColor;     // Phase F slice 17 — after colour grading (only valid when ColorGrading enabled)
        RGFramebufferHandle ToneMapColor;          // Phase F slice 17 — after tone mapping (always valid when ToneMapPass exists)
        RGFramebufferHandle VignetteColor;         // Phase F slice 17 — after vignette (only valid when Vignette enabled)
        RGFramebufferHandle FXAAColor;             // Phase F slice 16 — anti-aliased post-process output (only valid when FXAA enabled)
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
