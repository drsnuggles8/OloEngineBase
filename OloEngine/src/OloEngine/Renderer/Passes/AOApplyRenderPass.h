#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Standalone AO Apply post-process pass.
    //
    // Standalone AO stage inserted into the dynamic chain:
    //   SSSColor/SceneColor → AOApply → AOApplyColor → Bloom → ...
    //
    // Inputs:
    //   * Input framebuffer handle (SSSColor or SceneColor)
    //   * AO texture handle (from SSAORenderPass or GTAORenderPass)
    //   * Scene depth texture handle (for bilateral AO upsampling)
    //   * PostProcessUBO (binding 7), uploaded by Renderer3D
    //
    // Output:
    //   * AOApplyColor (RGBA16F) — AO-modulated scene color
    //
    // Passthrough semantics: when disabled (no AO technique active or AO
    // textures unavailable), the pass no-ops and GetTarget() returns the
    // input framebuffer.
    class AOApplyRenderPass : public RenderPass
    {
      public:
        AOApplyRenderPass();
        ~AOApplyRenderPass() override = default;

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

        void SetSceneDepthTextureID(u32 id) noexcept
        {
            m_SceneDepthTextureID = id;
        }

        void SetPostProcessUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_PostProcessUBO = ubo;
        }

      private:
        void CreateFramebuffer(u32 width, u32 height);

      private:
        bool m_Enabled = false;
        Ref<Framebuffer> m_OutputFB;

        Ref<Shader> m_SSAOApplyShader;
        Ref<UniformBuffer> m_PostProcessUBO;

        u32 m_SceneDepthTextureID = 0;
    };

} // namespace OloEngine
