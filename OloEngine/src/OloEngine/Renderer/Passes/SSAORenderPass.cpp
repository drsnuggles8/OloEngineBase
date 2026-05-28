#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <array>
#include <random>

namespace OloEngine
{
    SSAORenderPass::SSAORenderPass()
    {
        SetName("SSAOPass");
    }

    void SSAORenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedAOOutputTexture = {};
        m_SelectedBlurFramebuffer = {};

        if (!m_Settings.SSAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::SSAO)
            return;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }
        if (blackboard.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsTexture = blackboard.Scene.SceneNormals;
            [[maybe_unused]] const auto sceneNormalsRead = builder.Read(blackboard.Scene.SceneNormals, RGReadUsage::ShaderSample);
        }
        if (blackboard.AO.AOBuffer.IsValid())
        {
            m_SelectedAOOutputTexture = blackboard.AO.AOBuffer;
            builder.Write(blackboard.AO.AOBuffer, RGWriteUsage::RenderTarget);
        }

        if (blackboard.Scratch.SSAORaw.IsValid())
        {
            SetPrimaryOutputFramebufferHandle(blackboard.Scratch.SSAORaw);
            // Intra-pass write-then-sample: Pass 1 renders raw SSAO into this
            // framebuffer; Pass 2 (bilateral blur) samples it back as a
            // texture within the same Execute. Graph-owned scratch with no
            // prior writer to chain against.
            builder.AllowSamePassReadWrite(blackboard.Scratch.SSAORaw);
            builder.Write(blackboard.Scratch.SSAORaw, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto rawRead = builder.Read(blackboard.Scratch.SSAORaw, RGReadUsage::RenderTargetRead);
        }

        if (blackboard.Scratch.SSAOBlur.IsValid())
        {
            m_SelectedBlurFramebuffer = blackboard.Scratch.SSAOBlur;
            builder.Write(blackboard.Scratch.SSAOBlur, RGWriteUsage::RenderTarget);
        }
    }

    SSAORenderPass::~SSAORenderPass()
    {
        if (m_NoiseTexture != 0)
        {
            RenderCommand::DeleteTexture(m_NoiseTexture);
        }
    }

    void SSAORenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_HalfWidth = std::max(1u, spec.Width / 2);
        m_HalfHeight = std::max(1u, spec.Height / 2);

        // Load SSAO shaders
        m_SSAOShader = Shader::Create("assets/shaders/SSAO.glsl");
        m_SSAOBlurShader = Shader::Create("assets/shaders/SSAO_Blur.glsl");

        // Create 4x4 noise texture for random rotation
        CreateNoiseTexture();

        OLO_CORE_INFO("SSAORenderPass: Initialized with half-res {}x{}", m_HalfWidth, m_HalfHeight);
    }

    void SSAORenderPass::CreateNoiseTexture()
    {
        OLO_PROFILE_FUNCTION();

        // Generate 4x4 random rotation vectors in tangent space (xy rotation, z=0)
        std::mt19937 gen(42); // Fixed seed for deterministic noise
        std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);

        std::array<glm::vec2, 16> noise;
        for (auto& n : noise)
        {
            glm::vec2 v(dist(gen), dist(gen));
            f32 len = glm::length(v);
            n = (len > 1e-6f) ? v / len : glm::vec2(1.0f, 0.0f);
        }

        m_NoiseTexture = RenderCommand::CreateTexture2D(4, 4, GL_RG16F);
        RenderCommand::UploadTextureSubImage2D(m_NoiseTexture, 4, 4, GL_RG, GL_FLOAT, noise.data());
        RenderCommand::SetTextureParameter(m_NoiseTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        RenderCommand::SetTextureParameter(m_NoiseTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        RenderCommand::SetTextureParameter(m_NoiseTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);
        RenderCommand::SetTextureParameter(m_NoiseTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    void SSAORenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;

        if (!m_Settings.SSAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::SSAO || !IsReadyForExecution())
        {
            return;
        }

        // Phase F slice 37 — self-resolving SceneDepth and SceneNormals: look
        // up directly from the render graph blackboard so no per-frame
        // side-channel setter calls are needed from EndScene().
        u32 depthID = 0;
        u32 normalsID = 0;
        u32 aoOutputTexID = 0;
        if (m_SelectedSceneDepthTexture.IsValid())
            depthID = context.ResolveTexture(m_SelectedSceneDepthTexture);
        if (m_SelectedSceneNormalsTexture.IsValid())
            normalsID = context.ResolveTexture(m_SelectedSceneNormalsTexture);
        if (m_SelectedAOOutputTexture.IsValid())
            aoOutputTexID = context.ResolveTexture(m_SelectedAOOutputTexture);
        if (depthID == 0 || normalsID == 0 || aoOutputTexID == 0)
        {
            return;
        }

        // Phase D / H follow-up: resolve the raw SSAO scratch framebuffer from
        // the transient pool via the blackboard. This pass now requires the
        // graph-owned scratch target; the owned fallback framebuffer has been
        // retired.
        Ref<Framebuffer> rawFB;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            rawFB = context.ResolveFramebuffer(outputHandle);
        Ref<Framebuffer> blurFB;
        if (m_SelectedBlurFramebuffer.IsValid())
            blurFB = context.ResolveFramebuffer(m_SelectedBlurFramebuffer);
        if (!rawFB || !blurFB)
            return;

        m_Target = blurFB;

        // (Dropped the per-frame "inputs depthTex=N" trace: the AO output is
        // double-buffered so the texture ID flips every frame and the dedup
        // never held — fired ~60 times per second. Same broken pattern as the
        // GTAORenderPass / AOApplyRenderPass logs that were removed earlier.)

        // Upload SSAO parameters to UBO
        if (m_SSAOUBO && m_GPUData)
        {
            m_GPUData->Radius = m_Settings.SSAORadius;
            m_GPUData->Bias = m_Settings.SSAOBias;
            m_GPUData->Intensity = m_Settings.SSAOIntensity;
            m_GPUData->Samples = m_Settings.SSAOSamples;
            m_GPUData->ScreenWidth = static_cast<i32>(m_HalfWidth);
            m_GPUData->ScreenHeight = static_cast<i32>(m_HalfHeight);
            m_SSAOUBO->SetData(m_GPUData, SSAOUBOData::GetSize());
            m_SSAOUBO->Bind();
        }

        // --- Pass 1: Generate raw SSAO ---
        rawFB->Bind();
        context.SetViewport(0, 0, m_HalfWidth, m_HalfHeight);
        context.SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f }); // White = no occlusion
        context.Clear();
        context.SetDepthTest(false);
        context.SetBlendState(false);

        m_SSAOShader->Bind();

        // Bind scene depth at TEX_POSTPROCESS_DEPTH (slot 19)
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        // Bind scene view-space normals at TEX_SCENE_NORMALS (slot 22)
        context.BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsID);

        // Bind noise texture at TEX_SSAO_NOISE (slot 21)
        context.BindTexture(ShaderBindingLayout::TEX_SSAO_NOISE, m_NoiseTexture);

        DrawFullscreenTriangle();
        rawFB->Unbind();

        // --- Pass 2: Bilateral blur ---
        blurFB->Bind();
        context.SetViewport(0, 0, m_HalfWidth, m_HalfHeight);
        context.SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f });
        context.Clear();
        context.SetDepthTest(false);
        context.SetBlendState(false);

        m_SSAOBlurShader->Bind();

        // Bind raw SSAO result at slot 0 (texture from the transient or fallback FB)
        u32 rawSSAOID = rawFB->GetColorAttachmentRendererID(0);
        context.BindTexture(0, rawSSAOID);

        // Bind scene depth at TEX_POSTPROCESS_DEPTH (slot 19) for bilateral edge detection
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle();
        blurFB->Unbind();

        if (const u32 blurredAOTextureID = blurFB->GetColorAttachmentRendererID(0); blurredAOTextureID != 0 && blurredAOTextureID != aoOutputTexID)
        {
            glCopyImageSubData(
                blurredAOTextureID, GL_TEXTURE_2D, 0, 0, 0, 0,
                aoOutputTexID, GL_TEXTURE_2D, 0, 0, 0, 0,
                static_cast<GLsizei>(m_HalfWidth), static_cast<GLsizei>(m_HalfHeight), 1);
        }

        // Restore full-res viewport (will be set by next pass anyway, but be clean)
        context.SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SSAORenderPass::DrawFullscreenTriangle()
    {
        auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);
    }

    void SSAORenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_HalfWidth = std::max(1u, width / 2);
        m_HalfHeight = std::max(1u, height / 2);
        m_Target = nullptr;
    }

    void SSAORenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_HalfWidth = std::max(1u, width / 2);
        m_HalfHeight = std::max(1u, height / 2);
        m_Target = nullptr;
    }

    void SSAORenderPass::OnReset()
    {
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SelectedAOOutputTexture = {};
        m_SelectedBlurFramebuffer = {};
        m_Target = nullptr;
    }
} // namespace OloEngine
