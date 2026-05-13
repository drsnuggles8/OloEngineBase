#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/HZBGenerator.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    // @brief GTAO (Ground Truth Ambient Occlusion) render pass using compute.
    //
    // Orchestrates the full GTAO pipeline:
    //   1. HZB depth pyramid generation (via HZBGenerator)
    //   2. GTAO compute pass (XeGTAO: 9 slices × 3 samples)
    //   3. Edge-aware bilateral denoise (configurable passes, ping-pong)
    //
    // Final output now publishes through the graph-owned AOBuffer handle, and
    // the denoise ping-pong plus edge/HZB scratch are graph-owned too.
    // Coexists with SSAORenderPass behind an AOTechnique selector.
    class GTAORenderPass : public RenderGraphNode
    {
      public:
        GTAORenderPass();
        ~GTAORenderPass() override;

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
        void SetGTAOUBO(Ref<UniformBuffer> ubo, UBOStructures::GTAOUBO* gpuData)
        {
            m_GTAOUBO = ubo;
            m_GPUData = gpuData;
        }

        // Projection matrix needed for NDCToView calculations
        void SetProjectionMatrix(const glm::mat4& projection)
        {
            m_Projection = projection;
        }

        // View matrix needed to transform world-space GBuffer normals to view-space
        void SetViewMatrix(const glm::mat4& view)
        {
            m_ViewMatrix = view;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            const bool denoiseReady = !m_Settings.GTAODenoiseEnabled ||
                                      m_Settings.GTAODenoisePasses <= 0 ||
                                      (m_DenoiseShader && m_DenoiseShader->IsValid());
            return m_GTAOShader && m_GTAOShader->IsValid() &&
                   m_HilbertLUT &&
                   m_Width > 0u && m_Height > 0u &&
                   denoiseReady;
        }
        [[nodiscard]] u32 GetWidth() const
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const
        {
            return m_Height;
        }

        // Expose HZB for future SSR
        [[nodiscard]] HZBGenerator& GetHZBGenerator()
        {
            return m_HZBGenerator;
        }
        [[nodiscard]] const HZBGenerator& GetHZBGenerator() const
        {
            return m_HZBGenerator;
        }

      private:
        void GenerateHilbertLUT();
        void UploadGTAOUniforms();
        void DispatchGTAO(u32 aoOutputTextureID, u32 normalsTextureID, u32 edgeTexID);
        void DispatchDenoise(u32 edgeTexID, u32 pingTextureID, u32 pongTextureID);

        HZBGenerator m_HZBGenerator;

        Ref<ComputeShader> m_GTAOShader;
        Ref<ComputeShader> m_DenoiseShader;

        Ref<Texture2D> m_HilbertLUT; // 64×64 R16UI Hilbert curve index

        Ref<UniformBuffer> m_GTAOUBO;
        UBOStructures::GTAOUBO* m_GPUData = nullptr;

        PostProcessSettings m_Settings;
        glm::mat4 m_Projection{ 1.0f };
        glm::mat4 m_ViewMatrix{ 1.0f };
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedSceneNormalsTexture{};
        RGTextureHandle m_SelectedAOOutputTexture{};
        RGTextureHandle m_SelectedEdgeTexture{};
        RGTextureHandle m_SelectedHZBDepthTexture{};
        RGTextureHandle m_SelectedDenoisePingTexture{};
        RGTextureHandle m_SelectedDenoisePongTexture{};

        u32 m_Width = 0;
        u32 m_Height = 0;
    };
} // namespace OloEngine
