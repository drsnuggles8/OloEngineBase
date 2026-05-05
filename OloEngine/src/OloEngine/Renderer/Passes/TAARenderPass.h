#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone temporal anti-aliasing post-process pass.
    //
    // Phase F slice 19 — standalone TAA stage in the dynamic chain:
    //   AOApply/Bloom/DOF/MotionBlur/Precipitation → TAA → Fog → ...
    //
    // Owns:
    //   * Full-resolution RGBA16F output framebuffer (`TAAColor`)
    //   * Persistent RGBA16F history framebuffer imported next frame as
    //     `TAAHistory`
    //   * TAA parameters UBO at binding 32
    //
    // Inputs (resolved from the blackboard):
    //   * Post-process colour input framebuffer
    //   * Scene depth texture (required)
    //   * Velocity texture (optional, 0 = camera-only reprojection path)
    //   * MotionBlur UBO at binding 8 is uploaded globally by Renderer3D
    //
    // Passthrough semantics: when disabled the pass no-ops and GetTarget()
    // returns the input framebuffer so downstream stages naturally fall back
    // to PostProcessColor.
    class TAARenderPass : public RenderPass
    {
      public:
        TAARenderPass();
        ~TAARenderPass() override = default;

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

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }

        [[nodiscard]] u32 GetTAAHistoryTextureID() const
        {
            if (!m_TAAHistoryValid || !m_TAAHistoryFB)
                return 0;
            return m_TAAHistoryFB->GetColorAttachmentRendererID(0);
        }

      private:
        void CreateFramebuffers(u32 width, u32 height);

      private:
        bool m_Enabled = false;
        PostProcessSettings m_Settings;

        Ref<Framebuffer> m_OutputFB;
        Ref<Framebuffer> m_TAAHistoryFB;

        Ref<Shader> m_TAAShader;
        Ref<UniformBuffer> m_TAAUBO;
        bool m_TAAHistoryValid = false;
    };
} // namespace OloEngine
