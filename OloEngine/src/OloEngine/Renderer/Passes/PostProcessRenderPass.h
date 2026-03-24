#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Render pass that applies a chain of post-processing effects.
    //
    // This pass sits between the ParticlePass (scene rendering) and FinalPass (screen blit).
    // It uses two ping-pong RGBA16F framebuffers to chain effects. When no effects are
    // enabled it acts as a passthrough (GetTarget returns the input framebuffer).
    class PostProcessRenderPass : public RenderPass
    {
      public:
        PostProcessRenderPass();
        ~PostProcessRenderPass() override = default;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetInputFramebuffer(const Ref<Framebuffer>& input) override;
        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }
        void SetSceneDepthFramebuffer(const Ref<Framebuffer>& sceneFB)
        {
            m_SceneDepthFB = sceneFB;
        }
        void SetPostProcessUBO(Ref<UniformBuffer> ubo, PostProcessUBOData* gpuData)
        {
            m_PostProcessUBO = ubo;
            m_GPUData = gpuData;
        }
        void SetSSAOTexture(u32 textureID)
        {
            m_SSAOTextureID = textureID;
        }
        void SetFogEnabled(bool enabled)
        {
            m_FogEnabled = enabled;
        }
        void SetShadowMapCSMTextureID(u32 textureID)
        {
            m_ShadowMapCSMTextureID = textureID;
        }
        void SetPrecipitationScreenEffectsEnabled(bool enabled)
        {
            m_PrecipitationScreenEffectsEnabled = enabled;
        }

        // Hot-reload a post-process shader by name (stem, e.g. "PostProcess_Bloom_Threshold")
        void ReloadShader(const std::string& name);

      private:
        void CreatePingPongFramebuffers(u32 width, u32 height);
        void CreateBloomMipChain(u32 width, u32 height);
        void ExecuteBloom(Ref<Framebuffer> sceneColorFB);
        void DrawFullscreenTriangle();

        // Runs a single fullscreen shader pass: binds srcFB color 0 as texture, renders to dstFB
        void RunEffect(const Ref<Shader>& shader, Ref<Framebuffer> srcFB, Ref<Framebuffer> dstFB);

        Ref<Framebuffer> m_InputFramebuffer;
        Ref<Framebuffer> m_PingFB;
        Ref<Framebuffer> m_PongFB;
        Ref<Framebuffer> m_SceneDepthFB; // For DOF / Motion Blur depth access

        // Shaders for each effect (loaded once in Init)
        Ref<Shader> m_BloomThresholdShader;
        Ref<Shader> m_BloomDownsampleShader;
        Ref<Shader> m_BloomUpsampleShader;
        Ref<Shader> m_BloomCompositeShader;
        Ref<Shader> m_VignetteShader;
        Ref<Shader> m_ChromaticAberrationShader;
        Ref<Shader> m_ColorGradingShader;
        Ref<Shader> m_ToneMapShader;
        Ref<Shader> m_FXAAShader;
        Ref<Shader> m_DOFShader;
        Ref<Shader> m_MotionBlurShader;
        Ref<Shader> m_FogShader;
        Ref<Shader> m_SSAOApplyShader;
        Ref<Shader> m_PrecipitationShader;

        PostProcessSettings m_Settings;

        // UBO reference for per-pass updates (texel size, camera near/far)
        Ref<UniformBuffer> m_PostProcessUBO;
        PostProcessUBOData* m_GPUData = nullptr;

        u32 m_SSAOTextureID = 0;
        u32 m_ShadowMapCSMTextureID = 0;
        bool m_FogEnabled = false;
        bool m_PrecipitationScreenEffectsEnabled = false;
        Ref<UniformBuffer> m_PrecipitationScreenUBO;

        // Half-resolution volumetric fog framebuffers
        Ref<Framebuffer> m_FogHalfResFB; // RGBA16F: RGB = inscatter, A = transmittance
        Ref<Framebuffer> m_FogHistoryFB; // Temporal reprojection history
        u32 m_FogHalfWidth = 0;
        u32 m_FogHalfHeight = 0;
        Ref<Shader> m_FogUpsampleShader; // Bilateral upsample + composite

        // Bloom mip chain framebuffers (RGBA16F, progressively smaller)
        static constexpr u32 MAX_BLOOM_MIPS = 5;
        std::vector<Ref<Framebuffer>> m_BloomMipChain;

        // Tracks which ping-pong buffer was last written to
        bool m_LastWrittenIsPing = true;
        // Tracks if Execute() skipped all processing (passthrough to input FB)
        bool m_SkippedThisFrame = false;
    };
} // namespace OloEngine
