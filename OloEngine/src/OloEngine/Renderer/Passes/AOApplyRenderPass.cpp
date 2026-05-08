#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/AOApplyRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Framebuffer.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    AOApplyRenderPass::AOApplyRenderPass()
    {
        SetName("AOApplyPass");
        OLO_CORE_INFO("Creating AOApplyRenderPass.");
    }

    void AOApplyRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        CreateFramebuffer(spec.Width, spec.Height);

        m_SSAOApplyShader = Shader::Create("assets/shaders/PostProcess_SSAOApply.glsl");

        // Resource-aware RDG: AO Apply reads the accumulated scene color
        // (SSSColor or SceneColor), the AO buffer, and the scene depth (for
        // bilateral upsampling), and emits AO-modulated HDR scene color.
        DeclareRead(ResourceNames::SceneColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::SSSColor, ResourceHandle::Kind::Framebuffer);
        DeclareRead(ResourceNames::AOBuffer, ResourceHandle::Kind::Texture2D);
        DeclareRead(ResourceNames::SceneDepth, ResourceHandle::Kind::Texture2D);
        DeclareWrite(ResourceNames::AOApplyColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("AOApplyRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void AOApplyRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("AOApplyRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    Ref<Framebuffer> AOApplyRenderPass::GetTarget() const
    {
        return m_Target;
    }

    void AOApplyRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void AOApplyRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Self-resolving input framebuffer: prefer SSSColor
        // if written this frame, fall back to SceneColor.  Mirrors the
        // conditional that EndScene() previously computed and pushed via the
        // side-channel setter.
        const auto* board = context.GetBlackboard();
        Ref<Framebuffer> inputFramebuffer;
        Ref<Framebuffer> outputFramebuffer;
        u32 aoTextureID = 0;
        u32 sceneDepthID = 0;
        if (board)
        {
            if (board->SSSColor.IsValid())
            {
                if (auto resolvedSSS = context.ResolveFramebuffer(board->SSSColor))
                    inputFramebuffer = resolvedSSS;
            }

            if (!inputFramebuffer && board->SceneColor.IsValid())
            {
                if (auto resolvedScene = context.ResolveFramebuffer(board->SceneColor))
                    inputFramebuffer = resolvedScene;
            }

            if (board->AOApplyColor.IsValid())
            {
                if (auto resolvedOutput = context.ResolveFramebuffer(board->AOApplyColor))
                    outputFramebuffer = resolvedOutput;
            }

            aoTextureID = context.ResolveTexture(board->AOBuffer);
            sceneDepthID = context.ResolveTexture(board->SceneDepth);
        }

        if (!m_Enabled)
        {
            m_Target = inputFramebuffer;
            return;
        }

        if (!inputFramebuffer || !outputFramebuffer)
        {
            m_Target = nullptr;
            static u32 s_MissingInputOrOutputWarnings = 0;
            if (s_MissingInputOrOutputWarnings++ < 10)
            {
                OLO_CORE_WARN("AOApplyRenderPass: missing input/output (inputFB={}, outputFB={}, aoTex={}, depthTex={})",
                              inputFramebuffer ? inputFramebuffer->GetRendererID() : 0u,
                              outputFramebuffer ? outputFramebuffer->GetRendererID() : 0u,
                              aoTextureID,
                              sceneDepthID);
            }
            OLO_CORE_ASSERT(false, "AOApplyRenderPass enabled without resolved graph input/output");
            return;
        }

        const bool shaderReady = m_SSAOApplyShader && m_SSAOApplyShader->IsReady();
        if (!shaderReady || aoTextureID == 0 || sceneDepthID == 0)
        {
            m_Target = nullptr;
            static u32 s_InvalidExecutionStateWarnings = 0;
            if (s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("AOApplyRenderPass: enabled without complete execution state (shaderReady={}, aoTex={}, depthTex={})",
                              shaderReady, aoTextureID, sceneDepthID);
            }
            OLO_CORE_ASSERT(false, "AOApplyRenderPass enabled without ready shader or resolved AO/depth inputs");
            return;
        }

        m_Target = outputFramebuffer;

        {
            static u32 s_PrevAOTextureID = 0;
            if (aoTextureID != s_PrevAOTextureID)
            {
                OLO_CORE_INFO("AOApplyRenderPass: applying AO with aoTex={} depthTex={}", aoTextureID, sceneDepthID);
                s_PrevAOTextureID = aoTextureID;
            }
        }

        // Rebind the PostProcessUBO before any fullscreen shader reads it.
        // SetData() updates the buffer object but does not restore the
        // indexed binding (IBL precompute also uses binding 7).
        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();
        // Rebind SSAOUBO (binding 9) — other passes may displace this binding
        // between EndScene()'s upload and this Execute() call.
        if (m_SSAOUBO)
            m_SSAOUBO->Bind();

        constexpr u32 colorAttachment = 0;
        outputFramebuffer->Bind();

        {
            static u32 s_PrevInputFB = 0;
            static u32 s_PrevOutputFB = 0;
            static u32 s_PrevOutputTex = 0;
            const u32 inputFB = inputFramebuffer->GetRendererID();
            const u32 outputFB = outputFramebuffer->GetRendererID();
            const u32 outputTex = outputFramebuffer->GetColorAttachmentRendererID(0);
            if (inputFB != s_PrevInputFB || outputFB != s_PrevOutputFB || outputTex != s_PrevOutputTex)
            {
                OLO_CORE_TRACE("AOApplyRenderPass: inputFB={} outputFB={} outputTex={} aoTex={} depthTex={}",
                               inputFB, outputFB, outputTex, aoTextureID, sceneDepthID);
                s_PrevInputFB = inputFB;
                s_PrevOutputFB = outputFB;
                s_PrevOutputTex = outputTex;
            }
        }

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

        m_SSAOApplyShader->Bind();
        const u32 srcColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
        context.BindTexture(0, srcColorID);
        context.BindTexture(ShaderBindingLayout::TEX_SSAO, aoTextureID);
        // Scene depth is used by the apply shader for bilateral upsampling.
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepthID);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        RenderCommand::DrawIndexed(va);

        RenderCommand::SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void AOApplyRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void AOApplyRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void AOApplyRenderPass::OnReset()
    {
        m_Target = nullptr;
    }

} // namespace OloEngine
