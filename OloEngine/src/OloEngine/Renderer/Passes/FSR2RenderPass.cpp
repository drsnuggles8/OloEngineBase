#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/FSR2RenderPass.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Upscaling/TemporalUpscalePolicy.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    FSR2RenderPass::FSR2RenderPass()
    {
        SetName("FSR2Pass");
    }

    void FSR2RenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        // The colour candidate chain is EASU's, verbatim and for the same reason:
        // the freshest PRE-Bloom, pre-upscale HDR colour. PostProcessColor is
        // deliberately absent — once this pass runs, that alias points AT our own
        // output, so listing it would make the upscaler read its own result.
        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ContactShadowColor, ResourceNames::ContactShadowColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSRColor, ResourceNames::SSRColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSGIColor, ResourceNames::SSGIColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::AOApplyColor, ResourceNames::AOApplyColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SSSColor, ResourceNames::SSSColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        m_SceneDepth = {};
        m_Velocity = {};

        if (!m_Enabled)
            return;

        // REDUCED-resolution depth + velocity, captured the same way
        // DepthVelocityUpscalePass captures them and for the same reason: this
        // pass runs BEFORE that one, so these are still the render-resolution
        // buffers FSR2 requires rather than the display-res copies.
        m_SceneDepth = blackboard.Scene.SceneDepth;
        m_Velocity = blackboard.GBuffer.Velocity;
        if (m_SceneDepth.IsValid())
            (void)builder.Read(m_SceneDepth, RGReadUsage::ShaderSample);
        if (m_Velocity.IsValid())
            (void)builder.Read(m_Velocity, RGReadUsage::ShaderSample);

        if (!blackboard.Post.FSR2Color.IsValid())
            return;

        constexpr std::string_view fsr2VersionTag = "FSR2Pass";
        // ShaderImage, NOT RenderTarget — unlike EASU next door, this pass never
        // draws a fullscreen triangle into the target. FSR2 issues compute
        // dispatches and writes the result through an image store, so declaring a
        // render-target write would describe a hazard that does not happen and
        // miss the one that does.
        const auto outputHandle = builder.WriteNewVersion(blackboard.Post.FSR2Color, RGWriteUsage::ShaderImage, fsr2VersionTag);
        if (!outputHandle.IsValid())
            return;

        SetPrimaryOutputFramebufferHandle(outputHandle);
        SetPrimaryOutputTextureHandle(
            builder.CreateFramebufferAttachmentView(std::string(ResourceNames::FSR2ColorTexture) + "@" +
                                                        std::string(fsr2VersionTag),
                                                    outputHandle,
                                                    0u));
    }

    void FSR2RenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        // FSR2Color is graph-owned (declared in PopulateBlackboard, resolved per
        // frame in Execute); this pass never allocates one.
        m_Target = nullptr;

        m_Upscaler = TemporalUpscaler::Create();
        if (m_Upscaler && !m_Upscaler->IsAvailable())
        {
            // INFO, not WARN: on a Vulkan run or a non-Windows build this is the
            // expected state, and the pipeline silently uses the spatial upscaler.
            OLO_CORE_INFO("FSR2RenderPass: temporal upscaling {} — the FSR1 spatial upscaler will be used instead",
                          ToString(m_Upscaler->GetStatus()));
        }

        OLO_CORE_INFO("FSR2RenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    i32 FSR2RenderPass::GetJitterPhaseCount(u32 renderWidth, u32 displayWidth) const
    {
        return m_Upscaler ? m_Upscaler->GetJitterPhaseCount(renderWidth, displayWidth) : 1;
    }

    glm::vec2 FSR2RenderPass::GetJitterOffset(i32 phaseIndex, i32 phaseCount) const
    {
        return m_Upscaler ? m_Upscaler->GetJitterOffset(phaseIndex, phaseCount) : glm::vec2(0.0f);
    }

    void FSR2RenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        m_Target = nullptr;

        if (!m_Enabled || !m_Upscaler || !m_Upscaler->IsAvailable())
            return;

        RHI::ResourceHandle colorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            colorTextureID = context.ResolveTextureHandle(inputTextureHandle);

        RHI::ResourceHandle outputTextureID{};
        if (const auto outputTextureHandle = GetPrimaryOutputTextureHandle(); outputTextureHandle.IsValid())
            outputTextureID = context.ResolveTextureHandle(outputTextureHandle);

        Ref<Framebuffer> outputFramebuffer;
        if (const auto outputHandle = GetPrimaryOutputFramebufferHandle(); outputHandle.IsValid())
            outputFramebuffer = context.ResolveFramebuffer(outputHandle);

        const RHI::ResourceHandle depthTextureID =
            m_SceneDepth.IsValid() ? context.ResolveTextureHandle(m_SceneDepth) : RHI::ResourceHandle{};
        const RHI::ResourceHandle velocityTextureID =
            m_Velocity.IsValid() ? context.ResolveTextureHandle(m_Velocity) : RHI::ResourceHandle{};

        // Bailing here is not harmless. The graph has already declared FSR2Color
        // and aliased PostProcessColor onto it, so a silent return leaves the
        // whole display-res post chain reading a target nobody wrote — a BLACK
        // FRAME with no diagnostic. Name whichever input is missing, once per
        // distinct combination, so that failure is one log line rather than a
        // bisect.
        if (!colorTextureID.IsValid() || !outputTextureID.IsValid() || !outputFramebuffer ||
            !depthTextureID.IsValid() || !velocityTextureID.IsValid())
        {
            const u32 missing = (colorTextureID.IsValid() ? 0u : 1u) | (depthTextureID.IsValid() ? 0u : 2u) |
                                (velocityTextureID.IsValid() ? 0u : 4u) | (outputTextureID.IsValid() ? 0u : 8u) |
                                (outputFramebuffer ? 0u : 16u);
            if (missing != m_LastMissingInputMask)
            {
                OLO_CORE_WARN("FSR2Pass: skipping the upscale — missing{}{}{}{}{}. The post chain is aliased onto "
                              "FSR2Color, so this frame will be black.",
                              (missing & 1u) ? " colour" : "",
                              (missing & 2u) ? " depth" : "",
                              (missing & 4u) ? " velocity" : "",
                              (missing & 8u) ? " output-view" : "",
                              (missing & 16u) ? " output-framebuffer" : "");
                m_LastMissingInputMask = missing;
            }
            return;
        }
        m_LastMissingInputMask = 0u;

        const auto& outSpec = outputFramebuffer->GetSpecification();
        const u32 displayW = outSpec.Width;
        const u32 displayH = outSpec.Height;
        if (displayW == 0u || displayH == 0u)
            return;

        // renderW/H is derived from the display size and the render scale by the
        // SAME expression the pipeline used to size the scene band and that
        // EASURenderPass uses — floor(display * scale), clamped identically. A
        // divergence here would offset every reprojection by a fraction of a
        // pixel, which reads as softness rather than as a mismatch.
        const u32 renderW = TemporalUpscalePolicy::RenderExtentFromDisplay(displayW, m_RenderScale);
        const u32 renderH = TemporalUpscalePolicy::RenderExtentFromDisplay(displayH, m_RenderScale);

        TemporalUpscalerConfig config;
        config.DisplayWidth = displayW;
        config.DisplayHeight = displayH;
        config.MaxRenderWidth = displayW; // a runtime scale change must not force a reconfigure
        config.MaxRenderHeight = displayH;
        config.HighDynamicRange = true; // this pass runs before ToneMapRenderPass
        config.InvertedDepth = false;   // glm::perspective RH_NO, no glClipControl — see PlanarReflection.h
        config.InfiniteDepth = false;
        // FALSE, and the backend supplies a neutral 1.0 exposure instead. FSR2's
        // auto-exposure bakes its metered value INTO the output, which this
        // engine would then expose a second time in ToneMapRenderPass — see
        // OpenGLTemporalUpscaler::EnsureNeutralExposureTexture for the measured
        // signature (a pixel-correct first frame, then a constant 36% darker).
        config.AutoExposure = false;
        if (!m_Upscaler->Configure(config))
            return;

        TemporalUpscalerDispatch dispatch;
        dispatch.Color = colorTextureID;
        dispatch.Depth = depthTextureID;
        dispatch.Velocity = velocityTextureID;
        dispatch.Output = outputTextureID;
        dispatch.RenderWidth = renderW;
        dispatch.RenderHeight = renderH;
        dispatch.JitterPixels = m_JitterPixels;

        // The engine's G-Buffer writes o_Velocity = (ndcCurr - ndcPrev) * 0.5,
        // i.e. UV-space CURRENT-minus-PREVIOUS with +Y up (the GL texture
        // convention every one of those shaders is written in). FSR2 wants a
        // render-resolution PIXEL displacement pointing from the current pixel
        // BACK to the previous one, in a +Y-down frame. So:
        //   * the negative sign turns curr-minus-prev into prev-minus-curr;
        //   * the magnitude converts UV to pixels;
        //   * X and Y take the same sign because BOTH of those flips apply to X
        //     while for Y the UV-orientation flip cancels one of them... which is
        //     exactly the kind of reasoning that produces a plausible-but-wrong
        //     image, so this is verified by moving the camera and watching for
        //     trails, not by re-deriving it. See FSR2MotionVectorScaleTest.
        dispatch.MotionVectorScale = TemporalUpscalePolicy::MotionVectorScale(renderW, renderH);

        dispatch.DeltaTimeSeconds = m_DeltaTimeSeconds;
        dispatch.NearPlane = m_NearPlane;
        dispatch.FarPlane = m_FarPlane;
        dispatch.VerticalFovRadians = m_VerticalFovRadians;
        dispatch.EnableSharpening = m_Settings.FSR2SharpeningEnabled;
        dispatch.Sharpness = std::clamp(m_Settings.FSR2Sharpness, 0.0f, 1.0f);
        // See m_LastRenderWidth: a render-scale change invalidates the history.
        const bool renderExtentChanged = (renderW != m_LastRenderWidth) || (renderH != m_LastRenderHeight);
        dispatch.ResetHistory = m_ResetHistoryRequested || renderExtentChanged;

        // FSR2 records compute dispatches straight into the current context and
        // manages its own bindings. Anything the engine left bound is therefore
        // clobbered, so the pass is wrapped in a state guard by the caller and we
        // publish nothing here beyond the output image.
        if (!m_Upscaler->Dispatch(dispatch))
            return;

        m_ResetHistoryRequested = false;
        m_LastRenderWidth = renderW;
        m_LastRenderHeight = renderH;
        m_Target = outputFramebuffer;
    }

    void FSR2RenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Target = nullptr;
        // A resolution change invalidates every accumulated history texel.
        m_ResetHistoryRequested = true;
        m_LastRenderWidth = 0;
        m_LastRenderHeight = 0;
    }

    void FSR2RenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0u || height == 0u)
            return;
        SetupFramebuffer(width, height);
    }

    void FSR2RenderPass::OnReset()
    {
        m_Target = nullptr;
        m_ResetHistoryRequested = true;
        m_LastRenderWidth = 0;
        m_LastRenderHeight = 0;
    }
} // namespace OloEngine
