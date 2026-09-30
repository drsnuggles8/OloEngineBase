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
    // One draw, one node. The contract it implements is ADR 0020.
    //
    // WHAT IT COMPOSES (#1325). The tier replaces the indirect specular term S
    // that the lit colour already holds, and nothing else:
    // C' = C + c * (W * L - S) (include/ReflectionTierComposite.glsl, CPU twin
    // ComposeSpecularTier in ReflectionTier.h). S and its weight W come from
    // DeferredLightingPass's IndirectSpecular output; diffuse, emission and
    // direct light are left alone. Fresnel lives in W, never in the
    // confidence c.
    //
    // WHERE IT SITS. ADR 0020 evaluates the hierarchy BOTTOM-UP, so no tier
    // needs the confidence of a tier above it. This pass runs after
    // DeferredLightingPass, AOApply and SSGI (it reads the newest of
    // SSGIColor / AOApplyColor / SceneColor) and before SSRRenderPass, which
    // replaces the S this pass hands on in its second attachment
    // (RTReflectionSpecular) using the same delta. SSR's per-pixel confidence
    // never has to travel through its denoiser chain.
    //
    // It fills the gap SSR cannot: OFF-SCREEN and OCCLUDED hits. A miss leaves
    // the colour and S untouched, so the probe/IBL term stays the
    // environment's answer.
    //
    // WHAT IT COSTS IN BINDINGS: nothing new. The TLAS is a device address
    // inside UBO_RAY_TRACING (65), shared with RayTracedShadow.glsl and
    // RayTracingProbe.comp the way #978 established, and the same block
    // carries the material shader-heap address and sampler the alpha test
    // uses. The three GPU Scene tables come from their canonical SSBO bindings
    // (15/16/17).
    //
    // THE FALLBACK IS STRUCTURAL, NOT A FLAG. Every early-out copies the colour
    // and hands S on unchanged, and a zero confidence makes the delta exactly
    // zero, so the raster-only output is unchanged by construction rather than
    // by a test that happens to pass. The reason is counted in GetStats() and
    // reported once per change, never silently
    // (docs/agent-rules/no-silent-fallbacks.md).
    //
    // LIMITS. Masked candidates are alpha-tested against the material's albedo
    // alpha and canonical cutoff (HybridRayTracingAlpha.glsl, the helper
    // RayTracedShadow uses); the tier stands down with GPUSceneUnavailable when
    // the material heap is unresolved. Hits are shaded from material FACTORS,
    // untextured, and that is counted (ReflectionTierStats::HitsShadedUntextured);
    // canonical textured hit shading with a ray-footprint policy is #1355.
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
        RGTextureHandle m_SelectedPrefilterTexture{};
        // The indirect specular term the lighting composed and its weight
        // (issue #1325): the term this tier replaces, and what L is scaled by.
        RGTextureHandle m_SelectedIndirectSpecularTexture{};
        RGTextureHandle m_SelectedIndirectSpecularWeightTexture{};
    };

} // namespace OloEngine
