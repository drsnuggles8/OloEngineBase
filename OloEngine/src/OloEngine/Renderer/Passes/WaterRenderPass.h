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
        ~WaterRenderPass() override;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSceneFramebuffer(const Ref<Framebuffer>& fb);

      private:
        void EnsureRefractionTexture(u32 width, u32 height);

        Ref<Framebuffer> m_SceneFramebuffer;
        u32 m_RefractionTextureID = 0;
        u32 m_RefractionWidth = 0;
        u32 m_RefractionHeight = 0;
    };
} // namespace OloEngine
