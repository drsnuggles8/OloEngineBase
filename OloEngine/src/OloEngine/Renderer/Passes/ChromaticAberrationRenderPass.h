#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone chromatic aberration post-process pass.
    //
    // Phase F slice 17 — standalone effect in the dynamic post chain
    // following the pattern established by FXAARenderPass (slice 16).
    //
    // Sits first in the extracted-effect sub-chain:
    //   PostProcess → ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Operates in HDR space on the post-AO/Bloom/DOF/MotionBlur/TAA/Fog output.
    // Passthrough when `Enabled` is false; `GetTarget()` returns the input
    // framebuffer so downstream stages see the unmodified source.
    class ChromaticAberrationRenderPass : public RenderPass
    {
      public:
        ChromaticAberrationRenderPass();
        ~ChromaticAberrationRenderPass() override = default;

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

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept
        {
            return m_Enabled;
        }

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_PostProcessUBO;

        bool m_Enabled = false;
    };
} // namespace OloEngine
