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
    // It uses two ping-pong RGBA16F framebuffers to chain effects, then resolves
    // the final color into a stable output framebuffer for downstream passes.
    class PostProcessRenderPass : public RenderPass
    {
      public:
        PostProcessRenderPass();
        ~PostProcessRenderPass() override = default;

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
        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }
        void SetSceneDepthTextureHandle(RGTextureHandle handle)
        {
            m_SceneDepthHandle = handle;
        }
        void SetPostProcessUBO(Ref<UniformBuffer> ubo, PostProcessUBOData* gpuData)
        {
            m_PostProcessUBO = ubo;
            m_GPUData = gpuData;
        }
        void SetAOTextureHandle(RGTextureHandle handle)
        {
            m_AOTextureHandle = handle;
        }
        void SetAOTextureID(u32 textureID)
        {
            m_AOTextureID = textureID;
        }
        void SetFogEnabled(bool enabled)
        {
            m_FogEnabled = enabled;
        }
        void SetShadowMapCSMHandle(RGTextureHandle handle)
        {
            m_ShadowMapCSMHandle = handle;
        }
        void SetPrecipitationScreenEffectsEnabled(bool enabled)
        {
            m_PrecipitationScreenEffectsEnabled = enabled;
        }
        // Set the G-Buffer velocity texture (RT3) — pass 0 in Forward /
        // Forward+ to fall back to camera-only depth reprojection.
        void SetVelocityTextureHandle(RGTextureHandle handle)
        {
            m_VelocityTextureHandle = handle;
        }

        // Hot-reload a post-process shader by name (stem, e.g. "PostProcess_BloomThreshold")
        void ReloadShader(const std::string& name);

        // -------------------------------------------------------------------
        // Phase B — history accessors for FrameBlackboard population
        // -------------------------------------------------------------------
        // Returns the GL texture ID of the previous TAA accumulation result
        // (the buffer that will be sampled as "history" in the current frame).
        // Returns 0 if no valid history has been produced yet (first frame /
        // after a resize / after OnReset).
        [[nodiscard]] u32 GetTAAHistoryTextureID() const
        {
            if (!m_TAAHistoryValid || !m_TAAHistoryFB)
                return 0;
            return m_TAAHistoryFB->GetColorAttachmentRendererID(0);
        }

        // Returns the GL texture ID of the previous fog integration result.
        // Returns 0 when no fog history is available.
        [[nodiscard]] u32 GetFogHistoryTextureID() const
        {
            if (!m_FogHistoryFB)
                return 0;
            return m_FogHistoryFB->GetColorAttachmentRendererID(0);
        }

      private:
        void CreatePingPongFramebuffers(u32 width, u32 height);
        void CreateBloomMipChain(u32 width, u32 height);
        void ExecuteBloom(Ref<Framebuffer> sceneColorFB);
        void ResolveToOutput(const Ref<Framebuffer>& sourceFB);
        void DrawFullscreenTriangle();
        std::vector<Ref<Shader>*> GetAllShaderRefs();

        // Runs a single fullscreen shader pass: binds srcFB color 0 as texture, renders to dstFB
        void RunEffect(const Ref<Shader>& shader, Ref<Framebuffer> srcFB, Ref<Framebuffer> dstFB);

        Ref<Framebuffer> m_InputFramebuffer;
        RGFramebufferHandle m_InputFramebufferHandle;
        Ref<Framebuffer> m_PingFB;
        Ref<Framebuffer> m_PongFB;
        Ref<Framebuffer> m_OutputFB;

        // Graph-resolved per-frame inputs
        RGTextureHandle m_SceneDepthHandle;
        RGTextureHandle m_AOTextureHandle;
        RGTextureHandle m_ShadowMapCSMHandle;
        RGTextureHandle m_VelocityTextureHandle;
        u32 m_AOTextureID = 0;

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
        Ref<Shader> m_TAAShader;
        Ref<Shader> m_FogShader;
        Ref<Shader> m_SSAOApplyShader;
        Ref<Shader> m_PrecipitationShader;

        PostProcessSettings m_Settings;

        // UBO reference for per-pass updates (texel size, camera near/far)
        Ref<UniformBuffer> m_PostProcessUBO;
        PostProcessUBOData* m_GPUData = nullptr;
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

        // TAA history framebuffer (persists across frames). RGBA16F full-res.
        // After each TAA resolve we blit the resolved colour into this FB so
        // the next frame's TAA pass can sample it as "previous".
        Ref<Framebuffer> m_TAAHistoryFB;
        bool m_TAAHistoryValid = false; // false on first frame / after resize
        Ref<UniformBuffer> m_TAAUBO;    // binding 32 (TAAParams)
    };
} // namespace OloEngine
