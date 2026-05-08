#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    class SSSRenderPass : public RenderPass
    {
      public:
        SSSRenderPass();
        ~SSSRenderPass() override = default;

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

        void SetSettings(const SnowSettings& settings)
        {
            m_Settings = settings;
        }
        [[nodiscard]] bool IsReadyForExecution() const noexcept
        {
            return m_SSSBlurShader && m_SSSBlurShader->IsReady();
        }
        void SetSSSUBO(Ref<UniformBuffer> ubo, SSSUBOData* gpuData)
        {
            m_SSSUBO = ubo;
            m_GPUData = gpuData;
        }

      private:
        void DrawFullscreenTriangle(RGCommandContext& context);
        void CreateOutputFramebuffer(u32 width, u32 height);

        Ref<Shader> m_SSSBlurShader;
        Ref<UniformBuffer> m_SSSUBO;
        SSSUBOData* m_GPUData = nullptr;

        SnowSettings m_Settings;
    };
} // namespace OloEngine
