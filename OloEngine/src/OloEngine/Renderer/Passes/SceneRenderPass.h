#pragma once

#include "OloEngine/Renderer/Passes/RenderPass.h"

namespace OloEngine
{
    class SceneRenderPass : public RenderPass
    {
    public:
        SceneRenderPass();
        ~SceneRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override { return m_Target; }

        void SetupFramebuffer(uint32_t width, uint32_t height) override;
        void ResizeFramebuffer(uint32_t width, uint32_t height) override;
        void OnReset() override;
    };
} 