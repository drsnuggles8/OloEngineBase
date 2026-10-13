#pragma once

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"

namespace OloEngine
{
    // Late geometry loads the previous writer's contents and rasterizes into
    // exactly one attachment family. Resolved exports are produced by the
    // resolve node, not by every multisample writer (#1553).
    inline void DeclareGBufferWrites(RGBuilder& builder, const FrameBlackboard& board, const bool perSampleLighting)
    {
        const auto target = perSampleLighting && board.GBuffer.MultisampleFramebuffer.IsValid()
                                ? board.GBuffer.MultisampleFramebuffer
                                : board.GBuffer.ResolvedFramebuffer;
        if (target.IsValid())
        {
            [[maybe_unused]] const auto previous = builder.Read(target, RGReadUsage::RenderTargetRead);
            builder.Write(target, RGWriteUsage::RenderTarget);
        }
    }

    // These passes transfer current MS depth and then sample the resolved
    // result before rasterizing again. Virtual geometry does this between its
    // two raster phases; GPU occlusion does it before its phase-2 raster.
    inline void DeclareGBufferDepthResolve(RGBuilder& builder, const FrameBlackboard& board)
    {
        if (!board.GBuffer.SceneDepthMS.IsValid() || !board.Scene.SceneDepth.IsValid())
            return;
        [[maybe_unused]] const auto source = builder.ReadTransferSourceBeforeWrite(board.GBuffer.SceneDepthMS);
        builder.Write(board.Scene.SceneDepth, RGWriteUsage::TransferDest);
        [[maybe_unused]] const auto depth = builder.Read(board.Scene.SceneDepth, RGReadUsage::ShaderSample);
        builder.AllowSamePassReadWrite(board.Scene.SceneDepth);
    }
} // namespace OloEngine
