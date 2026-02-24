#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SSAORenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include <random>

namespace OloEngine
{
    SSAORenderPass::SSAORenderPass()
    {
        SetName("SSAOPass");
    }

    void SSAORenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        m_HalfWidth = std::max(1u, spec.Width / 2);
        m_HalfHeight = std::max(1u, spec.Height / 2);

        CreateSSAOFramebuffers(m_HalfWidth, m_HalfHeight);

        // Create fullscreen triangle (same pattern as PostProcessRenderPass)
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

        // Load SSAO shaders
        m_SSAOShader = Shader::Create("assets/shaders/SSAO.glsl");
        m_SSAOBlurShader = Shader::Create("assets/shaders/SSAO_Blur.glsl");

        // Create 4x4 noise texture for random rotation
        CreateNoiseTexture();

        OLO_CORE_INFO("SSAORenderPass: Initialized with half-res {}x{}", m_HalfWidth, m_HalfHeight);
    }

    void SSAORenderPass::CreateSSAOFramebuffers(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        FramebufferSpecification ssaoSpec;
        ssaoSpec.Width = width;
        ssaoSpec.Height = height;
        ssaoSpec.Samples = 1;
        ssaoSpec.Attachments = { FramebufferTextureFormat::RG16F };

        m_SSAOFramebuffer = Framebuffer::Create(ssaoSpec);
        m_BlurFramebuffer = Framebuffer::Create(ssaoSpec);
    }

    void SSAORenderPass::CreateNoiseTexture()
    {
        // Generate 4x4 random rotation vectors in tangent space (xy rotation, z=0)
        std::mt19937 gen(42); // Fixed seed for deterministic noise
        std::uniform_real_distribution<f32> dist(-1.0f, 1.0f);

        std::array<glm::vec2, 16> noise;
        for (auto& n : noise)
        {
            n = glm::normalize(glm::vec2(dist(gen), dist(gen)));
        }

        glCreateTextures(GL_TEXTURE_2D, 1, &m_NoiseTexture);
        glTextureStorage2D(m_NoiseTexture, 1, GL_RG16F, 4, 4);
        glTextureSubImage2D(m_NoiseTexture, 0, 0, 0, 4, 4, GL_RG, GL_FLOAT, noise.data());
        glTextureParameteri(m_NoiseTexture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_NoiseTexture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_NoiseTexture, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_NoiseTexture, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    void SSAORenderPass::Execute()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Settings.SSAOEnabled || !m_SceneFramebuffer || !m_SSAOShader || !m_SSAOBlurShader)
        {
            return;
        }

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
        }

        // --- Pass 1: Generate raw SSAO ---
        m_SSAOFramebuffer->Bind();
        RenderCommand::SetViewport(0, 0, m_HalfWidth, m_HalfHeight);
        RenderCommand::SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f }); // White = no occlusion
        RenderCommand::Clear();
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        m_SSAOShader->Bind();

        // Bind scene depth at TEX_POSTPROCESS_DEPTH (slot 19)
        u32 depthID = m_SceneFramebuffer->GetDepthAttachmentRendererID();
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        // Bind scene view-space normals at TEX_SCENE_NORMALS (slot 22)
        u32 normalsID = m_SceneFramebuffer->GetColorAttachmentRendererID(2);
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SCENE_NORMALS, normalsID);

        // Bind noise texture at TEX_SSAO_NOISE (slot 21)
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_SSAO_NOISE, m_NoiseTexture);

        DrawFullscreenTriangle();
        m_SSAOFramebuffer->Unbind();

        // --- Pass 2: Bilateral blur ---
        m_BlurFramebuffer->Bind();
        RenderCommand::SetViewport(0, 0, m_HalfWidth, m_HalfHeight);
        RenderCommand::SetClearColor({ 1.0f, 1.0f, 1.0f, 1.0f });
        RenderCommand::Clear();
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetBlendState(false);

        m_SSAOBlurShader->Bind();

        // Bind raw SSAO result at slot 0
        u32 rawSSAOID = m_SSAOFramebuffer->GetColorAttachmentRendererID(0);
        RenderCommand::BindTexture(0, rawSSAOID);

        // Bind scene depth at TEX_POSTPROCESS_DEPTH (slot 19) for bilateral edge detection
        RenderCommand::BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthID);

        DrawFullscreenTriangle();
        m_BlurFramebuffer->Unbind();

        // Restore full-res viewport (will be set by next pass anyway, but be clean)
        RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
    }

    void SSAORenderPass::DrawFullscreenTriangle()
    {
        m_FullscreenTriangleVA->Bind();
        RenderCommand::DrawIndexed(m_FullscreenTriangleVA);
    }

    u32 SSAORenderPass::GetSSAOTextureID() const
    {
        if (!m_Settings.SSAOEnabled || !m_BlurFramebuffer)
        {
            return 0;
        }
        return m_BlurFramebuffer->GetColorAttachmentRendererID(0);
    }

    Ref<Framebuffer> SSAORenderPass::GetTarget() const
    {
        return m_BlurFramebuffer;
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

        if (!m_SSAOFramebuffer)
        {
            CreateSSAOFramebuffers(m_HalfWidth, m_HalfHeight);
        }
        else
        {
            m_SSAOFramebuffer->Resize(m_HalfWidth, m_HalfHeight);
            m_BlurFramebuffer->Resize(m_HalfWidth, m_HalfHeight);
        }
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

        if (m_SSAOFramebuffer)
        {
            m_SSAOFramebuffer->Resize(m_HalfWidth, m_HalfHeight);
        }
        if (m_BlurFramebuffer)
        {
            m_BlurFramebuffer->Resize(m_HalfWidth, m_HalfHeight);
        }
    }

    void SSAORenderPass::OnReset()
    {
        // Nothing to reset per-frame
    }
} // namespace OloEngine
