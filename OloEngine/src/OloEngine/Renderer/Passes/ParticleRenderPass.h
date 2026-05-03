#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/OITBuffer.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
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
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::Mixed;
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetRenderCallback(RenderCallback callback);

        // Phase 6 OIT wiring. Provided by Renderer3D from the
        // OITResolveRenderPass; when OIT is enabled the provider returns
        // the lazy-materialised OITBuffer so this pass renders into it
        // instead of the scene FB. The provider is queried only when
        // `m_OITEnabled` is true so non-OIT frames do not trigger
        // creation. Phase F slice 15 — replaces the previously cached
        // `Ref<OITBuffer>` setter so the buffer can be transient w.r.t.
        // the OIT toggle.
        void SetOITBufferProvider(std::function<Ref<OITBuffer>()> provider)
        {
            m_OITBufferProvider = std::move(provider);
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
        std::function<Ref<OITBuffer>()> m_OITBufferProvider;
        RenderCallback m_RenderCallback;
        std::function<void()> m_AccumMarker;
        bool m_OITEnabled = false;
    };
} // namespace OloEngine
