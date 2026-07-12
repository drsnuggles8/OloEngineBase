#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Passes/FluidIntermediatesPass.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

namespace OloEngine
{
    // @brief Screen-space fluid composite (issue #630, pillar B).
    //
    // Joins the SceneColor read-modify-write chain (register after WaterPass;
    // WaterRenderPass is the pattern): copies the current scene colour into
    // the FluidRefraction scratch (glCopyImageSubData, TransferDest usage),
    // then draws one fullscreen triangle that shades every fluid-covered
    // pixel from the FluidIntermediatesPass outputs — smoothed view-depth
    // (normal reconstruction + refraction offset), thickness (Beer–Lambert
    // absorption + foam), environment cubemap reflection with Fresnel — and
    // writes the full 4-target scene MRT (colour / entity id / octahedral
    // view normal / velocity). Non-fluid pixels are discarded in the shader,
    // so depth test and blending are both OFF for the draw.
    //
    // Setup gates every declaration on the intermediates pass having pending
    // draws (all Setups run before any Execute) — the pipeline fingerprint
    // must hash the same gate (issue #530 class).
    class FluidCompositePass : public RenderGraphNode
    {
      public:
        FluidCompositePass();
        ~FluidCompositePass() override = default;

        FluidCompositePass(const FluidCompositePass&) = delete;
        FluidCompositePass& operator=(const FluidCompositePass&) = delete;
        FluidCompositePass(FluidCompositePass&&) = delete;
        FluidCompositePass& operator=(FluidCompositePass&&) = delete;

        void Init(const FramebufferSpecification& spec) override;
        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;
        [[nodiscard]] Ref<Framebuffer> GetTarget() const override;

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_CompositeShader && m_CompositeShader->IsReady() && m_FluidRenderUBO;
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        // Wired once by RenderPipeline::CreateFramePasses (cross-pass Ref,
        // VolumetricFog -> Fog pattern): source of the smoothed-depth /
        // thickness texture ids, the ran-this-frame gate, and the per-frame
        // appearance parameters.
        void SetIntermediatesPass(const Ref<FluidIntermediatesPass>& pass) noexcept
        {
            m_IntermediatesPass = pass;
        }

      private:
        bool m_Enabled = true;

        Ref<FluidIntermediatesPass> m_IntermediatesPass;

        Ref<Shader> m_CompositeShader;
        Ref<UniformBuffer> m_FluidRenderUBO;

        Ref<Framebuffer> m_SceneFramebuffer;

        RGTextureHandle m_SelectedSceneColorTexture{};
        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedRefractionTexture{};
    };
} // namespace OloEngine
