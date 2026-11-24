#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Scene/ComponentReflection.h"

namespace OloEngine
{
    // The owner supplies the animated model and clip through its normal mesh
    // and animation components. Its InstancedMeshComponent supplies world
    // placements and stable IDs. Custom is the member's phase offset in cycles.
    // An inline or asset placement can override the clip using ClipIndex.
    // Each member samples its clip at the owner clock's phase plus its offset.
    struct CrowdAnimationComponent
    {
        // Thresholds are projected diameters at the actual internal resolution.
        // A view keeps its tier until the diameter crosses the hysteresis band.
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1.0f, Max = 4096.0f)
        f32 m_NearPixels = 32.0f;

        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 1.0f, Max = 4096.0f)
        f32 m_FarPixels = 14.0f;

        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 0.45f)
        f32 m_Hysteresis = 0.1f;
    };
} // namespace OloEngine
