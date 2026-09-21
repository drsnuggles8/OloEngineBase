#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GBufferDebugPass.h"

#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <array>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Draw slot 0 -> colour attachment 0, nothing else.
        constexpr std::array<u32, 1> kAttachment0Only = { 0u };
    } // namespace

    GBufferDebugPass::GBufferDebugPass()
    {
        SetName("GBufferDebugPass");
    }

    void GBufferDebugPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);

        if (!m_GBuffer)
            return;

        // READS, and only reads. This pass is a diagnostic: it must not be a
        // reason the frame is different, so the per-sample colour resolve it
        // depends on is owned by DeferredOpaqueDecalPass — the last pass that
        // WRITES the G-Buffer, which already declares those writes. An earlier
        // draft resolved here instead and the L5 validator rejected it outright
        // with five WAR hazards against VirtualShadowMapMarkPass, which reads
        // the same handles earlier in the frame.
        //
        // The read edges are what make "extract from the final G-Buffer
        // version" a graph fact rather than a comment: the three late writers
        // declare writes on exactly these handles, so the builder derives the
        // ordering and the validator sees a read-after-write.
        //
        // Declared unconditionally, NOT gated on the selected debug channel:
        // the graph fingerprints topology and caches it, so a declaration
        // behind a runtime toggle is culled for the whole session the first
        // time the toggle is off (issue #1315). Execute() reads the channel.
        for (const auto handle : { blackboard.GBuffer.GBufferAlbedo,
                                   blackboard.GBuffer.GBufferNormal,
                                   blackboard.GBuffer.GBufferEmissive,
                                   blackboard.GBuffer.Velocity,
                                   blackboard.Scene.SceneDepth })
        {
            if (handle.IsValid())
            {
                [[maybe_unused]] const auto read = builder.Read(handle, RGReadUsage::ShaderSample);
            }
        }

        if (blackboard.Scene.SceneColor.IsValid())
        {
            SetPrimaryInputFramebufferHandle(blackboard.Scene.SceneColor);
            builder.Write(blackboard.Scene.SceneColor, RGWriteUsage::RenderTarget);
        }
    }

    Ref<Framebuffer> GBufferDebugPass::GetTarget() const
    {
        // The scene colour framebuffer — this pass writes attachment 0 and the
        // depth attachment of it, and owns no framebuffer of its own.
        return m_Target;
    }

    void GBufferDebugPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (const auto sceneHandle = GetPrimaryInputFramebufferHandle(); sceneHandle.IsValid())
        {
            if (auto resolvedSceneFB = context.ResolveFramebuffer(sceneHandle))
                m_Target = resolvedSceneFB;
        }

        if (m_DebugChannel == 0u || !m_GBuffer || !m_Target)
        {
            // Nothing extracted this frame. Retire the previous record rather
            // than leaving it to be read as current — a frozen debug image
            // presented as live is the failure this record exists to prevent.
            DebugViewProvenanceRegistry::Invalidate();
            return;
        }

        // Per-sample MSAA keeps the colour attachments multisample for
        // DeferredLighting_MSAA, so the single-sample framebuffer the blit
        // below reads is only current because DeferredOpaqueDecalPass — the
        // last G-Buffer writer, several nodes back — resolved it after its own
        // writes. That is recorded, not assumed: the capture line says so, and
        // GBuffer::MarkWritten demotes the record if a writer lands afterwards.
        //
        // GBuffer::Resolve puts RT2's flags lane back from one real sample
        // after the averaging blit (issue #996), so the packed material flags
        // the channel-3 view gathers are a value some fragment actually wrote.
        const bool resolvedByTheLastWriter = m_GBuffer->GetSampleCount() > 1u && m_PerSampleLighting;

        if (!BlitChannel(m_DebugChannel))
        {
            // Nothing was drawn, so nothing about this frame is true of the
            // viewport. Retiring the record is the whole contract: a stale
            // image reported as current is the defect, not the missing blit.
            DebugViewProvenanceRegistry::Invalidate();
            return;
        }

        DebugViewProvenance record;
        record.Frame = FrameResourceManager::Get().GetTotalFrameCount();
        record.Channel = m_DebugChannel;
        record.GBufferWriteVersion = m_GBuffer->GetWriteVersion();
        // Seeded equal, NOT asserted equal: this pass runs after every G-Buffer
        // writer in the deferred chain, so the version it just read is the
        // final one as far as it can tell. GBuffer::MarkWritten raises the
        // second number and demotes Stage if that turns out to be wrong, which
        // is the only way a claim about the rest of the frame can be checked
        // rather than trusted.
        record.GBufferFinalVersion = record.GBufferWriteVersion;
        record.SampleCount = m_GBuffer->GetSampleCount();
        record.PerSampleLighting = m_PerSampleLighting;
        record.ResolvedAfterLateWriters = resolvedByTheLastWriter;
        record.Stage = DebugViewStage::FinalGBuffer;
        record.Pass = GetName();
        record.LastGBufferWriter = m_GBuffer->GetLastWriter();
        DebugViewProvenanceRegistry::Publish(record);
    }

    bool GBufferDebugPass::BlitChannel(u32 channel)
    {
        OLO_PROFILE_FUNCTION();

        // RAII guard: captures GL state on entry and restores the core subset
        // (depth / blend / stencil / cull / polygon / scissor / viewport /
        // FBO bindings / active program) on destruction. The channel==3
        // branch below binds m_DebugRMAShader + the fullscreen-tri VAO and
        // mutates the read-buffer; those bindings are explicitly cleared
        // before return so this guard stays clean rather than acting as a
        // silenced "every binding is a leak" detector.
        GLStateGuard guard("GBufferDebugPass::BlitChannel", GLStateGuard::Policy::Restore);

        // Build the target FB's full multi-attachment draw-buffer list from
        // its spec. A hardcoded 4-entry restore breaks when a scene FB
        // configured with fewer or more color attachments is installed (e.g.
        // when TAA is disabled and velocity drops).
        const auto& targetSpec = m_Target->GetSpecification();
        u32 targetColorCount = 0;
        for (const auto& att : targetSpec.Attachments.Attachments)
        {
            const bool isDepth = (att.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                  att.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F);
            if (!isDepth && att.TextureFormat != FramebufferTextureFormat::None)
                ++targetColorCount;
        }

        // Channel 3 (RMA) needs data from TWO attachments — RT0.a (metallic)
        // and RT1.zw (roughness, AO). glBlitFramebuffer cannot swizzle, so
        // use a dedicated fullscreen shader for this one channel.
        if (channel == 3)
        {
            if (!m_DebugRMAShader && !m_DebugRMAShaderFailed)
            {
                m_DebugRMAShader = Shader::Create("assets/shaders/DebugGBuffer_RMA.glsl");
                if (!m_DebugRMAShader)
                {
                    m_DebugRMAShaderFailed = true;
                    OLO_CORE_ERROR("GBufferDebugPass: failed to create assets/shaders/DebugGBuffer_RMA.glsl - "
                                   "the roughness/metallic/AO debug channel will show nothing.");
                }
            }
            if (!m_DebugRMAShader)
                return false;

            m_Target->Bind();

            const RHI::ResourceHandle dstFB = m_Target->GetRHIHandle();
            RenderCommand::SetFramebufferDrawAttachments(dstFB, kAttachment0Only);

            const u32 w = m_GBuffer->GetWidth();
            const u32 h = m_GBuffer->GetHeight();
            RenderCommand::SetViewport(0, 0, w, h);
            RenderCommand::SetDepthTest(false);
            RenderCommand::SetDepthMask(false);
            RenderCommand::SetBlendState(false);

            m_DebugRMAShader->Bind();
            // Persistent: these are the scene pass's OWN G-Buffer attachments,
            // not graph-pooled targets (issue #691).
            HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_GBUFFER_ALBEDO,
                                             m_GBuffer->GetColorAttachmentHandle(GBuffer::Albedo),
                                             RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_GBUFFER_NORMAL,
                                             m_GBuffer->GetColorAttachmentHandle(GBuffer::Normal),
                                             RHI::HeapSlotLifetime::Persistent);

            auto va = MeshPrimitives::GetFullscreenTriangle();
            va->Bind();
            HeapBinding::FlushOffsets();
            RenderCommand::DrawIndexed(va);

            // Restore the scene FB's multi-attachment draw-buffer list so the
            // downstream passes (post-process, UI) find the expected slots
            // (including RT3 velocity for TAA). Count is computed from the
            // FB spec above rather than hardcoded.
            RenderCommand::RestoreAllFramebufferDrawAttachments(dstFB, targetColorCount);

            RenderCommand::SetDepthMask(true);
            RenderCommand::SetDepthTest(true);

            // Copy depth across so selection-outline / UI still depth-test.
            const RHI::ResourceHandle srcFB = m_GBuffer->GetSamplingFramebuffer()->GetRHIHandle();
            RenderCommand::BlitFramebuffer(
                srcFB, dstFB,
                0, 0, static_cast<i32>(w), static_cast<i32>(h),
                0, 0, static_cast<i32>(w), static_cast<i32>(h),
                RHI::BlitAspect::Depth, RHI::Filter::Nearest);

            // Unbind the blit shader + VAO so the RAII guard sees us leave
            // shader/program/VAO state at zero, matching entry expectations
            // for downstream passes that rebind their own.
            RenderCommand::BindShaderProgram(RHI::NullResource);
            RenderCommand::BindVertexArrayRaw(RHI::NullResource);
            return true;
        }

        u32 attachmentIndex = 0;
        switch (channel)
        {
            case 1:
                attachmentIndex = GBuffer::Albedo;
                break;
            case 2:
                attachmentIndex = GBuffer::Normal;
                break;
            case 4:
                attachmentIndex = GBuffer::Emissive;
                break;
            case 5:
                attachmentIndex = GBuffer::Velocity;
                break;
            default:
                attachmentIndex = GBuffer::Albedo;
                break;
        }

        const RHI::ResourceHandle srcFB = m_GBuffer->GetSamplingFramebuffer()->GetRHIHandle();
        const RHI::ResourceHandle dstFB = m_Target->GetRHIHandle();
        const u32 w = m_GBuffer->GetWidth();
        const u32 h = m_GBuffer->GetHeight();

        // Select source attachment on the read FB and destination attachment 0
        // on the draw FB. A framebuffer blit requires both FBs to have their
        // read / draw attachments pre-selected.
        RenderCommand::SetFramebufferReadAttachment(srcFB, attachmentIndex);
        RenderCommand::SetFramebufferDrawAttachments(dstFB, kAttachment0Only);

        RenderCommand::BlitFramebuffer(
            srcFB, dstFB,
            0, 0, static_cast<i32>(w), static_cast<i32>(h),
            0, 0, static_cast<i32>(w), static_cast<i32>(h),
            RHI::BlitAspect::Color, RHI::Filter::Nearest);

        // Restore the draw FB's draw-buffer list using the count captured
        // from the target FB spec above — narrowing to fewer attachments
        // would drop later-shader outputs (e.g. PBR_MultiLight's motion
        // vector at layout(location=3)), breaking TAA/MotionBlur.
        RenderCommand::RestoreAllFramebufferDrawAttachments(dstFB, targetColorCount);

        // Also copy depth so downstream passes (post-process, selection
        // outline, UI) have a coherent depth buffer.
        RenderCommand::BlitFramebuffer(
            srcFB, dstFB,
            0, 0, static_cast<i32>(w), static_cast<i32>(h),
            0, 0, static_cast<i32>(w), static_cast<i32>(h),
            RHI::BlitAspect::Depth, RHI::Filter::Nearest);

        // Reset the G-Buffer's read buffer to attachment 0 so any downstream
        // read on that FB picks up a deterministic default instead of the
        // last debug channel we selected.
        RenderCommand::SetFramebufferReadAttachment(srcFB, 0);
        return true;
    }
} // namespace OloEngine
