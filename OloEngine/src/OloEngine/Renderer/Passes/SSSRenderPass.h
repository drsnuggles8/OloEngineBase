#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief The snow subsurface blur (issue #1451).
    //
    // Snow scatters light below its surface, so the DIFFUSE half of a snow
    // pixel's lighting is blurred across its neighbours. Every lit pass hands
    // that half over in scene attachment 4 as (diffuse, -snowWeight) — the same
    // lane skin uses with a disjoint value range, include/SnowDiffusionCommon.glsl
    // — and this pass adds `strength * (blur(diffuse) - diffuse)` into scene
    // colour, where strength = snowWeight * SnowSettings::SSSIntensity.
    //
    // Three properties follow from that shape, and the placement keeps them:
    //
    //   * ONLY SNOW MOVES. A pixel that hands over no snow weight adds exactly
    //     zero, so every other surface is byte-identical with the blur on or off.
    //     The mask used to be scene-colour alpha, which every non-snow writer set
    //     to 1, so the blur covered the whole frame.
    //   * THE SPECULAR STAYS SHARP. Sparkle and highlights are not in the
    //     diffuse half, so they are not smeared.
    //   * TRANSPARENTS ARE NOT SMEARED. Registered right after SkinDiffusionPass
    //     and before the OIT / particle band, like skin diffusion and for the
    //     same reason: glass or a particle in front of snow composites over the
    //     blurred snow instead of being blurred into it.
    //
    // It writes scene colour IN PLACE by an additive blend and owns no target.
    // Identical on Forward, Forward+ and Deferred: DeferredLighting hands the
    // snow half over through the same lane.
    class SSSRenderPass : public RenderGraphNode
    {
      public:
        SSSRenderPass();
        ~SSSRenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        // Setup() declares nothing unless snow and its blur are both on.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(m_Settings.Enabled && m_Settings.SSSBlurEnabled);
        }
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSettings(const SnowSettings& settings)
        {
            m_Settings = settings;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Settings.Enabled && m_Settings.SSSBlurEnabled;
        }
        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_SSSBlurShader && m_SSSBlurShader->IsReady();
        }
        void SetSSSUBO(Ref<UniformBuffer> ubo, SSSUBOData* gpuData)
        {
            m_SSSUBO = ubo;
            m_GPUData = gpuData;
        }

      private:
        Ref<Shader> m_SSSBlurShader;
        Ref<UniformBuffer> m_SSSUBO;
        SSSUBOData* m_GPUData = nullptr;
        RGTextureHandle m_HandoffTexture{};
        RGTextureHandle m_SceneDepthTexture{};
        RGFramebufferHandle m_SceneColorFramebuffer{};

        SnowSettings m_Settings;
    };
} // namespace OloEngine
