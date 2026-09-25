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
} // namespace OloEngine
