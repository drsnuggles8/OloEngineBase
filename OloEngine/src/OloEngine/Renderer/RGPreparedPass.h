#pragma once

#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <functional>
#include <vector>

namespace OloEngine
{
    class RGCommandContext;

    // Resolved physical identities supplement the planner's logical graph
    // edges: two transient names can alias one backing allocation this frame.
    // Conservatively name a whole image/buffer, including every attachment a
    // prepared framebuffer can write. Read/read sharing is permitted.
    struct RGRecordingResourceUse
    {
        RHI::ResourceHandle Resource;
        bool Write = false;
    };

    // Prepared on the caller after earlier graph dependencies have joined.
    // Record owns/captures all resolved inputs and private mutable uploads. It
    // must not resolve graph resources, allocate GPU objects, issue queries,
    // read back, or mutate shared CPU state. Publish runs on the caller after
    // ordered execution, before a consumer can prepare its own recording.
    struct RGPreparedPass
    {
        std::function<void(RGCommandContext&)> Record;
        std::function<void()> Publish;
        std::vector<RGRecordingResourceUse> Resources;
        u32 InstanceCapacity = 1u;
    };

    [[nodiscard]] inline bool RecordingResourcesConflict(const RGPreparedPass& first, const RGPreparedPass& second)
    {
        for (const auto& left : first.Resources)
            for (const auto& right : second.Resources)
                if (left.Resource.IsValid() && left.Resource == right.Resource && (left.Write || right.Write))
                    return true;
        return false;
    }
} // namespace OloEngine
