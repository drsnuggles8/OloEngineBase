#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    // Shared by the two AO producers (SSAORenderPass, GTAORenderPass), which
    // write the SAME graph resource (`blackboard.AO.AOBuffer`) under the same
    // contract — so the "what does unwritten mean?" answer lives in one place
    // rather than being reimplemented, and drifting, in both (issue #771).
    //
    // AOApplyPass samples AOBuffer unconditionally and computes
    // `sceneColor * mix(1.0, ao, intensity)`. It has no signal for "my producer
    // skipped this frame", so an AO producer that returns early leaves the whole
    // frame multiplied by whatever the transient pool handed over. Freshly
    // allocated storage is not specified and is commonly zeroed — and ao = 0 is
    // not "no data", it is MAXIMUM occlusion, i.e. an exactly black frame.
    //
    // 1.0 is the identity: fully visible, so the apply pass becomes a no-op.
    inline void PublishAOTargetAsFullyVisible(RHI::ResourceHandle aoTarget)
    {
        if (!aoTarget.IsValid())
            return;

        RenderCommand::ClearTextureFloat(aoTarget, 0, glm::vec4(1.0f));
        // The consumer is AOApplyPass's sampler, so the clear needs a
        // texture-update barrier to be visible to a later fetch.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::TextureUpdate | MemoryBarrierFlags::TextureFetch |
                                     MemoryBarrierFlags::ShaderImageAccess);
    }
} // namespace OloEngine
