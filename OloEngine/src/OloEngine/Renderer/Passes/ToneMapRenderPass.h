#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone tone-mapping post-process pass (HDR → LDR).
    //
    // Phase F slice 17 — standalone effect in the dynamic post chain
    // following the pattern established by FXAARenderPass (slice 16).
    //
    // Sits third in the extracted-effect sub-chain:
    //   PostProcess → ChromAberration → ColorGrading → ToneMap → Vignette → FXAA
    //
    // Unlike the other extracted effects, tone mapping runs unconditionally
    // (no per-settings gate). `m_Enabled` defaults to `true` and is never
    // set false by Renderer3D. The `SetEnabled` setter is provided only for
    // debugging / future use.
    class ToneMapRenderPass : public RenderPass
    {
      public:
        ToneMapRenderPass();
        ~ToneMapRenderPass() override = default;

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

        // Defaults true — tone mapping runs every frame unconditionally.
        bool m_Enabled = true;
    };
} // namespace OloEngine
