#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/OITBuffer.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
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
    //     RendererSettings::DeferredSettings::OITEnabled): renders into the
    //     attached OITBuffer with per-attachment blend funcs
    //     (accum: GL_ONE/GL_ONE, revealage: GL_ZERO/GL_ONE_MINUS_SRC_COLOR)
    //     and order-independent accumulation. OITResolvePass composites
    //     the result over the scene FB afterwards.
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

        // Phase 6 OIT wiring. Provided by Renderer3D from the
        // OITResolveRenderPass; when non-null AND `m_OITEnabled` is true,
        // particles render into the OIT buffer instead of the scene FB.
        void SetOITBuffer(const Ref<OITBuffer>& oitBuffer)
        {
            m_OITBuffer = oitBuffer;
        }
        // Marker callback invoked after successful OIT accumulation so
        // OITResolvePass knows it has content to composite this frame.
        void SetOITAccumulationMarker(std::function<void()> marker)
        {
            m_AccumMarker = std::move(marker);
        }
        void SetOITEnabled(bool enabled) noexcept
        {
            m_OITEnabled = enabled;
        }

      private:
        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<OITBuffer> m_OITBuffer;
        RenderCallback m_RenderCallback;
        std::function<void()> m_AccumMarker;
        bool m_OITEnabled = false;
    };
} // namespace OloEngine
