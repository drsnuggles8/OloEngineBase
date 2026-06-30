#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>
#include <span>

#include <glad/gl.h>

namespace OloEngine
{
    SelectionOutlineRenderPass::SelectionOutlineRenderPass()
    {
        SetName("SelectionOutlinePass");
    }

    void SelectionOutlineRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneEntityTexture = {};
        m_SelectedJFAPingFramebuffer = {};
        m_SelectedJFAPongFramebuffer = {};

        (void)blackboard;
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FXAAColor, ResourceNames::FXAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::VignetteColor, ResourceNames::VignetteColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::UpscalerColor, ResourceNames::UpscalerColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ToneMapColor, ResourceNames::ToneMapColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ColorGradingColor, ResourceNames::ColorGradingColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ChromAbColor, ResourceNames::ChromAbColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FogColor, ResourceNames::FogColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (!IsReadyForExecution() || !blackboard.Post.SelectionOutlineColor.IsValid() || !blackboard.Scene.SceneEntityID.IsValid())
            return;

        m_SelectedSceneEntityTexture = blackboard.Scene.SceneEntityID;
        [[maybe_unused]] const auto sceneEntityRead = builder.Read(blackboard.Scene.SceneEntityID, RGReadUsage::ShaderSample);

        constexpr std::string_view selectionOutlineVersionTag = "SelectionOutlinePass";
        const auto outputHandle =
            builder.WriteNewVersion(blackboard.Post.SelectionOutlineColor, RGWriteUsage::RenderTarget, selectionOutlineVersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::SelectionOutlineColorTexture) + "@" +
                                                        std::string(selectionOutlineVersionTag),
                                                    outputHandle,
                                                    0u));

        // JFA passes shader-sample (texture()) the ping/pong color attachments
        // — these are not input-attachment reads. The hazard planner needs the
        // sample barrier, not a sub-pass attachment-read.
        // Intra-pass JFA ping-pong: each jump-flood iteration alternates
        // render-target binding between Ping and Pong while sampling from
        // the other, all inside this pass's Execute. Both targets are
        // graph-owned scratch with no prior writer to chain against.
        if (blackboard.Scratch.JFAPing.IsValid())
        {
            m_SelectedJFAPingFramebuffer = blackboard.Scratch.JFAPing;
            builder.AllowSamePassReadWrite(blackboard.Scratch.JFAPing);
            builder.Write(blackboard.Scratch.JFAPing, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto pingRead = builder.Read(blackboard.Scratch.JFAPing, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.JFAPong.IsValid())
        {
            m_SelectedJFAPongFramebuffer = blackboard.Scratch.JFAPong;
            builder.AllowSamePassReadWrite(blackboard.Scratch.JFAPong);
            builder.Write(blackboard.Scratch.JFAPong, RGWriteUsage::RenderTarget);
            [[maybe_unused]] const auto pongRead = builder.Read(blackboard.Scratch.JFAPong, RGReadUsage::ShaderSample);
        }
    }

    void SelectionOutlineRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        // JFA shaders
        m_JFAInitShader = Shader::Create("assets/shaders/JumpFlood_Init.glsl");
        m_JFAPassShader = Shader::Create("assets/shaders/JumpFlood_Pass.glsl");
        m_JFACompositeShader = Shader::Create("assets/shaders/JumpFlood_Composite.glsl");

        // UBOs
        m_OutlineUBO = UniformBuffer::Create(UBOStructures::SelectionOutlineUBO::GetSize(), ShaderBindingLayout::UBO_SELECTION_OUTLINE);
        m_JFAUbo = UniformBuffer::Create(UBOStructures::JumpFloodUBO::GetSize(), ShaderBindingLayout::UBO_JUMP_FLOOD);

        CreateFramebuffer(spec.Width, spec.Height);

        OLO_CORE_INFO("SelectionOutlineRenderPass: Initialized {}x{} (JFA, {} passes)", spec.Width, spec.Height, m_JFAPassCount);
    }

    void SelectionOutlineRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SelectionOutlineRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    void SelectionOutlineRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        u32 inputColorTextureID = 0u;
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTexture(inputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        u32 sceneEntityTextureID = 0u;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
        {
            if (auto fb = context.ResolveFramebuffer(outputHandle))
                outputFramebuffer = fb;
        }
        if (m_SelectedSceneEntityTexture.IsValid())
            sceneEntityTextureID = context.ResolveTexture(m_SelectedSceneEntityTexture);

        if (!outputFramebuffer || inputColorTextureID == 0u || sceneEntityTextureID == 0u)
        {
            m_Target = nullptr;
            return;
        }

        if (!m_Enabled || m_UBOData.SelectedCount == 0)
        {
            m_Target = nullptr;
            return;
        }

        if (const bool readyForExecution = IsReadyForExecution(); !readyForExecution)
        {
            m_Target = nullptr;
            if (static u32 s_InvalidExecutionStateWarnings = 0; s_InvalidExecutionStateWarnings++ < 10)
            {
                OLO_CORE_WARN("SelectionOutlineRenderPass: enabled without complete execution state (sceneEntityTex={}, shadersReady={}, selectedCount={})",
                              sceneEntityTextureID,
                              readyForExecution,
                              m_UBOData.SelectedCount);
            }
            OLO_CORE_ASSERT(false, "SelectionOutlineRenderPass enabled without ready shaders/UBOs or resolved SceneEntityID");
            return;
        }

        m_Target = outputFramebuffer;

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        constexpr u32 colorAttachment = 0;
        const auto& outputSpec = outputFramebuffer->GetSpecification();
        const u32 w = outputSpec.Width;
        const u32 h = outputSpec.Height;

        // Phase D / H follow-up: resolve JFA ping-pong framebuffers entirely
        // from the transient pool. The execute path no longer keeps owned
        // fallback framebuffers for headless / unit-test contexts.
        std::array<Ref<Framebuffer>, 2> jfaFBs{};
        if (m_SelectedJFAPingFramebuffer.IsValid())
            jfaFBs[0] = context.ResolveFramebuffer(m_SelectedJFAPingFramebuffer);
        if (m_SelectedJFAPongFramebuffer.IsValid())
            jfaFBs[1] = context.ResolveFramebuffer(m_SelectedJFAPongFramebuffer);
        if (!jfaFBs[0] || !jfaFBs[1])
        {
            m_Target = nullptr;
            if (static u32 s_MissingScratchWarnings = 0; s_MissingScratchWarnings++ < 10)
            {
                OLO_CORE_WARN("SelectionOutlineRenderPass: enabled without resolved JFA scratch (pingFB={}, pongFB={})",
                              jfaFBs[0] ? jfaFBs[0]->GetRendererID() : 0u,
                              jfaFBs[1] ? jfaFBs[1]->GetRendererID() : 0u);
            }
            OLO_CORE_ASSERT(false, "SelectionOutlineRenderPass enabled without resolved JFA scratch framebuffers");
            return;
        }

        const glm::vec4 texelSize(1.0f / static_cast<f32>(w), 1.0f / static_cast<f32>(h), 0.0f, 0.0f);

        // Upload SelectionOutlineUBO (entity IDs for Init pass)
        m_UBOData.TexelSize = texelSize;
        m_OutlineUBO->SetData(&m_UBOData, UBOStructures::SelectionOutlineUBO::GetSize());

        // =====================================================================
        // Pass 1: JFA Init — entity IDs → distance field seed
        // =====================================================================
        jfaFBs[0]->Bind();
        context.SetViewport(0, 0, w, h);
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        context.SetBlendState(false);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);
        context.Clear();

        // Bind entity-ID texture from the graph-published SceneColor attachment view.
        context.BindTexture(0, sceneEntityTextureID);

        m_JFAInitShader->Bind();
        m_JFAInitShader->SetInt("u_EntityID", 0);

        va->Bind();
        context.DrawIndexed(va);

        // =====================================================================
        // Pass 2: JFA Flood — ping-pong propagation passes
        // =====================================================================
        auto const jfaSteps = ComputeJFASteps(m_JFAPassCount);
        i32 readIndex = 0;

        for (i32 step : jfaSteps)
        {
            i32 writeIndex = (readIndex + 1) % 2;

            // Update JFA UBO with current step
            m_JFAUboData.TexelSize = texelSize;
            m_JFAUboData.Step = step;
            m_JFAUbo->SetData(&m_JFAUboData, UBOStructures::JumpFloodUBO::GetSize());

            jfaFBs[writeIndex]->Bind();
            context.SetViewport(0, 0, w, h);
            context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
            context.SetBlendState(false);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            RenderCommand::SetColorMask(true, true, true, true);
            context.Clear();

            // Bind previous JFA result
            context.BindTexture(0, jfaFBs[readIndex]->GetColorAttachmentRendererID(0));

            m_JFAPassShader->Bind();
            m_JFAPassShader->SetInt("u_Texture", 0);

            va->Bind();
            context.DrawIndexed(va);

            readIndex = writeIndex;
        }

        // =====================================================================
        // Pass 3: JFA Composite — distance field → anti-aliased outline
        // =====================================================================
        // Upload final JFA UBO with outline parameters
        m_JFAUboData.TexelSize = texelSize;
        m_JFAUboData.Step = 0; // Not used in composite
        m_JFAUbo->SetData(&m_JFAUboData, UBOStructures::JumpFloodUBO::GetSize());

        outputFramebuffer->Bind();
        context.SetViewport(0, 0, w, h);
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));
        context.SetBlendState(false);
        context.SetDepthTest(false);
        context.SetDepthMask(false);
        context.SetCulling(false);
        RenderCommand::DisableStencilTest();
        RenderCommand::DisableScissorTest();
        RenderCommand::SetPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        RenderCommand::SetColorMask(true, true, true, true);

        // Slot 0: scene color from the selected dynamic post-chain texture view.
        context.BindTexture(0, inputColorTextureID);
        // Slot 1: final JFA distance field from the graph-owned ping-pong scratch
        context.BindTexture(1, jfaFBs[readIndex]->GetColorAttachmentRendererID(0));

        m_JFACompositeShader->Bind();
        m_JFACompositeShader->SetInt("u_SceneColor", 0);
        m_JFACompositeShader->SetInt("u_JFAResult", 1);

        va->Bind();
        context.DrawIndexed(va);

        // Restore state
        context.SetDepthTest(true);
    }

    Ref<Framebuffer> SelectionOutlineRenderPass::GetTarget() const
    {
        if (!m_Enabled || m_UBOData.SelectedCount == 0)
            return nullptr;
        return m_Target;
    }

    void SelectionOutlineRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateFramebuffer(width, height);
    }

    void SelectionOutlineRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("SelectionOutlineRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;

        CreateFramebuffer(width, height);
    }

    void SelectionOutlineRenderPass::OnReset()
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;
        m_SelectedSceneEntityTexture = {};
        m_SelectedJFAPingFramebuffer = {};
        m_SelectedJFAPongFramebuffer = {};
    }

    void SelectionOutlineRenderPass::SetSelectedEntityIDs(std::span<const i32> ids)
    {
        constexpr u32 maxEntities = UBOStructures::SelectionOutlineUBO::MaxSelectedEntities;
        u32 count = static_cast<u32>(std::min(ids.size(), static_cast<size_t>(maxEntities)));

        m_UBOData.SelectedCount = static_cast<i32>(count);

        // Zero out all IDs first (use -1 as "no entity" sentinel)
        for (auto& v : m_UBOData.SelectedIDs)
        {
            v = glm::ivec4(-1);
        }

        // Pack IDs into ivec4 array
        for (u32 i = 0; i < count; ++i)
        {
            u32 vecIndex = i / 4;
            u32 compIndex = i % 4;
            m_UBOData.SelectedIDs[vecIndex][compIndex] = ids[i];
        }
    }

    void SelectionOutlineRenderPass::SetOutlineColor(const glm::vec4& color)
    {
        m_UBOData.OutlineColor = color;
        m_JFAUboData.OutlineColor = color;
    }

    void SelectionOutlineRenderPass::SetOutlineWidth(i32 width)
    {
        m_UBOData.OutlineWidth = std::max(1, width);

        // Translate integer pixel width into JFA smoothstep thickness.
        // Inner/outer define the anti-aliased band in normalized screen distance.
        constexpr f32 baseInner = 0.002f;
        constexpr f32 baseOuter = 0.004f;
        f32 scale = static_cast<f32>(m_UBOData.OutlineWidth);
        m_JFAUboData.OutlineThicknessInner = baseInner * scale;
        m_JFAUboData.OutlineThicknessOuter = baseOuter * scale;
    }

    void SelectionOutlineRenderPass::SetEnabled(bool enabled)
    {
        m_Enabled = enabled;
    }

    void SelectionOutlineRenderPass::SetOutlineThickness(f32 inner, f32 outer)
    {
        if (inner < 0.0f || outer < 0.0f || inner >= outer)
        {
            OLO_CORE_ERROR("SelectionOutlineRenderPass::SetOutlineThickness: invalid values (inner={}, outer={}). "
                           "Requires inner >= 0, outer >= 0, inner < outer.",
                           inner, outer);
            return;
        }
        m_JFAUboData.OutlineThicknessInner = inner;
        m_JFAUboData.OutlineThicknessOuter = outer;
    }

    void SelectionOutlineRenderPass::SetJFAPassCount(i32 count)
    {
        m_JFAPassCount = std::clamp(count, 1, 4);
    }

    SelectionOutlineRenderPass::JFAStepSequence SelectionOutlineRenderPass::ComputeJFASteps(i32 passCount)
    {
        passCount = std::clamp(passCount, 1, MaxJFAPasses);
        JFAStepSequence seq;
        for (i32 step = 1 << (passCount - 1); step >= 1; step >>= 1)
        {
            seq.Steps[static_cast<size_t>(seq.Count++)] = step;
        }
        return seq;
    }

} // namespace OloEngine
