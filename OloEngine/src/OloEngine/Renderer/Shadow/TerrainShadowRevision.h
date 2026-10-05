#pragma once

#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    // Only the inputs read by the terrain depth stages belong in the cached
    // silhouette identity. Material/VT feedback changes do not move a vertex.
    inline u64 TerrainShadowRevision(u64 revision, const ShaderBindingLayout::TerrainUBO& terrain)
    {
        const auto hash = [&revision](const auto& value)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&value);
            for (sizet i = 0; i < sizeof(value); ++i)
                revision = (revision ^ bytes[i]) * 1099511628211ull;
        };
        hash(terrain.WorldSizeAndHeightScale.x);
        hash(terrain.WorldSizeAndHeightScale.y);
        hash(terrain.WorldSizeAndHeightScale.z);
        hash(terrain.HeightmapResolution);
        hash(terrain.GpuDrivenMode);
        hash(terrain.GpuPatchGridRes);
        hash(terrain.TessFactors);
        hash(terrain.TessFactors2.y);
        hash(terrain.TessFactors2.w);
        return revision;
    }
} // namespace OloEngine
