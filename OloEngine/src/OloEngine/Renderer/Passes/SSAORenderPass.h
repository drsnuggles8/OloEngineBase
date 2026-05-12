#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    class SSAORenderPass : public RenderGraphNode
    {
      public:
        SSAORenderPass();
        ~SSAORenderPass() override;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] SubmissionModel GetSubmissionModel() const override
        {
            return SubmissionModel::ImmediateOnly;
        }
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }
        void SetSSAOUBO(Ref<UniformBuffer> ubo, SSAOUBOData* gpuData)
        {
            m_SSAOUBO = ubo;
            m_GPUData = gpuData;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept
        {
            return m_SSAOShader && m_SSAOShader->IsReady() &&
                   m_SSAOBlurShader && m_SSAOBlurShader->IsReady() &&
                   m_NoiseTexture != 0;
        }

      private:
        void CreateNoiseTexture();
        void DrawFullscreenTriangle();

        Ref<Shader> m_SSAOShader;
        Ref<Shader> m_SSAOBlurShader;

        Ref<UniformBuffer> m_SSAOUBO;
        SSAOUBOData* m_GPUData = nullptr;

        PostProcessSettings m_Settings;
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedSceneNormalsTexture{};
        RGTextureHandle m_SelectedAOOutputTexture{};
        RGFramebufferHandle m_SelectedBlurFramebuffer{};

        u32 m_NoiseTexture = 0;
        u32 m_HalfWidth = 0;
        u32 m_HalfHeight = 0;
    };
} // namespace OloEngine
