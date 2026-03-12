#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"

namespace OloEngine
{
    enum class StreamingActivationMode : u8
    {
        Proximity, // Distance-based auto load/unload
        Manual     // Script-driven only (LoadRegion/UnloadRegion)
    };

    struct StreamingVolumeComponent
    {
        AssetHandle RegionAssetHandle = 0; // Which .oloregion to stream
        StreamingActivationMode ActivationMode = StreamingActivationMode::Proximity;
        f32 LoadRadius = 200.0f;
        f32 UnloadRadius = 250.0f; // > LoadRadius for hysteresis
        bool IsLoaded = false;     // Runtime state (not serialized)
    };
} // namespace OloEngine
