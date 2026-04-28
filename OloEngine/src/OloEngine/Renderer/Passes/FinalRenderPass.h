#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    // @brief Render pass for the final screen output.
    //
    // This pass takes the output from a previous pass (typically the scene pass) and
    // renders it to the default framebuffer (screen) using a fullscreen triangle.
    // It can optionally apply post-processing effects.
    class FinalRenderPass : public RenderPass
    {
      public:
        FinalRenderPass();
        ~FinalRenderPass() override = default;

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
        void SetInputFramebuffer(const Ref<Framebuffer>& input) override;
        void SetInputFramebufferHandle(RGFramebufferHandle handle)
        {
            m_InputFramebufferHandle = handle;
        }
        [[nodiscard]] Ref<Framebuffer> GetInputFramebuffer() const;

      private:
        Ref<Framebuffer> m_InputFramebuffer; // The framebuffer to render to the screen
        RGFramebufferHandle m_InputFramebufferHandle;
        Ref<Shader> m_BlitShader; // Shader for blitting the framebuffer to the screen
    };
} // namespace OloEngine
