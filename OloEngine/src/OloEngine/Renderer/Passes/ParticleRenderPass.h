#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include <functional>

namespace OloEngine
{
    // @brief Render pass for transparent particle rendering.
    //
    // Executes between SceneRenderPass and FinalRenderPass.
    // Renders into the ScenePass framebuffer with depth testing enabled
    // (read-only, no depth write) so particles correctly occlude against
    // opaque scene geometry.
    class ParticleRenderPass : public RenderPass
    {
      public:
        using RenderCallback = std::function<void()>;

        ParticleRenderPass();
        ~ParticleRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSceneFramebuffer(const Ref<Framebuffer>& fb);
        void SetRenderCallback(RenderCallback callback);

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
        RenderCallback m_RenderCallback;
    };
} // namespace OloEngine
