#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/PostProcessRenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    PostProcessRenderPass::PostProcessRenderPass()
    {
        SetName("PostProcessPass");
        OLO_CORE_INFO("Creating PostProcessRenderPass.");
    }

    void PostProcessRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // Create ping-pong framebuffers (RGBA16F, no depth)
        CreatePingPongFramebuffers(spec.Width, spec.Height);

        // Create fullscreen triangle (identical to FinalRenderPass)
        struct FullscreenVertex
        {
            glm::vec3 Position;
            glm::vec2 TexCoord;
        };

        static_assert(sizeof(FullscreenVertex) == sizeof(f32) * 5,
                      "FullscreenVertex must be exactly 5 floats");

        FullscreenVertex vertices[3] = {
            { { -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { 3.0f, -1.0f, 0.0f }, { 2.0f, 0.0f } },
            { { -1.0f, 3.0f, 0.0f }, { 0.0f, 2.0f } }
        };

        u32 indices[3] = { 0, 1, 2 };

        m_FullscreenTriangleVA = VertexArray::Create();

        Ref<VertexBuffer> vertexBuffer = VertexBuffer::Create(
            static_cast<const void*>(vertices),
            static_cast<u32>(sizeof(vertices)));

        vertexBuffer->SetLayout({ { ShaderDataType::Float3, "a_Position" },
                                  { ShaderDataType::Float2, "a_TexCoord" } });

        Ref<IndexBuffer> indexBuffer = IndexBuffer::Create(indices, 3);

        m_FullscreenTriangleVA->AddVertexBuffer(vertexBuffer);
        m_FullscreenTriangleVA->SetIndexBuffer(indexBuffer);

        // Load all effect shaders
        m_BloomThresholdShader = Shader::Create("assets/shaders/PostProcess_BloomThreshold.glsl");
        m_BloomDownsampleShader = Shader::Create("assets/shaders/PostProcess_BloomDownsample.glsl");
        m_BloomUpsampleShader = Shader::Create("assets/shaders/PostProcess_BloomUpsample.glsl");
        m_BloomCompositeShader = Shader::Create("assets/shaders/PostProcess_BloomComposite.glsl");
        m_VignetteShader = Shader::Create("assets/shaders/PostProcess_Vignette.glsl");
        m_ChromaticAberrationShader = Shader::Create("assets/shaders/PostProcess_ChromaticAberration.glsl");
        m_ColorGradingShader = Shader::Create("assets/shaders/PostProcess_ColorGrading.glsl");
        m_ToneMapShader = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");
        m_FXAAShader = Shader::Create("assets/shaders/PostProcess_FXAA.glsl");
        m_DOFShader = Shader::Create("assets/shaders/PostProcess_DOF.glsl");
        m_MotionBlurShader = Shader::Create("assets/shaders/PostProcess_MotionBlur.glsl");
        m_SSAOApplyShader = Shader::Create("assets/shaders/PostProcess_SSAOApply.glsl");

        // Create bloom mip chain
        CreateBloomMipChain(spec.Width, spec.Height);

        OLO_CORE_INFO("PostProcessRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void PostProcessRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input)
    {
        m_InputFramebuffer = input;
    }

    void PostProcessRenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_InputFramebuffer)
        {
            OLO_CORE_ERROR("PostProcessRenderPass::Execute: No input framebuffer!");
            return;
        }

        // If no effects are enabled, skip — GetTarget() returns the input framebuffer directly
        bool anyEffectEnabled = m_Settings.BloomEnabled || m_Settings.VignetteEnabled || m_Settings.ChromaticAberrationEnabled || m_Settings.FXAAEnabled || m_Settings.DOFEnabled || m_Settings.MotionBlurEnabled || m_Settings.ColorGradingEnabled || (m_Settings.SSAOEnabled && m_SSAOTextureID != 0);

        // Always run tone mapping if we have the shader (it's the core of post-processing)
        if (!anyEffectEnabled && !m_ToneMapShader)
        {
            // No effects and no tone mapping — GetTarget() should return the input FB directly
            m_SkippedThisFrame = true;
            return;
        }
        m_SkippedThisFrame = false;

        // Set up the ping-pong chain
        Ref<Framebuffer> currentSource = m_InputFramebuffer;
        bool writeToP = true;

        auto applyEffect = [&](const Ref<Shader>& shader)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            RunEffect(shader, currentSource, dest);
            currentSource = dest;
            writeToP = !writeToP;
        };

        // === Effect chain order ===
        // 0. SSAO Apply (modulate scene color by AO factor)
        if (m_Settings.SSAOEnabled && m_SSAOApplyShader && m_SSAOTextureID != 0)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetBlendState(false);

            m_SSAOApplyShader->Bind();
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcColorID);
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_SSAO, m_SSAOTextureID);

            // Bind full-res scene depth for bilateral upsampling
            if (m_SceneDepthFB)
            {
                u32 depthID = m_SceneDepthFB->GetDepthAttachmentRendererID();
                RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);
            }

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 1. Bloom (threshold → downsample → upsample → composite)
        if (m_Settings.BloomEnabled && m_BloomThresholdShader && !m_BloomMipChain.empty())
        {
            ExecuteBloom(currentSource);
            // Composite bloom onto current source
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;

            dest->Bind();
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetBlendState(false);

            m_BloomCompositeShader->Bind();
            // Bind scene color at slot 0
            u32 sceneColorID = currentSource->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, sceneColorID);
            m_BloomCompositeShader->SetInt("u_SceneColor", 0);
            // Bind bloom result at slot 1
            u32 bloomColorID = m_BloomMipChain[0]->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(1, bloomColorID);
            m_BloomCompositeShader->SetInt("u_BloomColor", 1);

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 2. DOF
        if (m_Settings.DOFEnabled && m_DOFShader && m_SceneDepthFB)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetBlendState(false);

            m_DOFShader->Bind();
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcColorID);

            // Bind scene depth at slot 19
            u32 depthID = m_SceneDepthFB->GetDepthAttachmentRendererID();
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

            // UBO already has CameraNear/CameraFar from EndScene upload

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 3. Motion Blur
        if (m_Settings.MotionBlurEnabled && m_MotionBlurShader && m_SceneDepthFB)
        {
            Ref<Framebuffer> dest = writeToP ? m_PingFB : m_PongFB;
            dest->Bind();
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetBlendState(false);

            m_MotionBlurShader->Bind();
            u32 srcColorID = currentSource->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcColorID);
            m_MotionBlurShader->SetInt("u_Texture", 0);

            u32 depthID = m_SceneDepthFB->GetDepthAttachmentRendererID();
            RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);
            m_MotionBlurShader->SetInt("u_DepthTexture", ShaderBindingLayout::TEX_POSTPROCESS_DEPTH);

            DrawFullscreenTriangle();
            dest->Unbind();

            currentSource = dest;
            writeToP = !writeToP;
        }

        // 4. Chromatic Aberration
        if (m_Settings.ChromaticAberrationEnabled && m_ChromaticAberrationShader)
        {
            applyEffect(m_ChromaticAberrationShader);
        }

        // 5. Color Grading
        if (m_Settings.ColorGradingEnabled && m_ColorGradingShader)
        {
            applyEffect(m_ColorGradingShader);
        }

        // 6. Tone Mapping (HDR → LDR)
        if (m_ToneMapShader)
        {
            applyEffect(m_ToneMapShader);
        }

        // 7. Vignette (operates on LDR)
        if (m_Settings.VignetteEnabled && m_VignetteShader)
        {
            applyEffect(m_VignetteShader);
        }

        // 8. FXAA (must be last spatial filter, operates on LDR)
        if (m_Settings.FXAAEnabled && m_FXAAShader)
        {
            applyEffect(m_FXAAShader);
        }

        m_LastWrittenIsPing = !writeToP;
    }

    void PostProcessRenderPass::RunEffect(const Ref<Shader>& shader, Ref<Framebuffer> srcFB, Ref<Framebuffer> dstFB)
    {
        OLO_PROFILE_FUNCTION();

        dstFB->Bind();
        RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        RenderCommand::Clear();

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        shader->Bind();

        // Bind source framebuffer color attachment as texture at slot 0
        u32 srcColorID = srcFB->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, srcColorID);
        shader->SetInt("u_Texture", 0);

        DrawFullscreenTriangle();

        dstFB->Unbind();
    }

    void PostProcessRenderPass::DrawFullscreenTriangle()
    {
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    Ref<Framebuffer> PostProcessRenderPass::GetTarget() const
    {
        // If we skipped execution (no effects, no tone map), pass through the input
        if (m_SkippedThisFrame || !m_PingFB)
        {
            return m_InputFramebuffer;
        }

        return m_LastWrittenIsPing ? m_PingFB : m_PongFB;
    }

    void PostProcessRenderPass::CreatePingPongFramebuffers(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        FramebufferSpecification pingPongSpec;
        pingPongSpec.Width = width;
        pingPongSpec.Height = height;
        pingPongSpec.Samples = 1;
        pingPongSpec.Attachments = {
            FramebufferTextureFormat::RGBA16F
        };

        m_PingFB = Framebuffer::Create(pingPongSpec);
        m_PongFB = Framebuffer::Create(pingPongSpec);

        OLO_CORE_INFO("PostProcessRenderPass: Created ping-pong framebuffers {}x{}", width, height);
    }

    void PostProcessRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("PostProcessRenderPass::SetupFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (!m_PingFB)
        {
            CreatePingPongFramebuffers(width, height);
        }
        else
        {
            m_PingFB->Resize(width, height);
            m_PongFB->Resize(width, height);
        }
    }

    void PostProcessRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("PostProcessRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        if (m_PingFB)
        {
            m_PingFB->Resize(width, height);
        }
        if (m_PongFB)
        {
            m_PongFB->Resize(width, height);
        }

        // Resize bloom mip chain
        CreateBloomMipChain(width, height);

        OLO_CORE_INFO("PostProcessRenderPass: Resized to {}x{}", width, height);
    }

    void PostProcessRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
        {
            CreatePingPongFramebuffers(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            CreateBloomMipChain(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
            OLO_CORE_INFO("PostProcessRenderPass reset with dimensions {}x{}",
                          m_FramebufferSpec.Width, m_FramebufferSpec.Height);
        }
    }

    void PostProcessRenderPass::CreateBloomMipChain(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_BloomMipChain.clear();

        u32 mipWidth = width / 2;
        u32 mipHeight = height / 2;

        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (mipWidth < 2 || mipHeight < 2)
            {
                break;
            }

            FramebufferSpecification mipSpec;
            mipSpec.Width = mipWidth;
            mipSpec.Height = mipHeight;
            mipSpec.Samples = 1;
            mipSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

            m_BloomMipChain.push_back(Framebuffer::Create(mipSpec));

            mipWidth /= 2;
            mipHeight /= 2;
        }

        OLO_CORE_INFO("PostProcessRenderPass: Created bloom mip chain with {} levels", m_BloomMipChain.size());
    }

    void PostProcessRenderPass::ExecuteBloom(Ref<Framebuffer> sceneColorFB)
    {
        OLO_PROFILE_FUNCTION();

        if (m_BloomMipChain.empty())
        {
            return;
        }

        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        // Step 1: Threshold extract — scene HDR → bloom mip 0
        {
            m_BloomMipChain[0]->Bind();
            const auto& spec = m_BloomMipChain[0]->GetSpecification();
            RenderCommand::SetViewport(0, 0, spec.Width, spec.Height);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();

            m_BloomThresholdShader->Bind();
            u32 srcID = sceneColorFB->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcID);
            m_BloomThresholdShader->SetInt("u_Texture", 0);

            DrawFullscreenTriangle();
            m_BloomMipChain[0]->Unbind();
        }

        // Step 2: Progressive downsample
        for (size_t i = 1; i < m_BloomMipChain.size(); ++i)
        {
            auto srcMip = m_BloomMipChain[i - 1];
            auto dstMip = m_BloomMipChain[i];
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            RenderCommand::SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::Clear();

            m_BloomDownsampleShader->Bind();
            u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcID);

            // Update texel size in UBO for this mip level
            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            DrawFullscreenTriangle();
            dstMip->Unbind();
        }

        // Step 3: Progressive upsample (accumulate back up the chain)
        for (int i = static_cast<int>(m_BloomMipChain.size()) - 2; i >= 0; --i)
        {
            auto srcMip = m_BloomMipChain[static_cast<size_t>(i) + 1]; // smaller mip
            auto dstMip = m_BloomMipChain[static_cast<size_t>(i)];     // larger mip (accumulate into)
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            RenderCommand::SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            // Enable additive blending AFTER Bind() to be immune to Bind() resetting blend state
            RenderCommand::SetBlendState(true);
            RenderCommand::SetBlendFunc(GL_ONE, GL_ONE);
            // Don't clear — we're ADDING to existing content

            m_BloomUpsampleShader->Bind();
            u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            RenderCommand::BindTexture(0, srcID);

            // Update texel size in UBO for this mip level
            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            DrawFullscreenTriangle();
            dstMip->Unbind();
        }

        RenderCommand::SetBlendState(false);

        // Restore TexelSize to full resolution so subsequent effects (FXAA etc.) don't read stale mip values
        if (m_GPUData && m_PostProcessUBO)
        {
            m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(m_FramebufferSpec.Width);
            m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(m_FramebufferSpec.Height);
            m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
        }

        // Restore full viewport for subsequent passes
        RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }
} // namespace OloEngine
