#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Screen-Space Global Illumination (SSGI) post-process pass.
    //
    // Deferred-only stage inserted into the dynamic chain between AO apply and
    // SSR:
    //   AOApplyColor/SSSColor/SceneColor → SSGI → SSGIColor → SSR → Bloom → ...
    //
    // THREE DRAWS IN ONE NODE (issue #902), the CloudscapeRenderPass shape:
    //   A. PostProcess_SSGI.glsl          → SSGISignal   (the stochastic term
    //                                       ONLY: rgb = indirect diffuse,
    //                                       a = positive view depth)
    //   B. PostProcess_SSGIResolve.glsl   → SSGIResolved (temporal accumulation
    //                                       against SSGIHistory; the graph
    //                                       copies this into the history sink)
    //   C. PostProcess_SSGIComposite.glsl → SSGIColor    (upstream colour +
    //                                       resolved signal * intensity)
    //
    // The split is the whole point: compositing into the scene colour made the
    // output un-accumulable, because temporally blending it would smear the
    // base colour along with the noise. Draw B needs the current frame's signal
    // in a real texture (its 3x3 neighbourhood clip gathers neighbours that do
    // not exist yet inside draw A), which is why this is three draws and not
    // one shader with a history sample bolted on.
    //
    // The pass reads the lit scene colour plus the deferred G-Buffer (world
    // normal in RT1, albedo in RT0) and scene depth, then casts a
    // cosine-weighted hemisphere of short rays around each opaque pixel's view
    // normal and linear-marches them against scene depth. On a hit it samples the
    // upstream lit colour as incoming indirect radiance; the hemisphere mean,
    // tinted by the receiver albedo, is the one-bounce indirect diffuse. It is
    // ADDED to the lit colour (indirect diffuse is extra bounced light, not a
    // mirror substitution like SSR), scaled by the SSGI intensity, into a fresh
    // SSGIColor target (so the read/write of scene colour never aliases).
    //
    // Inputs:
    //   * Input framebuffer handle (AOApplyColor / SSSColor / SceneColor),
    //     selected during `Setup()` via the versioned name fallback.
    //   * Scene depth texture (for view-space position reconstruction + marching)
    //   * G-Buffer RT1 normal+roughness and RT0 albedo+metallic textures
    //   * SSGIUBO (binding 40), uploaded each frame by Renderer3D.
    //
    // Output:
    //   * SSGIColor (RGBA16F) — indirect-diffuse-composited scene colour.
    //   * SSGIResolved (RGBA16F, graph scratch) — extracted into SSGIHistory.
    //
    // Disabled / forward-path semantics: when the pass is disabled or the
    // G-Buffer is unavailable (forward / forward+), the graph omits SSGIColor so
    // downstream stages alias back to the upstream scene colour. There is no
    // runtime passthrough blit.
    class SSGIRenderPass : public RenderGraphNode
    {
      public:
        SSGIRenderPass();
        // Not defaulted: the pass owns the blue-noise tile texture (issue #706).
        ~SSGIRenderPass() override;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            // The UBO carries the camera matrices + ray params the shader needs;
            // executing without it would gather against stale/garbage state.
            return m_SSGIShader && m_SSGIShader->IsReady() &&
                   m_SSGIResolveShader && m_SSGIResolveShader->IsReady() &&
                   m_SSGICompositeShader && m_SSGICompositeShader->IsReady() && m_SSGIUBO;
        }

        void SetSSGIUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_SSGIUBO = ubo;
        }

        // Temporal-resolve knobs (issue #902). The *runtime* lanes — is there a
        // usable history this frame, is there a velocity buffer — are NOT set
        // here: only Execute knows whether those resources actually resolved,
        // and answering that from a snapshot taken before PopulateBlackboard ran
        // is how a first frame ends up blending against an uninitialised buffer.
        // Execute patches the TemporalParams vec4 in place before its draws.
        void SetTemporalSettings(bool enabled, f32 feedback, f32 clipGamma) noexcept
        {
            m_TemporalResolveEnabled = enabled;
            m_TemporalFeedback = feedback;
            m_TemporalClipGamma = clipGamma;
        }

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSGIShader;
        Ref<Shader> m_SSGIResolveShader;
        Ref<Shader> m_SSGICompositeShader;
        Ref<UniformBuffer> m_SSGIUBO;

        bool m_TemporalResolveEnabled = true;
        f32 m_TemporalFeedback = 0.92f;
        f32 m_TemporalClipGamma = 1.25f;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
        RGTextureHandle m_SelectedGBufferAlbedoTexture{};
        RGTextureHandle m_SelectedVelocityTexture{};
        RGTextureHandle m_SelectedHistoryTexture{};
        RGTextureHandle m_SelectedSurfaceHistoryTexture{};
        RGTextureHandle m_SelectedFirstMomentsHistoryTexture{};
        RGTextureHandle m_SelectedSecondMomentsHistoryTexture{};
        RGFramebufferHandle m_SelectedSignalFramebuffer{};
        RGFramebufferHandle m_SelectedResolvedFramebuffer{};

        // The shared blue-noise tile (issue #706), created once in Init(). Not a
        // graph resource: it is pass-owned, immutable and bound with a Persistent
        // heap lifetime.
        RHI::ResourceHandle m_BlueNoiseTexture{};
    };

} // namespace OloEngine
