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
    // FIVE DRAWS IN ONE NODE (issues #902, #708), the CloudscapeRenderPass shape.
    // Draws A-D run at the TRACE band — half the scene band when
    // SSGIHalfResolution is on — and only draw E is full resolution:
    //   A. PostProcess_SSGI.glsl          → SSGISignal   (the stochastic term
    //                                       ONLY: rgb = indirect diffuse,
    //                                       a = positive view depth; RT1 = the
    //                                       trace-band guide plane)
    //   B. PostProcess_SSGIPreBlur.glsl   → SSGIPreBlurred (small depth+normal
    //                                       guided blur, so the history has
    //                                       something stable to accumulate)
    //   C. PostProcess_SSGIResolve.glsl   → SSGIResolved (temporal accumulation
    //                                       against SSGIHistory; the graph
    //                                       copies this into the history sink)
    //   D. PostProcess_SSGIPostBlur.glsl  → SSGIDenoised (variance- and
    //                                       history-length-guided radius: wide
    //                                       where still noisy, narrow where
    //                                       converged)
    //   E. PostProcess_SSGIComposite.glsl → SSGIColor    (guided upscale of the
    //                                       denoised signal onto the full-res
    //                                       surface, plus upstream colour +
    //                                       signal * intensity)
    //
    // The ordering is the one denoisinator.md argues for: a cheap spatial pass
    // BEFORE the temporal one and a small one after, rather than either alone.
    // Pre-blur alone shimmers in motion; temporal alone reacts slowly and needs
    // one expensive wide filter to hide it. Doing both lets each be small.
    //
    // B and D are skipped entirely when their radius is 0, and the draw that
    // would have consumed their output binds the previous stage's instead —
    // so turning a stage off removes its cost rather than running a
    // pass-through. The graph shape does not change, which is what keeps the
    // radii out of the blackboard fingerprint.
    //
    // The split into separate draws is the whole point: compositing into the
    // scene colour made the output un-accumulable, because temporally blending
    // it would smear the base colour along with the noise. And every stage here
    // gathers a NEIGHBOURHOOD of the stage before it — the pre-blur's Poisson
    // disc, the resolve's 3x3 clip box, the post-blur's disc, the upscale's 2x2
    // footprint — so each one needs its input in a real texture that the whole
    // previous draw has finished writing. None of these can be folded into the
    // one before it.
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
    //   * SSGISignal / SSGIPreBlurred / SSGIDenoised (RGBA16F, graph scratch at
    //     the trace band) — the denoiser chain's intermediates. SSGISignal's
    //     attachment 1 is the guide plane extracted into SSGISurfaceHistory.
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
                   m_SSGIPreBlurShader && m_SSGIPreBlurShader->IsReady() &&
                   m_SSGIResolveShader && m_SSGIResolveShader->IsReady() &&
                   m_SSGIPostBlurShader && m_SSGIPostBlurShader->IsReady() &&
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

        // Denoiser-stage gates (issue #708). Only whether the two spatial
        // stages run at all: their radii travel in the UBO, which the pipeline
        // fills. A stage whose radius is 0 is SKIPPED, and its consumer binds
        // the previous stage's output instead — see Execute.
        void SetDenoiseSettings(bool preBlurEnabled, bool postBlurEnabled) noexcept
        {
            m_PreBlurEnabled = preBlurEnabled;
            m_PostBlurEnabled = postBlurEnabled;
        }

      private:
        bool m_Enabled = false;

        Ref<Shader> m_SSGIShader;
        Ref<Shader> m_SSGIPreBlurShader;
        Ref<Shader> m_SSGIResolveShader;
        Ref<Shader> m_SSGIPostBlurShader;
        Ref<Shader> m_SSGICompositeShader;
        Ref<UniformBuffer> m_SSGIUBO;

        bool m_PreBlurEnabled = true;
        bool m_PostBlurEnabled = true;

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
        RGFramebufferHandle m_SelectedPreBlurredFramebuffer{};
        RGFramebufferHandle m_SelectedResolvedFramebuffer{};
        RGFramebufferHandle m_SelectedDenoisedFramebuffer{};

        // The shared blue-noise tile (issue #706), created once in Init(). Not a
        // graph resource: it is pass-owned, immutable and bound with a Persistent
        // heap lifetime.
        RHI::ResourceHandle m_BlueNoiseTexture{};
    };

} // namespace OloEngine
