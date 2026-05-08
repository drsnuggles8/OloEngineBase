#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/BloomRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ResourceHandle.h"

#include <glad/gl.h>

#include <span>

namespace OloEngine
{
    BloomRenderPass::BloomRenderPass()
    {
        SetName("BloomPass");
    }

    void BloomRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffers(spec.Width, spec.Height);

        m_BloomThresholdShader = Shader::Create("assets/shaders/PostProcess_BloomThreshold.glsl");
        m_BloomDownsampleShader = Shader::Create("assets/shaders/PostProcess_BloomDownsample.glsl");
        m_BloomUpsampleShader = Shader::Create("assets/shaders/PostProcess_BloomUpsample.glsl");
        m_BloomCompositeShader = Shader::Create("assets/shaders/PostProcess_BloomComposite.glsl");

        DeclareRead(ResourceNames::PostProcessColor, ResourceHandle::Kind::Framebuffer);
        DeclareWrite(ResourceNames::BloomColor, ResourceHandle::Kind::Framebuffer);

        OLO_CORE_INFO("BloomRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void BloomRenderPass::CreateFramebuffers(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::CreateFramebuffers: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void BloomRenderPass::Execute()
    {
        RGCommandContext context;
        Execute(context);
    }

    void BloomRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Phase F slice 43 — self-resolving input framebuffer from the render-graph
        // blackboard. Prefer the direct upstream chain and only then the
        // PostProcess alias, so bloom remains robust if alias wiring lags a
        // frame during resize/toggle churn.
        Ref<Framebuffer> inputFramebuffer;
        Ref<Framebuffer> outputFramebuffer;
        if (const auto* board = context.GetBlackboard())
        {
            auto tryResolveValid = [&](const auto& handle)
            {
                if (inputFramebuffer || !handle.IsValid())
                    return;
                if (auto fb = context.ResolveFramebuffer(handle))
                {
                    if (fb->GetColorAttachmentRendererID(0) != 0)
                        inputFramebuffer = fb;
                }
            };

            tryResolveValid(board->AOApplyColor);
            tryResolveValid(board->SSSColor);
            tryResolveValid(board->PostProcessColor);
            tryResolveValid(board->SceneColor);

            if (board->BloomColor.IsValid())
            {
                if (auto fb = context.ResolveFramebuffer(board->BloomColor))
                    outputFramebuffer = fb;
            }
        }

        if (!m_Enabled)
        {
            m_Target = inputFramebuffer;
            m_LastFailureMask = 0;
            return;
        }

        // Phase D / H follow-up: resolve the bloom mip-chain entirely from the
        // transient pool. The execute path no longer seeds from an owned
        // fallback chain.
        std::array<Ref<Framebuffer>, MAX_BLOOM_MIPS> bloomMips{};
        u32 bloomMipCount = 0;
        u32 bloomMipHandleCount = 0;
        u32 bloomMipResolvedCount = 0;
        if (const auto* board = context.GetBlackboard())
        {
            for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
            {
                if (board->BloomMips[i].IsValid())
                {
                    ++bloomMipHandleCount;
                    if (auto fb = context.ResolveFramebuffer(board->BloomMips[i]))
                    {
                        bloomMips[i] = fb;
                        ++bloomMipResolvedCount;
                    }
                }
            }
        }
        for (u32 i = 0; i < MAX_BLOOM_MIPS; ++i)
        {
            if (bloomMips[i])
                bloomMipCount = i + 1;
            else
                break;
        }
        const bool shadersReady = IsReadyForExecution();

        constexpr u32 FAIL_NO_INPUT = 1u << 0u;
        constexpr u32 FAIL_NO_OUTPUT = 1u << 1u;
        constexpr u32 FAIL_NO_MIPS = 1u << 2u;
        constexpr u32 FAIL_NO_SHADERS = 1u << 3u;

        u32 failureMask = 0;
        if (!inputFramebuffer)
            failureMask |= FAIL_NO_INPUT;
        if (!outputFramebuffer)
            failureMask |= FAIL_NO_OUTPUT;
        if (bloomMipCount == 0)
            failureMask |= FAIL_NO_MIPS;
        if (!shadersReady)
            failureMask |= FAIL_NO_SHADERS;

        if (failureMask != 0)
        {
            if (m_LastFailureMask != failureMask)
            {
                OLO_CORE_ERROR("BloomRenderPass: prerequisites missing (mask=0x{:x}, inputFB={}, outputFB={}, mipCount={}, mipHandlesValid={}, mipResolved={}, shadersReady={})",
                               failureMask,
                               inputFramebuffer ? inputFramebuffer->GetRendererID() : 0u,
                               outputFramebuffer ? outputFramebuffer->GetRendererID() : 0u,
                               bloomMipCount,
                               bloomMipHandleCount,
                               bloomMipResolvedCount,
                               shadersReady);
            }

            m_Target = nullptr;
            m_LastFailureMask = failureMask;
            OLO_CORE_ASSERT(false, "BloomRenderPass enabled without complete graph/shader state");
            return;
        }

        {
            static u32 s_PrevInputFB = 0;
            static u32 s_PrevOutputFB = 0;
            static u32 s_PrevInputTex = 0;
            const u32 inputFB = inputFramebuffer->GetRendererID();
            const u32 outputFB = outputFramebuffer->GetRendererID();
            const u32 inputTex = inputFramebuffer->GetColorAttachmentRendererID(0);
            if (inputFB != s_PrevInputFB || outputFB != s_PrevOutputFB || inputTex != s_PrevInputTex)
            {
                OLO_CORE_TRACE("BloomRenderPass: inputFB={} outputFB={} inputTex={} mipCount={}",
                               inputFB, outputFB, inputTex, bloomMipCount);
                s_PrevInputFB = inputFB;
                s_PrevOutputFB = outputFB;
                s_PrevInputTex = inputTex;
            }

            if (inputFB == outputFB)
            {
                OLO_CORE_ERROR("BloomRenderPass: invalid feedback loop detected (inputFB == outputFB == {})", inputFB);
            }
        }

        if (m_LastFailureMask != 0)
        {
            OLO_CORE_INFO("BloomRenderPass: recovered (inputFB={}, outputFB={}, mipCount={})",
                          inputFramebuffer->GetRendererID(), outputFramebuffer->GetRendererID(), bloomMipCount);
            m_LastFailureMask = 0;
        }

        m_Target = outputFramebuffer;

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

        constexpr u32 colorAttachment = 0;

        // ------------------------------------------------------------------
        // Step 1: Threshold extract — scene HDR → bloom mip 0
        // ------------------------------------------------------------------
        {
            bloomMips[0]->Bind();
            const auto& spec = bloomMips[0]->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomThresholdShader->Bind();
            const u32 srcID = inputFramebuffer->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcID);
            m_BloomThresholdShader->SetInt("u_Texture", 0);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            bloomMips[0]->Unbind();
        }

        // ------------------------------------------------------------------
        // Step 2: Progressive downsample
        // ------------------------------------------------------------------
        for (size_t i = 1; i < static_cast<size_t>(bloomMipCount); ++i)
        {
            auto& srcMip = bloomMips[i - 1];
            auto& dstMip = bloomMips[i];
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            context.SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomDownsampleShader->Bind();
            const u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcID);

            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            dstMip->Unbind();
        }

        // ------------------------------------------------------------------
        // Step 3: Progressive upsample (additive accumulation back up the chain)
        // ------------------------------------------------------------------
        for (int i = static_cast<int>(bloomMipCount) - 2; i >= 0; --i)
        {
            auto& srcMip = bloomMips[static_cast<size_t>(i) + 1];
            auto& dstMip = bloomMips[static_cast<size_t>(i)];
            const auto& srcSpec = srcMip->GetSpecification();
            const auto& dstSpec = dstMip->GetSpecification();

            dstMip->Bind();
            context.SetViewport(0, 0, dstSpec.Width, dstSpec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            // Enable additive blending AFTER Bind() — Framebuffer::Bind() may
            // unconditionally re-enable GL_BLEND; set our state after the call.
            context.SetBlendState(true);
            RenderCommand::SetBlendFunc(GL_ONE, GL_ONE);
            // No clear — we're additively accumulating into existing content.

            m_BloomUpsampleShader->Bind();
            const u32 srcID = srcMip->GetColorAttachmentRendererID(0);
            context.BindTexture(0, srcID);

            if (m_GPUData && m_PostProcessUBO)
            {
                m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(srcSpec.Width);
                m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(srcSpec.Height);
                m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
            }

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            dstMip->Unbind();
        }

        // Restore blend to opaque default.
        context.SetBlendState(false);
        RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        context.SetDepthMask(true);

        // Restore TexelSize to full resolution so the composite shader (and
        // any subsequent UBO readers) sees the correct value.
        if (m_GPUData && m_PostProcessUBO)
        {
            m_GPUData->TexelSizeX = 1.0f / static_cast<f32>(m_FramebufferSpec.Width);
            m_GPUData->TexelSizeY = 1.0f / static_cast<f32>(m_FramebufferSpec.Height);
            m_PostProcessUBO->SetData(m_GPUData, PostProcessUBOData::GetSize());
        }

        // ------------------------------------------------------------------
        // Step 4: Composite bloom mip 0 onto scene color → output FB
        // ------------------------------------------------------------------
        {
            outputFramebuffer->Bind();
            const auto& outSpec = outputFramebuffer->GetSpecification();
            context.SetViewport(0, 0, outSpec.Width, outSpec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

            context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            context.Clear();

            m_BloomCompositeShader->Bind();

            const u32 sceneColorID = inputFramebuffer->GetColorAttachmentRendererID(0);
            context.BindTexture(0, sceneColorID);
            m_BloomCompositeShader->SetInt("u_SceneColor", 0);

            const u32 bloomColorID = bloomMips[0]->GetColorAttachmentRendererID(0);
            context.BindTexture(1, bloomColorID);
            m_BloomCompositeShader->SetInt("u_BloomColor", 1);

            const auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            RenderCommand::DrawIndexed(va);
            outputFramebuffer->Unbind();
        }

        // Restore full viewport for downstream passes.
        const auto& outputSpec = outputFramebuffer->GetSpecification();
        context.SetViewport(0, 0, outputSpec.Width, outputSpec.Height);
        context.SetDepthMask(true);
    }

    Ref<Framebuffer> BloomRenderPass::GetTarget() const
    {
        if (!m_Target)
            return nullptr;
        return m_Target;
    }

    void BloomRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::SetupFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);
    }

    void BloomRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("BloomRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffers(width, height);

        OLO_CORE_INFO("BloomRenderPass: Resized to {}x{}", width, height);
    }

    void BloomRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;
        m_LastFailureMask = 0;
    }
} // namespace OloEngine
