#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Shadow/ShadowTechnique.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>
#include <glm/glm.hpp>
#include <vector>

namespace OloEngine
{
    class ShadowMap;

    namespace RayTracing
    {
        class RayTracingScene;
    }

    // @brief Hybrid ray-traced soft shadows (issue #1056) — three draws, one node.
    //
    //   A. RayTracedShadow.glsl        -> RayTracedShadowSignal   (one ray per
    //                                     pixel per light against the #978
    //                                     TLAS: visibility + blocker distance)
    //   B. RayTracedShadowResolve.glsl -> RayTracedShadowResolved (temporal
    //                                     accumulation + moments; the
    //                                     RayTracedShadowHistory source)
    //   C. RayTracedShadowFilter.glsl  -> RayTracedShadowMask     (variance-
    //                                     guided spatial filter; the only
    //                                     output anything else reads)
    //
    // The split is the same one #902 made for SSGI and for the same reason:
    // draw B needs draw A's output in a real texture because it gathers a 3x3
    // neighbourhood that does not exist inside draw A, and draw C needs draw
    // B's for the same reason.
    //
    // WHERE IT SITS. After the last G-Buffer writer (it reads scene depth and
    // the world normal) and before DeferredLightingPass (which samples the
    // mask). It declares an execution dependency on RayTracingScenePass BY
    // NAME — that node's Setup comment reserved exactly this seam for the first
    // ray-query consumer, and the acceleration structure is not a graph
    // resource, so a name edge is the only edge the graph can see. Without it,
    // a reordering that moved the AS build after this pass would read a TLAS
    // that had not been built, silently.
    //
    // THE FALLBACK IS STRUCTURAL, NOT A FLAG. Execute is the only place that
    // knows whether the trace really ran, so it is where the ShadowUBO's
    // routing lanes are patched: they go up INACTIVE with the rest of the
    // shadow data and only this pass turns them on, after its draws. Every way
    // this pass can fail to produce a mask therefore leaves the lighting shader
    // on the raster branch by construction rather than by remembering to reset
    // a flag. The reason is counted in GetStats().
    //
    // DELIBERATELY NOT PARALLEL-RECORDED (#1013). Three ordered fullscreen
    // draws into three targets is one writer per object per region already, so
    // the conversion is available later; nothing here touches dispatcher or
    // heap-binding state.
    class RayTracedShadowPass : public RenderGraphNode
    {
      public:
        RayTracedShadowPass();
        ~RayTracedShadowPass() override;

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
            // The three shaders are only CREATED on a backend with hardware ray
            // tracing (GL_EXT_ray_query has no GL representation), so a null
            // here is the unsupported path rather than a load failure — and it
            // is why this predicate must not assert.
            return m_TraceShader && m_TraceShader->IsReady() && m_ResolveShader && m_ResolveShader->IsReady() &&
                   m_FilterShader && m_FilterShader->IsReady() && m_ParamsUBO;
        }

        // Wired at renderer init. Both borrowed, never owned.
        void SetRayTracingScene(const RayTracing::RayTracingScene* scene) noexcept
        {
            m_RayTracingScene = scene;
        }
        // The ShadowUBO owner, so Execute can patch the routing lanes in place.
        void SetShadowMap(ShadowMap* shadowMap) noexcept
        {
            m_ShadowMap = shadowMap;
        }
        void SetParamsUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_ParamsUBO = ubo;
        }

        // Per-frame inputs, forwarded from the renderer settings.
        void SetSettings(const RayTracedShadowSettings& settings) noexcept
        {
            m_Settings = settings;
        }
        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection) noexcept
        {
            m_View = view;
            m_Projection = projection;
        }
        void SetFrameIndex(u32 frameIndex) noexcept
        {
            m_FrameIndex = frameIndex;
        }
        // The frame's opted-in lights, in the order Scene found them. More than
        // kRayTracedShadowMaskChannels of them is not an error: the surplus is
        // counted as MaskChannelBudgetExhausted and falls back.
        void SetLightRequests(std::vector<RayTracedShadowLightRequest> requests) noexcept
        {
            m_LightRequests = std::move(requests);
        }
        // TLAS instances whose geometry is alpha-tested. They shadow as solid
        // here (no shader-visible sampler heap yet, #805) and the count is what
        // makes that diagnosable.
        void SetMaskedOccluderCount(u32 count) noexcept
        {
            m_MaskedOccluderCount = count;
        }

        [[nodiscard]] const ShadowTechniqueStats& GetStats() const noexcept
        {
            return m_Stats;
        }

      private:
        // Decide, count and pack this frame's routing. Split out of Execute so
        // the ordering — decide, then upload, then draw, then publish — is one
        // readable sequence and so the decision can be exercised on its own.
        // Returns the number of channels actually assigned.
        u32 ResolveTechniqueForFrame(bool graphResourcesResolved);

        bool m_Enabled = false;

        Ref<Shader> m_TraceShader;
        Ref<Shader> m_ResolveShader;
        Ref<Shader> m_FilterShader;
        Ref<UniformBuffer> m_ParamsUBO;

        const RayTracing::RayTracingScene* m_RayTracingScene = nullptr;
        ShadowMap* m_ShadowMap = nullptr;

        RayTracedShadowSettings m_Settings{};
        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
        u32 m_FrameIndex = 0;
        u32 m_MaskedOccluderCount = 0;

        std::vector<RayTracedShadowLightRequest> m_LightRequests;
        // Channel -> the request that won it, BY VALUE. Rebuilt every frame by
        // ResolveTechniqueForFrame. A pointer into m_LightRequests would be
        // one SetLightRequests call away from dangling, and the struct is five
        // scalars.
        std::array<RayTracedShadowLightRequest, kRayTracedShadowMaskChannels> m_ChannelLights{};
        ShadowTechniqueStats m_Stats{};

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
        RGTextureHandle m_SelectedVelocityTexture{};
        RGTextureHandle m_SelectedHistoryTexture{};
        RGTextureHandle m_SelectedSurfaceHistoryTexture{};
        RGTextureHandle m_SelectedMomentsHistoryTexture{};
        RGFramebufferHandle m_SelectedSignalFramebuffer{};
        RGFramebufferHandle m_SelectedResolvedFramebuffer{};

        // The shared blue-noise tile (issue #706). Pass-owned, immutable, bound
        // with a Persistent heap lifetime — not a graph resource.
        RHI::ResourceHandle m_BlueNoiseTexture{};
    };
} // namespace OloEngine
