#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PathTracing/GpuPathTracerTypes.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/TemporalHistoryRegistry.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <optional>

namespace OloEngine
{
    class EmissiveTriangleTable;
    class GPUScene;
    class MaterialTextureTable;

    namespace RayTracing
    {
        class RayTracingScene;
    }

    // @brief The GPU reference path tracer (issue #1055, #979 Phase 2). One
    // fullscreen ray-query draw, one node.
    //
    // WHAT IT IS FOR. The oracle everything downstream of #979 Phase 2 is
    // validated against — ReSTIR, the hybrid tiers — and the first in-frame
    // consumer of the #978 acceleration structures. It replaces the rasterised
    // scene colour with a progressively converging path-traced image; it is
    // not a shipping quality tier and makes no attempt to be cheap.
    //
    // WHERE IT SITS. After the screen-space chain (ContactShadow) and before
    // the upscalers and Bloom: its colour is the freshest pre-Bloom colour, so
    // the alias chain in RenderPipeline::PopulateBlackboard ranks it above
    // everything the rasteriser produced, and every post stage downstream
    // consumes the traced image with no further edits. The raster passes
    // still run — a reference mode that wanted to skip them would have to
    // change the graph's topology, which is a separate decision.
    //
    // ACCUMULATION goes through the temporal-history registry and NOTHING
    // ELSE: four RGBA32F planes under TemporalHistoryEffect::PathTracer,
    // extracted from this pass's own attachments every frame and handed back
    // as the previous frame's sums. Invalidation is the registry's — a camera
    // cut, a projection change, a resize, a scene reset — plus the two causes
    // this pass introduces: the camera MOVING (detected bitwise in
    // SetCameraMatrices; TAA reprojects, a path tracer cannot) and the GPU
    // Scene MUTATING (a dirty record after commit, detected in EndScene). The
    // sample index each pixel draws next is its accumulated count, so a run at
    // a fixed seed and sample cap is reproducible whatever frame it began on.
    //
    // THE FALLBACK IS STRUCTURAL. Every way this pass can fail to trace leaves
    // the upstream colour in place: the shader's first guard is a zero TLAS
    // address, uploaded deliberately when the tracer stands down, and every
    // early-out passes the input through. The reason is counted in GetStats()
    // and reported once per change (docs/agent-rules/no-silent-fallbacks.md).
    //
    // TEXTURES come through MaterialTextureTable and the descriptor heap the
    // shader indexes itself (ADR 0011 amendment (95)); where the backend
    // cannot index it, hits shade from the factors and masked geometry traces
    // as solid, both counted. Sphere-area lights are spherical emitters on
    // both tracers (PathTracer.cpp's ViewSphereLight), and each hit is shaded
    // with its material's own closure (Legacy or ClosureV2), as the CPU
    // reference dispatches.
    class GpuPathTracerPass : public RenderGraphNode
    {
      public:
        GpuPathTracerPass();
        ~GpuPathTracerPass() override = default;

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
            // The shader is only CREATED on a backend with hardware ray tracing
            // (GL_EXT_ray_query has no GL representation), so a null here is the
            // unsupported path rather than a load failure — which is why this
            // predicate must not assert.
            return m_Shader && m_Shader->IsReady() && m_ParamsUBO;
        }

        // Wired at renderer init. All borrowed, never owned.
        void SetRayTracingScene(const RayTracing::RayTracingScene* scene) noexcept
        {
            m_RayTracingScene = scene;
        }
        void SetGPUScene(const GPUScene* gpuScene) noexcept
        {
            m_GPUScene = gpuScene;
        }
        void SetEmissiveTable(const EmissiveTriangleTable* table) noexcept
        {
            m_EmissiveTable = table;
        }
        void SetMaterialTextureTable(const MaterialTextureTable* table) noexcept
        {
            m_MaterialTextures = table;
        }
        void SetParamsUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_ParamsUBO = ubo;
        }

        // Returns true when the change invalidates the accumulation — every
        // knob that changes the integral or the sample sequence, none of the
        // display-only ones. Pure, so a test can pin the list.
        [[nodiscard]] static bool SettingsChangeInvalidatesAccumulation(const GpuPathTracerSettings& previous,
                                                                        const GpuPathTracerSettings& next) noexcept;
        void SetSettings(const GpuPathTracerSettings& settings) noexcept;

        // The WORLD-space camera matrices plus the render origin this frame's
        // GPU Scene transforms were encoded against. The pass converts to the
        // render-relative frame the TLAS is built in before uploading. Use the
        // UNJITTERED projection: TAA jitter would read as a moving camera and
        // restart the accumulation every frame.
        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection,
                               const glm::vec3& renderOrigin) noexcept;

        // The cause that restarts the accumulation this frame — CameraCut for
        // a changed pose, FeatureToggled for an integral-changing setting —
        // or nothing. Consumed once; the caller invalidates the registry with
        // it BEFORE the blackboard acquires this frame's histories.
        [[nodiscard]] std::optional<TemporalHistoryInvalidationCause> ConsumeAccumulationRestartRequest() noexcept
        {
            const auto pending = m_RestartCause;
            m_RestartCause.reset();
            return pending;
        }

        // The shader loaded, i.e. this backend can trace at all. The gather of
        // the emissive table keys on this rather than on the device query: a
        // shader that failed to compile, or OLO_VULKAN_NO_RAY_TRACING=1 on a
        // capable device, would otherwise build a table nobody reads.
        [[nodiscard]] bool IsShaderLoaded() const noexcept
        {
            return m_Shader != nullptr;
        }

        // Decide whether the tracer runs this frame and fill the fallback
        // half of GetStats(). Called from the per-frame wiring AFTER the GPU
        // Scene commit and the emissive gather, every frame, so the verdict
        // and the counters are fresh whether or not the graph culls the pass
        // — the one reason Execute alone can see (no target) is added there.
        void ResolveAvailabilityForFrame();

        [[nodiscard]] const GpuPathTracerStats& GetStats() const noexcept
        {
            return m_Stats;
        }

      private:
        // Sets the verdict in m_Stats and logs it once per change.
        void ReportFallback(GpuPathTracerFallbackReason reason);
        void CountSceneForStats();
        // Logs whether hits shade from textures, once per change, with why not.
        void ReportTextureAvailability();

        bool m_Enabled = false;

        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_ParamsUBO;

        const RayTracing::RayTracingScene* m_RayTracingScene = nullptr;
        const GPUScene* m_GPUScene = nullptr;
        const EmissiveTriangleTable* m_EmissiveTable = nullptr;
        const MaterialTextureTable* m_MaterialTextures = nullptr;

        GpuPathTracerSettings m_Settings{};
        bool m_HasSettings = false;
        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
        glm::vec3 m_RenderOrigin{ 0.0f };
        bool m_HasCamera = false;
        std::optional<TemporalHistoryInvalidationCause> m_RestartCause;

        GpuPathTracerStats m_Stats{};
        // The accumulation state, kept OUTSIDE m_Stats because that is reset
        // every frame: the per-pixel count the history carries (uniform across
        // the image by construction) and how many frames in a row it restarted.
        u32 m_AccumulatedSamplesPerPixel = 0;
        u32 m_ConsecutiveRestarts = 0;
        // Seeded to Count — a value no verdict can equal — so the FIRST verdict
        // of the process is always reported (the reflection tier's rule).
        GpuPathTracerFallbackReason m_LastReportedFallback = GpuPathTracerFallbackReason::Count;
        bool m_ReportedLightsBeyondShaderBound = false;
        bool m_ReportedEmissiveTableUnaddressable = false;
        std::optional<bool> m_ReportedTexturesAvailable;

        RGTextureHandle m_SelectedHistoryTexture{};
        RGTextureHandle m_SelectedMomentsHistoryTexture{};
        RGTextureHandle m_SelectedAlbedoHistoryTexture{};
        RGTextureHandle m_SelectedNormalHistoryTexture{};
        RGTextureHandle m_SelectedPrefilterTexture{};
    };

} // namespace OloEngine
