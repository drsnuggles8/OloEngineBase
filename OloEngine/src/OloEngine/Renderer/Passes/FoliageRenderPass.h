#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"

namespace OloEngine
{
    // @brief Render pass for terrain foliage (grass, plants, etc.)
    //
    // Uses the command bucket system for sorted dispatch of DrawFoliageLayerCommands.
    // Renders into the ScenePass framebuffer after opaque scene geometry.
    // Each foliage layer on each terrain entity becomes one POD command.
    //
    // This pass follows the Molecular Matters design — foliage layers are POD
    // commands submitted to a command bucket, sorted by DrawKey, and dispatched
    // through the standard CommandDispatch table.
    class FoliageRenderPass : public CommandBufferRenderPass
    {
      public:
        FoliageRenderPass();
        ~FoliageRenderPass() override = default;

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
