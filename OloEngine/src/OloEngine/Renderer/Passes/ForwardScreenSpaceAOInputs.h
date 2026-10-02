#pragma once

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine
{
    // A forward geometry pass whose shaders apply screen-space AO to their
    // ambient term (issue #1452) samples the AO buffer and the prepass depth
    // copy through include/ForwardScreenSpaceAO.glsl. Declaring the two reads
    // is what orders the pass after the AO producer and keeps both transients
    // alive while it draws. Returns whether the frame has a forward AO buffer.
    inline bool ReadForwardScreenSpaceAOInputs(RGBuilder& builder, const FrameBlackboard& board)
    {
        if (board.Config.Path == RenderingPath::Deferred || !board.AO.AOBuffer.IsValid() ||
            !board.Scene.ForwardAODepth.IsValid())
        {
            return false;
        }
        [[maybe_unused]] const auto aoRead = builder.Read(board.AO.AOBuffer, RGReadUsage::ShaderSample);
        [[maybe_unused]] const auto depthRead = builder.Read(board.Scene.ForwardAODepth, RGReadUsage::ShaderSample);
        return true;
    }

    // What a forward geometry pass's SHARE of the depth-normal prepass writes
    // (issue #1452's GPU-driven share, #1474's foliage share): it draws into
    // the scene target after ScenePrepassPass and copies the AO depth again, so
    // the forward shaders' AO upsample includes its geometry. SceneDepth and
    // SceneNormals need no copy: they are views of the scene target (#1332),
    // so the AO passes registered after the share read its draws already.
    // Declares nothing, and returns false, without a forward AO buffer — the
    // one consumer the share exists for — so that frame draws exactly as it
    // did before the share existed.
    struct ForwardPrepassShareExports
    {
        RGTextureHandle ForwardAODepth;
    };

    inline bool DeclareForwardPrepassShare(RGBuilder& builder, const FrameBlackboard& board,
                                           ForwardPrepassShareExports& out)
    {
        out = {};
        if (board.Config.Path == RenderingPath::Deferred || !board.AO.AOBuffer.IsValid() ||
            !board.Scene.ForwardAODepth.IsValid())
        {
            return false;
        }
        builder.DependsOnPass("ScenePrepassPass");
        if (board.Scene.SceneColor.IsValid())
            builder.Write(board.Scene.SceneColor, RGWriteUsage::RenderTarget);
        out.ForwardAODepth = board.Scene.ForwardAODepth;
        builder.Write(board.Scene.ForwardAODepth, RGWriteUsage::TransferDest);
        return true;
    }

    // The one copy the forward prepass writers keep (#1332): the scene target's
    // depth into ForwardAODepth, which every forward shader samples for the AO
    // upsample while depth-testing against the live attachment, so it cannot be
    // a view. Identities throughout (issue #691): the destination is a graph
    // transient and the self-copy guard compares OBJECTS.
    inline void CopyDepthIntoForwardAODepth(const RGCommandContext& context, const Ref<Framebuffer>& sceneTarget,
                                            const RGTextureHandle forwardAODepth)
    {
        if (!forwardAODepth.IsValid() || !sceneTarget)
            return;
        const auto& spec = sceneTarget->GetSpecification();
        const RHI::ResourceHandle source = sceneTarget->GetDepthAttachmentHandle();
        const RHI::ResourceHandle destination = context.ResolveTextureHandle(forwardAODepth);
        if (spec.Width == 0u || spec.Height == 0u || !source.IsValid() || !destination.IsValid() || destination == source)
            return;
        RenderCommand::CopyImageSubData(source, RendererAPI::TextureTargetType::Texture2D,
                                        destination, RendererAPI::TextureTargetType::Texture2D,
                                        spec.Width, spec.Height);
    }
} // namespace OloEngine
