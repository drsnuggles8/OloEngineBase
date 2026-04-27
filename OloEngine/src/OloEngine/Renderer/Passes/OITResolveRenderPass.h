#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/OITBuffer.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    // @brief Weighted-blended OIT composite pass.
    //
    // Runs after transparent passes (Particles, Water, etc.) have
    // accumulated into OITBuffer. Samples accum + revealage and composites
    // the resolved transparent colour over the scene FB in a single
    // fullscreen draw.
    //
    // Passthrough semantics: when `Enabled` is false (`OITEnabled` in
    // `RendererSettings::DeferredSettings`) the pass no-ops and
    // `GetTarget()` returns the input framebuffer so downstream stages
    // (SSS, post-process) see the unmodified scene colour. Transparent
    // passes also branch on the same flag and fall back to their classic
    // alpha-blend path.
    //
    // Storage: this pass owns the OITBuffer so transparent passes can
    // fetch it via `GetOITBuffer()` and switch render targets when the
    // toggle is on.
    class OITResolveRenderPass : public RenderPass
    {
      public:
        OITResolveRenderPass();
        ~OITResolveRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetInputFramebuffer(const Ref<Framebuffer>& input) override
        {
            m_InputFramebuffer = input;
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

        // Transparent passes call this to obtain the accumulation FBs and
        // switch their render target. Returns null until Init() has run.
        [[nodiscard]] const Ref<OITBuffer>& GetOITBuffer() const noexcept
        {
            return m_OITBuffer;
        }

        // Flag set by transparent passes when they successfully emitted
        // into the OIT buffer. Reset on every frame in Execute(); when
        // false the composite step is skipped.
        void MarkAccumulationWritten() noexcept
        {
            m_HasAccumulation = true;
        }

      private:
        void DrawFullscreenTriangle(RGCommandContext& context);

        Ref<Framebuffer> m_InputFramebuffer;
        Ref<OITBuffer> m_OITBuffer;
        Ref<Shader> m_ResolveShader;

        bool m_Enabled = false;
        bool m_HasAccumulation = false;
    };
} // namespace OloEngine
