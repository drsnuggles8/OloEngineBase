#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/FluidRenderData.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <vector>

namespace OloEngine
{
    // @brief Screen-space fluid intermediates (issue #630, pillar B).
    //
    // Renders the GPU fluid particle SSBOs (bindings 21/22/28) into the
    // screen-space intermediate textures the FluidCompositePass shades from:
    //
    //   1. Depth splat  — instanced camera-facing sphere impostors write the
    //      per-pixel nearest fluid view-depth (positive metres) into an R32F
    //      target, z-tested against a pass-owned DEPTH_COMPONENT32F buffer and
    //      discarded behind the opaque scene depth. 0 = "no fluid" sentinel.
    //   2. Thickness    — the same splats accumulate (ONE/ONE additive, depth
    //      test off) chord thickness into RG16F: r = thickness, g = speed-
    //      weighted thickness (foam numerator).
    //   3. Bilateral smooth — FluidSmooth.comp ping-pongs the depth A→B→A
    //      (kSmoothIterations, even so the result lands back in A) with a
    //      depth-aware gaussian, turning the sphere shells into a surface.
    //
    // The outputs are consumed by FluidCompositePass via RAW texture ids
    // (GetSmoothedDepthTextureID / GetThicknessTextureID), outside graph
    // resource tracking — hence SideEffect::NeverCull (this pass culls
    // nothing; the "no draws" early-out in Setup is the real gate, and the
    // pipeline fingerprint must hash that gate — issue #530 class).
    //
    // Render targets are pass-owned raw GL objects (WaterRenderPass-style raw
    // GL is precedent) because FramebufferTextureFormat has no single-channel
    // R32F format and the smoothing compute needs r32f image bindings.
    class FluidIntermediatesPass : public RenderGraphNode
    {
      public:
        FluidIntermediatesPass();
        ~FluidIntermediatesPass() override;

        FluidIntermediatesPass(const FluidIntermediatesPass&) = delete;
        FluidIntermediatesPass& operator=(const FluidIntermediatesPass&) = delete;
        FluidIntermediatesPass(FluidIntermediatesPass&&) = delete;
        FluidIntermediatesPass& operator=(FluidIntermediatesPass&&) = delete;

        void Init(const FramebufferSpecification& spec) override;
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_DepthSplatShader && m_DepthSplatShader->IsReady() &&
                   m_ThicknessShader && m_ThicknessShader->IsReady() &&
                   m_SmoothShader && m_SmoothShader->IsValid() &&
                   m_FluidRenderUBO && m_SplatVAO;
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        // Per-frame draw list, moved in from ConfigurePassesForFrame and
        // consumed (cleared) by Execute — one-shot like the particle render
        // callback. Invalid entries (zero SSBO ids / zero count / non-finite
        // radius) are dropped at Execute time.
        void SetFrameDraws(std::vector<FluidRenderData> draws)
        {
            m_FrameDraws = std::move(draws);
        }

        // True between SetFrameDraws and Execute when at least one draw is
        // pending. FluidCompositePass::Setup gates its resource declarations
        // on this (all Setups run before any Execute), and the pipeline
        // fingerprint must hash the same condition.
        [[nodiscard]] bool HasPendingDraws() const noexcept
        {
            return !m_FrameDraws.empty();
        }

        // Raw GL id of the smoothed R32F view-depth texture (0 until targets
        // exist). Valid for sampling only when RanThisFrame() is true.
        [[nodiscard]] u32 GetSmoothedDepthTextureID() const noexcept
        {
            return m_DepthTexA;
        }

        // Raw GL id of the RG16F thickness texture (r = thickness metres,
        // g = speed-weighted thickness). Same validity contract as above.
        [[nodiscard]] u32 GetThicknessTextureID() const noexcept
        {
            return m_ThicknessTex;
        }

        // True when Execute reached the splat/smooth dispatches this frame.
        // FluidCompositePass gates its Execute on this.
        [[nodiscard]] bool RanThisFrame() const noexcept
        {
            return m_RanThisFrame;
        }

        // Appearance parameters of the first drawn fluid this frame (tint,
        // absorption, radius, foam, entity id) — the composite re-uploads the
        // FluidRenderUBO from these. Only meaningful when RanThisFrame().
        // v1 limitation: with multiple fluids the composite shades every
        // fluid pixel with the first fluid's appearance.
        [[nodiscard]] const FluidRenderData& GetLastAppearance() const noexcept
        {
            return m_LastAppearance;
        }

      private:
        void CreateTargets(u32 width, u32 height);
        void ReleaseTargets();
        void UploadDrawUBO(const FluidRenderData& draw, f32 cameraNear, f32 cameraFar);

        // Even iteration count keeps the smoothed result in m_DepthTexA.
        static constexpr u32 kSmoothIterations = 2;
        static_assert(kSmoothIterations % 2 == 0,
                      "GetSmoothedDepthTextureID returns texture A — the A->B->A ping-pong must end on A");
        static constexpr f32 kDefaultBlurRadiusPx = 6.0f;
        static constexpr u32 kSmoothLocalSize = 16; // matches FluidSmooth.comp local_size_x/y

        bool m_Enabled = true;
        bool m_RanThisFrame = false;

        std::vector<FluidRenderData> m_FrameDraws;
        FluidRenderData m_LastAppearance{};

        Ref<Shader> m_DepthSplatShader;
        Ref<Shader> m_ThicknessShader;
        Ref<ComputeShader> m_SmoothShader;
        Ref<UniformBuffer> m_FluidRenderUBO;
        Ref<VertexArray> m_SplatVAO;

        RGTextureHandle m_SelectedSceneDepthTexture{};

        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_DepthTexA = 0;    // R32F — splat target + final smoothed result
        u32 m_DepthTexB = 0;    // R32F — smoothing ping-pong partner
        u32 m_ThicknessTex = 0; // RG16F — additive thickness accumulation
        u32 m_SplatZTex = 0;    // DEPTH_COMPONENT32F — nearest-splat z-test
        u32 m_DepthFBO = 0;     // COLOR0 = m_DepthTexA, DEPTH = m_SplatZTex
        u32 m_ThicknessFBO = 0; // COLOR0 = m_ThicknessTex
    };
} // namespace OloEngine
