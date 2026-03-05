#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"

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

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
    };
} // namespace OloEngine
