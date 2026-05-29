#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"

namespace OloEngine
{
    // @brief Render pass for animated water surfaces.
    //
    // Uses the command bucket system for sorted dispatch of DrawWaterCommands.
    // Renders into the ScenePass framebuffer after foliage geometry but before decals.
    // Water is rendered with alpha blending for transparency support.
    //
    // This pass follows the Molecular Matters design — water surfaces are POD
    // commands submitted to a command bucket, sorted by DrawKey, and dispatched
    // through the standard CommandDispatch table.
    class WaterRenderPass : public CommandBufferRenderPass
    {
      public:
        WaterRenderPass();
        ~WaterRenderPass() override = default;

        WaterRenderPass(const WaterRenderPass&) = delete;
        WaterRenderPass& operator=(const WaterRenderPass&) = delete;
        WaterRenderPass(WaterRenderPass&&) = delete;
        WaterRenderPass& operator=(WaterRenderPass&&) = delete;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        Ref<Framebuffer> m_SceneFramebuffer;

      private:
        RGTextureHandle m_SelectedSceneColorTexture{};
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedSceneNormalsTexture{};
        RGTextureHandle m_SelectedRefractionTexture{};

        // Dedicated depth target capturing the nearest wavy water surface per
        // pixel (depth-only re-render of the water geometry). Published to
        // Renderer3D for the underwater-fog stage. Persists across frames; resized
        // with the viewport. Owned here (not the shared scene FBO) so adding it
        // can't perturb the scene/G-buffer attachment layout.
        Ref<Framebuffer> m_WaterDepthFB;
    };
} // namespace OloEngine
