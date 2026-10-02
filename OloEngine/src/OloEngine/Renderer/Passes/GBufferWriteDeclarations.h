#pragma once

#include "OloEngine/Renderer/FrameBlackboard.h"
#include "OloEngine/Renderer/RGBuilder.h"

namespace OloEngine
{
    // What a pass that rasterizes into the Deferred G-Buffer after ScenePass
    // declares (VirtualGeometryPass, DeferredGPUOcclusionPass): a write on every
    // G-Buffer view the lighting, AO, SSR and temporal readers sample, so the
    // graph orders it after ScenePass and before each of them. The views ARE the
    // attachments it draws into, so nothing is copied (issue #1332).
    //
    // Declared unconditionally on every Deferred frame -- not gated on the
    // frame's draw count, because the toggles that empty a pass's work flip at
    // runtime without rebuilding the graph -- and including the multisample
    // companions, which are only valid while the G-Buffer is multisample.
    inline void DeclareGBufferWrites(RGBuilder& builder, const FrameBlackboard& board)
    {
        for (const RGTextureHandle handle :
             { board.Scene.SceneDepth, board.Scene.SceneNormals, board.GBuffer.Velocity, board.GBuffer.GBufferAlbedo,
               board.GBuffer.GBufferNormal, board.GBuffer.GBufferEmissive, board.GBuffer.GBufferAlbedoMS,
               board.GBuffer.GBufferNormalMS, board.GBuffer.GBufferEmissiveMS, board.GBuffer.VelocityMS,
               board.GBuffer.SceneDepthMS })
        {
            if (handle.IsValid())
                builder.Write(handle, RGWriteUsage::TransferDest);
        }
    }
} // namespace OloEngine
