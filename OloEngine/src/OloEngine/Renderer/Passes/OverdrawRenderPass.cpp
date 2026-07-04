#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/OverdrawRenderPass.h"

#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

#include <span>
#include <string>

namespace OloEngine
{
    OverdrawRenderPass::OverdrawRenderPass()
    {
        SetName("OverdrawPass");
        OLO_CORE_INFO("Creating OverdrawRenderPass.");
    }

    void OverdrawRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_Target = nullptr;

        // Order after ScenePass so its opaque command bucket is already batched
        // when we replay it (mirrors PlanarReflectionRenderPass). Declared
        // unconditionally so the ordering holds even on a cached-graph frame.
        builder.DependsOnPass("ScenePass");

        // OverdrawColor is only declared when the debug view is on (its enable is
        // hashed into the blackboard fingerprint, so the graph rebuilds when it
        // flips). While off, downstream aliases straight back to the normal
        // composite and this pass does nothing.
        if (!m_Enabled || !blackboard.Post.OverdrawColor.IsValid())
            return;

        constexpr std::string_view overdrawVersionTag = "OverdrawPass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.OverdrawColor,
                                                          RGWriteUsage::RenderTarget, overdrawVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::OverdrawColorTexture) + "@" +
                                                        std::string(overdrawVersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void OverdrawRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_HeatmapShader = Shader::Create("assets/shaders/PostProcess_OverdrawHeatmap.glsl");

        OLO_CORE_INFO("OverdrawRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void OverdrawRenderPass::EnsureAccumFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        if (!m_AccumFramebuffer)
        {
            FramebufferSpecification spec;
            spec.Width = width;
            spec.Height = height;
            // Single HDR colour attachment, no depth: additive fragment counting
            // needs float (no [0,1] clamp) and no depth test at all.
            spec.Attachments = { FramebufferTextureFormat::RGBA16F };
            m_AccumFramebuffer = Framebuffer::Create(spec);
        }
        else if (m_AccumFramebuffer->GetSpecification().Width != width ||
                 m_AccumFramebuffer->GetSpecification().Height != height)
        {
            m_AccumFramebuffer->Resize(width, height);
        }
    }

    void OverdrawRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;

        if (!m_Enabled || !m_ScenePass)
            return;

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }

        const bool shaderReady = m_HeatmapShader && m_HeatmapShader->IsReady();
        if (!outputFramebuffer || !shaderReady)
        {
            if (static u32 s_MissingWarnings = 0; s_MissingWarnings++ < 10)
            {
                OLO_CORE_WARN("OverdrawRenderPass: missing output ({}) or heatmap shader ({})",
                              outputFramebuffer != nullptr, shaderReady);
            }
            return;
        }

        // Size the accumulation target to the graph output (kept at the live
        // viewport resolution) so the geometry rasterises at the same resolution
        // the frame is shown at — robust to viewport resize regardless of whether
        // this pass received a ResizeFramebuffer call.
        const u32 accumW = outputFramebuffer->GetSpecification().Width;
        const u32 accumH = outputFramebuffer->GetSpecification().Height;
        EnsureAccumFramebuffer(accumW, accumH);
        if (!m_AccumFramebuffer)
            return;

        auto& rendererAPI = RenderCommand::GetRendererAPI();

        // Restore the full core GL subset on exit so the geometry replay (which
        // rebinds the scene camera, FBO, program, blend/depth state) cannot poison
        // the passes that follow — same guard PlanarReflectionRenderPass uses.
        GLStateGuard guard("OverdrawRenderPass", GLStateGuard::Policy::Restore);

        // ---- Pass 1: accumulate per-pixel overdraw count -------------------
        m_AccumFramebuffer->Bind();
        RenderCommand::SetViewport(0, 0, accumW, accumH);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        // Start every pixel at zero; the additive replay adds 1 per covered fragment.
        m_AccumFramebuffer->ClearAllAttachments({ 0.0f, 0.0f, 0.0f, 0.0f }, -1);

        // Re-establish shared scene resources the scene pass left bound (camera UBO,
        // shadow maps, IBL — the depth-only overdraw programs only need the camera
        // UBO + instance SSBO, but rebinding is cheap and matches the reflection
        // replay) and re-run the already-batched opaque bucket with the overdraw
        // shader swap + additive/depth-off state active.
        CommandDispatch::BindSceneResources();
        CommandDispatch::SetOverdrawActive(true);
        m_ScenePass->GetCommandBucket().Execute(rendererAPI);
        CommandDispatch::SetOverdrawActive(false);
        CommandDispatch::InvalidateRenderStateCache();
        m_AccumFramebuffer->Unbind();

        // ---- Pass 2: heat-map the count into the graph output --------------
        constexpr u32 colorAttachment = 0;
        outputFramebuffer->Bind();
        RenderCommand::SetViewport(0, 0, outputFramebuffer->GetSpecification().Width,
                                   outputFramebuffer->GetSpecification().Height);
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::SetBlendState(false);
        RenderCommand::DisableCulling();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_HeatmapShader->Bind();
        context.BindTexture(0, m_AccumFramebuffer->GetColorAttachmentRendererID(0));

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        outputFramebuffer->Unbind();
        m_Target = outputFramebuffer;
    }

    void OverdrawRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
    }

    void OverdrawRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        if (m_AccumFramebuffer)
            m_AccumFramebuffer->Resize(width, height);
    }

    void OverdrawRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
