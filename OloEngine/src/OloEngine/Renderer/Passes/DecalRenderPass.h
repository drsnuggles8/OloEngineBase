#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/OITBuffer.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Shader.h"

#include <functional>
#include <utility>

namespace OloEngine
{
    // @brief Render pass for deferred projected decals.
    //
    // Uses the command bucket system for sorted dispatch of DrawDecalCommands.
    // Renders into the ScenePass framebuffer after scene and foliage geometry,
    // reading the scene depth buffer for projection.
    //
    // This pass follows the Molecular Matters design — decals are POD commands
    // submitted to a command bucket, sorted by DrawKey, and dispatched through
    // the standard CommandDispatch table.
    class DecalRenderPass : public CommandBufferRenderPass
    {
      public:
        DecalRenderPass();
        ~DecalRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSceneFramebuffer(const Ref<Framebuffer>& fb);

        // Deferred-path entry point. Called by SceneRenderPass while the
        // G-Buffer is still bound, right after the main G-Buffer MRT write.
        // Drains the decal command bucket with the G-Buffer variant shader,
        // writing only into colour attachment 0 (albedo). The regular
        // graph-scheduled Execute() then sees an empty bucket and no-ops.
        //
        // writeTargetFB:      framebuffer decals rasterize into (MS in MSAA
        //                     per-sample mode, resolved FB otherwise).
        // depthSamplingFB:    framebuffer whose depth attachment the decal
        //                     fragment shader samples to reconstruct world
        //                     position (always single-sample so the shader
        //                     can use a plain `sampler2D`). When equal to
        //                     writeTargetFB, the single-arg form is used.
        void ExecuteOnGBuffer(Ref<Framebuffer> gbufferFB)
        {
            ExecuteOnGBuffer(gbufferFB, gbufferFB);
        }
        void ExecuteOnGBuffer(Ref<Framebuffer> writeTargetFB,
                              Ref<Framebuffer> depthSamplingFB);

        // WB-OIT wiring (forward-path only). In the deferred path decals are
        // written into the G-Buffer via `ExecuteOnGBuffer` and are naturally
        // order-independent (depth test + G-buffer writes are not blended).
        // In the forward path they were previously alpha-blended in scene
        // FB; when an OIT buffer is attached and enabled, `Execute()` routes
        // them through the same WB-OIT compositing pipeline as water /
        // particles. See `WaterRenderPass::SetOITBuffer` for invariants.
        void SetOITBuffer(const Ref<OITBuffer>& oitBuffer) noexcept
        {
            m_OITBuffer = oitBuffer;
        }
        void SetOITEnabled(bool enabled) noexcept
        {
            m_OITEnabled = enabled;
        }
        void SetOITAccumulationMarker(std::function<void()> marker)
        {
            m_AccumMarker = std::move(marker);
        }
        void SetOITShader(const Ref<Shader>& shader) noexcept
        {
            m_OITShader = shader;
        }

      private:
        Ref<Framebuffer> m_SceneFramebuffer;

        Ref<OITBuffer> m_OITBuffer;
        Ref<Shader> m_OITShader;
        std::function<void()> m_AccumMarker;
        bool m_OITEnabled = false;

        // Set by `ExecuteOnGBuffer` (deferred path) after it has drained the
        // opaque decal packets into the G-Buffer. The graph-scheduled
        // `Execute()` then knows to skip already-rendered opaque packets and
        // only drain `transparent == 1` packets (compositing them over the
        // already-lit scene colour). Cleared by `Execute()` once it has
        // reset the bucket.
        bool m_OpaqueDecalsDrained = false;
    };
} // namespace OloEngine
