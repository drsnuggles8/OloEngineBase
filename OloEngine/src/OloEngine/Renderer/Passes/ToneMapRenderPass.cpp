#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/ToneMapRenderPass.h"

#include "OloEngine/Renderer/AutoExposure.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderPipelineBuilderInternal.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <algorithm>
#include <array>

namespace OloEngine
{
    ToneMapRenderPass::ToneMapRenderPass()
    {
        SetName("ToneMapPass");
    }

    void ToneMapRenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        m_SelectedSceneDepthTexture = {};
        // Scene depth feeds the underwater fog stage (eye-space distance for the
        // Beer-Lambert falloff). Depth is produced far upstream by the scene
        // pass, so reading it here doesn't reorder the post chain. The fog
        // stage self-skips in the shader when the camera is above water.
        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Post.UpscaledSceneDepthTexture.IsValid() ? blackboard.Post.UpscaledSceneDepthTexture : blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(m_SelectedSceneDepthTexture, RGReadUsage::ShaderSample);
        }

        [[maybe_unused]] const auto input = RenderPipelineBuilderInternal::ReadFirstValidVersionedInputForPass(
            builder,
            this,
            {
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ColorGradingColor, ResourceNames::ColorGradingColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::ChromAbColor, ResourceNames::ChromAbColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::FogColor, ResourceNames::FogColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PrecipitationColor, ResourceNames::PrecipitationColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::CloudsColor, ResourceNames::CloudsColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::TAAColor, ResourceNames::TAAColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::MotionBlurColor, ResourceNames::MotionBlurColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::DOFColor, ResourceNames::DOFColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::BloomColor, ResourceNames::BloomColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::PostProcessColor, ResourceNames::PostProcessColorTexture),
                RenderPipelineBuilderInternal::MakeCandidateBaseNames(ResourceNames::SceneColor, ResourceNames::SceneColorTexture),
            });

        if (blackboard.Post.ToneMapColor.IsValid())
        {
            constexpr std::string_view toneMapVersionTag = "ToneMapPass";
            const auto outputHandle = builder.WriteNewVersion(blackboard.Post.ToneMapColor, RGWriteUsage::RenderTarget, toneMapVersionTag);
            if (!outputHandle.IsValid())
                return;

            SetPrimaryOutputFramebufferHandle(outputHandle);
            SetPrimaryOutputTextureHandle(
                builder.CreateFramebufferAttachmentView(std::string(ResourceNames::ToneMapColorTexture) + "@" +
                                                            std::string(toneMapVersionTag),
                                                        outputHandle,
                                                        0u));
        }
    }

    void ToneMapRenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;

        CreateFramebuffer(spec.Width, spec.Height);

        m_Shader = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");

        // The auto-exposure result buffer (binding 20) is always created so the
        // tone-map shader's SSBO read is valid even when metering never runs.
        // [0] = exposure (<=0 sentinel => use manual), [1] = adapted luminance.
        // The compute shaders + histogram buffer are created lazily on first use.
        if (!m_ExposureStateBuffer)
        {
            m_ExposureStateBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(f32) * 4),
                                                          ShaderBindingLayout::SSBO_AUTO_EXPOSURE_STATE,
                                                          StorageBufferUsage::DynamicCopy);
            const std::array<f32, 4> initial = { -1.0f, 0.0f, 0.0f, 0.0f }; // exposure=-1 (manual), adapted=0 (uninit)
            m_ExposureStateBuffer->SetData(initial.data(), static_cast<u32>(initial.size() * sizeof(f32)));
            m_AutoExposureActiveLastFrame = false;
        }

        OLO_CORE_INFO("ToneMapRenderPass: Initialized with viewport {}x{}", spec.Width, spec.Height);
    }

    void ToneMapRenderPass::CreateFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("ToneMapRenderPass::CreateFramebuffer: Invalid dimensions {}x{}", width, height);
            m_Target = nullptr;
            return;
        }

        m_Target = nullptr;
    }

    bool ToneMapRenderPass::EnsureAutoExposureResources()
    {
        if (!m_HistogramShader)
            m_HistogramShader = ComputeShader::Create("assets/shaders/compute/AutoExposureHistogram.comp");
        if (!m_AverageShader)
            m_AverageShader = ComputeShader::Create("assets/shaders/compute/AutoExposureAverage.comp");
        if (!m_HistogramBuffer)
        {
            m_HistogramBuffer = StorageBuffer::Create(static_cast<u32>(sizeof(u32) * AutoExposure::kHistogramBins),
                                                      ShaderBindingLayout::SSBO_AUTO_EXPOSURE_HISTOGRAM,
                                                      StorageBufferUsage::DynamicCopy);
        }
        const bool histogramReady = m_HistogramShader && m_HistogramShader->IsValid();
        const bool averageReady = m_AverageShader && m_AverageShader->IsValid();
        return histogramReady && averageReady && m_HistogramBuffer && m_ExposureStateBuffer;
    }

    void ToneMapRenderPass::RunAutoExposureMetering(const RGCommandContext& context, RHI::ResourceHandle hdrTextureID,
                                                    u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0u || height == 0u || !hdrTextureID.IsValid() || !EnsureAutoExposureResources())
            return;

        // Sanitise the metering window (defends against bad serialized / scripted values).
        f32 minLogLum = std::isfinite(m_AutoExposure.MinLogLuminance) ? m_AutoExposure.MinLogLuminance : -8.0f;
        f32 maxLogLum = std::isfinite(m_AutoExposure.MaxLogLuminance) ? m_AutoExposure.MaxLogLuminance : 3.5f;
        if (!(maxLogLum > minLogLum))
            maxLogLum = minLogLum + 1.0f;
        const f32 logLumRange = maxLogLum - minLogLum;
        const f32 invLogLumRange = 1.0f / logLumRange;

        // Cap the metering resolution so per-bin atomic counts stay well inside a
        // uint and the cost is independent of screen size. Bilinear down-sampling
        // (textureLod) keeps the metered average representative.
        constexpr u32 kMeterDimCap = 512u;
        constexpr u32 kLocalSize = 16u; // matches AutoExposureHistogram.comp local_size
        const u32 meterW = std::min(width, kMeterDimCap);
        const u32 meterH = std::min(height, kMeterDimCap);

        // --- Histogram pass: bin every metered texel by log-luminance ---
        m_HistogramBuffer->ClearData();
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        m_HistogramShader->Bind();
        context.BindTexture(0, hdrTextureID);
        m_HistogramShader->SetFloat2("u_MeterSize", glm::vec2(static_cast<f32>(meterW), static_cast<f32>(meterH)));
        m_HistogramShader->SetFloat("u_MinLogLum", minLogLum);
        m_HistogramShader->SetFloat("u_InvLogLumRange", invLogLumRange);
        m_HistogramBuffer->Bind();
        RenderCommand::DispatchCompute((meterW + kLocalSize - 1u) / kLocalSize,
                                       (meterH + kLocalSize - 1u) / kLocalSize, 1u);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

        // --- Average pass: reduce histogram -> adapt -> exposure (1 workgroup) ---
        m_AverageShader->Bind();
        m_HistogramBuffer->Bind();
        m_ExposureStateBuffer->Bind();
        m_AverageShader->SetFloat("u_MinLogLum", minLogLum);
        m_AverageShader->SetFloat("u_LogLumRange", logLumRange);
        m_AverageShader->SetFloat("u_Dt", std::max(m_AutoExposure.DeltaTime, 0.0f));
        m_AverageShader->SetFloat("u_SpeedUp", m_AutoExposure.SpeedUp);
        m_AverageShader->SetFloat("u_SpeedDown", m_AutoExposure.SpeedDown);
        m_AverageShader->SetFloat("u_ExposureCompensation", m_AutoExposure.Compensation);
        m_AverageShader->SetFloat("u_MinExposure", m_AutoExposure.MinExposure);
        m_AverageShader->SetFloat("u_MaxExposure", m_AutoExposure.MaxExposure);
        // Same defence as the metering window above: RenderPipeline copies these
        // straight out of PostProcessSettings without routing through
        // SanitizeAutoExposure, so a script/MCP/editor write can hand us a NaN.
        // The exposure state SSBO is PERSISTENT across frames, so a single NaN
        // frame does not just render wrong — it latches, and every later frame
        // adapts from a NaN. Defaults match SanitizeAutoExposure's.
        const f32 lowPercentile = std::isfinite(m_AutoExposure.LowPercentile) ? m_AutoExposure.LowPercentile : 0.80f;
        const f32 highPercentile = std::isfinite(m_AutoExposure.HighPercentile) ? m_AutoExposure.HighPercentile : 0.98f;
        m_AverageShader->SetFloat("u_LowPercentile", lowPercentile);
        m_AverageShader->SetFloat("u_HighPercentile", highPercentile);
        RenderCommand::DispatchCompute(1u, 1u, 1u);
        // Make the exposure write visible to the tone-map fragment shader's SSBO read.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    void ToneMapRenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        // Sample-only consumer: input framebuffer is intentionally not
        // resolved here — see ReadFirstValidVersionedInputForPass docs.
        RHI::ResourceHandle inputColorTextureID{};
        if (const auto inputTextureHandle = GetPrimaryInputTextureHandle(); inputTextureHandle.IsValid())
            inputColorTextureID = context.ResolveTextureHandle(inputTextureHandle);

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

        if (!inputColorTextureID.IsValid() || !outputFramebuffer || !m_Shader)
        {
            m_Target = nullptr;
            if (static u32 s_MissingInputWarnings = 0; outputFramebuffer && m_Shader && !inputColorTextureID.IsValid() && s_MissingInputWarnings++ < 10)
            {
                OLO_CORE_WARN("ToneMapRenderPass: No valid setup-selected input texture resolved");
            }
            return;
        }

        m_Target = outputFramebuffer;

        // Automatic exposure / eye adaptation: meter the HDR input before the
        // tone-map draw. Two compute passes leave the metered exposure in
        // m_ExposureStateBuffer[0]; the tone-map shader reads it via SSBO
        // binding 20 (a non-positive sentinel there means "use manual exposure").
        if (m_AutoExposure.Enabled)
        {
            const auto& meterSpec = outputFramebuffer->GetSpecification();
            RunAutoExposureMetering(context, inputColorTextureID, meterSpec.Width, meterSpec.Height);
            m_AutoExposureActiveLastFrame = true;
        }
        else if (m_AutoExposureActiveLastFrame)
        {
            // Restore the manual-exposure sentinel once when auto-exposure is turned off.
            if (const f32 sentinel = -1.0f; m_ExposureStateBuffer)
                m_ExposureStateBuffer->SetData(&sentinel, static_cast<u32>(sizeof(f32)), 0);
            m_AutoExposureActiveLastFrame = false;
        }
        else
        {
            // Auto-exposure already inactive (sentinel already in place) — nothing to do.
        }

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
        RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
        RenderCommand::SetColorMask(true, true, true, true);

        constexpr u32 colorAttachment = 0;
        context.SetDrawBuffers(std::span<const u32>(&colorAttachment, 1));

        context.SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
        context.Clear();

        // Underwater fog UBO — bind so the shader stage can read the tint
        // params. The shader self-skips when the camera is above water, so a
        // missing/zeroed UBO is harmless. See §7.2.
        if (m_UnderwaterFogUBO)
            m_UnderwaterFogUBO->Bind();

        m_Shader->Bind();

        // Auto-exposure result (binding 20). Always bound so the shader's SSBO
        // read is valid; holds a -1 sentinel when metering is off (=> manual).
        if (m_ExposureStateBuffer)
            m_ExposureStateBuffer->Bind();

        // FrameTransient: graph-owned (context.ResolveTextureHandle), so the
        // descriptor comes from the per-frame ring rather than being memoised onto
        // a pooled object the planner may hand to a different resource next frame.
        //
        // The `SetInt("u_Texture", 0)` that used to follow is gone: the shader has
        // declared `layout(binding = 0)` all along (glsl-shaders.md §5 forbids
        // glUniform1i for samplers), so it was already redundant — and under the
        // bindless variant `u_Texture` is a #define rather than a uniform, so it
        // would log a "uniform not found" warning every frame.
        context.BindTextureOrHeapOffset(0, inputColorTextureID, RHI::HeapSlotLifetime::FrameTransient);

        // Scene depth for the underwater fog distance reconstruction.
        RHI::ResourceHandle depthTextureID{};
        if (m_SelectedSceneDepthTexture.IsValid())
            depthTextureID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (depthTextureID.IsValid())
        {
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, depthTextureID,
                                            RHI::HeapSlotLifetime::FrameTransient);
        }

        // Per-pixel water-surface depth (nearest wavy surface) captured by the
        // water pass — lets the underwater fog find the real water boundary per
        // pixel instead of assuming a flat plane. 0 when no water rendered.
        const RHI::ResourceHandle waterDepthTexture = Renderer3D::GetWaterSurfaceDepthTextureID();
        // Persistent: renderer-owned, not acquired from the graph's transient pool.
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_UNDERWATER_WATER_DEPTH, waterDepthTexture,
                                        RHI::HeapSlotLifetime::Persistent);

        const auto va = MeshPrimitives::GetFullscreenTriangle();
        va->Bind();
        context.FlushHeapOffsets();
        context.DrawIndexed(va);

        // Leave the depth slot clean for subsequent passes that share the layout.
        //
        // THIS IS THE CALL SITE THAT MOTIVATED THE RESERVED NULL DESCRIPTOR. Under
        // the heap there is no bind to clear — the shader reads an OFFSET — so
        // leaving the previous one in the table would keep a later pass sampling
        // this frame's depth through a perfectly valid index. The seam stages
        // kNullHeapOffset instead, and the flush below publishes it: without that,
        // the clear would only take effect whenever some other pass happened to
        // flush next.
        if (depthTextureID.IsValid())
            context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, RHI::NullResource,
                                            RHI::HeapSlotLifetime::FrameTransient);
        context.BindTextureOrHeapOffset(ShaderBindingLayout::TEX_UNDERWATER_WATER_DEPTH, RHI::NullResource,
                                        RHI::HeapSlotLifetime::Persistent);
        context.FlushHeapOffsets();

        // Leave the auto-exposure storage buffers unbound so a metered exposure
        // value can't leak into other passes or tests/tools that drive the
        // tone-map shader directly without rebinding slot SSBO_AUTO_EXPOSURE_STATE.
        if (m_ExposureStateBuffer)
            m_ExposureStateBuffer->Unbind();
        if (m_HistogramBuffer)
            m_HistogramBuffer->Unbind();

        context.SetDepthMask(true);
        outputFramebuffer->Unbind();
    }

    void ToneMapRenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ToneMapRenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        CreateFramebuffer(width, height);
    }

    void ToneMapRenderPass::OnReset()
    {
        m_Target = nullptr;
        m_SelectedSceneDepthTexture = {};
    }
} // namespace OloEngine
