#pragma once

#include "OloEngine/Renderer/ResourceHandle.h"

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
        RGTextureHandle GBufferAlbedo;   // RT0 — albedo + metallic
        RGTextureHandle GBufferNormal;   // RT1 — world-space normals
        RGTextureHandle GBufferMetallic; // RT2 — roughness / metallic / packed AO
        RGTextureHandle GBufferEmissive; // RT3 — emissive HDR
        RGTextureHandle Velocity;        // Screen-space motion vectors for TAA

        // -----------------------------------------------------------------------
        // Ambient occlusion
        // -----------------------------------------------------------------------
        RGTextureHandle AOBuffer; // SSAO or GTAO output (depending on active technique)

        // -----------------------------------------------------------------------
        // Shadow maps
        // -----------------------------------------------------------------------
        RGTextureHandle ShadowMapCSM;   // Cascaded shadow map array
        RGTextureHandle ShadowMapSpot;  // Spot-light shadow atlas
        RGTextureHandle ShadowMapPoint; // Point-light shadow cube array

        // -----------------------------------------------------------------------
        // Post-process chain outputs
        // -----------------------------------------------------------------------
        RGFramebufferHandle SSSColor;              // Output of SSS stage (or passthrough scene color)
        RGFramebufferHandle PostProcessColor;      // Tonemapped + bloom + effects output
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
