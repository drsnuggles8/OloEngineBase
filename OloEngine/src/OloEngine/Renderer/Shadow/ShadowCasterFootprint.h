#pragma once

#include "OloEngine/Renderer/BoundingVolume.h"

namespace OloEngine
{
    // Stable scene instance/part identity, world bounds and silhouette revision.
    // Key zero uses a conservative geometry/transform identity for legacy callers.
    struct ShadowCasterFootprint
    {
        u64 Key = 0;
        BoundingBox Bounds = NoBounds;
        u64 Revision = 0;
    };
} // namespace OloEngine
