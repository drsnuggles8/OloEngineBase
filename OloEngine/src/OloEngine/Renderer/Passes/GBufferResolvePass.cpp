#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/GBufferResolvePass.h"

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/RGBuilder.h"

namespace OloEngine
{
    GBufferResolvePass::GBufferResolvePass(const bool afterLateWriters)
        : m_AfterLateWriters(afterLateWriters)
    {
        SetName(afterLateWriters ? "GBufferResolvePass" : "GBufferEarlyResolvePass");
    }

    GBufferResolvePass::~GBufferResolvePass() = default;

    void GBufferResolvePass::SetGBuffer(const Ref<GBuffer>& buffer)
    {
        m_GBuffer = buffer;
    }

    void GBufferResolvePass::AppendDeclarationInputs(RGDeclarationKey& key) const
    {
        key.Add(m_PerSampleLighting);
    }

    void GBufferResolvePass::Setup(RGBuilder& builder, FrameBlackboard& board)
    {
        RenderGraphNode::Setup(builder, board);
        m_Resolve = false;
        if (board.Config.Path != RenderingPath::Deferred)
            return;

        const auto resolved = board.GBuffer.ResolvedFramebuffer;
        const auto multisample = board.GBuffer.MultisampleFramebuffer;
        m_Resolve = board.Config.GBufferSamples > 1u && m_PerSampleLighting == m_AfterLateWriters &&
                    resolved.IsValid() && multisample.IsValid();
        if (m_Resolve)
        {
            // Whole-framebuffer accesses cover all six colour lanes (including
            // entity ID and baked GI) and depth, not just the lighting inputs.
            [[maybe_unused]] const auto source = builder.Read(multisample, RGReadUsage::TransferSource);
            builder.Write(resolved, RGWriteUsage::TransferDest);
            // The emissive alpha bitfield uses a shader resolve after the blits.
            [[maybe_unused]] const auto flags = builder.Read(board.GBuffer.GBufferEmissiveMS, RGReadUsage::ShaderSample);
            builder.Write(board.GBuffer.GBufferEmissive, RGWriteUsage::RenderTarget);
        }
    }

    void GBufferResolvePass::Execute(RGCommandContext& /*context*/)
    {
        if (m_Resolve && m_GBuffer)
            m_GBuffer->Resolve();
    }
} // namespace OloEngine
