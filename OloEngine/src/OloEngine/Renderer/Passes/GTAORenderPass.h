#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/HZBGenerator.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/VRCS/ShadingRateClassifier.h"

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
        bool SupportsWholePassRecording() const noexcept override
        {
            return true;
        }
        RGPreparedPass PrepareParallelRecording(RGCommandContext& context) override;
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

        // VRCS classifier (issue #683). Owned here for the same reason
        // HZBGenerator is: it is a self-contained GPU utility, GTAO is its
        // first consumer, and the HZB precedent shows a second consumer can
        // share one instance without either pass owning a graph node. Exposed
        // so the next adopter (and VRCSClassifierGpuTest) can reach the same
        // classification rather than dispatching a second one.
        [[nodiscard]] ShadingRateClassifier& GetShadingRateClassifier()
        {
            return m_ShadingRateClassifier;
        }
        [[nodiscard]] const ShadingRateClassifier& GetShadingRateClassifier() const
        {
            return m_ShadingRateClassifier;
        }

        // True when the LAST Execute both classified and consumed shading
        // rates. Reports what actually happened, not what the settings asked
        // for — the two differ whenever classification declined (no depth or
        // normals, a shader that failed to compile, a zero-sized viewport).
        [[nodiscard]] bool WasVariableRateActive() const noexcept
        {
            return m_VRCSActiveLastExecute;
        }

      private:
        struct RecordingInputs
        {
            RHI::ResourceHandle Depth, Normals, PreviousColor, AO, Edge, Ping, Pong;
            bool Denoise = false;
            UBOStructures::GTAOUBO GPUData;
        };
        void RecordPrepared(RecordingInputs& inputs);
        void GenerateHilbertLUT();
        // Clear the AO target to "fully visible" (1.0). The identity for a
        // frame GTAO could not produce; a zero-initialised transient reads as
        // FULL occlusion and multiplies the scene to black (issue #771).
        // `skipReason` non-null also emits a rate-limited warning naming the
        // early return that fired; pass nullptr for the routine seeding case.
        static void PublishNoOcclusion(RHI::ResourceHandle aoOutputTexture, const char* skipReason);
        static RGPreparedPass PrepareNoOcclusion(RHI::ResourceHandle texture, const char* reason);
        void UploadGTAOUniforms(UBOStructures::GTAOUBO& data, UniformBuffer& upload);
        void DispatchGTAO(RHI::ResourceHandle aoOutputTexture, RHI::ResourceHandle normalsTexture,
                          RHI::ResourceHandle edgeTexture);
        void DispatchDenoise(RHI::ResourceHandle edgeTexture, RHI::ResourceHandle pingTexture,
                             RHI::ResourceHandle pongTexture);

        HZBGenerator m_HZBGenerator;
        ShadingRateClassifier m_ShadingRateClassifier;

        Ref<ComputeShader> m_GTAOShader;
        Ref<ComputeShader> m_DenoiseShader;
        // Per-ping-pong-pass blur axis; allocated by primary preparation.
        // Each dispatch uploads a new private version (issue #691).
        Ref<UniformBuffer> m_DenoiseUBO;

        Ref<Texture2D> m_HilbertLUT; // 64×64 R16UI Hilbert curve index

        Ref<UniformBuffer> m_GTAOUBO;
        Ref<UniformBuffer> m_RecordingGTAOUBO;
        UBOStructures::GTAOUBO* m_GPUData = nullptr;

        PostProcessSettings m_Settings;
        glm::mat4 m_Projection{ 1.0f };
        glm::mat4 m_ViewMatrix{ 1.0f };
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedSceneNormalsTexture{};
        // True when m_SelectedSceneNormalsTexture holds VIEW-space normals (the
        // forward path) rather than the world-space G-Buffer normals the deferred
        // path supplies. GTAO.comp converts with u_ViewMatrix, so this decides
        // whether that conversion runs or is replaced by identity.
        bool m_SceneNormalsAreViewSpace = false;
        RGTextureHandle m_SelectedAOOutputTexture{};
        RGTextureHandle m_SelectedEdgeTexture{};
        RGTextureHandle m_SelectedHZBDepthTexture{};
        RGTextureHandle m_SelectedDenoisePingTexture{};
        RGTextureHandle m_SelectedDenoisePongTexture{};
        // Previous frame's resolved colour, for the VRCS luminance term
        // (issue #683). The TAA history is the engine's only full-screen
        // previous-frame colour, so with TAA off this stays invalid and
        // classification simply drops that signal — which makes it more
        // conservative, never less.
        RGTextureHandle m_SelectedPreviousColorTexture{};

        // Monotonic Execute counter, used only as the classifier's
        // once-per-frame stamp. NOT m_GPUData->NoiseIndex, which wraps at 256
        // and would make the classifier skip a dispatch every 256th frame.
        //
        // GTAO is currently the only VRCS consumer, so a per-pass counter is a
        // faithful frame stamp. WHEN A SECOND PASS ADOPTS VRCS this must become
        // a renderer-wide frame index, or the two passes will disagree about
        // which frame it is and each will re-dispatch classification — the
        // exact duplication the stamp exists to prevent. There is no such
        // engine-wide monotonic counter today (InflightFrameManager's index is
        // a ring position, not a frame number).
        u64 m_FrameCounter = 0;
        bool m_VRCSActiveLastExecute = false;

        u32 m_Width = 0;
        u32 m_Height = 0;
        // Band size the last completed Execute ran at. A mismatch against
        // m_Width/m_Height is the structural event (issue #771): the scratch
        // targets were resized and the transient pool evicted, so this frame's
        // AO chain is on freshly allocated — and therefore unspecified —
        // storage. Deliberately NOT reset by OnReset(): that hook has no call
        // sites (see docs/agent-rules/render-pipeline-caches.md), and a stale
        // value here only costs one extra clear.
        u32 m_LastExecutedWidth = 0;
        u32 m_LastExecutedHeight = 0;
    };
} // namespace OloEngine
