#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRDITechnique.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <array>

namespace OloEngine
{
    class EmissiveTriangleTable;
    class GPUScene;
    class MaterialTextureTable;

    namespace RayTracing
    {
        class RayTracingScene;
    }

    // @brief ReSTIR DI — many-light / emissive direct illumination (issue #1140,
    // #979 Phase 3). Four fullscreen ray-query draws, one node.
    //
    //   A. ReSTIR_DI_InitialSample.glsl -> ReSTIRDIInitial   (RIS over the whole
    //                                      emitter set at a uniform source
    //                                      density, plus one visibility ray on
    //                                      the survivor)
    //   B. ReSTIR_DI_TemporalReuse.glsl -> ReSTIRDITemporal  (merge last frame's
    //                                      reservoir, gated on the #976
    //                                      history-validity layer, then cap M)
    //   C. ReSTIR_DI_SpatialReuse.glsl  -> ReSTIRDISpatial0/1 (k neighbours with
    //                                      the shift Jacobian; ping-ponged so
    //                                      SpatialPasses can run it more than
    //                                      once)
    //   D. ReSTIR_DI_Resolve.glsl       -> ReSTIRDIRadiance  (one visibility ray
    //                                      on the final survivor, the debug
    //                                      views, and the variance moments)
    //
    // The split is the one #902 made for SSGI and #1056 for ray-traced shadows,
    // for the same reason: each draw gathers a neighbourhood that does not exist
    // inside the previous one, so the previous one's output has to be a real
    // texture.
    //
    // WHAT THIS IS, AND WHAT IT IS NOT. It is a TIER OVER THE SHARED SCENE, not
    // a second renderer. It consumes the same canonical instance / material /
    // light identities as raster and the path tracer (#977), the same versioned
    // closure (#975), the same TLAS (#978) and the same surface-history layer
    // (#976), and its light-sampling densities are the PATH TRACER'S, extracted
    // into include/LightSampling.glsl rather than re-derived — because the
    // oracle this tier is validated against and the tier itself must not
    // disagree about the measure, or the comparison proves nothing.
    //
    // WHERE IT SITS. After the last G-Buffer writer and after RayTracingScenePass
    // (a by-name edge: the acceleration structure is not a graph resource);
    // before DeferredLightingPass, which samples ReSTIRDIRadiance at
    // TEX_RESTIR_DI_RADIANCE INSTEAD of running its own punctual and area-light
    // loop. Clustered lighting, shadow maps, DDGI and SSGI all stay exactly
    // where they are — #979 is explicit that they are permanent tiers.
    //
    // THE FALLBACK IS STRUCTURAL, NOT A FLAG. The deferred lighting shader
    // branches on whether the radiance target exists, and every way this pass
    // can fail to produce one leaves it invalid, so the clustered branch is what
    // happens by default and the ReSTIR branch has to be actively earned. The
    // reason is counted in GetStats() and reported once per change
    // (docs/agent-rules/no-silent-fallbacks.md).
    //
    // ACCUMULATION goes through the temporal-history registry and NOTHING ELSE:
    // five RGBA32F planes under TemporalHistoryEffect::ReSTIRDI, extracted from
    // this pass's own attachments every frame. The reservoir planes carry a
    // LayoutVersion, so a packing change invalidates last frame's reservoirs
    // rather than reinterpreting them — reading a v1 reservoir as v2 is not a
    // crash, it is a plausible wrong image.
    //
    // DELIBERATELY NOT PARALLEL-RECORDED (#1013). Four ordered fullscreen draws
    // into four targets is already one writer per object per region, so the
    // conversion is available later; nothing here touches dispatcher or
    // heap-binding state.
    class ReSTIRDIPass : public RenderGraphNode
    {
      public:
        ReSTIRDIPass();
        ~ReSTIRDIPass() override = default;

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
            // The four shaders are only CREATED on a backend with hardware ray
            // tracing (GL_EXT_ray_query has no GL representation), so a null
            // here is the unsupported path rather than a load failure — which is
            // why this predicate must not assert.
            return m_InitialShader && m_InitialShader->IsReady() && m_TemporalShader &&
                   m_TemporalShader->IsReady() && m_SpatialShader && m_SpatialShader->IsReady() &&
                   m_ResolveShader && m_ResolveShader->IsReady() && m_ParamsUBO;
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
        // The SAME table the path tracer's NEE samples, so the two share the
        // emitter set and the 1/totalArea density rather than each building one.
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

        void SetSettings(const ReSTIRDISettings& settings) noexcept
        {
            m_Settings = SanitizeReSTIRDISettings(settings);
            // What the sanitizer had to change. Non-zero means the frame will do
            // LESS than the settings asked for, which is exactly the kind of
            // thing a user never discovers from the image.
            m_SettingsClamped = (settings == m_Settings) ? 0u : 1u;
        }
        [[nodiscard]] const ReSTIRDISettings& GetSettings() const noexcept
        {
            return m_Settings;
        }

        // `view` is the WORLD view matrix and `renderOrigin` the camera-relative
        // origin GPU Scene encoded this frame's transforms against (issue #429).
        // Both are needed for the reason RayTracedShadowPass documents at
        // length: the TLAS is built from render-relative transforms, so a ray
        // traced from an absolute world position misses by exactly the origin —
        // silently, and only once the camera is far enough out for the grid to
        // have snapped.
        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection,
                               const glm::mat4& previousViewProjection, const glm::vec3& renderOrigin) noexcept
        {
            m_View = view;
            m_Projection = projection;
            m_PreviousViewProjection = previousViewProjection;
            m_RenderOrigin = renderOrigin;
        }
        // The sampler decorrelator. A FIXED value makes a run reproducible,
        // which is what the oracle comparison needs.
        void SetFrameIndex(u32 frameIndex) noexcept
        {
            m_FrameIndex = frameIndex;
        }

        // Compute the verdict and fill the counters for this frame WITHOUT
        // executing, called from ConfigurePassesForFrame every frame whether or
        // not the graph goes on to declare a target.
        //
        // WHY IT IS NOT ENOUGH TO DO THIS IN Execute. A pass whose output the
        // graph never declared is CULLED, so Execute never runs — and that is
        // precisely the case on every machine without ray tracing, which is
        // every CI runner and the fallback arm generally. A tier that only
        // reported from Execute would answer "switched off in the render
        // settings" to a user who had just switched it on. Execute recomputes
        // with the real target availability and wins when the pass does run.
        void ResolveAvailabilityForFrame()
        {
            // TARGET AVAILABILITY IS PASSED AS TRUE HERE, and that is not a lie
            // by omission — it is the same rule GpuPathTracerPass states: "only
            // Execute can see whether the graph produced the target". Passing
            // false instead makes this call report TargetUnavailable on EVERY
            // frame, including the frames the tier is running perfectly, and
            // since it runs once per frame from the per-frame wiring it then
            // overwrites Execute's real verdict — so the counters read
            // "the graph produced no reservoir targets" while the reservoirs are
            // being built. Measured live before this comment existed.
            //
            // What is left is the set of guards that CAN be answered from here
            // (the setting, the shaders, the device, the TLAS, the GPU Scene, the
            // engagement criterion), and Execute overwrites with the truth when
            // it runs. When the pass is culled because a G-BUFFER input is
            // missing, this reports optimistically — and that case is exactly
            // what the graph-declaration warn in RenderPipeline covers, naming
            // the five inputs.
            (void)ResolveTechniqueForFrame(true);
        }

        [[nodiscard]] const ReSTIRDIStats& GetStats() const noexcept
        {
            return m_Stats;
        }

      private:
        // Decide the technique for this frame and fill the counters. Called
        // FIRST and unconditionally from Execute, so every exit below it has
        // already counted its reason — which is what makes "the radiance target
        // was not produced" and "the lighting shader took the clustered branch"
        // the same event rather than two that have to be kept in sync.
        [[nodiscard]] bool ResolveTechniqueForFrame(bool graphResourcesResolved);

        bool m_Enabled = false;
        ReSTIRDISettings m_Settings{};
        ReSTIRDIStats m_Stats{};
        u32 m_SettingsClamped = 0;

        Ref<Shader> m_InitialShader;
        Ref<Shader> m_TemporalShader;
        Ref<Shader> m_SpatialShader;
        Ref<Shader> m_ResolveShader;
        Ref<UniformBuffer> m_ParamsUBO;

        const RayTracing::RayTracingScene* m_RayTracingScene = nullptr;
        const GPUScene* m_GPUScene = nullptr;
        const EmissiveTriangleTable* m_EmissiveTable = nullptr;
        const MaterialTextureTable* m_MaterialTextures = nullptr;

        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
        glm::mat4 m_PreviousViewProjection{ 1.0f };
        glm::vec3 m_RenderOrigin{ 0.0f };
        u32 m_FrameIndex = 0;

        FramebufferSpecification m_FramebufferSpec{};

        // Selected in Setup, resolved in Execute.
        RGTextureHandle m_SelectedSceneDepth{};
        RGTextureHandle m_SelectedGBufferAlbedo{};
        RGTextureHandle m_SelectedGBufferNormal{};
        RGTextureHandle m_SelectedGBufferEmissive{};
        RGTextureHandle m_SelectedVelocity{};
        RGFramebufferHandle m_SelectedInitial{};
        RGFramebufferHandle m_SelectedTemporal{};
        std::array<RGFramebufferHandle, 2> m_SelectedSpatial{};
        RGTextureHandle m_SelectedReservoirSampleHistory{};
        RGTextureHandle m_SelectedReservoirRadianceHistory{};
        RGTextureHandle m_SelectedReservoirStateHistory{};
        RGTextureHandle m_SelectedSurfaceHistory{};
        RGTextureHandle m_SelectedMomentsHistory{};

        // Reported once per CHANGE, never once per frame: this is the line that
        // tells a user why their thousand-light scene is still clustered, and a
        // per-frame version would be indistinguishable from spam.
        ReSTIRDIFallbackReason m_LastReportedFallback = ReSTIRDIFallbackReason::None;
    };
} // namespace OloEngine
