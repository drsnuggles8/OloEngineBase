#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    class SSSRenderPass : public RenderPass
    {
      public:
        SSSRenderPass();
        ~SSSRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSceneFramebuffer(const Ref<Framebuffer>& sceneFB)
        {
            m_SceneFramebuffer = sceneFB;
        }
        void SetSettings(const SnowSettings& settings)
        {
            m_Settings = settings;
        }
        void SetSSSUBO(Ref<UniformBuffer> ubo, SSSUBOData* gpuData)
        {
            m_SSSUBO = ubo;
            m_GPUData = gpuData;
        }

      private:
        void DrawFullscreenTriangle();

        Ref<Framebuffer> m_SceneFramebuffer;

        Ref<Shader> m_SSSBlurShader;
        Ref<VertexArray> m_FullscreenTriangleVA;
        Ref<UniformBuffer> m_SSSUBO;
        SSSUBOData* m_GPUData = nullptr;

        SnowSettings m_Settings;
    };
} // namespace OloEngine
