#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
    // @brief Render pass for the final screen output.
    //
    // This pass takes the output from a previous pass (typically the scene pass) and
    // renders it to the default framebuffer (screen) using a fullscreen triangle.
    // It can optionally apply post-processing effects.
    class FinalRenderPass : public RenderGraphNode
    {
      public:
        FinalRenderPass();
        ~FinalRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

      private:
        Ref<Shader> m_BlitShader; // Shader for blitting the framebuffer to the screen
    };
} // namespace OloEngine
