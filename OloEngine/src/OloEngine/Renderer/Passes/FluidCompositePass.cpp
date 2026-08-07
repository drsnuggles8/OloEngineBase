#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FluidCompositePass.h"

#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

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

        const RHI::ResourceHandle fluidDepthID = m_IntermediatesPass->GetSmoothedDepthTextureID();
        const RHI::ResourceHandle fluidThicknessID = m_IntermediatesPass->GetThicknessTextureID();
        if (!fluidDepthID.IsValid() || !fluidThicknessID.IsValid())
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

        RHI::ResourceHandle sceneColorID{};
        RHI::ResourceHandle sceneDepthID{};
        RHI::ResourceHandle refractionTexID{};
        if (m_SelectedSceneColorTexture.IsValid())
            sceneColorID = context.ResolveTextureHandle(m_SelectedSceneColorTexture);
        if (m_SelectedSceneDepthTexture.IsValid())
            sceneDepthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedRefractionTexture.IsValid())
            refractionTexID = context.ResolveTextureHandle(m_SelectedRefractionTexture);
        if (!sceneColorID.IsValid() || !sceneDepthID.IsValid() || !refractionTexID.IsValid())
            return;

        GLStateGuard guard("FluidCompositePass", GLStateGuard::Policy::Ignore);

        // Snapshot the pre-fluid scene colour for refraction sampling.
        RenderCommand::CopyImageSubData(sceneColorID, RendererAPI::TextureTargetType::Texture2D,
                                        refractionTexID, RendererAPI::TextureTargetType::Texture2D,
                                        fbWidth, fbHeight);

        // Upload the appearance parameters of this frame's fluid. Counts.z
        // carries the environment-map-present flag for the reflection branch.
        const RHI::ResourceHandle environmentMap = Renderer3D::GetGlobalEnvironmentMapHandle();
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
                                    environmentMap.IsValid() ? 1u : 0u, 0u);
            m_FluidRenderUBO->SetData(&ubo, sizeof(ubo));
            m_FluidRenderUBO->Bind();
        }

        m_SceneFramebuffer->Bind();

        // The shader discards non-fluid pixels — no depth test, no blending,
        // no depth writes.
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetBlendState(false);

        // BIND THE SHADER BEFORE ITS INPUTS. The binding seam asks
        // Shader::IsBoundProgramBindless() to choose between writing an offset and
        // issuing a bind, and that describes the program CURRENTLY in flight — so
        // binding first makes every input take the fallback path while the
        // composite shader, which IS the bindless variant, reads offsets nobody
        // wrote. The fluid then renders completely transparent, with no error
        // anywhere (glsl-shaders.md 5b).
        m_CompositeShader->Bind();

        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_FLUID_DEPTH, fluidDepthID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_FLUID_THICKNESS, fluidThicknessID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_WATER_REFRACTION, refractionTexID, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_WATER_DEPTH, sceneDepthID, RHI::HeapSlotLifetime::FrameTransient);
        if (environmentMap.IsValid())
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_ENVIRONMENT, environmentMap,
                                            RHI::HeapSlotLifetime::FrameTransient, HeapBinding::CubeSampler(),
                                            RHI::NullSamplerKind::Cube);
        const auto fullscreenTriangle = MeshPrimitives::GetFullscreenTriangle();
        fullscreenTriangle->Bind();
        context.FlushHeapOffsets();
        context.DrawIndexed(fullscreenTriangle);

        // Restore scene-pass defaults.
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::SetDepthFunc(RHI::CompareOp::Less);
        CommandDispatch::InvalidateRenderStateCache();

        // Unbind every sampler slot we touched — stale bindings leak into any
        // later pass sharing the sampler layout.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_FLUID_DEPTH, RHI::NullResource, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_FLUID_THICKNESS, RHI::NullResource, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_WATER_REFRACTION, RHI::NullResource, RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_WATER_DEPTH, RHI::NullResource, RHI::HeapSlotLifetime::FrameTransient);
        if (environmentMap.IsValid())
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_ENVIRONMENT, RHI::NullResource,
                                            RHI::HeapSlotLifetime::FrameTransient, HeapBinding::CubeSampler(),
                                            RHI::NullSamplerKind::Cube);

        m_SceneFramebuffer->Unbind();
    }

    Ref<Framebuffer> FluidCompositePass::GetTarget() const
    {
        OLO_PROFILE_FUNCTION();
        return m_SceneFramebuffer;
    }
} // namespace OloEngine
