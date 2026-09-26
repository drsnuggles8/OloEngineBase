#pragma once

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"
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
    // the scene target after ScenePrepassPass and re-exports the depth, the
    // view normals and the AO depth copy, so the AO passes registered after it
    // read versions that include its geometry. Declares nothing, and returns
    // false, without a forward AO buffer — the one consumer the share exists
    // for — so that frame draws exactly as it did before the share existed.
    struct ForwardPrepassShareExports
    {
        RGTextureHandle SceneDepth;
        RGTextureHandle SceneNormals;
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
        if (board.Scene.SceneDepth.IsValid())
        {
            out.SceneDepth = board.Scene.SceneDepth;
            builder.Write(board.Scene.SceneDepth, RGWriteUsage::TransferDest);
        }
        if (board.Scene.SceneNormals.IsValid())
        {
            out.SceneNormals = board.Scene.SceneNormals;
            builder.Write(board.Scene.SceneNormals, RGWriteUsage::TransferDest);
        }
        out.ForwardAODepth = board.Scene.ForwardAODepth;
        builder.Write(board.Scene.ForwardAODepth, RGWriteUsage::TransferDest);
        return true;
    }
} // namespace OloEngine
