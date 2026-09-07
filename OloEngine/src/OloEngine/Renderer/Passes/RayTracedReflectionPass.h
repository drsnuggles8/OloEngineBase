#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ReflectionTier.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class GPUScene;

    namespace RayTracing
    {
        class RayTracingScene;
    }

    // @brief The RAY-QUERY tier of the reflection hierarchy (issue #1057).
    // One draw, one node. The contract it implements is ADR 0019.
    //
    // WHERE IT SITS, AND WHY THAT IS THE DESIGN. ADR 0019 evaluates the
    // hierarchy BOTTOM-UP — every tier lerps over the colour it was handed, so
    // no tier needs the confidence of a tier ABOVE it. This pass therefore runs
    // AFTER DeferredLightingPass (whose output already carries the probe-over-IBL
    // blend, the two tiers below) and BEFORE SSRRenderPass (the tier above),
    // which then lerps over this pass's output using its own existing delta
    // composite. Not one line of SSR's five-stage denoiser chain changes.
    //
    // The alternative reading of the same ordering — the better tier claims its
    // share and hands the residual down — is the same algebra and would need
    // SSR's per-pixel confidence transported through that chain, whose only
    // spare lane deliberately carries view depth on every path.
    //
    // It fills exactly the gap SSR cannot: OFF-SCREEN and OCCLUDED hits. A ray
    // that misses contributes nothing, so the probe/IBL tier below answers —
    // reflecting the sky here would double-count it.
    //
    // WHAT IT COSTS IN BINDINGS: nothing new. The TLAS is a device address
    // inside UBO_RAY_TRACING (65), shared with RayTracedShadow.glsl and
    // RayTracingProbe.comp the way #978 established; the three GPU Scene tables
    // come from their canonical SSBO bindings (15/16/17).
    //
    // THE FALLBACK IS STRUCTURAL, NOT A FLAG. Every way this pass can fail to
    // trace leaves the pixel's confidence at exactly 0, and `mix(base, L, 0)` is
    // `base` — so the raster-only output is byte-identical when the tier is off
    // BY CONSTRUCTION rather than by a test that happens to pass. The reason is
    // counted in GetStats() and reported once per change, never silently
    // (docs/agent-rules/no-silent-fallbacks.md).
    //
    // FIRST-SLICE LIMITS, both deliberate and both recorded in ADR 0019 §4:
    // hits are shaded from UNTEXTURED material factors (arbitrary-material
    // sampling is blocked on the shader-visible sampler heap, #805), and masked
    // geometry reflects as solid (the same trade RayTracedShadowPass makes).
    // Both are counted, not commented.
    class RayTracedReflectionPass : public RenderGraphNode
    {
      public:
        RayTracedReflectionPass();
        ~RayTracedReflectionPass() override = default;

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
            return m_ReflectionShader && m_ReflectionShader->IsReady() && m_ParamsUBO;
        }

        // Wired at renderer init. All borrowed, never owned.
        void SetRayTracingScene(const RayTracing::RayTracingScene* scene) noexcept
        {
            m_RayTracingScene = scene;
        }
        // The GPU Scene owns the instance / geometry / material tables the
        // shader resolves a hit through, and the slot counts its bounds checks
        // need. Without it a hit cannot be shaded, which is a counted fallback.
        void SetGPUScene(const GPUScene* gpuScene) noexcept
        {
            m_GPUScene = gpuScene;
        }
        void SetParamsUBO(const Ref<UniformBuffer>& ubo) noexcept
        {
            m_ParamsUBO = ubo;
        }

        void SetSettings(const RayTracedReflectionSettings& settings) noexcept
        {
            m_Settings = settings;
        }

        // The dominant directional light, in world space. `hasSun` false leaves
        // a hit lit by the environment alone rather than by a guessed sun.
        void SetSunLight(const glm::vec3& directionToSun, const glm::vec3& radiance, bool hasSun) noexcept
        {
            m_SunDirection = directionToSun;
            m_SunRadiance = radiance;
            m_HasSun = hasSun;
        }

        void SetFrameIndex(u32 frameIndex) noexcept
        {
            m_FrameIndex = frameIndex;
        }

        // The WORLD-space camera matrices plus the render origin this frame's
        // GPU Scene transforms were encoded against. Execute converts the view
        // to render-relative before uploading, because the TLAS is built from
        // those same render-relative transforms — a world-space ray origin
        // would miss the geometry by the origin offset, growing with distance
        // from it.
        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection,
                               const glm::vec3& renderOrigin) noexcept
        {
            m_View = view;
            m_Projection = projection;
            m_RenderOrigin = renderOrigin;
        }

        [[nodiscard]] const ReflectionTierStats& GetStats() const noexcept
        {
            return m_Stats;
        }

      private:
        // Fills m_Stats and returns whether the trace may run this frame.
        [[nodiscard]] bool ResolveAvailabilityForFrame(bool graphResourcesResolved);

        bool m_Enabled = false;

        Ref<Shader> m_ReflectionShader;
        Ref<UniformBuffer> m_ParamsUBO;

        const RayTracing::RayTracingScene* m_RayTracingScene = nullptr;
        const GPUScene* m_GPUScene = nullptr;

        RayTracedReflectionSettings m_Settings{};
        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
        glm::vec3 m_RenderOrigin{ 0.0f };
        glm::vec3 m_SunDirection{ 0.0f, 1.0f, 0.0f };
        glm::vec3 m_SunRadiance{ 0.0f };
        bool m_HasSun = false;
        u32 m_FrameIndex = 0;

        ReflectionTierStats m_Stats{};
        // Seeded to Count — a value no verdict can equal — so the FIRST
        // verdict of the process is always reported. Seeding it to None
        // would swallow the "the tier is active" line on a session that
        // works, which is exactly the line that tells a user the switch
        // they just flipped took effect.
        ReflectionTierFallbackReason m_LastReportedFallback = ReflectionTierFallbackReason::Count;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedGBufferNormalTexture{};
        RGTextureHandle m_SelectedGBufferAlbedoTexture{};
        RGTextureHandle m_SelectedPrefilterTexture{};
    };

} // namespace OloEngine
