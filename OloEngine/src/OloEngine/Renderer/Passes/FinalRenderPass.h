#pragma once

#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    class FinalRenderPass : public RenderPass
    {
    public:
        FinalRenderPass();
        ~FinalRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override { return m_Target; }
        
        void SetInputFramebuffer(const Ref<Framebuffer>& input) { m_InputFramebuffer = input; }
        [[nodiscard]] Ref<Framebuffer> GetInputFramebuffer() const { return m_InputFramebuffer; }

        void SetupFramebuffer(uint32_t width, uint32_t height) override;
        void ResizeFramebuffer(uint32_t width, uint32_t height) override;
        void OnReset() override;

    private:
        // Creates a full-screen triangle mesh for the final pass
        void CreateFullscreenTriangle();

    private:
        Ref<Framebuffer> m_InputFramebuffer;
        Ref<Shader> m_BlitShader;
        Ref<VertexArray> m_FullscreenTriangleVA;
    };
} 