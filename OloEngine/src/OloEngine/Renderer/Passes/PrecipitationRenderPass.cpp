#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/PrecipitationRenderPass.h"

#include "OloEngine/Precipitation/ScreenSpacePrecipitation.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

namespace OloEngine
{
    PrecipitationRenderPass::PrecipitationRenderPass()
    {
        SetName("PrecipitationPass");
    }

    void PrecipitationRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_PrecipitationShader = Shader::Create("assets/shaders/PostProcess_Precipitation.glsl");
        m_PrecipitationScreenUBO = UniformBuffer::Create(
            PrecipitationScreenUBOData::GetSize(), ShaderBindingLayout::UBO_PRECIPITATION_SCREEN);

        DeclareRead(ResourceNames::TAAColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::PrecipitationColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("PrecipitationRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void PrecipitationRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("PrecipitationRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_OutputFB = nullptr;
            return;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = width;
        fbSpec.Height = height;
        fbSpec.Samples = 1;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA16F };

        m_OutputFB = Framebuffer::Create(fbSpec);
    }

    void PrecipitationRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void PrecipitationRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 44 — self-resolving input framebuffer from the render-graph
        // blackboard. Preference chain: TAA > MotionBlur > DOF > Bloom > PostProcess.
        Ref<Framebuffer> inputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->TAAColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->MotionBlurColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->DOFColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->BloomColor))
                    inputFramebuffer = fb;
            if (!inputFramebuffer)
                if (auto fb = context.ResolveFramebuffer(board->PostProcessColor))
                    inputFramebuffer = fb;
        }
        if (!m_Enabled)
            return;

        if (!inputFramebuffer || !m_OutputFB || !m_PrecipitationShader || !m_PrecipitationScreenUBO)
            return;

        m_OutputFB->Bind();

        const auto& outSpec = m_OutputFB->GetSpecification();
        context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetBlendState(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_PrecipitationShader->Bind();

        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        m_PrecipitationShader->SetInt("u_Texture", 0);

        {
            PrecipitationScreenUBOData uboData;
            uboData.StreakParams = ScreenSpacePrecipitation::GetStreakParams();
            const auto lensData = ScreenSpacePrecipitation::GetLensImpactGPUData();
            for (u32 i = 0; i < ScreenSpacePrecipitation::MAX_LENS_IMPACTS; ++i)
            {
                uboData.LensImpacts[i].PositionAndSize = lensData[i].PositionAndSize;
                uboData.LensImpacts[i].TimeParams = lensData[i].TimeParams;
            }
            m_PrecipitationScreenUBO->SetData(&uboData, PrecipitationScreenUBOData::GetSize());
            m_PrecipitationScreenUBO->Bind();
        }

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        m_OutputFB->Unbind();
    }

    Ref<Framebuffer> PrecipitationRenderPass::GetTarget() const
    {
        if (!m_Enabled || !m_OutputFB)
            return nullptr;
        return m_OutputFB;
    }

    void PrecipitationRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void PrecipitationRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void PrecipitationRenderPass::OnReset() {}
} // namespace OloEngine
