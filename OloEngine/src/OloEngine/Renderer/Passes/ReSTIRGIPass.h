#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRGITechnique.h"
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

    // @brief ReSTIR GI — constrained one-bounce diffuse path reuse (issue #1169,
    // #979 Phase 3, second half). Four fullscreen ray-query draws, one node.
    //
    //   A. ReSTIR_GI_InitialSample.glsl -> ReSTIRGIInitial  (cosine-hemisphere
    //                                      bounces; each vertex shaded with its
    //                                      DIFFUSE lobe from one NEE draw plus
    //                                      the probe cache read AT THAT VERTEX)
    //   B. ReSTIR_GI_TemporalReuse.glsl -> ReSTIRGITemporal (merge last frame's
    //                                      reservoir, gated on the #976 history
    //                                      layer AND on the sample's AGE, with a
    //                                      real reconnection Jacobian, then cap M)
    //   C. ReSTIR_GI_SpatialReuse.glsl  -> ReSTIRGISpatial0/1 (k neighbours with
    //                                      the shift Jacobian and the domain
    //                                      gate; ping-ponged so SpatialPasses can
    //                                      run it more than once)
    //   D. ReSTIR_GI_Resolve.glsl       -> ReSTIRGIRadiance  (one RECONNECTION
    //                                      ray on the final survivor, the debug
    //                                      views, and the variance moments)
    //
    // READ docs/design/restir-gi-reconnection-shift.md. It derives the measure,
    // the shift and the Jacobian, and it was written before any of this existed
    // because #1169 requires that.
    //
    // WHAT THIS IS, AND WHAT IT IS NOT. It is a TIER OVER THE SHARED SCENE, not a
    // second renderer. It consumes the same canonical instance / material / light
    // identities as raster and the path tracer (#977), the same versioned closure
    // (#975), the same TLAS (#978) and the same surface-history layer (#976), and
    // its ray-hit machinery and light-sampling densities are the PATH TRACER'S,
    // shared through include/RayTracedSurfaceHit.glsl and
    // include/LightSampling.glsl rather than re-derived — because the oracle this
    // tier is validated against and the tier itself must not disagree about what
    // a ray hits or about the measure, or the comparison proves nothing.
    //
    // WHERE IT SITS. After the last G-Buffer writer, after RayTracingScenePass (a
    // by-name edge: the acceleration structure is not a graph resource) and after
    // the DDGI probe update (whose atlases the bounce vertex reads); before
    // DeferredLightingPass, which samples ReSTIRGIRadiance at
    // TEX_RESTIR_GI_RADIANCE INSTEAD of the ambient ladder's DIFFUSE rung.
    //
    // THE HAND-OFF WITH DDGI, which is the #979 non-goal this tier exists not to
    // break: the probe cache is read at exactly ONE vertex per path, and enabling
    // this tier MOVES which vertex that is — from x0 (the ambient ladder) to x1
    // (draw A's bounce vertex). It is never read twice, DDGI keeps supplying
    // bounces 2 and beyond, and SSGI — a THIRD estimate of the same integral —
    // stands down and is COUNTED doing so. SelectIndirectDiffuseSources in
    // ReSTIRGITechnique.h is that rule as a pure function, and ReSTIRGIContractTest
    // asserts it.
    //
    // THE FALLBACK IS STRUCTURAL, NOT A FLAG. The deferred lighting shader
    // branches on whether the radiance target carries alpha, and every way this
    // pass can fail to produce one leaves it zero, so the ambient ladder is what
    // happens by default and the ReSTIR branch has to be actively earned. The
    // reason is counted in GetStats() and reported once per change
    // (docs/agent-rules/no-silent-fallbacks.md).
    //
    // ACCUMULATION goes through the temporal-history registry and NOTHING ELSE:
    // five RGBA32F planes under TemporalHistoryEffect::ReSTIRGI. The reservoir
    // planes carry kGIReservoirLayoutVersion, which is SEPARATE from DI's — the
    // two layouts share an encoding but not a shape, and one shared number would
    // make every DI packing bump invalidate GI history for no reason.
    //
    // DELIBERATELY NOT PARALLEL-RECORDED (#1013). Four ordered fullscreen draws
    // into four targets is already one writer per object per region, so the
    // conversion is available later; nothing here touches dispatcher or
    // heap-binding state.
    class ReSTIRGIPass : public RenderGraphNode
    {
      public:
        ReSTIRGIPass();
        ~ReSTIRGIPass() override = default;

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
            // tracing (GL_EXT_ray_query has no GL representation), so a null here
            // is the unsupported path rather than a load failure — which is why
            // this predicate must not assert.
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
        // The SAME table the path tracer's NEE samples, so the bounce vertex's
        // NEE draw and the oracle share the emitter set and the 1/totalArea
        // density rather than each building one.
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

        void SetSettings(const ReSTIRGISettings& settings) noexcept
        {
            m_Settings = SanitizeReSTIRGISettings(settings);
            // What the sanitizer had to change. Non-zero means the frame will do
            // LESS than the settings asked for, which is exactly the kind of
            // thing a user never discovers from the image.
            m_SettingsClamped = (settings == m_Settings) ? 0u : 1u;
        }
        [[nodiscard]] const ReSTIRGISettings& GetSettings() const noexcept
        {
            return m_Settings;
        }

        // The uniform environment a bounce ray that escapes collects, and the
        // intensity applied to the prefiltered cube on top of it. THE SAME PAIR
        // GpuPathTracerSettings carries, taken from the same settings, so the
        // tier and the oracle it is validated against agree about the sky.
        //
        // `cubeBound` is the third argument because the intensity ALONE cannot
        // answer whether there is a sky: it defaults to 1.0, so a scene with no
        // environment map at all reported an environment, and the engagement
        // criterion's NoIndirectSourceInScene arm became unreachable - a
        // stand-down that can never fire is a stand-down that was never tested.
        // Whether a prefiltered cube is actually bound is a frame fact only the
        // pipeline holds, so it is passed in the way SetProbeVolumeAvailable is.
        void SetEnvironment(const glm::vec3& uniformRadiance, f32 cubeIntensity, bool cubeBound) noexcept
        {
            m_UniformEnvironmentRadiance = uniformRadiance;
            m_EnvironmentCubeIntensity = cubeIntensity;
            m_EnvironmentCubeBound = cubeBound;
        }

        // Whether the probe ladder has anything to hand the BOUNCE VERTEX. Fed
        // from the pipeline rather than probed here, because "a probe volume is
        // bound" is a scene fact this pass has no view of — and the DDGI tail is
        // reported as having run rather than assumed from the setting.
        void SetProbeVolumeAvailable(bool available) noexcept
        {
            m_ProbeVolumeAvailable = available;
        }

        // `view` is the WORLD view matrix and `renderOrigin` the camera-relative
        // origin GPU Scene encoded this frame's transforms against (issue #429).
        // Both are needed for the reason RayTracedShadowPass documents at length:
        // the TLAS is built from render-relative transforms, so a ray traced from
        // an absolute world position misses by exactly the origin — silently, and
        // only once the camera is far enough out for the grid to have snapped.
        //
        // THE PREVIOUS FRAME'S PAIR IS KEPT HERE, not uploaded by the caller: the
        // temporal draw reconstructs last frame's shading point from it for the
        // reconnection Jacobian, and the only place that knows what was uploaded
        // LAST frame is the object that uploaded it. A caller-supplied "previous"
        // matrix is how a frame that was culled leaves a stale one behind.
        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection,
                               const glm::vec3& renderOrigin) noexcept
        {
            m_View = view;
            m_Projection = projection;
            m_RenderOrigin = renderOrigin;
        }
        // The sampler decorrelator. A FIXED value makes a run reproducible, which
        // is what the oracle comparison needs.
        void SetFrameIndex(u32 frameIndex) noexcept
        {
            m_FrameIndex = frameIndex;
        }

        // Whether every ReSTIR GI history plane the registry is holding was
        // acquired under ReSTIR::kGIReservoirLayoutVersion. READ FROM THE
        // REGISTRY, NOT ASSUMED — this pass cannot derive it, because the registry
        // drops a history whose descriptor changed, so by the time a handle
        // reaches Execute a version bump is indistinguishable from a resize or a
        // first frame. Same arrangement, and the same reason, as ReSTIRDIPass's.
        void SetHistoryLayoutMatches(bool matches) noexcept
        {
            m_HistoryLayoutMatches = matches;
        }

        // Whether the user asked for SSGI this frame. Taken so the hand-off can
        // be REPORTED rather than inferred: when this tier is live it stands SSGI
        // down (a third estimate of the same integral would double-count), and
        // "my SSGI slider does nothing" needs a counter to answer it.
        void SetSSGIRequested(bool requested) noexcept
        {
            m_SSGIRequested = requested;
        }

        // Compute the verdict and fill the counters for this frame WITHOUT
        // executing, called from ConfigurePassesForFrame every frame whether or
        // not the graph goes on to declare a target.
        //
        // WHY IT IS NOT ENOUGH TO DO THIS IN Execute. A pass whose output the
        // graph never declared is CULLED, so Execute never runs — and that is
        // precisely the case on every machine without ray tracing, which is every
        // CI runner and the fallback arm generally. A tier that only reported from
        // Execute would answer "switched off in the render settings" to a user who
        // had just switched it on. Execute recomputes with the real target
        // availability and wins when the pass does run.
        void ResolveAvailabilityForFrame()
        {
            // TARGET AVAILABILITY IS PASSED AS TRUE HERE, and that is not a lie by
            // omission — it is the same rule GpuPathTracerPass and ReSTIRDIPass
            // state: only Execute can see whether the graph produced the target.
            // Passing false instead makes this call report TargetUnavailable on
            // EVERY frame, including the frames the tier is running perfectly, and
            // since it runs once per frame from the per-frame wiring it then
            // overwrites Execute's real verdict.
            (void)ResolveTechniqueForFrame(true);
        }

        [[nodiscard]] const ReSTIRGIStats& GetStats() const noexcept
        {
            return m_Stats;
        }

        // What the frame is allowed to add where. The pipeline reads this to stand
        // SSGI down and the deferred lighting pass to drop its ambient DIFFUSE
        // rung — one answer, from one function, rather than three passes each
        // deciding.
        [[nodiscard]] IndirectDiffuseSources GetIndirectDiffuseSources() const noexcept
        {
            return m_Stats.Sources;
        }

      private:
        // Decide the technique for this frame and fill the counters. Called FIRST
        // and unconditionally from Execute, so every exit below it has already
        // counted its reason.
        [[nodiscard]] bool ResolveTechniqueForFrame(bool graphResourcesResolved);

        bool m_Enabled = false;
        ReSTIRGISettings m_Settings{};
        ReSTIRGIStats m_Stats{};
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
        glm::vec3 m_RenderOrigin{ 0.0f };
        glm::vec3 m_UniformEnvironmentRadiance{ 0.0f };
        f32 m_EnvironmentCubeIntensity = 1.0f;
        bool m_EnvironmentCubeBound = false;
        bool m_ProbeVolumeAvailable = false;
        bool m_SSGIRequested = false;
        u32 m_FrameIndex = 0;

        // LAST FRAME'S UPLOADED reconstruction, for the temporal draw's
        // reconnection Jacobian. Kept by the pass because the pass is the only
        // thing that knows what it uploaded, and guarded by m_HavePrevFrame so the
        // FIRST frame does not reconstruct a previous shading point from an
        // identity matrix — which would put every vertex at the origin and reject
        // every reuse, silently, on exactly the frame nobody looks at.
        glm::mat4 m_PrevRelativeView{ 1.0f };
        glm::mat4 m_PrevProjection{ 1.0f };
        glm::vec3 m_PrevRenderOrigin{ 0.0f };
        bool m_HavePrevFrame = false;

        bool m_HistoryLayoutMatches = true;

        FramebufferSpecification m_FramebufferSpec{};

        // Selected in Setup, resolved in Execute.
        RGTextureHandle m_SelectedSceneDepth{};
        RGTextureHandle m_SelectedGBufferAlbedo{};
        RGTextureHandle m_SelectedGBufferNormal{};
        RGTextureHandle m_SelectedGBufferEmissive{};
        RGTextureHandle m_SelectedVelocity{};
        RGTextureHandle m_SelectedPrefilterTexture{};
        RGFramebufferHandle m_SelectedInitial{};
        RGFramebufferHandle m_SelectedTemporal{};
        std::array<RGFramebufferHandle, 2> m_SelectedSpatial{};
        RGTextureHandle m_SelectedReservoirSampleHistory{};
        RGTextureHandle m_SelectedReservoirRadianceHistory{};
        RGTextureHandle m_SelectedReservoirStateHistory{};
        RGTextureHandle m_SelectedSurfaceHistory{};
        RGTextureHandle m_SelectedMomentsHistory{};

        // Reported once per CHANGE, never once per frame: this is the line that
        // tells a user why their scene still has probe-grid GI, and a per-frame
        // version would be indistinguishable from spam.
        ReSTIRGIFallbackReason m_LastReportedFallback = ReSTIRGIFallbackReason::None;
    };
} // namespace OloEngine
