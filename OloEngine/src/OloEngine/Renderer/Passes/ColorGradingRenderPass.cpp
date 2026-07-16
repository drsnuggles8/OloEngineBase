#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ColorGradingRenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glad/gl.h>

#include <array>
#include <vector>

namespace OloEngine
{
    ColorGradingRenderPass::ColorGradingRenderPass()
    {
        SetName("ColorGradingPass");
    }

    ColorGradingRenderPass::~ColorGradingRenderPass()
    {
        if (m_IdentityLUTTexture != 0)
        {
            RenderCommand::DeleteTexture(m_IdentityLUTTexture);
            m_IdentityLUTTexture = 0;
        }
    }

    void ColorGradingRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ChromAbColor, ResourceNames::ChromAbColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FogColor, ResourceNames::FogColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::CloudsColor, ResourceNames::CloudsColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
            });

        if (!m_Enabled)
            return;

        // Publish the pass-owned raw GL identity LUT so it resolves through
        // RenderGraph::GetRegisteredResources() — without this both
        // olo_render_list_targets and olo_render_capture_target are blind to it
        // (issue #607). Import-only: the pass binds it directly by raw id, so
        // there is deliberately no Read/Write declaration to change the graph's
        // ordering or culling.
        //
        // Reached only past the !m_Enabled early-out, so the import appears
        // exactly on the frames the pass runs. That gate is fed by
        // data.PostProcess.ColorGradingEnabled, which is already hashed into the
        // pipeline fingerprint (RenderPipeline.cpp HashBool(ColorGradingEnabled)),
        // so the topology-keyed caches invalidate correctly when it flips
        // (docs/agent-rules/render-pipeline-caches.md).
        if (m_IdentityLUTTexture != 0)
        {
            // 16x16x16 identity LUT laid out as a 256x16 strip (see CreateIdentityLUT).
            RGResourceDesc lutDesc =
                RGResourceDesc::FromHandleKind(ResourceHandle::Kind::Texture2D, kIdentityLUTTargetName);
            lutDesc.Format = RGResourceFormat::RGBA8UNorm;
            lutDesc.Width = 256;
            lutDesc.Height = 16;
            [[maybe_unused]] const RGTextureHandle lutHandle =
                builder.ImportTexture(kIdentityLUTTargetName, m_IdentityLUTTexture, lutDesc);
        }

        if (blackboard.Post.ColorGradingColor.IsValid())
        {
            constexpr std::string_view colorGradingVersionTag = "ColorGradingPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.ColorGradingColor, RGWriteUsage::RenderTarget, colorGradingVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::ColorGradingColorTexture) + "@" +
                                                            std::string(colorGradingVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void ColorGradingRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ColorGrading.glsl");

        CreateIdentityLUT();

        OLO_CORE_INFO("ColorGradingRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ColorGradingRenderPass::CreateIdentityLUT()
    {
        OLO_PROFILE_FUNCTION();

        // 16x16x16 LUT laid out as a 256x16 horizontal strip of 16 tiles.
        // The shader at PostProcess_ColorGrading.glsl assumes LUT_SIZE = 16.
        constexpr u32 lutSize = 16u;
        constexpr u32 stripWidth = lutSize * lutSize; // 256
        constexpr u32 stripHeight = lutSize;          // 16

        std::vector<u8> pixels(stripWidth * stripHeight * 4u);
        for (u32 b = 0; b < lutSize; ++b)
        {
            for (u32 g = 0; g < lutSize; ++g)
            {
                for (u32 r = 0; r < lutSize; ++r)
                {
                    const u32 x = b * lutSize + r;
                    const u32 y = g;
                    const u32 idx = (y * stripWidth + x) * 4u;
                    pixels[idx + 0] = static_cast<u8>((r * 255u) / (lutSize - 1u));
                    pixels[idx + 1] = static_cast<u8>((g * 255u) / (lutSize - 1u));
                    pixels[idx + 2] = static_cast<u8>((b * 255u) / (lutSize - 1u));
                    pixels[idx + 3] = 255u;
                }
            }
        }

        m_IdentityLUTTexture = RenderCommand::CreateTexture2D(stripWidth, stripHeight, GL_RGBA8);
        RenderCommand::UploadTextureSubImage2D(m_IdentityLUTTexture, stripWidth, stripHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        // Linear filtering + clamp-to-edge so the shader's intra-tile bilinear
        // and inter-tile mix() interpolate cleanly without wrap artifacts.
        RenderCommand::SetTextureParameter(m_IdentityLUTTexture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        RenderCommand::SetTextureParameter(m_IdentityLUTTexture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        RenderCommand::SetTextureParameter(m_IdentityLUTTexture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        RenderCommand::SetTextureParameter(m_IdentityLUTTexture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    void ColorGradingRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ColorGradingRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void ColorGradingRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto fb = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = fb;
        }

        if (!m_Enabled)
        {
            m_Target = nullptr;
            return;
        }

        if (inputColorTextureID == 0u || !outputFramebuffer || !m_Shader)
        {
            m_Target = nullptr;
            return;
        }

        m_Target = outputFramebuffer;

        if (m_PostProcessUBO)
            m_PostProcessUBO->Bind();

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

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        m_Shader->Bind();

        context.BindTexture(0, inputColorTextureID);
        m_Shader->SetInt("u_Texture", 0);

        // The fragment shader emits its output entirely from the LUT — without
        // a texture at TEX_POSTPROCESS_LUT it samples zero and the screen goes
        // black. Bind the identity LUT as a default so enabling the toggle is
        // a no-op when no user LUT has been wired up.
        context.BindTexture(ShaderBindingLayout::TEX_POSTPROCESS_LUT, m_IdentityLUTTexture);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.DrawIndexed(va);

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void ColorGradingRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ColorGradingRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ColorGradingRenderPass::OnReset()
    {
        m_Target = nullptr;
    }
} // namespace OloEngine
