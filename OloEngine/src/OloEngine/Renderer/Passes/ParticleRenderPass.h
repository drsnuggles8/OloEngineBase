#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include <functional>

namespace OloEngine
{
    // @brief Render pass for transparent particle rendering.
    //
    // Executes between SceneRenderPass and (OITResolvePass → SSSPass → ...).
    //
    // Two code paths:
    //   - Classic (OIT off): renders into the ScenePass framebuffer with
    //     depth-test read-only and GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA
    //     alpha blending. Sort order is back-to-front via DrawKey.
    //   - Weighted-blended OIT (Phase 6, toggle via
    //     RendererSettings::OITEnabled): renders into the
    //     graph-owned OIT framebuffer with per-attachment blend funcs
    //     (accum: GL_ONE/GL_ONE, revealage: GL_ZERO/GL_ONE_MINUS_SRC_COLOR)
    //     and order-independent accumulation. OITResolvePass composites
    //     the result over the scene FB afterwards.
    class ParticleRenderPass : public RenderGraphNode
    {
      public:
        using RenderCallback = std::function<void()>;

        ParticleRenderPass();
        ~ParticleRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetRenderCallback(RenderCallback callback);
        [[nodiscard]] bool HasRenderCallback() const noexcept
        {
            return static_cast<bool>(m_RenderCallback);
        }
        void SetOITEnabled(bool enabled) noexcept
        {
            m_OITEnabled = enabled;
        }

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
        RGFramebufferHandle m_SelectedOITFramebuffer;
        RenderCallback m_RenderCallback;
        bool m_OITEnabled = false;
    };
} // namespace OloEngine
