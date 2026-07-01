#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/DepthVelocityUpscalePass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace OloEngine
{
    DepthVelocityUpscalePass::DepthVelocityUpscalePass()
    {
        SetName("DepthVelocityUpscalePass");
    }

    void DepthVelocityUpscalePass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        if (!m_Enabled)
        {
            m_ReducedDepth = {};
            m_ReducedVelocity = {};
            return;
        }

        // Capture the CURRENT (reduced-resolution) depth + velocity handles as our
        // inputs BEFORE swapping the blackboard to the full-res outputs. The graph
        // orders this pass after their producers via these reads.
        m_ReducedDepth = blackboard.Scene.SceneDepth;
        m_ReducedVelocity = blackboard.GBuffer.Velocity;
        if (m_ReducedDepth.IsValid())
            (void)builder.Read(m_ReducedDepth, RGReadUsage::ShaderSample);
        if (m_ReducedVelocity.IsValid())
            (void)builder.Read(m_ReducedVelocity, RGReadUsage::ShaderSample);

        if (!blackboard.Post.UpscaledDepthVelocity.IsValid() || !m_ReducedDepth.IsValid())
            return;

        constexpr std::string_view versionTag = "DepthVelocityUpscalePass";
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.UpscaledDepthVelocity, RGWriteUsage::RenderTarget, versionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);

        // Publish the full-res depth/velocity as STABLE blackboard views. We do
        // NOT mutate blackboard.Scene.SceneDepth / GBuffer.Velocity here — the
        // graph runs a forward + reversed determinism build that shares the
        // persistent blackboard, so a Setup-time mutation reads back as a
        // self-feedback hazard. Instead, the post-band consumers (DOF, Fog,
        // MotionBlur, TAA, ToneMap) prefer these published views in their own
        // Setup (a read-only, order-independent decision).
        blackboard.Post.UpscaledSceneDepthTexture = builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::UpscaledSceneDepthTexture) + "@" + std::string(versionTag), outputHandle, 0u);
        blackboard.Post.UpscaledVelocityTexture = builder.CreateFramebufferAttachmentView(
            std::string(ResourceNames::UpscaledVelocityTexture) + "@" + std::string(versionTag), outputHandle, 1u);
    }

    void DepthVelocityUpscalePass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Target = nullptr; // graph-owned output, resolved per-frame in Execute

        m_Shader = Shader::Create("assets/shaders/PostProcess_UpscaleDepthVelocity.glsl");
        m_UBO = UniformBuffer::Create(EASUUBOData::GetSize(), ShaderBindingLayout::UBO_EASU);

        OLO_CORE_INFO("DepthVelocityUpscalePass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void DepthVelocityUpscalePass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Enabled || !m_ReducedDepth.IsValid() || !m_Shader || !m_UBO)
        {
            m_Target = nullptr;
            return;
        }

        const u32 depthTextureID = context.ResolveTexture(m_ReducedDepth);
        const u32 velocityTextureID = m_ReducedVelocity.IsValid() ? context.ResolveTexture(m_ReducedVelocity) : 0u;

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto resolvedOutput = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = resolvedOutput;
        }

        if (depthTextureID == 0u || !outputFramebuffer)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;
        outputFramebuffer->Bind();

        const auto& outSpec = outputFramebuffer->GetSpecification();
        const auto outW = outSpec.Width;
        const auto outH = outSpec.Height;
        context.SetViewport(0, 0, outW, outH);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);

        constexpr std::array<u32, 2> drawBuffers = { 0u, 1u };
        context.SetDrawBuffers(std::span<const u32>(drawBuffers.data(), drawBuffers.size()));

        context.SetClearColor({ 1.0f, 0.0f, 0.0f, 0.0f }); // depth cleared to far, velocity to zero
        context.Clear();

        m_Shader->Bind();

        context.BindTexture(0, depthTextureID);
        m_Shader->SetInt("u_Depth", 0);
        context.BindTexture(1, velocityTextureID);
        m_Shader->SetInt("u_Velocity", 1);

        const f32 scale = std::clamp(m_RenderScale, 0.25f, 1.0f);
        const auto renderW = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(outW) * scale)));
        const auto renderH = std::max(1u, static_cast<u32>(std::floor(static_cast<f32>(outH) * scale)));

        EASUUBOData data;
        data.InputSizeAndTexel = glm::vec4(static_cast<f32>(renderW), static_cast<f32>(renderH),
                                           1.0f / static_cast<f32>(renderW), 1.0f / static_cast<f32>(renderH));
        data.SampleBounds = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
        m_UBO->SetData(&data, EASUUBOData::GetSize());
        m_UBO->Bind();

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void DepthVelocityUpscalePass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Target = nullptr;
    }

    void DepthVelocityUpscalePass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Target = nullptr;
    }

    void DepthVelocityUpscalePass::OnReset()
    {
        m_Target = nullptr;
        m_ReducedDepth = {};
        m_ReducedVelocity = {};
    }
} // namespace OloEngine
