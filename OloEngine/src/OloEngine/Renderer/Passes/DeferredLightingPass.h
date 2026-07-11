#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>
#include <glm/glm.hpp>

namespace OloEngine
{
    // @brief Deferred lighting composition pass.
    //
    // Reads a 4-RT G-Buffer and writes fully-lit scene colour into the
    // forward scene framebuffer's colour attachment 0. This keeps
    // downstream passes (PostProcess, Selection Outline, UIComposite)
    // oblivious to the rendering path.
    //
    // Current iteration supports directional / point / spot lights via
    // the `MultiLightBuffer` UBO; shadow sampling, IBL, Forward+ tile
    // evaluation and per-sample MSAA lighting are Phase 3/5 follow-ups.
    //
    // If `RenderingPath::Deferred` is inactive or the G-Buffer was not
    // provided, Execute() is a no-op — the pass is safe to register
    // unconditionally in the render graph.
    class DeferredLightingPass : public RenderGraphNode
    {
      public:
        DeferredLightingPass();
        ~DeferredLightingPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        // The G-Buffer is provided by SceneRenderPass each frame; null
        // turns the pass into a no-op.
        void SetGBuffer(const Ref<GBuffer>& gbuffer) noexcept
        {
            m_GBuffer = gbuffer;
        }
        // Phase F slice 41 — SetSceneColorHandle removed; Execute() self-resolves
        // Forwarded from RendererSettings; 0 = lighting, non-zero = skip
        // (SceneRenderPass's BlitGBufferDebug already wrote a channel).
        void SetDebugChannel(u32 channel) noexcept
        {
            m_DebugChannel = channel;
        }

        // Controls per-sample MSAA shading. When true (and GBuffer sample
        // count > 1), Execute() binds the multisample G-Buffer attachments
        // and uses the sampler2DMS shader variant; otherwise it samples
        // the resolved single-sample copy via the default shader.
        void SetPerSampleLighting(bool enable) noexcept
        {
            m_PerSampleLighting = enable;
        }

      private:
        struct SelectedInputs
        {
            RGTextureHandle GBufferAlbedo;
            RGTextureHandle GBufferNormal;
            RGTextureHandle GBufferEmissive;
            RGTextureHandle Velocity;
            RGTextureHandle SceneDepth;
            RGTextureHandle AOBuffer;
            RGTextureHandle ShadowMapCSM;
            RGTextureHandle ShadowMapAtlas; // local-light shadow atlas (issue #435)
            // Comparison-OFF raw-depth view GL ids (PCSS blocker search); 0 = none.
            u32 ShadowMapCSMRawID = 0;
            u32 ShadowMapAtlasRawID = 0;
            RGTextureHandle IrradianceMap;
            RGTextureHandle PrefilterMap;
            RGTextureHandle BrdfLut;
        };

        Ref<Shader> m_Shader;     // sampler2D variant (non-MSAA / resolved)
        Ref<Shader> m_ShaderMSAA; // sampler2DMS variant (per-sample)
        Ref<GBuffer> m_GBuffer;
        Ref<Framebuffer> m_SceneFramebuffer;
        Ref<UniformBuffer> m_ControlsUBO;
        SelectedInputs m_SelectedInputs{};
        u32 m_DebugChannel = 0;
        bool m_PerSampleLighting = true;
        bool m_UseMSAAShading = false;
    };
} // namespace OloEngine
