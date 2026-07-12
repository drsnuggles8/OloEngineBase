#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FluidCompositePass.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <algorithm>

namespace OloEngine
{
    FluidCompositePass::FluidCompositePass()
    {
        OLO_PROFILE_FUNCTION();
        SetName("FluidCompositePass");
        OLO_CORE_INFO("Creating FluidCompositePass.");
    }

    void FluidCompositePass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_CompositeShader = Shader::Create("assets/shaders/FluidComposite.glsl");
        if (!m_CompositeShader)
        {
            OLO_CORE_ERROR("FluidCompositePass: Failed to load FluidComposite.glsl");
        }

        // Own UBO on the shared fluid-render binding point; the passes run
        // back to back and each uploads before its draw, so the last Bind
        // winning is exactly the behaviour we want.
        m_FluidRenderUBO = UniformBuffer::Create(
            UBOStructures::FluidRenderUBO::GetSize(),
            ShaderBindingLayout::UBO_FLUID_RENDER);
    }

    void FluidCompositePass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_SelectedSceneColorTexture = {};
        m_SelectedSceneDepthTexture = {};
        m_SelectedRefractionTexture = {};

        // Declare NOTHING when there is no fluid this frame (or a required
        // resource is missing) — the pipeline fingerprint must hash this gate.
        // HasPendingDraws is stable across all Setups: every Setup runs
        // before any Execute consumes the draw list.
        if (!m_Enabled || !m_IntermediatesPass || !m_IntermediatesPass->HasPendingDraws())
            return;
        if (!board.Scene.SceneColor.IsValid() || !board.Scene.SceneColorTexture.IsValid() ||
            !board.Scene.SceneDepthAttachment.IsValid() || !board.Scratch.FluidRefraction.IsValid())
        {
            return;
        }

        // Inter-pass RMW of SceneColor: bind the prior version as the render
        // target and advertise a renamed output (WaterRenderPass pattern).
        SetPrimaryInputFramebufferHandle(board.Scene.SceneColor);
        constexpr std::string_view fluidSceneColorVersionTag = "FluidCompositePass";
        [[maybe_unused]] const auto sceneColorNew =
            builder.WriteNewVersion(board.Scene.SceneColor, RGWriteUsage::RenderTarget, fluidSceneColorVersionTag);
        builder.DependsOnPreviousWriter(ResourceNames::SceneColor);

        m_SelectedSceneColorTexture = board.Scene.SceneColorTexture;
        [[maybe_unused]] const auto sceneColorRead =
            builder.Read(board.Scene.SceneColorTexture, RGReadUsage::ShaderSample);

        m_SelectedSceneDepthTexture = board.Scene.SceneDepthAttachment;
        [[maybe_unused]] const auto sceneDepthRead =
            builder.Read(board.Scene.SceneDepthAttachment, RGReadUsage::ShaderSample);

        // Refraction scratch: intra-pass copy-then-sample. glCopyImageSubData
        // SceneColor -> FluidRefraction, then sampled in the composite draw —
        // a transfer write, NOT ShaderImage (the barrier planner would emit
        // the wrong fence type otherwise; Water refraction precedent).
        m_SelectedRefractionTexture = board.Scratch.FluidRefraction;
        builder.AllowSamePassReadWrite(board.Scratch.FluidRefraction);
        builder.Write(board.Scratch.FluidRefraction, RGWriteUsage::TransferDest);
        [[maybe_unused]] const auto refractionRead =
            builder.Read(board.Scratch.FluidRefraction, RGReadUsage::ShaderSample);

        // The smoothed-depth / thickness inputs are raw texture ids outside
        // graph tracking — pin the producer explicitly.
        builder.DependsOnPass("FluidIntermediatesPass");
    }

    void FluidCompositePass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Enabled || !m_IntermediatesPass || !m_IntermediatesPass->RanThisFrame() ||
            !IsReadyForExecution())
        {
            return;
        }

        const u32 fluidDepthID = m_IntermediatesPass->GetSmoothedDepthTextureID();
        const u32 fluidThicknessID = m_IntermediatesPass->GetThicknessTextureID();
        if (fluidDepthID == 0 || fluidThicknessID == 0)
            return;

        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_SceneFramebuffer = resolvedSceneFB;
        }
        if (!m_SceneFramebuffer)
            return;

        const u32 fbWidth = m_SceneFramebuffer->GetSpecification().Width;
        const u32 fbHeight = m_SceneFramebuffer->GetSpecification().Height;
        if (fbWidth == 0 || fbHeight == 0)
            return;

        u32 sceneColorID = 0;
        u32 sceneDepthID = 0;
        u32 refractionTexID = 0;
        if (m_SelectedSceneColorTexture.IsValid())
            sceneColorID = context.ResolveTexture(m_SelectedSceneColorTexture);
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (m_SelectedRefractionTexture.IsValid())
            refractionTexID = context.ResolveTexture(m_SelectedRefractionTexture);
        if (sceneColorID == 0 || sceneDepthID == 0 || refractionTexID == 0)
            return;

        GLStateGuard guard("FluidCompositePass", GLStateGuard::Policy::Ignore);

        // Snapshot the pre-fluid scene colour for refraction sampling.
        glCopyImageSubData(
            sceneColorID, GL_TEXTURE_2D, 0, 0, 0, 0,
            refractionTexID, GL_TEXTURE_2D, 0, 0, 0, 0,
            static_cast<GLsizei>(fbWidth), static_cast<GLsizei>(fbHeight), 1);

        // Upload the appearance parameters of this frame's fluid. Counts.z
        // carries the environment-map-present flag for the reflection branch.
        const u32 environmentMapID = Renderer3D::GetGlobalEnvironmentMapID();
        {
            const FluidRenderData& appearance = m_IntermediatesPass->GetLastAppearance();

            f32 cameraNear = 0.1f;
            f32 cameraFar = 1000.0f;
            ClusteredLighting::ExtractClipPlanes(Renderer3D::GetProjectionMatrix(), cameraNear, cameraFar);

            UBOStructures::FluidRenderUBO ubo{};
            ubo.TintRadius = glm::vec4(appearance.Tint, appearance.ParticleRadius);
            ubo.AbsorptionParams = glm::vec4(appearance.AbsorptionColor, appearance.AbsorptionScale);
            ubo.FoamParams = glm::vec4(appearance.FoamSpeedThreshold, 1.0f, 0.0f, 0.0f);
            ubo.SmoothParams = glm::vec4(0.0f, std::max(appearance.ParticleRadius * 4.0f, 1.0e-3f),
                                         cameraNear, cameraFar);
            ubo.ScreenParams = glm::vec4(static_cast<f32>(fbWidth), static_cast<f32>(fbHeight),
                                         1.0f / static_cast<f32>(fbWidth), 1.0f / static_cast<f32>(fbHeight));
            ubo.Counts = glm::uvec4(appearance.ParticleUpperBound,
                                    static_cast<u32>(appearance.EntityID),
                                    environmentMapID != 0 ? 1u : 0u, 0u);
            m_FluidRenderUBO->SetData(&ubo, sizeof(ubo));
            m_FluidRenderUBO->Bind();
        }

        m_SceneFramebuffer->Bind();

        context.BindTexture(ShaderBindingLayout::TEX_FLUID_DEPTH, fluidDepthID);
        context.BindTexture(ShaderBindingLayout::TEX_FLUID_THICKNESS, fluidThicknessID);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, refractionTexID);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthID);
        if (environmentMapID != 0)
            context.BindTexture(ShaderBindingLayout::TEX_ENVIRONMENT, environmentMapID);

        // The shader discards non-fluid pixels — no depth test, no blending,
        // no depth writes.
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(false);

        m_CompositeShader->Bind();
        const auto fullscreenTriangle = MeshPrimitives::GetFullscreenTriangle();
        fullscreenTriangle->Bind();
        context.DrawIndexed(fullscreenTriangle);

        // Restore scene-pass defaults.
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(GL_LESS);
        CommandDispatch::InvalidateRenderStateCache();

        // Unbind every sampler slot we touched — stale bindings leak into any
        // later pass sharing the sampler layout.
        context.BindTexture(ShaderBindingLayout::TEX_FLUID_DEPTH, 0);
        context.BindTexture(ShaderBindingLayout::TEX_FLUID_THICKNESS, 0);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_REFRACTION, 0);
        context.BindTexture(ShaderBindingLayout::TEX_WATER_DEPTH, 0);
        if (environmentMapID != 0)
            context.BindTexture(ShaderBindingLayout::TEX_ENVIRONMENT, 0);

        m_SceneFramebuffer->Unbind();
    }

    Ref<Framebuffer> FluidCompositePass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        return m_SceneFramebuffer;
    }
} // namespace OloEngine
