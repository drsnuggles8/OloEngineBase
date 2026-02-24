#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

namespace OloEngine
{
    class SSAORenderPass : public RenderPass
    {
      public:
        SSAORenderPass();
        ~SSAORenderPass() override;

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
        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }
        void SetSSAOUBO(Ref<UniformBuffer> ubo, SSAOUBOData* gpuData)
        {
            m_SSAOUBO = ubo;
            m_GPUData = gpuData;
        }

        [[nodiscard]] u32 GetSSAOTextureID() const;

      private:
        void CreateSSAOFramebuffers(u32 width, u32 height);
        void CreateNoiseTexture();
        void DrawFullscreenTriangle();

        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<Framebuffer> m_SSAOFramebuffer;
        Ref<Framebuffer> m_BlurFramebuffer;

        Ref<Shader> m_SSAOShader;
        Ref<Shader> m_SSAOBlurShader;

        Ref<VertexArray> m_FullscreenTriangleVA;
        Ref<UniformBuffer> m_SSAOUBO;
        SSAOUBOData* m_GPUData = nullptr;

        PostProcessSettings m_Settings;

        u32 m_NoiseTexture = 0;
        u32 m_HalfWidth = 0;
        u32 m_HalfHeight = 0;
    };
} // namespace OloEngine
