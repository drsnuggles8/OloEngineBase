#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ComputeShader.h"
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
    // Final output is an R8 AO texture readable at GetGTAOTextureID().
    // Coexists with SSAORenderPass behind an AOTechnique selector.
    class GTAORenderPass : public RenderPass
    {
      public:
        GTAORenderPass();
        ~GTAORenderPass() override;

        void Init(const FramebufferSpecification& spec) override;
        void Execute() override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSceneFramebuffer(const Ref<Framebuffer>& sceneFB) { m_SceneFramebuffer = sceneFB; }
        void SetSettings(const PostProcessSettings& settings) { m_Settings = settings; }
        void SetGTAOUBO(Ref<UniformBuffer> ubo, UBOStructures::GTAOUBO* gpuData)
        {
            m_GTAOUBO = ubo;
            m_GPUData = gpuData;
        }

        // Projection matrix needed for NDCToView calculations
        void SetProjectionMatrix(const glm::mat4& projection) { m_Projection = projection; }

        [[nodiscard]] u32 GetGTAOTextureID() const;

        // Expose HZB for future SSR
        [[nodiscard]] HZBGenerator& GetHZBGenerator() { return m_HZBGenerator; }
        [[nodiscard]] const HZBGenerator& GetHZBGenerator() const { return m_HZBGenerator; }

      private:
        void CreateGTAOTextures(u32 width, u32 height);
        void GenerateHilbertLUT();
        void UploadGTAOUniforms();
        void DispatchGTAO();
        void DispatchDenoise();

        Ref<Framebuffer> m_SceneFramebuffer;
        HZBGenerator m_HZBGenerator;

        Ref<ComputeShader> m_GTAOShader;
        Ref<ComputeShader> m_DenoiseShader;

        // AO textures: ping-pong pair for denoise + edge texture
        Ref<Texture2D> m_AOTexture0;  // Primary AO output / denoise ping
        Ref<Texture2D> m_AOTexture1;  // Denoise pong
        Ref<Texture2D> m_EdgeTexture; // Edge weights for bilateral blur
        Ref<Texture2D> m_HilbertLUT;  // 64×64 R16UI Hilbert curve index

        Ref<UniformBuffer> m_GTAOUBO;
        UBOStructures::GTAOUBO* m_GPUData = nullptr;

        PostProcessSettings m_Settings;
        glm::mat4 m_Projection{ 1.0f };

        u32 m_Width = 0;
        u32 m_Height = 0;
    };
} // namespace OloEngine
