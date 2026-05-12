#pragma once

#include "OloEngine/Core/Base.h"
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
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::Mixed;
        }
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
    };
} // namespace OloEngine
